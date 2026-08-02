/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/SchedulerTimer.h"
#include "pedigree/kernel/machine/SchedulerTimerHandler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/time/Time.h"
#include "system/kernel/machine/hosted/SchedulerTimer.h"
#include "system/kernel/machine/hosted/Timer.h"

#include <signal.h>

extern "C" int hostedSchedulerExitUserProbe(void *parameter);
extern "C" void hostedSchedulerExitUserProbeTimedOut(void *parameter);

#if HOSTED && BITS_64
asm(
    ".text\n"
    ".globl hostedSchedulerExitUserProbe\n"
    ".type hostedSchedulerExitUserProbe,@function\n"
    "hostedSchedulerExitUserProbe:\n"
    "movq %rdi,%r12\n"
    "movq $1,0(%r12)\n"
    "subq $16,%rsp\n"
    "movl $228,%eax\n"
    "movl $1,%edi\n"
    "movq %rsp,%rsi\n"
    "syscall\n"
    "testq %rax,%rax\n"
    "js 2f\n"
    "movq 0(%rsp),%r13\n"
    "addq $3,%r13\n"
    "movq $100000,%r14\n"
    "1:\n"
    "pause\n"
    "decq %r14\n"
    "jnz 1b\n"
    "movq $100000,%r14\n"
    "movl $228,%eax\n"
    "movl $1,%edi\n"
    "movq %rsp,%rsi\n"
    "syscall\n"
    "testq %rax,%rax\n"
    "js 2f\n"
    "cmpq %r13,0(%rsp)\n"
    "jl 1b\n"
    "2:\n"
    "movq $1,8(%r12)\n"
    "subq $16,%rsp\n"
    "movq $0xa00,0(%rsp)\n"
    "movl $14,%eax\n"
    "xorl %edi,%edi\n"
    "movq %rsp,%rsi\n"
    "xorl %edx,%edx\n"
    "movl $8,%r10d\n"
    "syscall\n"
    "call hostedSetKernelFs@PLT\n"
    "movq 24(%r12),%rsp\n"
    "andq $-16,%rsp\n"
    "movq %r12,%rdi\n"
    "call hostedSchedulerExitUserProbeTimedOut@PLT\n"
    "ud2\n"
    ".size hostedSchedulerExitUserProbe,.-hostedSchedulerExitUserProbe\n");
#endif

namespace
{
class HostedAccountingProcess : public Process
{
  public:
    explicit HostedAccountingProcess(Thread *driver)
        : Process(DeferredPublication(), driver->getParent()), m_Driver(driver),
          calls(0), user(0), profile(0), failures(0)
    {
        enableTimeAccountingReports();
        description() += "hosted deferred accounting probe";
        publish();
    }

    ~HostedAccountingProcess() override
    {
        prepareForDestruction();
    }

    Thread *m_Driver;
    Atomic<size_t> calls;
    Atomic<size_t> user;
    Atomic<size_t> profile;
    Atomic<size_t> failures;

  private:
    void reportTimesUpdated(
        Time::Timestamp userTotal,
        Time::Timestamp profileTotal) override
    {
        Thread *current = Processor::information().getCurrentThread();
        const size_t processCount =
            Scheduler::instance().getNumProcesses();
        const size_t threadCount = getNumThreads();
        (void) threadCount;
        if (
            !Processor::getInterrupts() || Processor::inDeviceHardIrq() ||
            !current || current == m_Driver || !processCount ||
            userTotal < user || profileTotal < profile)
        {
            failures += 1;
        }
        user = static_cast<size_t>(userTotal);
        profile = static_cast<size_t>(profileTotal);
        calls += 1;
    }
};

struct AccountingThreadContext
{
    AccountingThreadContext(Process *process, Time::Timestamp kernelBefore)
        : process(process), kernelBefore(kernelBefore), ran(0),
          firstSliceAccounted(0)
    {
    }

    Process *process;
    Time::Timestamp kernelBefore;
    Atomic<size_t> ran;
    Atomic<size_t> firstSliceAccounted;
};

int accountedKernelThread(void *parameter)
{
    AccountingThreadContext *context =
        reinterpret_cast<AccountingThreadContext *>(parameter);
    Scheduler::instance().yield();
    context->firstSliceAccounted =
        context->process->getKernelTime() > context->kernelBefore;
    context->ran = 1;
    return 0;
}

bool deferredTimeAccountingWorker()
{
    Thread *driver = Processor::information().getCurrentThread();
    HostedAccountingProcess *process = new HostedAccountingProcess(driver);

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    process->publishTimeAccountingForHostedTest(13, 7);
    Processor::setInterrupts(interruptsWereEnabled);

    constexpr size_t Attempts = 10000;
    for (size_t attempt = 0; !process->calls && attempt < Attempts; ++attempt)
    {
        Scheduler::instance().yield();
    }

    const bool exactWorkerBatch =
        process->calls == 1 && process->user == 13 &&
        process->profile == 20 && !process->failures;

    AccountingThreadContext threadContext(
        process, process->getKernelTime());
    Thread *accountedThread = new Thread(
        process, accountedKernelThread, &threadContext, nullptr, false, true,
        true);
    accountedThread->setName("hosted accounting first-slice probe");
    const bool accountedThreadStarted = accountedThread->start();
    const bool accountedThreadJoined =
        accountedThreadStarted && accountedThread->joinForCompletion();
    if (!accountedThreadStarted)
    {
        delete accountedThread;
    }

    for (size_t attempt = 0;
         process->profile == 20 && attempt < Attempts; ++attempt)
    {
        Scheduler::instance().yield();
    }
    const bool firstKernelSliceAccounted =
        accountedThreadStarted && accountedThreadJoined &&
        threadContext.ran && threadContext.firstSliceAccounted &&
        process->user == 13 && process->profile > 20 && !process->failures;

    process->closeTimeAccountingForHostedTest();
    const size_t callsBeforeLatePublication = process->calls;
    process->publishTimeAccountingForHostedTest(101, 211);
    for (size_t attempt = 0; attempt < 32; ++attempt)
    {
        Scheduler::instance().yield();
    }
    const bool latePublicationDiscarded =
        process->calls == callsBeforeLatePublication;
    delete process;

    const bool passed =
        exactWorkerBatch && firstKernelSliceAccounted &&
        latePublicationDiscarded;
    if (!passed)
    {
        ERROR(
            "HOSTED-WAIT-TEST: FAIL deferred-time-accounting-worker: "
            "accounting escaped its ordinary worker, first-slice, or "
            "teardown boundary");
    }
    else
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS deferred-time-accounting-worker");
    }
    return passed;
}

struct ContextSwitchContext
{
    explicit ContextSwitchContext(Thread *driver)
        : driver(driver), phase(0), switchReturns(0), bookkeepingCalls(0),
          restoreBoundaries(0), tickCalls(0), targetCalls(0), failures(0)
    {
    }

    Thread *driver;
    Atomic<size_t> phase;
    Atomic<size_t> switchReturns;
    Atomic<size_t> bookkeepingCalls;
    Atomic<size_t> restoreBoundaries;
    Atomic<size_t> tickCalls;
    Atomic<size_t> targetCalls;
    Atomic<size_t> failures;
};

ContextSwitchContext *g_ContextSwitchContext = nullptr;

struct SchedulerTimerContext
{
    SchedulerTimerContext() : calls(0), failures(0)
    {
    }

    Atomic<size_t> calls;
    Atomic<size_t> failures;
};

SchedulerTimerContext *g_SchedulerTimerContext = nullptr;

constexpr int DeferredTimerExitCode = 73;

struct SchedulerExitContext;

struct SchedulerExitUserProbeState
{
    uintptr_t ready;
    uintptr_t timedOut;
    SchedulerExitContext *context;
    uintptr_t kernelStackTop;
};

static_assert(
    __builtin_offsetof(SchedulerExitUserProbeState, ready) == 0 &&
        __builtin_offsetof(SchedulerExitUserProbeState, timedOut) == 8 &&
        __builtin_offsetof(SchedulerExitUserProbeState, kernelStackTop) == 24,
    "hosted scheduler user probe assembly layout changed");

struct SchedulerExitContext
{
    SchedulerExitContext()
        : user{0, 0, this, 0}, target(nullptr), event(nullptr),
          userStackBase(0), userStackTop(0), tickCalls(0), queued(0),
          eventCalls(0), exitCalls(0), failures(0)
    {
    }

    SchedulerExitUserProbeState user;
    Thread *target;
    Event *event;
    uintptr_t userStackBase;
    uintptr_t userStackTop;
    Atomic<size_t> tickCalls;
    Atomic<size_t> queued;
    Atomic<size_t> eventCalls;
    Atomic<size_t> exitCalls;
    Atomic<size_t> failures;
};

SchedulerExitContext *g_SchedulerExitContext = nullptr;

void schedulerExitEventHandler(size_t)
{
    SchedulerExitContext *context = __atomic_load_n(
        &g_SchedulerExitContext, __ATOMIC_ACQUIRE);
    if (!context)
    {
        return;
    }

    Thread *current = Processor::information().getCurrentThread();
    if (
        current != context->target || !Processor::getInterrupts() ||
        Processor::inDeviceHardIrq() || current->getHostedSignalDepth() ||
        current->currentTimeAccountingMode() != CpuTimeMode::Kernel)
    {
        context->failures += 1;
    }
    context->eventCalls += 1;
    current->deferProcessExit(DeferredTimerExitCode);
}

class SchedulerExitEvent : public Event
{
  public:
    SchedulerExitEvent()
        : Event(
              reinterpret_cast<uintptr_t>(&schedulerExitEventHandler), false)
    {
    }

    size_t serialize(uint8_t *) override
    {
        return 0;
    }

    size_t getNumber() override
    {
        return 0x45584954;
    }
};

class SchedulerExitSubsystem : public Subsystem
{
  public:
    explicit SchedulerExitSubsystem(SchedulerExitContext &context)
        : Subsystem(None), m_Context(context)
    {
    }

    void exit(int code) override
    {
        Thread *current = Processor::information().getCurrentThread();
        if (
            code != DeferredTimerExitCode || current != m_Context.target ||
            m_Context.eventCalls != 1 || !Processor::getInterrupts() ||
            Processor::inDeviceHardIrq() ||
            current->getHostedSignalDepth() ||
            current->currentTimeAccountingMode() != CpuTimeMode::Kernel)
        {
            m_Context.failures += 1;
        }
        m_Context.exitCalls += 1;
        Thread::threadExited();
    }

    bool kill(KillReason, Thread *) override
    {
        return false;
    }

    bool invoke(const char *, Vector<String> &, Vector<String> &) override
    {
        return false;
    }

    bool invoke(
        const char *, Vector<String> &, Vector<String> &,
        SyscallState &) override
    {
        return false;
    }

    bool invoke(
        File *, const String &, Vector<String> &, Vector<String> &) override
    {
        return false;
    }

    bool invoke(
        File *, const String &, Vector<String> &, Vector<String> &,
        SyscallState &) override
    {
        return false;
    }

    File *findFile(const String &, File *) override
    {
        return nullptr;
    }

  private:
    SchedulerExitContext &m_Context;
};

class SchedulerExitProcess : public Process
{
  public:
    SchedulerExitProcess(SchedulerExitContext &context, Process *parent)
        : Process(DeferredPublication(), parent)
    {
        setSubsystem(new SchedulerExitSubsystem(context));
        description() += "hosted scheduler return-tail exit probe";
        publish();
    }

    ~SchedulerExitProcess() override
    {
        prepareForDestruction();
    }
};

void queueExitEventFromSchedulerTick(uint64_t delta, InterruptState &state)
{
    SchedulerExitContext *context = __atomic_load_n(
        &g_SchedulerExitContext, __ATOMIC_ACQUIRE);
    Thread *current = Processor::information().getCurrentThread();
    if (!context)
    {
        return;
    }
    if (!__atomic_load_n(&context->user.ready, __ATOMIC_ACQUIRE))
    {
        return;
    }
    if (current != context->target)
    {
        return;
    }
    if (
        state.kernelMode() || current->getStateLevel() ||
        state.getStackPointer() < context->userStackBase ||
        state.getStackPointer() > context->userStackTop)
    {
        return;
    }
    if (!context->queued.compareAndSwap(0, 1))
    {
        return;
    }

    const uint64_t interval = 100 * Time::Multiplier::Millisecond;
    if (
        delta < interval || (delta % interval) || Processor::getInterrupts() ||
        current->getHostedSignalDepth() != 1 ||
        Processor::inDeviceHardIrq() ||
        state.kernelMode() ||
        state.getStackPointer() < context->userStackBase ||
        state.getStackPointer() > context->userStackTop ||
        current->currentTimeAccountingMode() != CpuTimeMode::Kernel ||
        state.getInterruptNumber() != SIGUSR2 ||
        state.getInterruptSource() != HostedSchedulerTimer::sourceForTest())
    {
        context->failures += 1;
    }

    // Test-only injection at the exact historical window: the old timer path
    // dispatched this preallocated Event before returning from the hard IRQ.
    if (!current->sendEvent(context->event))
    {
        context->failures += 1;
    }
    context->tickCalls += 1;
}

bool schedulerTimerExitDeferral()
{
    constexpr const char *Test = "scheduler-timer-exit-return-tail";
    SchedulerExitContext context;
    SchedulerExitEvent event;
    context.event = &event;
    const bool timerSlowed = HostedTimer::setSignalIntervalForTest(
        4 * Time::Multiplier::Second);
    Thread *driver = Processor::information().getCurrentThread();
    SchedulerExitProcess *process =
        driver ? new SchedulerExitProcess(context, driver->getParent())
               : nullptr;
    VirtualAddressSpace::Stack *userStack =
        process ? process->getAddressSpace()->allocateStack() : nullptr;
    Thread *target = nullptr;
    if (userStack)
    {
        context.userStackBase =
            reinterpret_cast<uintptr_t>(userStack->getBase());
        context.userStackTop =
            reinterpret_cast<uintptr_t>(userStack->getTop());
        target = new Thread(
            process, hostedSchedulerExitUserProbe, &context.user,
            userStack->getTop(), false, true, true);
        context.target = target;
        context.user.kernelStackTop =
            reinterpret_cast<uintptr_t>(target->getKernelStack());
        target->setName("hosted scheduler exit return-tail probe");
    }

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    __atomic_store_n(&g_SchedulerExitContext, &context, __ATOMIC_RELEASE);
    HostedSchedulerTimer::setHardContextHookForTest(
        queueExitEventFromSchedulerTick);
    Processor::setInterrupts(interruptsWereEnabled);

    const bool started = target && target->start();
    const bool joined = started && target->joinForCompletion();
    if (target && !started)
    {
        delete target;
    }

    Processor::setInterrupts(false);
    HostedSchedulerTimer::setHardContextHookForTest(nullptr);
    __atomic_store_n(
        &g_SchedulerExitContext, static_cast<SchedulerExitContext *>(nullptr),
        __ATOMIC_RELEASE);
    const bool timerRestored = HostedTimer::setSignalIntervalForTest(
        Time::Multiplier::Millisecond);
    Processor::setInterrupts(interruptsWereEnabled);

    event.waitForDeliveries();
    if (userStack)
    {
        process->getAddressSpace()->freeStack(userStack);
    }
    if (process)
    {
        delete process;
    }

    const bool passed =
        timerSlowed && timerRestored && started && joined &&
        context.user.ready && !context.user.timedOut &&
        context.tickCalls == 1 &&
        context.queued == 1 && context.eventCalls == 1 &&
        context.exitCalls == 1 && !context.failures;
    if (!passed)
    {
        ERROR(
            "HOSTED-WAIT-TEST: FAIL " << Test
                                       << ": process exit ran before the "
                                          "IRQ return-to-user tail");
    }
    else
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "scheduler-timer-exit-return-tail");
    }
    return passed;
}

struct HostedSignalSwitchContext
{
    explicit HostedSignalSwitchContext(Thread *driver)
        : driver(driver), target(nullptr), armed(0), waiting(0), computing(0),
          driverTicks(0), targetTicks(0), ran(0), failures(0)
    {
    }

    Thread *driver;
    Thread *target;
    Atomic<size_t> armed;
    Atomic<size_t> waiting;
    Atomic<size_t> computing;
    Atomic<size_t> driverTicks;
    Atomic<size_t> targetTicks;
    Atomic<size_t> ran;
    Atomic<size_t> failures;
};

HostedSignalSwitchContext *g_HostedSignalSwitchContext = nullptr;

void observeHostedAutodisarmTick(uint64_t delta, InterruptState &state)
{
    HostedSignalSwitchContext *context = __atomic_load_n(
        &g_HostedSignalSwitchContext, __ATOMIC_ACQUIRE);
    if (!context || !context->armed)
    {
        return;
    }

    Thread *current = Processor::information().getCurrentThread();
    if (current == context->driver)
    {
        context->driverTicks += 1;
        return;
    }
    if (
        current != context->target || !context->computing ||
        !context->targetTicks.compareAndSwap(0, 1))
    {
        return;
    }

    const uint64_t interval = 100 * Time::Multiplier::Millisecond;
    if (
        delta < interval || (delta % interval) || !state.kernelMode() ||
        !current->getHostedSignalDepth() ||
        Processor::hostedSignalFrameDepthForTest() < 2 ||
        Processor::getInterrupts() || Processor::inDeviceHardIrq() ||
        current->currentTimeAccountingMode() != CpuTimeMode::Kernel ||
        state.getInterruptNumber() != SIGUSR2 ||
        state.getInterruptSource() != HostedSchedulerTimer::sourceForTest())
    {
        context->failures += 1;
    }
}

int hostedSignalSwitchTarget(void *parameter)
{
    HostedSignalSwitchContext *context =
        reinterpret_cast<HostedSignalSwitchContext *>(parameter);
    context->waiting = 1;
    while (!context->armed)
    {
        Scheduler::instance().yield();
    }

    // This continuation is selected by the real scheduler signal while its
    // frame remains live on another Pedigree stack.
    Processor::setInterrupts(true);
    __pedigree_hosted::sigset_t mask;
    __pedigree_hosted::sigprocmask(0, nullptr, &mask);
    Thread *current = Processor::information().getCurrentThread();
    if (
        !current || current->getHostedSignalDepth() ||
        !Processor::hostedSignalFrameDepthForTest() ||
        !Processor::getInterrupts() ||
        __pedigree_hosted::sigismember(&mask, SIGUSR1) ||
        __pedigree_hosted::sigismember(&mask, SIGUSR2))
    {
        context->failures += 1;
    }

    context->computing = 1;
    __pedigree_hosted::timespec startedAt = {};
    __pedigree_hosted::timespec now = {};
    __pedigree_hosted::clock_gettime(CLOCK_MONOTONIC, &startedAt);
    while (!context->targetTicks)
    {
        Processor::pause();
        __pedigree_hosted::clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - startedAt.tv_sec >= 3)
        {
            context->failures += 1;
            break;
        }
    }
    context->ran = 1;
    return 0;
}

bool hostedSignalMaskSpansContextSwitch()
{
    constexpr const char *Test = "hosted-signal-autodisarm-preemption";
    HostedSignalSwitchContext context(
        Processor::information().getCurrentThread());
    Thread *target = new Thread(
        Scheduler::instance().getKernelProcess(), hostedSignalSwitchTarget,
        &context, nullptr, false, true, true);
    target->setName("hosted signal-mask context-switch probe");
    context.target = target;
    const bool started = target->start();

    constexpr size_t Attempts = 10000;
    for (size_t attempt = 0;
         started && !context.waiting && attempt < Attempts; ++attempt)
    {
        Scheduler::instance().yield();
    }

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    __atomic_store_n(
        &g_HostedSignalSwitchContext, &context, __ATOMIC_RELEASE);
    HostedSchedulerTimer::setHardContextHookForTest(
        observeHostedAutodisarmTick);
    context.armed = 1;
    Processor::setInterrupts(interruptsWereEnabled);
    const Time::Timestamp deadline =
        Time::getTicks() + (3 * Time::Multiplier::Second);
    while (!context.ran && Time::getTicks() < deadline)
    {
        Processor::pause();
    }

    Processor::setInterrupts(false);
    HostedSchedulerTimer::setHardContextHookForTest(nullptr);
    __atomic_store_n(
        &g_HostedSignalSwitchContext,
        static_cast<HostedSignalSwitchContext *>(nullptr),
        __ATOMIC_RELEASE);
    Processor::setInterrupts(interruptsWereEnabled);

    // A failed preemption must not leave the probe stack live while its
    // context record goes out of scope.
    for (size_t attempt = 0;
         started && !context.ran && attempt < Attempts; ++attempt)
    {
        Scheduler::instance().yield();
    }

    const bool joined = context.ran && target->joinForCompletion();
    if (!started)
    {
        delete target;
    }

    const bool passed =
        started && context.waiting && context.computing &&
        context.driverTicks && context.targetTicks == 1 && context.ran &&
        joined && !context.failures;
    if (!passed)
    {
        ERROR(
            "HOSTED-WAIT-TEST: FAIL " << Test
                                       << ": a switched-in compute thread "
                                          "did not receive its next real "
                                          "scheduler tick");
    }
    else
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "hosted-signal-autodisarm-preemption");
    }
    return passed;
}

void observeSchedulerTimerHardContext(uint64_t delta, InterruptState &state)
{
    SchedulerTimerContext *context = __atomic_load_n(
        &g_SchedulerTimerContext, __ATOMIC_ACQUIRE);
    if (!context)
    {
        return;
    }

    Thread *current = Processor::information().getCurrentThread();
    const uint64_t interval = 100 * Time::Multiplier::Millisecond;
    if (delta < interval || (delta % interval) || !current ||
        !current->getHostedSignalDepth() ||
        !Processor::onHostedExecutionThread() ||
        Processor::inDeviceHardIrq() ||
        Processor::deviceHardIrqDepthForTest() != 0 ||
        state.getInterruptNumber() != SIGUSR2 ||
        state.getInterruptSource() != HostedSchedulerTimer::sourceForTest())
    {
        context->failures += 1;
    }
    context->calls += 1;
}

bool schedulerTimerHardContext()
{
    constexpr const char *Test = "hosted-scheduler-timer-hard-context";
    SchedulerTimerContext context;

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    __atomic_store_n(
        &g_SchedulerTimerContext, &context, __ATOMIC_RELEASE);
    HostedSchedulerTimer::setHardContextHookForTest(
        observeSchedulerTimerHardContext);
    Processor::setInterrupts(interruptsWereEnabled);

    const Time::Timestamp deadline =
        Time::getTicks() + (2 * Time::Multiplier::Second);
    while (!context.calls && Time::getTicks() < deadline)
    {
        Scheduler::instance().yield();
    }

    Processor::setInterrupts(false);
    HostedSchedulerTimer::setHardContextHookForTest(nullptr);
    __atomic_store_n(
        &g_SchedulerTimerContext,
        static_cast<SchedulerTimerContext *>(nullptr), __ATOMIC_RELEASE);
    Processor::setInterrupts(interruptsWereEnabled);

    const bool passed = context.calls && !context.failures;
    if (!passed)
    {
        ERROR(
            "HOSTED-WAIT-TEST: FAIL " << Test
                                      << ": the scheduler callback did not "
                                         "suspend only its device-hard marker");
    }
    else
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "hosted-scheduler-timer-hard-context");
    }
    return passed;
}

class ConflictingSchedulerTimerHandler : public SchedulerTimerHandler
{
  public:
    ConflictingSchedulerTimerHandler() : calls(0)
    {
    }

    void timer(uint64_t, InterruptState &) override
    {
        calls += 1;
    }

    Atomic<size_t> calls;
};

bool schedulerTimerSingleOwner()
{
    constexpr const char *Test = "hosted-scheduler-timer-single-owner";
    SchedulerTimer *timer = Machine::instance().getSchedulerTimer();
    SchedulerTimerHandler *owner =
        HostedSchedulerTimer::publishedHandlerForTest();
    ConflictingSchedulerTimerHandler conflicting;

    const bool ownerPublished = owner != nullptr;
    const bool nullRegistrationRejected =
        timer && !timer->registerHandler(nullptr);
    const bool duplicateRejected =
        timer && !timer->registerHandler(owner);
    const bool conflictRejected =
        timer && !timer->registerHandler(&conflicting);
    const bool nullRemovalRejected =
        timer && !timer->removeHandler(nullptr);
    const bool wrongOwnerRejected =
        timer && !timer->removeHandler(&conflicting);
    const bool ownerPreserved =
        HostedSchedulerTimer::publishedHandlerForTest() == owner;

    const bool passed =
        timer && ownerPublished && nullRegistrationRejected &&
        duplicateRejected && conflictRejected && nullRemovalRejected &&
        wrongOwnerRejected && ownerPreserved && !conflicting.calls;
    if (!passed)
    {
        ERROR(
            "HOSTED-WAIT-TEST: FAIL " << Test
                                       << ": handler ownership was replaced "
                                          "or removed by a non-owner");
    }
    else
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "hosted-scheduler-timer-single-owner");
    }
    return passed;
}

class QueuedTickHandler : public HardIrqHandler
{
  public:
    explicit QueuedTickHandler(ContextSwitchContext &context)
        : m_Context(context)
    {
    }

    bool irq(irq_id_t, InterruptState &) override
    {
        const size_t phase = m_Context.phase;
        if (phase && phase < 4)
        {
            m_Context.tickCalls += 1;
            if (phase != 3 || Processor::getInterrupts() ||
                !Processor::inDeviceHardIrq() ||
                Processor::deviceHardIrqDepthForTest() != 1)
            {
                m_Context.failures += 1;
            }
            m_Context.phase = 4;
        }
        return true;
    }

  private:
    ContextSwitchContext &m_Context;
};

void contextSwitchHook(ProcessorBase::HostedContextSwitchStage stage)
{
    ContextSwitchContext *context =
        __atomic_load_n(&g_ContextSwitchContext, __ATOMIC_ACQUIRE);
    if (!context ||
        Processor::information().getCurrentThread() != context->driver)
    {
        return;
    }

    switch (stage)
    {
        case ProcessorBase::HostedContextSwitchStage::SwitchStateReturnedMasked:
            if (!context->phase.compareAndSwap(0, 1))
            {
                return;
            }
            context->switchReturns += 1;
            if (Processor::getInterrupts() ||
                !Processor::queueHostedSchedulerTickForTest())
            {
                context->failures += 1;
            }
            break;
        case ProcessorBase::HostedContextSwitchStage::
            SchedulerBookkeepingComplete:
            if (context->phase == static_cast<size_t>(1))
            {
                context->bookkeepingCalls += 1;
                if (Processor::getInterrupts() ||
                    !context->phase.compareAndSwap(1, 2))
                {
                    context->failures += 1;
                }
            }
            break;
        case ProcessorBase::HostedContextSwitchStage::
            SchedulerRestoringInterrupts:
            if (context->phase == static_cast<size_t>(2))
            {
                context->restoreBoundaries += 1;
                if (Processor::getInterrupts() ||
                    !context->phase.compareAndSwap(2, 3))
                {
                    context->failures += 1;
                }
            }
            break;
    }
}

int contextSwitchTarget(void *parameter)
{
    ContextSwitchContext *context =
        reinterpret_cast<ContextSwitchContext *>(parameter);
    context->targetCalls += 1;
    return 0;
}

bool check(bool condition, const char *detail)
{
    if (condition)
    {
        return true;
    }

    ERROR(
        "HOSTED-WAIT-TEST: FAIL context-switch-interrupt-restore: " << detail);
    return false;
}
}  // namespace

extern "C" void hostedSchedulerExitUserProbeTimedOut(void *parameter)
{
    SchedulerExitUserProbeState *state =
        reinterpret_cast<SchedulerExitUserProbeState *>(parameter);
    if (state && state->context)
    {
        state->context->failures += 1;
    }
    Thread::threadExited();
}

bool runHostedSchedulerRegressions()
{
    if (
        !hostedSignalMaskSpansContextSwitch() ||
        !schedulerTimerSingleOwner() || !schedulerTimerHardContext() ||
        !schedulerTimerExitDeferral() ||
        !deferredTimeAccountingWorker())
    {
        return false;
    }

    Thread *driver = Processor::information().getCurrentThread();
    ContextSwitchContext context(driver);
    QueuedTickHandler tickHandler(context);
    IrqManager *irqManager = Machine::instance().getIrqManager();
    const irq_id_t tickId =
        irqManager->registerHardIsaIrqHandler(
            1, &tickHandler, IrqPolicy::syntheticHard());

    Thread *target = new Thread(
        Scheduler::instance().getKernelProcess(), contextSwitchTarget, &context,
        nullptr, false, true, true);
    target->setName("hosted context-switch IRQ target");

    __atomic_store_n(&g_ContextSwitchContext, &context, __ATOMIC_RELEASE);
    Processor::setHostedContextSwitchHook(contextSwitchHook);
    const bool started = target->start();

    constexpr size_t Attempts = 10000;
    bool completed = false;
    for (size_t attempt = 0; started && attempt < Attempts; ++attempt)
    {
        if (context.phase == static_cast<size_t>(4) &&
            target->isReapableForHostedTest())
        {
            completed = true;
            break;
        }
        Scheduler::instance().yield();
    }

    Processor::setHostedContextSwitchHook(nullptr);
    __atomic_store_n(
        &g_ContextSwitchContext, static_cast<ContextSwitchContext *>(nullptr),
        __ATOMIC_RELEASE);

    const bool targetJoined =
        target->isReapableForHostedTest() && target->joinForCompletion();
    const bool handlerRemoved =
        tickId && irqManager->unregisterHandler(tickId, &tickHandler);

    const bool passed = check(
        tickId && started && completed && targetJoined && handlerRemoved &&
            context.switchReturns == 1 && context.bookkeepingCalls == 1 &&
            context.restoreBoundaries == 1 && context.tickCalls == 1 &&
            context.targetCalls == 1 && context.failures == 0,
        "the queued scheduler IRQ escaped the masked post-switch boundary");
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS context-switch-interrupt-restore");
    }
    return passed;
}
