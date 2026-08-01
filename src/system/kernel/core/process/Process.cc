/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#if THREADS

#include "pedigree/kernel/process/Process.h"
#include "modules/system/users/Group.h"
#include "modules/system/users/User.h"
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/MemoryAllocator.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/ZombieQueue.h"
#include "pedigree/kernel/utilities/utility.h"

Process *Process::m_pInitProcess = 0;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
Process::TerminationElectionHook Process::m_TerminationElectionHook = nullptr;
Process::OrphanPublicationHook Process::m_OrphanPublicationHook = nullptr;
#endif

namespace
{
bool canAdoptChildren(Process *pProcess, Process *pExcluded = 0)
{
    if (!pProcess || pProcess == pExcluded)
    {
        return false;
    }

    const Process::ProcessState state = pProcess->getState();
    return state == Process::Active || state == Process::Suspended;
}
}  // namespace

Process::ThreadLease::ThreadLease()
    : m_pProcess(nullptr), m_pThread(nullptr), m_TerminationDeferral(false)
{
}

Process::ThreadLease::ThreadLease(Process *process, Thread *thread)
    : m_pProcess(process), m_pThread(thread),
      m_TerminationDeferral(process && thread)
{
}

Process::ThreadLease::ThreadLease(ThreadLease &&other)
    : m_pProcess(other.m_pProcess), m_pThread(other.m_pThread),
      m_TerminationDeferral(
          pedigree_std::move(other.m_TerminationDeferral))
{
    other.m_pProcess = nullptr;
    other.m_pThread = nullptr;
}

Process::ThreadLease::~ThreadLease()
{
    reset();
}

Process::ThreadLease &Process::ThreadLease::operator=(ThreadLease &&other)
{
    if (this != &other)
    {
        reset();
        m_TerminationDeferral =
            pedigree_std::move(other.m_TerminationDeferral);
        m_pProcess = other.m_pProcess;
        m_pThread = other.m_pThread;
        other.m_pProcess = nullptr;
        other.m_pThread = nullptr;
    }
    return *this;
}

void Process::ThreadLease::reset()
{
    Process *process = m_pProcess;
    Thread *thread = m_pThread;
    m_pProcess = nullptr;
    m_pThread = nullptr;
    if (process)
    {
        process->releaseThreadLease(thread);
    }
    m_TerminationDeferral = TerminationDeferral(false);
}

Process::Process() : Process(DeferredPublication())
{
    publish();
}

Process::Process(DeferredPublication)
    : m_Threads(), m_NextTid(0),
      m_Id(Scheduler::instance().reserveProcessId()), str(), m_pParent(0),
      m_pAddressSpace(&VirtualAddressSpace::getKernelAddressSpace()),
      m_ExitStatus(0), m_Cwd(0), m_Ctty(0), m_SpaceAllocator(false),
      m_DynamicSpaceAllocator(false), m_pUser(0), m_pGroup(0),
      m_pEffectiveUser(0), m_pEffectiveGroup(0), m_pDynamicLinker(0),
      m_pSubsystem(0), m_ChildStateWaiters(), m_TerminationWaiters(),
      m_SuspensionWaiters(), m_ThreadJoinWaiters(),
      m_nThreadJoinOperations(0), m_bThreadJoinAdmissionClosed(false),
      m_ExternalLeaseLock(false), m_ExternalLeaseWaiters(),
      m_nExternalLeases(0),
      m_bExternalLeaseAdmissionClosed(false),
      m_bUnreportedSuspend(false), m_bUnreportedResume(false), m_State(Active),
      m_bDestroying(false), m_bPublished(false), m_bUnregistered(false),
      m_pTerminatingThread(0), m_nTerminationParticipants(0),
      m_bTerminationRendezvousStarted(false),
      m_bTerminationCleanupStarted(false), m_bTerminationSealed(false),
      m_bTerminationReapable(false), m_Lock(false), m_Metadata(),
      m_LastKernelEntry(0),
      m_LastUserspaceEntry(0), m_pRootFile(0), m_bSharedAddressSpace(false)
{
    resetCounts();
    m_Metadata.startTime = Time::getTimeNanoseconds();

    getSpaceAllocator().free(
        getAddressSpace()->getUserStart(),
        getAddressSpace()->getUserReservedStart() -
            getAddressSpace()->getUserStart());
    if (getAddressSpace()->getDynamicStart())
    {
        getDynamicSpaceAllocator().free(
            getAddressSpace()->getDynamicStart(),
            getAddressSpace()->getDynamicEnd() -
                getAddressSpace()->getDynamicStart());
    }
}

Process::Process(Process *pParent, bool bCopyOnWrite)
    : Process(DeferredPublication(), pParent, bCopyOnWrite)
{
    publish();
}

Process::Process(
    DeferredPublication, Process *pParent, bool bCopyOnWrite)
    : m_Threads(), m_NextTid(0),
      m_Id(Scheduler::instance().reserveProcessId()), str(),
      m_pParent(pParent),
      m_pAddressSpace(0), m_ExitStatus(0), m_Cwd(pParent->m_Cwd),
      m_Ctty(pParent->m_Ctty), m_SpaceAllocator(pParent->m_SpaceAllocator),
      m_DynamicSpaceAllocator(pParent->m_DynamicSpaceAllocator),
      m_pUser(pParent->m_pUser), m_pGroup(pParent->m_pGroup),
      m_pEffectiveUser(pParent->m_pEffectiveUser),
      m_pEffectiveGroup(pParent->m_pEffectiveGroup),
      m_pDynamicLinker(pParent->m_pDynamicLinker), m_pSubsystem(0),
      m_ChildStateWaiters(), m_TerminationWaiters(), m_SuspensionWaiters(),
      m_ThreadJoinWaiters(), m_nThreadJoinOperations(0),
      m_bThreadJoinAdmissionClosed(false),
      m_ExternalLeaseLock(false), m_ExternalLeaseWaiters(),
      m_nExternalLeases(0),
      m_bExternalLeaseAdmissionClosed(false),
      m_bUnreportedSuspend(false), m_bUnreportedResume(false),
      m_State(Active), m_bDestroying(false), m_bPublished(false),
      m_bUnregistered(false), m_pTerminatingThread(0),
      m_nTerminationParticipants(0),
      m_bTerminationRendezvousStarted(false),
      m_bTerminationCleanupStarted(false), m_bTerminationSealed(false),
      m_bTerminationReapable(false), m_Lock(false),
      m_Metadata(pParent->m_Metadata), m_LastKernelEntry(0),
      m_LastUserspaceEntry(0), m_pRootFile(pParent->m_pRootFile),
      m_bSharedAddressSpace(!bCopyOnWrite)
{
    m_pAddressSpace = pParent->m_pAddressSpace->clone(bCopyOnWrite);
    str = pParent->str;

    // Annotate the temporary description.
    if (m_bSharedAddressSpace)
    {
        str += "<C>";  // C for cloned (i.e. shared address space)
    }
    else
    {
        str += "<F>";  // F for forked.
    }
}

void Process::publish()
{
    if (m_bPublished)
    {
        FATAL("Process::publish() called more than once.");
    }

    Process *pRequestedParent = getParent();
    if (!pRequestedParent)
    {
        Scheduler::instance().addProcess(this);
        m_bPublished = true;
        return;
    }

    Scheduler::ProcessLease requestedParent;
    if (!Scheduler::instance().acquireProcess(
            requestedParent, pRequestedParent))
    {
        Scheduler::ProcessLease init;
        const bool initAcquired =
            Scheduler::instance().acquireProcess(init, Process::getInit());
        Process *publishParent =
            initAcquired && canAdoptChildren(init.get(), this)
                ? init.get()
                : nullptr;
        if (publishParent)
        {
            auto publishGuard =
                publishParent->m_ChildStateWaiters.acquire();
            if (canAdoptChildren(publishParent, this))
            {
                __atomic_store_n(
                    &m_pParent, publishParent, __ATOMIC_RELEASE);
                Scheduler::instance().addProcess(this);
                m_bPublished = true;
                publishGuard.wakeAll();
                return;
            }
        }
        __atomic_store_n(
            &m_pParent, static_cast<Process *>(nullptr), __ATOMIC_RELEASE);
        Scheduler::instance().addProcess(this);
        m_bPublished = true;
        return;
    }

    // Publication and parent teardown use the same child-state guard. If the
    // requested parent has already begun exiting, publish under a live init or
    // as an orphan instead of appearing after the parent's reparenting scan.
    auto parentGuard =
        requestedParent->m_ChildStateWaiters.acquire();
    Process *pPublishParent = pRequestedParent;
    if (!canAdoptChildren(pPublishParent))
    {
        pPublishParent = nullptr;
    }

    if (!pPublishParent)
    {
        Scheduler::ProcessLease publishParent;
        const bool publishParentAcquired =
            Scheduler::instance().acquireProcess(
                publishParent, Process::getInit());
        if (
            publishParentAcquired &&
            publishParent.get() != pRequestedParent &&
            canAdoptChildren(publishParent.get(), this))
        {
            auto publishGuard =
                publishParent->m_ChildStateWaiters.acquire();
            pPublishParent = publishParent.get();
            __atomic_store_n(
                &m_pParent, pPublishParent, __ATOMIC_RELEASE);
            Scheduler::instance().addProcess(this);
            m_bPublished = true;
            parentGuard.wakeAll();
            publishGuard.wakeAll();
            return;
        }
    }

    if (!canAdoptChildren(pPublishParent, this))
    {
        pPublishParent = 0;
    }
    __atomic_store_n(&m_pParent, pPublishParent, __ATOMIC_RELEASE);
    Scheduler::instance().addProcess(this);
    m_bPublished = true;
    parentGuard.wakeAll();
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void Process::makeOrphanBeforePublicationForHostedTest()
{
    if (m_bPublished)
    {
        FATAL(
            "Process test fixture attempted to become an orphan after "
            "publication.");
    }
    __atomic_store_n(
        &m_pParent, static_cast<Process *>(nullptr), __ATOMIC_RELEASE);
}
#endif

void Process::prepareForDestruction()
{
    Process *expectedInit = this;
    __atomic_compare_exchange_n(
        &m_pInitProcess, &expectedInit, static_cast<Process *>(0), false,
        __ATOMIC_RELEASE, __ATOMIC_RELAXED);

    if (!m_bPublished || m_bUnregistered)
    {
        closeExternalLeaseAdmission();
        drainExternalLeases();
        return;
    }

    // Remove ourselves from enumeration while holding the same parent-owned
    // guard waitpid uses to inspect child pointers. Once this releases, a new
    // scan cannot discover this object and an existing scan has finished.
    bool unregistered = false;
    while (!unregistered)
    {
        Process *pParent = getParent();
        if (pParent)
        {
            Scheduler::ProcessLease parent;
            if (!Scheduler::instance().acquireProcess(parent, pParent))
            {
                if (getParent() != pParent)
                {
                    continue;
                }
                FATAL(
                    "Process retained an unpinned parent during "
                    "destruction.");
            }
            auto guard = parent->m_ChildStateWaiters.acquire();
            if (getParent() != pParent)
            {
                continue;
            }

            Scheduler::instance().removeProcess(this);
            m_bUnregistered = true;
            guard.wakeAll();
            unregistered = true;
        }
        else
        {
            Scheduler::instance().removeProcess(this);
            m_bUnregistered = true;
            unregistered = true;
        }
    }

    drainExternalLeases();
}

Process::~Process()
{
    TerminationDeferral terminationDeferral;
    prepareForDestruction();

    // A joiner releases this lease only after it has stopped using both the
    // target Thread and this Process. Closing admission first makes the drain
    // a one-way lifetime barrier.
    while (true)
    {
        auto joinGuard = m_ThreadJoinWaiters.acquire();
        m_bThreadJoinAdmissionClosed = true;
        if (!m_nThreadJoinOperations)
        {
            break;
        }

        const WaitQueue::WakeReason reason = joinGuard.waitForCompletion(
            WaitQueue::Channel(this), Thread::ProcessWait,
            reinterpret_cast<uintptr_t>(this));
        (void) reason;
    }

    // Make sure we have full mutual exclusion on the Subsystem before we lock
    // here. This ensures we have full access to the subsystem and avoids a case
    // where we lock here but the subsystem destruction needs to reschedule to
    // acquire the subsystem locks.
    if (m_pSubsystem)
    {
        m_pSubsystem->acquire();
    }

    // Close topology mutation while holding the vector lock, then drop the
    // spinlock before any lease drain or destructor can sleep. A remover that
    // entered earlier finishes before this transition; later add/remove calls
    // observe m_bDestroying and cannot mutate m_Threads.
    {
        LockGuard<Spinlock> guard(m_Lock);
        m_bDestroying = true;
    }

    for (Vector<Thread *>::Iterator it = m_Threads.begin();
         it != m_Threads.end(); ++it)
    {
        Thread *pThread = *it;
        pThread->closeExternalLeaseAdmissionAndDrain();
        auto threadExitGuard = pThread->m_JoinWaiters.acquire();
        if (!pThread->m_bReapable)
        {
            FATAL(
                "Process::~Process invariant failed for pid "
                << Dec << m_Id << ", tid " << pThread->getId()
                << ": thread status "
                << static_cast<size_t>(pThread->getStatus())
                << " is not off-stack/reapable.");
        }
        if (pThread->m_bJoinClaimed)
        {
            FATAL(
                "Process::~Process invariant failed for pid "
                << Dec << m_Id << ", tid " << pThread->getId()
                << ": a join claim escaped the lifetime barrier.");
        }
        if (pThread == Processor::information().getCurrentThread())
        {
            FATAL(
                "Process::~Process invariant failed for pid "
                << Dec << m_Id
                << ": destructor is running on a thread stack it would free.");
        }
    }

    // Reapable threads have no live stack users. Process destruction owns any
    // objects retained for join, including process-exit participants.
    for (Vector<Thread *>::Iterator it = m_Threads.begin();
         it != m_Threads.end(); ++it)
    {
        delete *it;
    }

    if (m_pSubsystem)
        delete m_pSubsystem;

    VirtualAddressSpace &VAddressSpace =
        Processor::information().getVirtualAddressSpace();

    bool bInterrupts = Processor::getInterrupts();
    Processor::setInterrupts(false);

    Processor::switchAddressSpace(*m_pAddressSpace);
    m_pAddressSpace->revertToKernelAddressSpace();
    Processor::switchAddressSpace(VAddressSpace);

    delete m_pAddressSpace;

    str.append("<Z>");

    Processor::setInterrupts(bInterrupts);

}

bool Process::beginThreadJoin()
{
    auto guard = m_ThreadJoinWaiters.acquire();
    if (m_bThreadJoinAdmissionClosed)
    {
        return false;
    }

    ++m_nThreadJoinOperations;
    return true;
}

void Process::endThreadJoin()
{
    auto guard = m_ThreadJoinWaiters.acquire();
    if (!m_nThreadJoinOperations)
    {
        FATAL(
            "Process::endThreadJoin underflow for pid "
            << Dec << m_Id << ".");
    }

    --m_nThreadJoinOperations;
    if (!m_nThreadJoinOperations)
    {
        guard.wakeAll(
            WaitQueue::WakeReason::Signalled,
            WaitQueue::Channel(this));
    }
}

bool Process::beginExternalLease()
{
    LockGuard<Spinlock> guard(m_ExternalLeaseLock);
    if (m_bExternalLeaseAdmissionClosed)
    {
        return false;
    }

    ++m_nExternalLeases;
    return true;
}

void Process::endExternalLease()
{
    bool wake = false;
    {
        LockGuard<Spinlock> guard(m_ExternalLeaseLock);
        if (!m_nExternalLeases)
        {
            FATAL(
                "Process external lease underflow for pid "
                << Dec << m_Id << ".");
        }

        --m_nExternalLeases;
        wake = !m_nExternalLeases;
    }

    // WaitQueue wakeup may enter Scheduler::threadStatusChanged. Never retain
    // the predicate lock (or its outer scheduler lock) across that call.
    if (wake)
    {
        m_ExternalLeaseWaiters.wakeAll(
            WaitQueue::WakeReason::Signalled,
            WaitQueue::Channel(this));
    }
}

void Process::closeExternalLeaseAdmission()
{
    LockGuard<Spinlock> guard(m_ExternalLeaseLock);
    m_bExternalLeaseAdmissionClosed = true;
}

void Process::drainExternalLeases()
{
    TerminationDeferral terminationDeferral;
    while (true)
    {
        auto guard = m_ExternalLeaseWaiters.acquire();
        {
            LockGuard<Spinlock> stateGuard(m_ExternalLeaseLock);
            m_bExternalLeaseAdmissionClosed = true;
            if (!m_nExternalLeases)
            {
                return;
            }
        }

        const WaitQueue::WakeReason reason = guard.waitForCompletion(
            WaitQueue::Channel(this), Thread::ProcessWait,
            reinterpret_cast<uintptr_t>(this));
        (void) reason;
    }
}

void Process::releaseThreadLease(Thread *thread)
{
    if (thread)
    {
        thread->endExternalLease();
    }
    endExternalLease();
}

size_t Process::addThread(Thread *pThread)
{
    LockGuard<Spinlock> guard(m_Lock);
    if (!pThread)
        return ~0;
    const ProcessState state = getState();
    if (
        m_bDestroying || m_bTerminationSealed ||
        state == Terminated || state == Reaped)
    {
        FATAL(
            "Process::addThread invariant failed for pid "
            << Dec << m_Id
            << ": a thread cannot be published after exit rendezvous seals.");
    }
    if (state == Terminating)
    {
        // A create-vs-exit race joins the rendezvous but never runs its entry
        // point. The add worker retires this Created thread off-stack.
        pThread->m_bProcessExitOwned = true;
        pThread->m_bProcessExitParticipant = true;
        __atomic_store_n(
            &pThread->m_UnwindState, Thread::TerminateThread,
            __ATOMIC_RELEASE);
        ++m_nTerminationParticipants;
    }
    m_Threads.pushBack(pThread);
    return m_NextTid += 1;
}

void Process::removeThread(Thread *pThread)
{
    RecursingLockGuard<Spinlock> guard(m_Lock);

    // The destructor owns its vector iteration and deliberately leaves removal
    // until the whole Process object disappears. Logical process termination,
    // however, can outlive detached Thread destruction while waitpid waits.
    if (m_bDestroying)
        return;
    for (Vector<Thread *>::Iterator it = m_Threads.begin();
         it != m_Threads.end(); it++)
    {
        if (*it == pThread)
        {
            m_Threads.erase(it);
            break;
        }
    }

    if (m_pSubsystem)
        m_pSubsystem->threadRemoved(pThread);
}

size_t Process::getNumThreads()
{
    LockGuard<Spinlock> guard(m_Lock);
    return m_Threads.count();
}

bool Process::acquireThread(ThreadLease &lease, size_t n)
{
    Thread *thread = nullptr;
    {
        LockGuard<Spinlock> guard(m_Lock);
        if (n < m_Threads.count() && beginExternalLease())
        {
            thread = m_Threads[n];
            if (!thread || !thread->beginExternalLease())
            {
                endExternalLease();
                thread = nullptr;
            }
        }
    }

    if (!thread)
    {
        lease.reset();
        return false;
    }

    lease = ThreadLease(this, thread);
    return true;
}

bool Process::acquireThread(ThreadLease &lease, Thread *expected)
{
    if (!expected)
    {
        lease.reset();
        return false;
    }

    Thread *thread = nullptr;
    {
        LockGuard<Spinlock> guard(m_Lock);
        for (Vector<Thread *>::Iterator it = m_Threads.begin();
             it != m_Threads.end(); ++it)
        {
            if (*it == expected)
            {
                thread = *it;
                break;
            }
        }
        if (!thread || !beginExternalLease())
        {
            thread = nullptr;
        }
        else if (!thread->beginExternalLease())
        {
            endExternalLease();
            thread = nullptr;
        }
    }

    if (!thread)
    {
        lease.reset();
        return false;
    }

    lease = ThreadLease(this, thread);
    return true;
}

bool Process::transitionState(ProcessState expected, ProcessState desired)
{
    return __atomic_compare_exchange_n(
        &m_State, &expected, desired, false, __ATOMIC_ACQ_REL,
        __ATOMIC_ACQUIRE);
}

void Process::transitionToTerminating()
{
    while (true)
    {
        const ProcessState state = getState();
        if (state == Terminating)
        {
            return;
        }
        if (state != Active && state != Suspended)
        {
            FATAL(
                "Process state cannot transition to Terminating from "
                << Dec << static_cast<size_t>(state) << " for pid " << m_Id
                << ".");
        }
        if (transitionState(state, Terminating))
        {
            return;
        }
    }
}

void Process::reap()
{
    if (!transitionState(Terminated, Reaped) && getState() != Reaped)
    {
        FATAL(
            "Process state cannot transition to Reaped from "
            << Dec << static_cast<size_t>(getState()) << " for pid " << m_Id
            << ".");
    }
}

void Process::markTerminating()
{
    transitionToTerminating();
}

bool Process::beginTermination()
{
    m_Lock.acquire();
    Thread *pCurrentThread =
        Processor::information().getCurrentThread();
    if (!pCurrentThread || pCurrentThread->getParent() != this)
    {
        FATAL(
            "Process::beginTermination invariant failed for pid "
            << Dec << m_Id
            << ": exit must be initiated by a thread in the target process.");
    }

    if (m_bTerminationRendezvousStarted)
    {
        const bool isOwner = m_pTerminatingThread == pCurrentThread;
        if (
            !isOwner && !pCurrentThread->m_bProcessExitParticipant &&
            !pCurrentThread->m_bReapable)
        {
            FATAL(
                "Process exit competitor was absent from the rendezvous for "
                "pid "
                << Dec << m_Id << ", tid " << pCurrentThread->getId() << ".");
        }
        m_Lock.release();
        return isOwner;
    }
    if (m_bTerminationSealed || m_bTerminationReapable)
    {
        FATAL(
            "Process::beginTermination observed an invalid completed "
            "rendezvous for pid "
            << Dec << m_Id << ".");
    }
    m_bTerminationRendezvousStarted = true;

    Vector<Thread *> peersToTerminate;
    bool currentThreadParticipates = false;
    for (Vector<Thread *>::Iterator it = m_Threads.begin();
         it != m_Threads.end(); ++it)
    {
        Thread *pThread = *it;
        auto threadExitGuard = pThread->m_JoinWaiters.acquire();
        if (pThread->m_bReapable)
        {
            continue;
        }
        if (
            pThread->m_bProcessExitOwned !=
            pThread->m_bProcessExitParticipant)
        {
            FATAL(
                "Process::beginTermination invariant failed for pid "
                << Dec << m_Id << ", tid " << pThread->getId()
                << ": thread has inconsistent process-exit ownership.");
        }

        if (!pThread->m_bProcessExitParticipant)
        {
            pThread->m_bProcessExitOwned = true;
            pThread->m_bProcessExitParticipant = true;
            ++m_nTerminationParticipants;
        }

        if (pThread == pCurrentThread)
        {
            currentThreadParticipates = true;
        }
        else if (!pThread->m_bExitStarted)
        {
            peersToTerminate.pushBack(pThread);
        }
    }

    if (!currentThreadParticipates)
    {
        FATAL(
            "Process::beginTermination invariant failed for pid "
            << Dec << m_Id
            << ": initiating thread is absent or already off-stack.");
    }

    {
        auto completionGuard = m_TerminationWaiters.acquire();
        m_pTerminatingThread = pCurrentThread;
        m_bTerminationReapable = false;
    }
    transitionToTerminating();

    // Init cannot accept any more orphans once its own teardown starts.
    Process *expectedInit = this;
    __atomic_compare_exchange_n(
        &m_pInitProcess, &expectedInit, static_cast<Process *>(0), false,
        __ATOMIC_RELEASE, __ATOMIC_RELAXED);

#if VERBOSE_KERNEL
    Scheduler::ProcessLease logParent;
    if (Scheduler::instance().acquireProcess(logParent, getParent()))
        NOTICE("Kill: " << m_Id << " (parent: " << logParent->getId() << ")");
    else
        NOTICE("Kill: " << m_Id << " (parent: <orphan>)");
#endif

    // Reparent every child, including terminated-but-unreaped children. Direct
    // deletion here races both waitpid and a child still switching off-stack.
    Scheduler::ProcessLease newParent;
    const bool newParentAcquired =
        Scheduler::instance().acquireProcess(newParent, Process::getInit());
    Process *pNewParent = newParentAcquired ? newParent.get() : nullptr;
    auto moveChildren = [this](Process *pParent) {
        while (true)
        {
            Process *pChild =
                Scheduler::instance().getChildProcess(this, 0);
            if (!pChild)
            {
                break;
            }

            __atomic_store_n(
                &pChild->m_pParent, pParent, __ATOMIC_RELEASE);
        }
    };

    {
        auto oldParentGuard = m_ChildStateWaiters.acquire();
        if (pNewParent)
        {
            auto newParentGuard =
                pNewParent->m_ChildStateWaiters.acquire();
            if (!canAdoptChildren(pNewParent, this))
            {
                pNewParent = 0;
            }
            moveChildren(pNewParent);
            oldParentGuard.wakeAll();
            newParentGuard.wakeAll();
        }
        else
        {
            moveChildren(0);
            oldParentGuard.wakeAll();
        }
    }

    // Peer wakeup can make another same-core thread runnable immediately.
    // The current thread remains a rendezvous participant, so dropping m_Lock
    // here cannot make Process destruction observable.
    m_Lock.release();

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    if (m_TerminationElectionHook)
    {
        m_TerminationElectionHook(this, pCurrentThread);
    }
#endif

    // Peers take a thread-only exit path. Re-entering Subsystem::exit() here
    // would rerun process teardown and recreate the historical deadlocks.
    for (Vector<Thread *>::Iterator it = peersToTerminate.begin();
         it != peersToTerminate.end(); ++it)
    {
        (*it)->setUnwindState(Thread::TerminateThread);
    }

    return true;
}

bool Process::quiesceTermination()
{
    Thread *pCurrentThread =
        Processor::information().getCurrentThread();
    {
        LockGuard<Spinlock> guard(m_Lock);
        if (
            !m_bTerminationRendezvousStarted ||
            m_pTerminatingThread != pCurrentThread)
        {
            return false;
        }
        if (m_bTerminationCleanupStarted)
        {
            return false;
        }

        m_bTerminationCleanupStarted = true;
        m_bTerminationSealed = true;
    }

    while (true)
    {
        auto completionGuard = m_TerminationWaiters.acquire();
        m_Lock.acquire();
        const size_t participants = m_nTerminationParticipants;
        m_Lock.release();

        if (participants == 0)
        {
            FATAL(
                "Process exit owner disappeared before teardown for pid "
                << Dec << m_Id << ".");
        }
        if (participants == 1)
        {
            return true;
        }

        const WaitQueue::WakeReason reason =
            completionGuard.waitForCompletion(
                WaitQueue::Channel(), Thread::ProcessWait,
                reinterpret_cast<uintptr_t>(
                    __builtin_return_address(0)));
        (void) reason;
    }
}

void Process::finishTermination()
{
    Thread *pCurrentThread =
        Processor::information().getCurrentThread();
    {
        LockGuard<Spinlock> guard(m_Lock);
        if (
            !m_bTerminationCleanupStarted ||
            m_pTerminatingThread != pCurrentThread ||
            m_nTerminationParticipants != 1 ||
            !pCurrentThread->m_bProcessExitParticipant)
        {
            FATAL(
                "Process::finishTermination called without exclusive "
                "off-stack teardown ownership for pid "
                << Dec << m_Id << ".");
        }
    }

    // Derived cleanup may acquire blocking locks without coupling them to the
    // Process lock.
    processTerminated();

    // Add to the zombie queue if the process is an orphan.
    if (!getParent())
    {
        NOTICE(
            "Process::kill() - process is an orphan, adding to ZombieQueue.");

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        OrphanPublicationHook publicationHook = __atomic_load_n(
            &m_OrphanPublicationHook, __ATOMIC_ACQUIRE);
        if (publicationHook)
        {
            publicationHook(
                this, OrphanPublicationPhase::Preparing,
                Processor::getInterrupts(), m_Lock.acquired());
        }
#endif
        ZombieQueue::instance().addObject(new ZombieProcess(this));
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        if (publicationHook)
        {
            publicationHook(
                this, OrphanPublicationPhase::Published,
                Processor::getInterrupts(), m_Lock.acquired());
        }
#endif
    }

    m_Lock.acquire();
    if (
        !m_bTerminationCleanupStarted ||
        m_pTerminatingThread != pCurrentThread ||
        m_nTerminationParticipants != 1 ||
        !pCurrentThread->m_bProcessExitParticipant)
    {
        FATAL(
            "Process::finishTermination lost exclusive off-stack teardown "
            "ownership for pid "
            << Dec << m_Id << ".");
    }

    // Parent-owned processes are reaped by waitpid; orphan publication above
    // may already have blocked its worker on the off-stack completion.
#if VERBOSE_KERNEL
    if (getParent())
    {
        NOTICE(
            "Process::kill() - not adding to ZombieQueue, process has a "
            "parent.");
    }
#endif
    Processor::information().getScheduler().killCurrentThread(&m_Lock);

    FATAL("Should never get here");
}

void Process::kill()
{
    if (!beginTermination())
    {
        Processor::information().getScheduler().killCurrentThread();
    }
    if (!quiesceTermination())
    {
        FATAL(
            "Process::kill failed to claim teardown for pid "
            << Dec << m_Id << ".");
    }
    finishTermination();
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void Process::setTerminationElectionHook(TerminationElectionHook hook)
{
    m_TerminationElectionHook = hook;
}

void Process::setOrphanPublicationHook(OrphanPublicationHook hook)
{
    __atomic_store_n(&m_OrphanPublicationHook, hook, __ATOMIC_RELEASE);
}
#endif

void Process::suspend()
{
    bool published = false;
    bool enteredSuspended = false;
    while (!published)
    {
        Process *pParent = getParent();
        if (pParent)
        {
            Scheduler::ProcessLease parent;
            if (!Scheduler::instance().acquireProcess(parent, pParent))
            {
                if (getParent() != pParent)
                {
                    continue;
                }
                FATAL("Process::suspend retained an unpinned parent.");
            }
            auto guard = parent->m_ChildStateWaiters.acquire();
            if (getParent() != pParent)
            {
                continue;
            }

            auto suspensionGuard = m_SuspensionWaiters.acquire();
            if (transitionState(Active, Suspended))
            {
                m_bUnreportedSuspend = true;
                m_ExitStatus = 0x7F;
                enteredSuspended = true;
                guard.wakeAll();
            }
            published = true;
        }
        else
        {
            auto suspensionGuard = m_SuspensionWaiters.acquire();
            if (transitionState(Active, Suspended))
            {
                m_bUnreportedSuspend = true;
                m_ExitStatus = 0x7F;
                enteredSuspended = true;
            }
            published = true;
        }
    }

    if (!enteredSuspended && getState() != Suspended)
    {
        return;
    }

    // Notify the parent only for the thread that won the state transition.
    if (enteredSuspended)
    {
        Scheduler::ProcessLease parent;
        if (
            Scheduler::instance().acquireProcess(parent, getParent()) &&
            parent->getSubsystem())
        {
            Process::ThreadLease parentThread;
            bool parentThreadAcquired = false;
            {
                auto relationGuard =
                    parent->m_ChildStateWaiters.acquire();
                if (getParent() == parent.get())
                {
                    parentThreadAcquired = parent->acquireThread(
                        parentThread, static_cast<size_t>(0));
                }
            }
            if (parentThreadAcquired)
            {
                parent->getSubsystem()->threadException(
                    parentThread.get(), Subsystem::Child);
            }
        }
    }

    while (true)
    {
        auto guard = m_SuspensionWaiters.acquire();
        if (getState() != Suspended)
        {
            return;
        }

        const WaitQueue::WakeReason wakeReason = guard.wait(
            WaitQueue::Channel(), Thread::ProcessWait,
            reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
        if (
            wakeReason == WaitQueue::WakeReason::Terminating ||
            wakeReason == WaitQueue::WakeReason::Unwinding)
        {
            return;
        }
    }
}

void Process::resume()
{
    bool published = false;
    while (!published)
    {
        Process *pParent = getParent();
        if (pParent)
        {
            Scheduler::ProcessLease parent;
            if (!Scheduler::instance().acquireProcess(parent, pParent))
            {
                if (getParent() != pParent)
                {
                    continue;
                }
                FATAL("Process::resume retained an unpinned parent.");
            }
            auto guard = parent->m_ChildStateWaiters.acquire();
            if (getParent() != pParent)
            {
                continue;
            }

            auto suspensionGuard = m_SuspensionWaiters.acquire();
            if (transitionState(Suspended, Active))
            {
                m_bUnreportedResume = true;
                m_ExitStatus = 0xFF;
                suspensionGuard.wakeAll();
                guard.wakeAll();
            }
            published = true;
        }
        else
        {
            auto suspensionGuard = m_SuspensionWaiters.acquire();
            if (transitionState(Suspended, Active))
            {
                m_bUnreportedResume = true;
                m_ExitStatus = 0xFF;
                suspensionGuard.wakeAll();
            }
            published = true;
        }
    }
}

int64_t Process::getUserId() const
{
    if (!getUser())
    {
        return -1;
    }
    return getUser()->getId();
}

int64_t Process::getGroupId() const
{
    if (!getGroup())
    {
        return -1;
    }
    return getGroup()->getId();
}

int64_t Process::getEffectiveUserId() const
{
    if (!getEffectiveUser())
    {
        return -1;
    }
    return getEffectiveUser()->getId();
}

int64_t Process::getEffectiveGroupId() const
{
    if (!getEffectiveGroup())
    {
        return -1;
    }
    return getEffectiveGroup()->getId();
}

void Process::getSupplementalGroupIds(Vector<int64_t> &vec) const
{
    // no-op
}

bool Process::waitUntilTerminationReapable()
{
    while (true)
    {
        auto guard = m_TerminationWaiters.acquire();
        if (m_bTerminationReapable)
        {
            return true;
        }
        if (
            Processor::information().getCurrentThread() ==
            m_pTerminatingThread)
        {
            return false;
        }

        const WaitQueue::WakeReason wakeReason = guard.waitForCompletion(
            WaitQueue::Channel(), Thread::ProcessWait,
            reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
        if (
            wakeReason == WaitQueue::WakeReason::Terminating ||
            wakeReason == WaitQueue::WakeReason::Unwinding)
        {
            // Reaper cancellation cannot make deleting a live Process safe.
            // Keep the wrapper alive until the off-stack completion arrives.
            continue;
        }
    }
}

bool Process::terminatingThreadReapable(
    Thread *pThread, bool &wakeOwner)
{
    wakeOwner = false;
    if (!pThread->m_bProcessExitParticipant)
    {
        return false;
    }
    if (!m_nTerminationParticipants)
    {
        FATAL(
            "Process exit rendezvous underflow for pid "
            << Dec << m_Id << ", tid " << pThread->getId() << ".");
    }

    pThread->m_bProcessExitParticipant = false;
    --m_nTerminationParticipants;
    if (
        m_bTerminationRendezvousStarted &&
        m_nTerminationParticipants == 0)
    {
        m_bTerminationSealed = true;
        return true;
    }

    wakeOwner = m_bTerminationRendezvousStarted;
    return false;
}

void Process::publishTermination()
{
    {
        RecursingLockGuard<Spinlock> processGuard(m_Lock);
        if (m_nTerminationParticipants)
        {
            FATAL(
                "Process termination published with "
                << Dec << m_nTerminationParticipants
                << " live rendezvous participants for pid " << m_Id << ".");
        }
        for (Vector<Thread *>::Iterator it = m_Threads.begin();
             it != m_Threads.end(); ++it)
        {
            Thread *pThread = *it;
            auto threadExitGuard = pThread->m_JoinWaiters.acquire();
            if (!pThread->m_bReapable)
            {
                FATAL(
                    "Process termination published before tid "
                    << Dec << pThread->getId() << " of pid " << m_Id
                    << " switched off-stack.");
            }
        }
    }

    bool published = false;
    while (!published)
    {
        Process *pParent = getParent();
        if (pParent)
        {
            Scheduler::ProcessLease parent;
            if (!Scheduler::instance().acquireProcess(parent, pParent))
            {
                if (getParent() != pParent)
                {
                    continue;
                }
                FATAL(
                    "Process::publishTermination retained an unpinned "
                    "parent.");
            }
            auto guard = parent->m_ChildStateWaiters.acquire();
            if (getParent() != pParent)
            {
                continue;
            }

            if (!transitionState(Terminating, Terminated))
            {
                FATAL(
                    "Process state was not Terminating while publishing pid "
                    << Dec << m_Id << ".");
            }
            guard.wakeAll();
            published = true;
        }
        else
        {
            if (!transitionState(Terminating, Terminated))
            {
                FATAL(
                    "Process state was not Terminating while publishing pid "
                    << Dec << m_Id << ".");
            }
            published = true;
        }
    }

    // This is deliberately the final Process access in the scheduler callback.
    // A ZombieQueue worker may begin destruction as soon as it observes this
    // completion.
    auto guard = m_TerminationWaiters.acquire();
    m_pTerminatingThread = 0;
    m_bTerminationReapable = true;
    guard.wakeAll();
}

Process *Process::getInit()
{
    return __atomic_load_n(&m_pInitProcess, __ATOMIC_ACQUIRE);
}

void Process::setInit(Process *pProcess)
{
    Process *expected = 0;
    __atomic_compare_exchange_n(
        &m_pInitProcess, &expected, pProcess, false, __ATOMIC_RELEASE,
        __ATOMIC_RELAXED);
}

#endif  // THREADS
