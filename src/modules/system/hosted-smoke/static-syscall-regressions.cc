/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "modules/Module.h"
#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/net-syscalls.h"
#include "modules/subsys/posix/poll-syscalls.h"
#include "modules/subsys/posix/system-syscalls.h"
#include "modules/subsys/pedigree-c/pedigreecSyscallNumbers.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#undef PEDIGREE_INIT_SIGRET
#undef PEDIGREE_SIGRET
#include "modules/subsys/posix/syscalls/posixSyscallNumbers.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/SyscallManager.h"

namespace
{
constexpr size_t HostedAttempts = 10000;

class DescriptorRetirementProbe : public FileDescriptor
{
  public:
    explicit DescriptorRetirementProbe(Atomic<size_t> &destructions)
        : FileDescriptor(), m_Destructions(destructions)
    {
    }

    ~DescriptorRetirementProbe() override
    {
        m_Destructions += 1;
    }

  private:
    Atomic<size_t> &m_Destructions;
};

class PollGenerationProbe : public NetworkSyscalls
{
  public:
    PollGenerationProbe(
        Atomic<size_t> &registrations, Atomic<size_t> &unpolls,
        Atomic<size_t> &destructions)
        : NetworkSyscalls(AF_UNSPEC, SOCK_STREAM, 0),
          m_Registrations(registrations), m_Unpolls(unpolls),
          m_Destructions(destructions), m_Ready(0), m_Waiter(nullptr)
    {
    }

    ~PollGenerationProbe() override
    {
        m_Destructions += 1;
    }

    int connect(const struct sockaddr_storage *, socklen_t) override
    {
        return -1;
    }

    ssize_t sendto_msg(const struct msghdr *) override
    {
        return -1;
    }

    ssize_t recvfrom_msg(struct msghdr *) override
    {
        return -1;
    }

    int listen(int) override
    {
        return -1;
    }

    int bind(const struct sockaddr_storage *, socklen_t) override
    {
        return -1;
    }

    int accept(struct sockaddr_storage *, socklen_t *, int) override
    {
        return -1;
    }

    int getpeername(struct sockaddr_storage *, socklen_t *) override
    {
        return -1;
    }

    int getsockname(struct sockaddr_storage *, socklen_t *) override
    {
        return -1;
    }

    int setsockopt(int, int, const void *, socklen_t) override
    {
        return -1;
    }

    int getsockopt(int, int, void *, socklen_t *) override
    {
        return -1;
    }

    bool canPoll() const override
    {
        return true;
    }

    bool poll(
        bool &read, bool &write, bool &error, Semaphore *waiter) override
    {
        const bool readable = read && m_Ready;
        read = readable;
        write = false;
        error = false;
        if (waiter && !readable)
        {
            m_Waiter = waiter;
            m_Registrations += 1;
        }
        return readable;
    }

    void unPoll(Semaphore *waiter) override
    {
        m_Unpolls += 1;
        if (m_Waiter == waiter)
        {
            m_Waiter = nullptr;
        }
    }

    void makeReadable()
    {
        m_Ready = 1;
        Semaphore *waiter = m_Waiter;
        if (waiter)
        {
            waiter->release();
        }
    }

    const void *waiterAddress() const
    {
        return m_Waiter;
    }

  private:
    Atomic<size_t> &m_Registrations;
    Atomic<size_t> &m_Unpolls;
    Atomic<size_t> &m_Destructions;
    Atomic<size_t> m_Ready;
    Atomic<Semaphore *> m_Waiter;
};

struct DescriptorCloseContext
{
    DescriptorCloseContext(PosixSubsystem *subsystem, size_t fd)
        : subsystem(subsystem), fd(fd), release(0, false), entered(0),
          acquired(0), usedAfterClose(0), returned(0)
    {
    }

    PosixSubsystem *subsystem;
    size_t fd;
    Semaphore release;
    Atomic<size_t> entered;
    Atomic<size_t> acquired;
    Atomic<size_t> usedAfterClose;
    Atomic<size_t> returned;
};

int holdDescriptorAcrossBlock(void *parameter)
{
    DescriptorCloseContext *context =
        reinterpret_cast<DescriptorCloseContext *>(parameter);
    DescriptorLease descriptor;
    context->acquired = context->subsystem->acquireFileDescriptor(
                            context->fd, descriptor)
                            ? 1
                            : 0;
    context->entered += 1;

    if (!context->release.acquireForCompletion())
    {
        context->returned += 1;
        return 1;
    }

    if (descriptor && descriptor->fd == context->fd)
    {
        context->usedAfterClose += 1;
    }
    context->returned += 1;
    return 0;
}

bool descriptorClosePinning(Process *kernelProcess)
{
    constexpr size_t DescriptorNumber = 37;
    Process *process = new Process(kernelProcess);
    PosixSubsystem *subsystem = new PosixSubsystem;
    process->setSubsystem(subsystem);

    Atomic<size_t> destructions(0);
    DescriptorRetirementProbe *probe =
        new DescriptorRetirementProbe(destructions);
    probe->fd = DescriptorNumber;
    subsystem->addFileDescriptor(DescriptorNumber, probe);

    DescriptorCloseContext context(subsystem, DescriptorNumber);
    Thread *worker = new Thread(
        kernelProcess, holdDescriptorAcrossBlock, &context, nullptr, false,
        true, true);
    worker->setName("hosted descriptor pin holder");

    bool passed = worker->start();
    bool blocked = false;
    for (size_t attempt = 0; attempt < HostedAttempts && passed; ++attempt)
    {
        Thread::WaitDebugInfo info = {};
        if (
            context.entered && worker->getWaitDebugInfo(info) && info.queue &&
            info.queued && info.channelOwner == &context.release &&
            worker->getStatus() == Thread::Sleeping)
        {
            blocked = true;
            break;
        }
        Scheduler::instance().yield();
    }

    passed = passed && blocked && context.acquired == 1;
    DescriptorLease closing;
    const bool closeAcquired =
        subsystem->acquireFileDescriptor(DescriptorNumber, closing);
    const bool closed = closeAcquired &&
                        subsystem->closeFileDescriptor(
                            DescriptorNumber, closing);
    closing.reset();
    passed = passed && closed;

    DescriptorLease unpublished;
    passed = passed &&
             !subsystem->acquireFileDescriptor(
                 DescriptorNumber, unpublished) &&
             destructions == 0;

    context.release.release();
    passed = worker->join() && passed;
    passed = passed && context.returned == 1 &&
             context.usedAfterClose == 1 && destructions == 1;

    delete process;

    if (!passed)
    {
        ERROR(
            "HOSTED-SYSCALL-TEST: FAIL descriptor-close-pinning: "
            "close did not unpublish immediately while retaining the active "
            "operation");
        return false;
    }

    NOTICE("HOSTED-SYSCALL-TEST: PASS descriptor-close-pinning");
    return true;
}

bool descriptorCloseGeneration(Process *kernelProcess)
{
    constexpr size_t DescriptorNumber = 38;
    Process *process = new Process(kernelProcess);
    PosixSubsystem *subsystem = new PosixSubsystem;
    process->setSubsystem(subsystem);

    Atomic<size_t> oldDestructions(0);
    Atomic<size_t> replacementDestructions(0);
    DescriptorRetirementProbe *oldDescriptor =
        new DescriptorRetirementProbe(oldDestructions);
    oldDescriptor->fd = DescriptorNumber;
    oldDescriptor->offset = 1;
    subsystem->addFileDescriptor(DescriptorNumber, oldDescriptor);

    DescriptorLease oldLease;
    bool passed = subsystem->acquireFileDescriptor(
        DescriptorNumber, oldLease);

    DescriptorRetirementProbe *replacement =
        new DescriptorRetirementProbe(replacementDestructions);
    replacement->fd = DescriptorNumber;
    replacement->offset = 2;
    subsystem->addFileDescriptor(DescriptorNumber, replacement);

    // An in-flight close of the old generation must not remove a descriptor
    // which has since reused the same numeric fd.
    passed = passed &&
             !subsystem->closeFileDescriptor(DescriptorNumber, oldLease) &&
             oldDestructions == 0 && replacementDestructions == 0;

    DescriptorLease replacementLease;
    passed = passed &&
             subsystem->acquireFileDescriptor(
                 DescriptorNumber, replacementLease) &&
             replacementLease->offset == 2;

    oldLease.reset();
    passed = passed && oldDestructions == 1 &&
             replacementDestructions == 0;

    const bool replacementClosed =
        subsystem->closeFileDescriptor(DescriptorNumber, replacementLease);
    DescriptorLease unpublished;
    passed = passed && replacementClosed &&
             !subsystem->acquireFileDescriptor(
                 DescriptorNumber, unpublished) &&
             replacementDestructions == 0;
    replacementLease.reset();
    passed = passed && replacementDestructions == 1;

    delete process;

    if (!passed)
    {
        ERROR(
            "HOSTED-SYSCALL-TEST: FAIL descriptor-close-generation: "
            "an old close removed a reused descriptor generation");
        return false;
    }

    NOTICE("HOSTED-SYSCALL-TEST: PASS descriptor-close-generation");
    return true;
}

struct PollCloseReuseContext
{
    explicit PollCloseReuseContext(size_t fd)
        : descriptor{static_cast<int>(fd), POLLIN, 0}, result(-2),
          entered(0), returned(0)
    {
    }

    struct pollfd descriptor;
    Atomic<int> result;
    Atomic<size_t> entered;
    Atomic<size_t> returned;
};

int pollAcrossCloseReuse(void *parameter)
{
    PollCloseReuseContext *context =
        reinterpret_cast<PollCloseReuseContext *>(parameter);
    context->entered += 1;
    context->result = posix_poll_safe(&context->descriptor, 1, -1);
    context->returned += 1;
    return context->result == 1 ? 0 : 1;
}

bool pollCloseReuseCleanup(Process *kernelProcess)
{
    constexpr size_t DescriptorNumber = 39;
    Process *process = new Process(kernelProcess);
    PosixSubsystem *subsystem = new PosixSubsystem;
    process->setSubsystem(subsystem);

    Atomic<size_t> aRegistrations(0);
    Atomic<size_t> aUnpolls(0);
    Atomic<size_t> aNetworkDestructions(0);
    Atomic<size_t> aDescriptorDestructions(0);
    PollGenerationProbe *aNetwork = new PollGenerationProbe(
        aRegistrations, aUnpolls, aNetworkDestructions);
    SharedPointer<NetworkSyscalls> aNetworkKeepalive(aNetwork);
    DescriptorRetirementProbe *aDescriptor =
        new DescriptorRetirementProbe(aDescriptorDestructions);
    aDescriptor->fd = DescriptorNumber;
    aDescriptor->offset = 1;
    aDescriptor->networkImpl = aNetworkKeepalive;
    subsystem->addFileDescriptor(DescriptorNumber, aDescriptor);

    Atomic<size_t> bRegistrations(0);
    Atomic<size_t> bUnpolls(0);
    Atomic<size_t> bNetworkDestructions(0);
    Atomic<size_t> bDescriptorDestructions(0);
    PollGenerationProbe *bNetwork = new PollGenerationProbe(
        bRegistrations, bUnpolls, bNetworkDestructions);
    SharedPointer<NetworkSyscalls> bNetworkKeepalive(bNetwork);
    DescriptorRetirementProbe *bDescriptor =
        new DescriptorRetirementProbe(bDescriptorDestructions);
    bDescriptor->fd = DescriptorNumber;
    bDescriptor->offset = 2;
    bDescriptor->networkImpl = bNetworkKeepalive;

    PollCloseReuseContext context(DescriptorNumber);
    Thread *worker = new Thread(
        process, pollAcrossCloseReuse, &context, nullptr, false, true, true);
    worker->setName("hosted poll close-reuse worker");
    const bool started = worker->start();
    bool blockedOnA = false;
    for (size_t attempt = 0; attempt < HostedAttempts && started; ++attempt)
    {
        Thread::WaitDebugInfo info = {};
        if (
            context.entered && aRegistrations &&
            worker->getWaitDebugInfo(info) && info.queue && info.queued &&
            info.channelOwner == aNetwork->waiterAddress() &&
            worker->getStatus() == Thread::Sleeping)
        {
            blockedOnA = true;
            break;
        }
        Scheduler::instance().yield();
    }

    bool passed = started && blockedOnA && aRegistrations == 1;
    DescriptorLease closingA;
    const bool acquiredA = subsystem->acquireFileDescriptor(
        DescriptorNumber, closingA);
    const bool closedA = acquiredA && subsystem->closeFileDescriptor(
                                          DescriptorNumber, closingA);
    closingA.reset();
    subsystem->addFileDescriptor(DescriptorNumber, bDescriptor);
    passed = passed && closedA && aDescriptorDestructions == 0 &&
             aNetworkDestructions == 0;

    if (aRegistrations)
    {
        aNetwork->makeReadable();
    }
    else
    {
        // Failure cleanup: if the worker did not pin A, allow any lookup of B
        // to finish rather than leaving the hosted smoke run blocked.
        bNetwork->makeReadable();
    }

    const bool joined = started && worker->join();
    passed = passed && joined && context.returned == 1 &&
             context.result == 1 &&
             (context.descriptor.revents & POLLIN) && aUnpolls == 1 &&
             bUnpolls == 0 && bRegistrations == 0 &&
             aDescriptorDestructions == 1 &&
             aNetworkDestructions == 0;
    aNetworkKeepalive.reset();
    passed = passed && aNetworkDestructions == 1;

    DescriptorLease closingB;
    const bool acquiredB = subsystem->acquireFileDescriptor(
        DescriptorNumber, closingB);
    const bool closedB = acquiredB && subsystem->closeFileDescriptor(
                                          DescriptorNumber, closingB);
    closingB.reset();
    passed = passed && closedB && bDescriptorDestructions == 1 &&
             bNetworkDestructions == 0;
    bNetworkKeepalive.reset();
    passed = passed && bNetworkDestructions == 1;

    delete process;

    if (!passed)
    {
        ERROR(
            "HOSTED-SYSCALL-TEST: FAIL poll-close-reuse-cleanup: "
            "poll cleanup followed the reused fd instead of its registered "
            "descriptor generation");
        return false;
    }

    NOTICE("HOSTED-SYSCALL-TEST: PASS poll-close-reuse-cleanup");
    return true;
}

struct PosixTeardownContext
{
    explicit PosixTeardownContext(Process *process)
        : process(process), releaseGate(0, false), holderEntered(0),
          holderReturned(0), reaperEntered(0), processDeleted(0)
    {
    }

    Process *process;
    Semaphore releaseGate;
    Atomic<size_t> holderEntered;
    Atomic<size_t> holderReturned;
    Atomic<size_t> reaperEntered;
    Atomic<size_t> processDeleted;
};

int holdMemoryMapLifecycleGate(void *parameter)
{
    PosixTeardownContext *context =
        reinterpret_cast<PosixTeardownContext *>(parameter);
    MemoryMapManager::instance().acquireLifecycleGateForHostedTest();
    context->holderEntered += 1;
    const bool released = context->releaseGate.acquireForCompletion();
    MemoryMapManager::instance().releaseLifecycleGateForHostedTest();
    context->holderReturned += 1;
    return released ? 0 : 1;
}

int deletePosixProcess(void *parameter)
{
    PosixTeardownContext *context =
        reinterpret_cast<PosixTeardownContext *>(parameter);
    context->reaperEntered += 1;
    delete context->process;
    context->processDeleted += 1;
    return 0;
}

bool posixTeardownContention(Process *kernelProcess)
{
    Process *process = new Process(kernelProcess);
    process->setSubsystem(new PosixSubsystem);
    PosixTeardownContext context(process);

    Thread *holder = new Thread(
        kernelProcess, holdMemoryMapLifecycleGate, &context, nullptr, false,
        true, true);
    holder->setName("hosted mmap lifecycle holder");
    bool passed = holder->start();

    for (size_t attempt = 0;
         attempt < HostedAttempts && passed && !context.holderEntered;
         ++attempt)
    {
        Scheduler::instance().yield();
    }
    passed = passed && context.holderEntered == 1;

    Thread *reaper = nullptr;
    bool blocked = false;
    if (passed)
    {
        reaper = new Thread(
            kernelProcess, deletePosixProcess, &context, nullptr, false, true,
            true);
        reaper->setName("hosted POSIX process reaper");
        passed = reaper->start();

        for (size_t attempt = 0; attempt < HostedAttempts && passed; ++attempt)
        {
            Thread::WaitDebugInfo info = {};
            if (
                context.reaperEntered && reaper->getWaitDebugInfo(info) &&
                info.queue && info.queued &&
                info.channelOwner ==
                    MemoryMapManager::instance()
                        .lifecycleGateAddressForHostedTest() &&
                reaper->getStatus() == Thread::Sleeping)
            {
                blocked = true;
                break;
            }
            Scheduler::instance().yield();
        }
        passed = passed && blocked && context.processDeleted == 0;
    }

    context.releaseGate.release();
    passed = holder->join() && passed;
    if (reaper)
    {
        passed = reaper->join() && passed;
        passed = passed && context.processDeleted == 1;
    }
    else
    {
        delete process;
    }

    if (!passed)
    {
        ERROR(
            "HOSTED-SYSCALL-TEST: FAIL posix-teardown-contention: "
            "real PosixSubsystem destruction did not sleep and resume on "
            "the memory-map lifecycle gate");
        return false;
    }

    NOTICE("HOSTED-SYSCALL-TEST: PASS posix-teardown-contention");
    return true;
}

bool zeroResultWinsSignal(Thread *thread)
{
    thread->setErrno(0);
    thread->setInterruptionReason(Thread::InterruptedBySignal);
    const bool completed = finishInterruptibleSocketCall(
        thread, static_cast<ssize_t>(0));
    const bool passed = completed &&
                        thread->getInterruptionReason() ==
                            Thread::NotInterrupted &&
                        !thread->getErrno();
    if (!passed)
    {
        ERROR(
            "HOSTED-SYSCALL-TEST: FAIL socket-zero-result-signal: "
            "EOF or zero-length success was replaced with EINTR");
        thread->clearInterruption();
        thread->setErrno(0);
        return false;
    }

    NOTICE("HOSTED-SYSCALL-TEST: PASS socket-zero-result-signal");
    return true;
}

bool cloneStateDropsParentErrnoDestination()
{
    long error = 0;
    SyscallState parent = {};
    parent.error_ptr = reinterpret_cast<uintptr_t>(&error);
    parent.result = 37;

    const SyscallState child = posix_copy_clone_state(parent);
    const bool passed = !child.error_ptr && child.result == parent.result &&
                        parent.error_ptr == reinterpret_cast<uintptr_t>(&error);
    if (!passed)
    {
        ERROR(
            "HOSTED-SYSCALL-TEST: FAIL clone-errno-lifetime: "
            "the child retained its parent's stack-local errno destination");
        return false;
    }

    NOTICE("HOSTED-SYSCALL-TEST: PASS clone-errno-lifetime");
    return true;
}

bool entry()
{
    Thread *thread =
        Processor::information().getCurrentThread();
    if (!thread || thread->getStateLevel())
    {
        ERROR(
            "HOSTED-SYSCALL-TEST: FAIL real-event-boundaries: "
            "module initialisation was not at base state");
        return false;
    }

    Process *kernelProcess = Scheduler::instance().getKernelProcess();
    if (
        !kernelProcess || !descriptorClosePinning(kernelProcess) ||
        !descriptorCloseGeneration(kernelProcess) ||
        !pollCloseReuseCleanup(kernelProcess) ||
        !posixTeardownContention(kernelProcess) ||
        !zeroResultWinsSignal(thread) ||
        !cloneStateDropsParentErrnoDestination())
    {
        return false;
    }

    SyscallManager &manager = SyscallManager::instance();
    static char pedigreeCModule[] = "pedigree-c";
    const uintptr_t sigretResult =
        manager.syscall(posix, PEDIGREE_SIGRET);
    const uintptr_t unwindResult =
        manager.syscall(posix, PEDIGREE_UNWIND_SIGNAL);
    const uintptr_t eventReturnResult =
        manager.syscall(pedigree_c, PEDIGREE_EVENT_RETURN);
    const uintptr_t selfUnloadResult = manager.syscall(
        pedigree_c, PEDIGREE_MODULE_UNLOAD,
        reinterpret_cast<uintptr_t>(pedigreeCModule));
    const uintptr_t stillLoadedResult = manager.syscall(
        pedigree_c, PEDIGREE_MODULE_IS_LOADED,
        reinterpret_cast<uintptr_t>(pedigreeCModule));

    if (
        sigretResult != static_cast<uintptr_t>(-1) ||
        unwindResult != static_cast<uintptr_t>(-1) ||
        eventReturnResult != static_cast<uintptr_t>(-1) ||
        selfUnloadResult != static_cast<uintptr_t>(-1) ||
        stillLoadedResult != 1 || thread->getStateLevel() ||
        thread->getErrno())
    {
        ERROR(
            "HOSTED-SYSCALL-TEST: FAIL real-event-boundaries: "
            "a public misuse path escaped its lifetime boundary");
        return false;
    }

    NOTICE(
        "HOSTED-SYSCALL-TEST: PASS real-event-boundaries");
    return true;
}

void exit()
{
}
}  // namespace

MODULE_INFO(
    "hosted-syscall-smoke", &entry, &exit, "posix", "pedigree-c");
