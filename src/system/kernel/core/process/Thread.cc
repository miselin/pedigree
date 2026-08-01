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

#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/InputManager.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/ProcessorThreadAllocator.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Uninterruptible.h"
#include "pedigree/kernel/processor/NMFaultHandler.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/utilities/ExtensibleBitmap.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/MemoryAllocator.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
namespace
{
Thread::StateTransitionHook g_StateTransitionHook = nullptr;
Thread::JoinOperationHook g_JoinOperationHook = nullptr;
using EventAdmissionHook = void (*)(Thread *);
EventAdmissionHook g_EventAdmissionHook = nullptr;
Thread *g_EventAdmissionTarget = nullptr;

struct HostedStateCleanupOrder
{
    size_t values[8] = {};
    size_t count = 0;
};

struct HostedStateCleanupItem
{
    HostedStateCleanupOrder *order;
    size_t value;
};

void hostedStateCleanupCallback(void *context)
{
    HostedStateCleanupItem *item =
        reinterpret_cast<HostedStateCleanupItem *>(context);
    if (item && item->order && item->order->count < 8)
    {
        item->order->values[item->order->count++] = item->value;
    }
}

void observeStateTransition(
    Thread::StateTransitionWindow window, Thread *thread, size_t previousLevel,
    size_t nextLevel)
{
    Thread::StateTransitionHook hook =
        __atomic_load_n(&g_StateTransitionHook, __ATOMIC_ACQUIRE);
    if (hook)
    {
        hook(window, thread, previousLevel, nextLevel);
    }
}
}  // namespace
#endif

Thread::Thread(
    Process *pParent, ThreadStartFunc pStartFunction, void *pParam,
    void *pStack, bool semiUser, bool bDontPickCore, bool delayedStart)
    : m_pParent(pParent)
{
    if (pParent == 0)
    {
        FATAL("Thread::Thread(): Parent process was NULL!");
    }

    // Initialise our kernel stack.
    m_pAllocatedStack = 0;

    // Initialise state level zero
    m_StateLevels[0].m_pAuxillaryStack = 0;
    allocateStackAtLevel(0);

    Thread *pCurrent = Processor::information().getCurrentThread();
    if (pCurrent && pCurrent->getParent() == pParent)
    {
        m_StateLevels[0].m_SignalMask =
            pCurrent->m_StateLevels[pCurrent->m_nStateLevel].m_SignalMask;
    }

    // If we've been given a user stack pointer, we are a user mode thread.
    bool bUserMode = true;
    void *requestedStack = pStack;
    if (pStack == 0)
    {
        bUserMode = false;
        VirtualAddressSpace::Stack *kernelStack =
            m_StateLevels[0].m_pAuxillaryStack =
                m_StateLevels[0].m_pKernelStack;
        m_StateLevels[0].m_pKernelStack = 0;

        if (kernelStack)
            pStack = kernelStack->getTop();
    }

    if (semiUser)
    {
        // Still have a kernel stack for when we jump to user mode, but start
        // the thread in kernel mode first.
        bUserMode = false;

        // If no stack was given and we allocated, extract that allocated stack
        // back out again so we have a kernel stack proper.
        if (!requestedStack)
        {
            m_StateLevels[0].m_pKernelStack =
                m_StateLevels[0].m_pAuxillaryStack;
        }
    }

    m_Id = m_pParent->addThread(this);

    // Firstly, grab our lock so that the scheduler cannot preemptively load
    // balance us while we're starting.
    m_Lock.acquire();

    if (
        delayedStart ||
        getUnwindState() == Thread::TerminateThread)
    {
        m_Status = Created;
    }

    // Add to the scheduler
    if (!bDontPickCore)
    {
        ProcessorThreadAllocator::instance().addThread(
            this, pStartFunction, pParam, bUserMode, pStack);
    }
    else
    {
        Scheduler::instance().addThread(
            this, Processor::information().getScheduler());
        Processor::information().getScheduler().addThread(
            this, pStartFunction, pParam, bUserMode, pStack);
    }
}

Thread::Thread(Process *pParent)
    : m_pParent(pParent), m_pScheduler(&Processor::information().getScheduler())
{
    if (pParent == 0)
    {
        FATAL("Thread::Thread(): Parent process was NULL!");
    }
    m_Id = m_pParent->addThread(this);

    // Initialise our kernel stack.
    // Kernel-mode threads use the auxiliary stack allocated above.

    // Still add the idle thread to the Scheduler for things like
    // threadInSchedule
    Scheduler::instance().addThread(this, *m_pScheduler);
}

Thread::Thread(Process *pParent, SyscallState &state, bool delayedStart)
    : m_pParent(pParent)
{
    if (pParent == 0)
    {
        FATAL("Thread::Thread(): Parent process was NULL!");
    }

    // Initialise our kernel stack.
    // m_pKernelStack =
    // VirtualAddressSpace::getKernelAddressSpace().allocateStack();
    m_pAllocatedStack = 0;

    // Initialise state level zero
    allocateStackAtLevel(0);

    Thread *pCurrent = Processor::information().getCurrentThread();
    if (pCurrent)
    {
        m_StateLevels[0].m_SignalMask =
            pCurrent->m_StateLevels[pCurrent->m_nStateLevel].m_SignalMask;

        // A forked process inherits its alternate signal stack. A new thread
        // sharing the same process starts with the stack disabled.
        if (pCurrent->getParent() != pParent)
        {
            m_AlternateSignalStack = pCurrent->m_AlternateSignalStack;
        }

#if X64
        NMFaultHandler::inheritCurrentThreadFpuState(this);
#endif
    }

    m_Id = m_pParent->addThread(this);

    // SyscallState variant has to be called from the parent thread, so this is
    // OK to do.
    if (pCurrent->m_bTlsBaseOverride)
    {
        // Override our TLS base too (but this will be in the copied address
        // space).
        m_bTlsBaseOverride = true;
        m_pTlsBase = pCurrent->m_pTlsBase;
    }

    m_Lock.acquire();

    if (
        delayedStart ||
        getUnwindState() == Thread::TerminateThread)
    {
        m_Status = Created;
    }

    // Now we are ready to go into the scheduler.
    ProcessorThreadAllocator::instance().addThread(this, state);
}

Thread::~Thread()
{
    {
        LockGuard<Spinlock> leaseGuard(m_ExternalLeaseLock);
        if (
            !m_bExternalLeaseAdmissionClosed ||
            m_nExternalLeases || m_bExternalLeaseReleaseInProgress)
        {
            FATAL(
                "Thread destroyed before external leases were closed and "
                "drained.");
        }
    }

    for (size_t level = 0; level < MAX_NESTED_EVENTS; ++level)
    {
        if (
            __atomic_load_n(
                &m_pDeferredScopes[level], __ATOMIC_ACQUIRE))
        {
            FATAL(
                "Thread destroyed with armed state cleanup records.");
        }
    }
    if (
        __atomic_load_n(
            &m_TerminationDeferralDepth, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&m_EventDeferralDepth, __ATOMIC_ACQUIRE))
    {
        FATAL(
            "Thread destroyed with active deferral scopes: terminal="
            << Dec
            << __atomic_load_n(
                   &m_TerminationDeferralDepth, __ATOMIC_ACQUIRE)
            << ", event="
            << __atomic_load_n(
                   &m_EventDeferralDepth, __ATOMIC_ACQUIRE)
            << ".");
    }

    if (InputManager::instance().removeCallbackByThread(this))
    {
        WARNING("A thread is being removed, but it never removed itself from "
                "InputManager.");
        WARNING(
            "This warning indicates an application or kernel module is buggy!");
    }

    // Before removing from the scheduler, terminate if needed.
    if (!m_bShutdown)
    {
        shutdown();
    }

    // Clean up allocated stacks at each level.
    for (size_t i = 0; i < MAX_NESTED_EVENTS; i++)
    {
        cleanStateLevel(i);
    }

    // Clean up TLS base.
    if (m_pTlsBase && m_pParent && !m_bTlsBaseOverride)
    {
        // Unmap the TLS base.
        if (m_pParent->getAddressSpace()->isMapped(m_pTlsBase))
        {
            physical_uintptr_t phys = 0;
            size_t flags = 0;
            m_pParent->getAddressSpace()->getMapping(m_pTlsBase, phys, flags);
            m_pParent->getAddressSpace()->unmap(m_pTlsBase);
            PhysicalMemoryManager::instance().freePage(phys);
        }

        // Give the address space back to the process.
        uintptr_t base = reinterpret_cast<uintptr_t>(m_pTlsBase);
        m_pParent->m_Lock.acquire(true);
        if (m_pParent->getAddressSpace()->getDynamicStart())
            m_pParent->getDynamicSpaceAllocator().free(base, THREAD_TLS_SIZE);
        else
            m_pParent->getSpaceAllocator().free(base, THREAD_TLS_SIZE);
        m_pParent->m_Lock.release();
    }
    else if (m_pTlsBase && !m_bTlsBaseOverride)
    {
        ERROR("Thread: no parent, but a TLS base exists.");
    }

    // Remove us from the scheduler.
    Scheduler::instance().removeThread(this);

    EMIT_IF(X86_COMMON)
    {
        // Make sure the floating-point fault handler doesn't care about us anymore
        NMFaultHandler::instance().threadTerminated(this);
    }

    if (m_pParent)
        m_pParent->removeThread(this);
}

void Thread::shutdown()
{
    {
        auto senderGuard = m_EventSenderDrainWaiters.acquire();
        LockGuard<Spinlock> guard(m_Lock);
        if (m_bShutdown)
        {
            return;
        }
        m_bShutdown = true;
    }

    // Admission and this predicate share one WaitQueue guard. A sender which
    // passed the shutdown check therefore either releases its pin before this
    // check or publishes a wake after this waiter is visible.
    while (true)
    {
        auto senderGuard = m_EventSenderDrainWaiters.acquire();
        {
            LockGuard<Spinlock> guard(m_Lock);
            if (!m_EventSendersInFlight)
            {
                break;
            }
        }

        const WaitQueue::WakeReason reason =
            senderGuard.waitForCompletion(
                WaitQueue::Channel(this), Thread::EventWait,
                reinterpret_cast<uintptr_t>(this));
        (void) reason;
    }

    // Cancel every outstanding wait before any state-level stack can be freed.
    for (size_t level = 0; level < MAX_NESTED_EVENTS; ++level)
    {
        WaitQueue::Waiter &waiter = m_StateLevels[level].m_Waiter;
        WaitQueue *queue = waiter.loadQueue();
        if (queue)
        {
            queue->cancel(
                &waiter, WaitQueue::WakeReason::Terminating);
        }
    }

    // Once shutdown is visible, no sender can publish another event. Remove
    // each queued registration under the thread lock, but complete it outside
    // the lock because completion can wake waiters or destroy the Event.
    while (true)
    {
        Event *event = nullptr;
        {
            LockGuard<Spinlock> guard(m_Lock);
            if (!m_EventQueue.count())
            {
                break;
            }
            event = m_EventQueue.popFront();
        }

        event->completeDelivery(this);
    }

    // Make a joiner runnable before the scheduler chooses our replacement.
    // This matters during shutdown after the idle thread has been retired.
    // join() still checks m_bReapable and cannot delete us until the scheduler
    // has switched off this stack.
    {
        auto guard = m_JoinWaiters.acquire();
        m_bExitStarted = true;
        guard.wakeAll();
    }

    // This is only an exit-announced scheduler state. Join completion is
    // deliberately delayed until markReapable().
    m_Status = AwaitingJoin;
}

void Thread::forceToStartupProcessor()
{
    if (m_pScheduler == Scheduler::instance().getBootstrapProcessorScheduler())
    {
        // No need to move - we already think we're associated with the right
        // CPU, and that's all we'll do below anyway.
        return;
    }

    if (Processor::information().getCurrentThread() != this)
    {
        ERROR("Thread::forceToStartupProcessor must be run as the desired "
              "thread.");
        return;
    }

    Scheduler::instance().removeThread(this);
    m_pScheduler = Scheduler::instance().getBootstrapProcessorScheduler();
    Scheduler::instance().addThread(this, *m_pScheduler);
    Scheduler::instance().yield();
}

void Thread::setStatus(Thread::Status s)
{
    if (m_Status == Thread::Zombie)
    {
        if (s != Thread::Zombie)
        {
            WARNING("Error condition in Thread::setStatus, more info below...");
            WARNING("Parent process ID: " << m_pParent->getId());
            FATAL("Thread::setStatus called with non-zombie status, when the "
                  "thread is a zombie!");
        }

        return;
    }

    Thread::Status previousStatus = m_Status;

    m_Status = s;

    if (s == Thread::Zombie)
    {
        Vector<Event *> pendingEvents;

        // Wipe out any pending events that currently exist.
        for (List<Event *>::Iterator it = m_EventQueue.begin();
             it != m_EventQueue.end(); ++it)
        {
            pendingEvents.pushBack(*it);
        }

        m_EventQueue.clear();

        for (auto pEvent : pendingEvents)
        {
            pEvent->completeDelivery(this);
        }

        // Process termination is published from deleteThread(), after the
        // scheduler has switched away from this stack.
    }

    if (m_Status == Thread::Ready && previousStatus != Thread::Running)
    {
    }

    if (m_pScheduler)
    {
        m_pScheduler->threadStatusChanged(this);
    }
}

bool Thread::start()
{
    {
        LockGuard<Spinlock> guard(m_Lock);
        if (
            m_Status != Thread::Created ||
            getUnwindState() == Thread::TerminateThread ||
            m_bStartRequested)
        {
            return false;
        }

        m_bStartRequested = true;
    }

    // The add worker parks while holding m_Lock, then observes this
    // out-of-lock publication. This ordering makes start the wake predicate
    // without allowing the worker to preempt us under the thread lock.
    Scheduler::instance().threadStatusChanged(this);
    return true;
}

bool Thread::setSchedulerReadyPredicate(
    SchedulerReadyPredicate predicate, void *context)
{
    LockGuard<Spinlock> guard(m_Lock);
    if (m_Status != Created || m_bStartRequested)
    {
        return false;
    }

    m_SchedulerReadyPredicate = predicate;
    m_SchedulerReadyContext = context;
    return true;
}

SchedulerState &Thread::state()
{
    return *(m_StateLevels[m_nStateLevel].m_State);
}

SchedulerState *Thread::pushState()
{
    const size_t previousLevel =
        __atomic_load_n(&m_nStateLevel, __ATOMIC_ACQUIRE);
    if ((previousLevel + 1) >= MAX_NESTED_EVENTS)
    {
        ERROR("Thread: Max nested events!");
        return nullptr;
    }
    const size_t nextLevel = previousLevel + 1;

    if (
        __atomic_load_n(
            &m_pDeferredScopes[nextLevel], __ATOMIC_ACQUIRE))
    {
        FATAL(
            "Thread state level reused with an armed cleanup record.");
    }

    // Prepare the unused level before publishing it to remote event senders.
    // Stack allocation and replacing an old SharedPointer can free memory, so
    // neither belongs in the short publication critical section below.
    allocateStackAtLevel(nextLevel);
    m_StateLevels[nextLevel].m_InhibitMask =
        m_StateLevels[previousLevel].m_InhibitMask;
#if X64
    // State levels are reused. A new handler must not inherit an FPU image
    // left behind by an earlier handler at the same nesting depth.
    m_StateLevels[nextLevel].m_State->flags &= ~(1U << 1);
#endif
    m_StateLevels[nextLevel].m_InterruptionReason = NotInterrupted;
    m_StateLevels[nextLevel].m_bDispatchingWaitEvent = false;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    observeStateTransition(
        StatePushBeforePublish, this, previousLevel, nextLevel);
#endif

    SchedulerState *previousState = m_StateLevels[previousLevel].m_State;
    {
        LockGuard<Spinlock> guard(m_Lock);
        if (m_nStateLevel != previousLevel)
        {
            FATAL("Thread state level changed during push publication.");
        }

        // Refresh mutable per-level scalars while serialised with their public
        // accessors, then release-publish the completely prepared level.
        m_StateLevels[nextLevel].m_SignalMask =
            m_StateLevels[previousLevel].m_SignalMask;
        m_StateLevels[nextLevel].m_Errno =
            m_StateLevels[previousLevel].m_Errno;
        __atomic_store_n(&m_nStateLevel, nextLevel, __ATOMIC_RELEASE);
    }

    setKernelStack();

    return previousState;
}

void Thread::popState(bool clean)
{
    const size_t origStateLevel =
        __atomic_load_n(&m_nStateLevel, __ATOMIC_ACQUIRE);

    if (origStateLevel == 0)
    {
        ERROR("Thread: Potential error: popStack() called with state level 0!");
        ERROR("Thread: (ignore this if longjmp has been called)");
        return;
    }

    if (
        __atomic_load_n(
            &m_pDeferredScopes[origStateLevel], __ATOMIC_ACQUIRE))
    {
        FATAL(
            "Normal state pop attempted with armed cleanup records.");
    }

    const size_t nextLevel = origStateLevel - 1;
    {
        LockGuard<Spinlock> guard(m_Lock);
        if (m_nStateLevel != origStateLevel)
        {
            FATAL("Thread state level changed during pop publication.");
        }
        __atomic_store_n(&m_nStateLevel, nextLevel, __ATOMIC_RELEASE);
    }

    setKernelStack();

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    observeStateTransition(
        StatePopAfterPublish, this, origStateLevel, nextLevel);
#endif

    if (clean)
    {
        cleanStateLevel(origStateLevel);
    }
}

void Thread::abandonCurrentState(bool clean)
{
    const size_t level =
        __atomic_load_n(&m_nStateLevel, __ATOMIC_ACQUIRE);
    if (!level)
    {
        FATAL("Cannot abandon the base Thread state.");
    }
    retireDeferredScopes(false, level);
    popState(clean);
}

void Thread::abandonAllStates()
{
    while (getStateLevel())
    {
        // The caller is still running on the outermost physical stack until
        // its no-return transition, so every logical pop preserves storage.
        abandonCurrentState(false);
    }
}

VirtualAddressSpace::Stack *Thread::getStateUserStack()
{
    return m_StateLevels[m_nStateLevel].m_pUserStack;
}

void Thread::setStateUserStack(VirtualAddressSpace::Stack *st)
{
    m_StateLevels[m_nStateLevel].m_pUserStack = st;
}

size_t Thread::getStateLevel() const
{
    return __atomic_load_n(&m_nStateLevel, __ATOMIC_ACQUIRE);
}

void Thread::threadExited()
{
    Processor::information().getScheduler().killCurrentThread();
}

void Thread::allocateStackAtLevel(size_t stateLevel)
{
    if (stateLevel >= MAX_NESTED_EVENTS)
        stateLevel = MAX_NESTED_EVENTS - 1;
    if (m_StateLevels[stateLevel].m_pKernelStack == 0)
        m_StateLevels[stateLevel].m_pKernelStack =
            VirtualAddressSpace::getKernelAddressSpace().allocateStack();
}

void *Thread::getKernelStack()
{
    if (m_nStateLevel >= MAX_NESTED_EVENTS)
        FATAL("m_nStateLevel > MAX_NESTED_EVENTS: " << m_nStateLevel << "...");
    if (m_StateLevels[m_nStateLevel].m_pKernelStack != 0)
    {
        return m_StateLevels[m_nStateLevel].m_pKernelStack->getTop();
    }
    else
    {
        return 0;
    }
}

void *Thread::getKernelStackBase(size_t *size) const
{
    if (m_nStateLevel >= MAX_NESTED_EVENTS)
        FATAL("m_nStateLevel > MAX_NESTED_EVENTS: " << m_nStateLevel << "...");
    if (m_StateLevels[m_nStateLevel].m_pKernelStack != 0)
    {
        auto stack = m_StateLevels[m_nStateLevel].m_pKernelStack;
        *size = stack->getSize();
        return stack->getBase();
    }
    else
    {
        ERROR("No kernel stack at this level!");
        *size = 0;
        return 0;
    }
}

void Thread::setKernelStack()
{
    uintptr_t stack = 0;
    if (m_StateLevels[m_nStateLevel].m_pKernelStack)
    {
        stack = reinterpret_cast<uintptr_t>(
            m_StateLevels[m_nStateLevel].m_pKernelStack->getTop());
    }
    Processor::information().setKernelStack(stack);
}

void Thread::pokeState(size_t stateLevel, SchedulerState &state)
{
    if (stateLevel >= MAX_NESTED_EVENTS)
    {
        ERROR(
            "Thread::pokeState(): stateLevel `" << stateLevel
                                                << "' is over the maximum.");
        return;
    }
    *(m_StateLevels[stateLevel].m_State) = state;
}

bool Thread::sendEvent(Event *pEvent)
{
    Event::SendLease eventSendLease = pEvent->beginSend();
    if (!eventSendLease)
    {
        return false;
    }

    bool accepted = false;
    {
        auto senderGuard = m_EventSenderDrainWaiters.acquire();
        {
            LockGuard<Spinlock> guard(m_Lock);
            if (m_bShutdown || m_Status == Zombie)
            {
                return false;
            }
            ++m_EventSendersInFlight;
        }
    }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    EventAdmissionHook admissionHook =
        __atomic_load_n(&g_EventAdmissionHook, __ATOMIC_ACQUIRE);
    Thread *admissionTarget =
        __atomic_load_n(&g_EventAdmissionTarget, __ATOMIC_ACQUIRE);
    if (admissionHook && admissionTarget == this)
    {
        admissionHook(this);
    }
#endif

    // Event registration can allocate. The in-flight pin lets this happen
    // outside m_Lock without opening a registration-vs-destruction gap.
    const bool eventRegistered = pEvent->registerThread(this);

    bool duplicate = false;
    bool wakeThread = false;
    PerProcessorScheduler *readyScheduler = nullptr;
    if (eventRegistered)
    {
        // Serialise queue inspection in waitForEvent() with event publication.
        auto eventWaitGuard = m_EventWaiters.acquire();
        {
            LockGuard<Spinlock> guard(m_Lock);
            if (!m_bShutdown && m_Status != Zombie)
            {
                if (pEvent->isSignalEvent())
                {
                    for (List<Event *>::Iterator it = m_EventQueue.begin();
                         it != m_EventQueue.end(); ++it)
                    {
                        if (
                            (*it)->isSignalEvent() &&
                            (*it)->getNumber() == pEvent->getNumber())
                        {
                            duplicate = true;
                            break;
                        }
                    }
                }

                if (!duplicate)
                {
                    m_EventQueue.pushBack(pEvent);
                    wakeThread = hasDeliverableEventsUnlocked() &&
                                 interruptWaitUnlocked(
                                     WaitQueue::WakeReason::Event,
                                     readyScheduler);
                }
                accepted = true;
            }
        }

    }

    if (!eventRegistered)
    {
        accepted = false;
    }
    else if (!accepted)
    {
        pEvent->deregisterThread(this);
    }
    else if (duplicate)
    {
        pEvent->completeDelivery(this);
    }
    else if (wakeThread)
    {
        assert(readyScheduler);
        readyScheduler->publishReadyFromWait(this);
    }

    {
        auto senderGuard = m_EventSenderDrainWaiters.acquire();
        bool drained = false;
        {
            LockGuard<Spinlock> guard(m_Lock);
            assert(m_EventSendersInFlight);
            drained = !--m_EventSendersInFlight;
        }
        if (drained)
        {
            senderGuard.wakeAll(
                WaitQueue::WakeReason::Signalled,
                WaitQueue::Channel(this));
        }
    }
    return accepted;
}

void Thread::waitForEvent(
    WaitQueue::AbandonCallback onAbandon, void *abandonContext)
{
    while (true)
    {
        bool ready = false;
        WaitQueue::WakeReason reason = WaitQueue::WakeReason::Spurious;
        {
            auto guard = m_EventWaiters.acquire();

            m_Lock.acquire();
            ready = hasDeliverableEventsUnlocked();
            m_Lock.release();
            if (!ready)
            {
                reason = guard.wait(
                    WaitQueue::Channel(), Thread::EventWait,
                    reinterpret_cast<uintptr_t>(
                        __builtin_return_address(0)),
                    onAbandon, abandonContext);
            }
        }

        if (ready)
        {
            // A pre-existing event did not pass through WaitQueue::wait(), so
            // dispatch it explicitly after dropping the event-wait guard and
            // identify it as the event which satisfied this wait.
            const size_t stateLevel = getStateLevel();
            m_StateLevels[stateLevel].m_bDispatchingWaitEvent = true;
            Processor::information().getScheduler().checkEventState(0);
            m_StateLevels[stateLevel].m_bDispatchingWaitEvent = false;
            return;
        }

        if (
            reason == WaitQueue::WakeReason::Event ||
            reason == WaitQueue::WakeReason::Terminating ||
            reason == WaitQueue::WakeReason::Unwinding)
        {
            return;
        }
    }
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
namespace
{
Atomic<size_t> g_HostedPrequeuedEventCalls(0);
Atomic<size_t> g_HostedShutdownEventCalls(0);
Atomic<size_t> g_HostedShutdownEventDestructions(0);
Atomic<size_t> g_HostedShutdownThreadCalls(0);
Atomic<size_t> g_HostedSelfRetireCalls(0);
Atomic<size_t> g_HostedSelfRetireDestructions(0);
Atomic<size_t> g_HostedAdmissionRetireDestructions(0);
Event *g_pHostedSelfRetireEvent = nullptr;

struct HostedAdmissionRetireContext
{
    HostedAdmissionRetireContext(Event *event, size_t destructionsBefore)
        : event(event), destructionsBefore(destructionsBefore), calls(0),
          destroyedInsideHook(0)
    {
    }

    Event *event;
    size_t destructionsBefore;
    Atomic<size_t> calls;
    Atomic<size_t> destroyedInsideHook;
};

HostedAdmissionRetireContext *g_pHostedAdmissionRetireContext = nullptr;

void hostedPrequeuedEventHandler(size_t)
{
    g_HostedPrequeuedEventCalls += 1;
}

void hostedShutdownEventHandler(size_t)
{
    g_HostedShutdownEventCalls += 1;
}

void hostedSelfRetireEventHandler(size_t)
{
    Event *event = g_pHostedSelfRetireEvent;
    g_pHostedSelfRetireEvent = nullptr;
    if (event)
    {
        event->retire();
        g_HostedSelfRetireCalls += 1;
    }
}

void hostedAdmissionRetireHook(Thread *)
{
    HostedAdmissionRetireContext *context =
        g_pHostedAdmissionRetireContext;
    if (!context)
    {
        return;
    }

    context->calls += 1;
    context->event->retire();
    if (
        g_HostedAdmissionRetireDestructions !=
        context->destructionsBefore)
    {
        context->destroyedInsideHook += 1;
    }
}

class HostedPrequeuedEvent : public Event
{
  public:
    HostedPrequeuedEvent()
        : Event(
              reinterpret_cast<uintptr_t>(&hostedPrequeuedEventHandler),
              false)
    {
    }

    size_t serialize(uint8_t *) override
    {
        return 0;
    }

    size_t getNumber() override
    {
        return 0x57414954;
    }
};

class HostedShutdownStableEvent : public Event
{
  public:
    HostedShutdownStableEvent()
        : Event(
              reinterpret_cast<uintptr_t>(&hostedShutdownEventHandler),
              false)
    {
    }

    size_t serialize(uint8_t *) override
    {
        return 0;
    }

    size_t getNumber() override
    {
        return 0x53484453;
    }
};

class HostedShutdownDeletableEvent : public Event
{
  public:
    HostedShutdownDeletableEvent()
        : Event(
              reinterpret_cast<uintptr_t>(&hostedShutdownEventHandler),
              true)
    {
    }

    ~HostedShutdownDeletableEvent() override
    {
        g_HostedShutdownEventDestructions += 1;
    }

    size_t serialize(uint8_t *) override
    {
        return 0;
    }

    size_t getNumber() override
    {
        return 0x53484444;
    }
};

class HostedSelfRetireEvent : public Event
{
  public:
    HostedSelfRetireEvent()
        : Event(
              reinterpret_cast<uintptr_t>(&hostedSelfRetireEventHandler),
              false)
    {
    }

    ~HostedSelfRetireEvent() override
    {
        g_HostedSelfRetireDestructions += 1;
    }

    size_t serialize(uint8_t *) override
    {
        return 0;
    }

    size_t getNumber() override
    {
        return 0x53455254;
    }
};

class HostedAdmissionRetireEvent : public Event
{
  public:
    HostedAdmissionRetireEvent()
        : Event(
              reinterpret_cast<uintptr_t>(&hostedShutdownEventHandler),
              false)
    {
    }

    ~HostedAdmissionRetireEvent() override
    {
        g_HostedAdmissionRetireDestructions += 1;
    }

    size_t serialize(uint8_t *) override
    {
        return 0;
    }

    size_t getNumber() override
    {
        return 0x41525254;
    }
};

int hostedShutdownThread(void *)
{
    g_HostedShutdownThreadCalls += 1;
    return 0;
}

struct HostedStatePublicationContext
{
    HostedStatePublicationContext(Thread *thread, Event *event)
        : thread(thread), event(event), calls(0), failures(0)
    {
    }

    Thread *thread;
    Event *event;
    Atomic<size_t> calls;
    Atomic<size_t> failures;
};

HostedStatePublicationContext *g_StatePublicationContext = nullptr;

void hostedStatePublicationHook(
    Thread::StateTransitionWindow window, Thread *thread, size_t previousLevel,
    size_t nextLevel)
{
    HostedStatePublicationContext *context =
        __atomic_load_n(&g_StatePublicationContext, __ATOMIC_ACQUIRE);
    if (!context)
    {
        return;
    }

    context->calls += 1;
    const size_t expectedVisibleLevel =
        window == Thread::StatePushBeforePublish ? previousLevel : nextLevel;
    if (
        thread != context->thread ||
        thread->getStateLevel() != expectedVisibleLevel ||
        !thread->sendEvent(context->event))
    {
        context->failures += 1;
    }
}

struct HostedDeliveryLeaseContext
{
    explicit HostedDeliveryLeaseContext(Event *event)
        : event(event), entered(0), completed(0)
    {
    }

    Event *event;
    Atomic<size_t> entered;
    Atomic<size_t> completed;
};

int hostedDeliveryLeaseWaiter(void *parameter)
{
    HostedDeliveryLeaseContext *context =
        reinterpret_cast<HostedDeliveryLeaseContext *>(parameter);
    context->entered += 1;
    context->event->waitForDeliveries();
    context->completed += 1;
    return 0;
}
}  // namespace

void Thread::setStateTransitionHook(StateTransitionHook hook)
{
    __atomic_store_n(&g_StateTransitionHook, hook, __ATOMIC_RELEASE);
}

void Thread::setJoinOperationHook(JoinOperationHook hook)
{
    __atomic_store_n(&g_JoinOperationHook, hook, __ATOMIC_RELEASE);
}

bool Thread::isReapableForHostedTest()
{
    auto guard = m_JoinWaiters.acquire();
    return m_bReapable;
}

bool Thread::waitUntilReapableForHostedTest()
{
    TerminationDeferral terminationDeferral;
    while (true)
    {
        auto guard = m_JoinWaiters.acquire();
        if (m_bReapable)
        {
            return true;
        }

        const WaitQueue::WakeReason reason = guard.waitForCompletion(
            WaitQueue::Channel(), Thread::Joining,
            reinterpret_cast<uintptr_t>(this));
        (void) reason;
    }
}

bool Thread::runHostedPrequeuedEventRegression()
{
    if (Processor::information().getCurrentThread() != this)
    {
        return false;
    }

    constexpr size_t Iterations = 16;
    HostedPrequeuedEvent event;
    const size_t initialStateLevel = getStateLevel();
    const size_t callsBefore = g_HostedPrequeuedEventCalls;
    for (size_t iteration = 0; iteration < Iterations; ++iteration)
    {
        if (
            !sendEvent(&event) || event.pendingCount() != 1 ||
            !hasEvent(&event))
        {
            return false;
        }

        waitForEvent();

        if (
            g_HostedPrequeuedEventCalls !=
                (callsBefore + iteration + 1) ||
            event.pendingCount() != 0 || hasEvent(&event) ||
            getStateLevel() != initialStateLevel)
        {
            return false;
        }
    }

    return true;
}

bool Thread::runHostedStatePublicationRegression()
{
    if (
        Processor::information().getCurrentThread() != this ||
        (getStateLevel() + 1) >= MAX_NESTED_EVENTS)
    {
        return false;
    }

    HostedPrequeuedEvent event;
    HostedStatePublicationContext context(this, &event);
    const size_t initialStateLevel = getStateLevel();
    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);

    __atomic_store_n(
        &g_StatePublicationContext, &context, __ATOMIC_RELEASE);
    setStateTransitionHook(hostedStatePublicationHook);

    SchedulerState *previousState = pushState();
    const bool pushed =
        previousState && getStateLevel() == (initialStateLevel + 1);
    if (previousState)
    {
        popState();
    }

    setStateTransitionHook(nullptr);
    __atomic_store_n(
        &g_StatePublicationContext,
        static_cast<HostedStatePublicationContext *>(nullptr),
        __ATOMIC_RELEASE);

    const bool publishedSafely =
        pushed && getStateLevel() == initialStateLevel && context.calls == 2 &&
        context.failures == 0 && event.pendingCount() == 2 &&
        hasEvent(&event);
    cullEvent(&event);
    const bool deliveriesCulled =
        event.pendingCount() == 0 && !hasEvent(&event);

    Processor::setInterrupts(interruptsWereEnabled);
    return publishedSafely && deliveriesCulled;
}

bool Thread::runHostedStateCleanupRegression()
{
    const size_t initialLevel = getStateLevel();
    HostedStateCleanupOrder order;
    HostedStateCleanupItem oldItem{&order, 0};
    HostedStateCleanupItem firstItem{&order, 1};
    HostedStateCleanupItem secondItem{&order, 2};
    HostedStateCleanupItem normalItem{&order, 3};
    HostedStateCleanupItem baseItem{&order, 4};
    HostedStateCleanupItem levelItem{&order, 5};
    DeferredScopeRecord oldRecord;
    DeferredScopeRecord firstRecord;
    AtomicStateCleanupRecord secondRecord;
    DeferredScopeRecord normalRecord;
    DeferredScopeRecord baseRecord;
    AtomicStateCleanupRecord levelRecord;

    armStateCleanup(
        oldRecord, hostedStateCleanupCallback, &oldItem);
    const size_t checkpoint = stateCleanupCheckpoint();
    armStateCleanup(
        firstRecord, hostedStateCleanupCallback, &firstItem);
    armAtomicStateCleanup(
        secondRecord, hostedStateCleanupCallback, &secondItem);
    retireDeferredScopesAfter(checkpoint);

    const bool checkpointPassed =
        order.count == 2 && order.values[0] == 2 &&
        order.values[1] == 1 && oldRecord.armed &&
        !firstRecord.armed && !secondRecord.armed;
    disarmStateCleanup(oldRecord);

    armStateCleanup(
        normalRecord, hostedStateCleanupCallback, &normalItem);
    disarmStateCleanup(normalRecord);
    const bool normalPassed =
        order.count == 2 && !normalRecord.armed;

    armStateCleanup(
        baseRecord, hostedStateCleanupCallback, &baseItem);
    const bool pushed = pushState() != nullptr;
    if (pushed)
    {
        armAtomicStateCleanup(
            levelRecord, hostedStateCleanupCallback, &levelItem);
        abandonCurrentState(false);
    }
    const bool levelPassed =
        pushed && getStateLevel() == initialLevel &&
        order.count == 3 && order.values[2] == 5 &&
        baseRecord.armed && !levelRecord.armed;
    disarmStateCleanup(baseRecord);

    return checkpointPassed && normalPassed && levelPassed &&
           order.count == 3;
}

void Thread::withDeferredScopeLockForTest(DeferredScopeLockHook hook)
{
    m_DeferredScopeRegressionLock.acquire();
    if (hook)
    {
        hook();
    }
    m_DeferredScopeRegressionLock.release();
}

bool Thread::runHostedEventDeliveryLeaseRegression()
{
    if (Processor::information().getCurrentThread() != this)
    {
        return false;
    }

    HostedPrequeuedEvent event;
    if (!sendEvent(&event))
    {
        return false;
    }

    Event::Delivery delivery = getNextEvent();
    if (!delivery || delivery.get() != &event || hasEvent(&event) ||
        event.pendingCount() != 1)
    {
        delivery.reset();
        cullEvent(&event);
        return false;
    }

    HostedDeliveryLeaseContext context(&event);
    Thread *waiterA = new Thread(
        Scheduler::instance().getKernelProcess(), hostedDeliveryLeaseWaiter,
        &context, nullptr, false, true);
    waiterA->setName("hosted event-delivery lease waiter A");

    while (context.entered != static_cast<size_t>(1))
    {
        Scheduler::instance().yield();
    }

    for (size_t i = 0; i < 4; ++i)
    {
        Scheduler::instance().yield();
    }
    Thread::WaitDebugInfo waiterInfo = {};
    const bool closePublished =
        waiterA->getWaitDebugInfo(waiterInfo) &&
        waiterInfo.channelOwner == &event &&
        waiterInfo.queued;
    const bool rejectedAfterClose =
        closePublished && !sendEvent(&event);
    const bool leaseHeld =
        context.completed == 0 && event.pendingCount() == 1;

    delivery.reset();
    while (context.completed != static_cast<size_t>(1))
    {
        Scheduler::instance().yield();
    }

    const bool waiterAJoined = waiterA->join();
    const bool deliveryLeasePassed =
        closePublished && rejectedAfterClose && leaseHeld && waiterAJoined &&
        event.pendingCount() == 0;

    HostedPrequeuedEvent deferredEvent;
    const size_t callsBefore = g_HostedPrequeuedEventCalls;
    bool nestedDeferralPassed = false;
    {
        Uninterruptible outer;
        {
            Uninterruptible inner;
            if (!sendEvent(&deferredEvent))
            {
                return false;
            }
            Processor::information().getScheduler().checkEventState(0);
            nestedDeferralPassed =
                g_HostedPrequeuedEventCalls == callsBefore &&
                hasEvent(&deferredEvent) &&
                deferredEvent.pendingCount() == 1;
        }

        Processor::information().getScheduler().checkEventState(0);
        nestedDeferralPassed =
            nestedDeferralPassed &&
            g_HostedPrequeuedEventCalls == callsBefore &&
            hasEvent(&deferredEvent) &&
            deferredEvent.pendingCount() == 1;
    }

    Processor::information().getScheduler().checkEventState(0);
    nestedDeferralPassed =
        nestedDeferralPassed &&
        g_HostedPrequeuedEventCalls == (callsBefore + 1) &&
        !hasEvent(&deferredEvent) && deferredEvent.pendingCount() == 0;

    const size_t retireCallsBefore = g_HostedSelfRetireCalls;
    const size_t retireDestructionsBefore =
        g_HostedSelfRetireDestructions;
    HostedSelfRetireEvent *retiringEvent =
        new HostedSelfRetireEvent;
    g_pHostedSelfRetireEvent = retiringEvent;
    const bool retireQueued = sendEvent(retiringEvent);
    if (retireQueued)
    {
        waitForEvent();
    }
    else
    {
        g_pHostedSelfRetireEvent = nullptr;
        delete retiringEvent;
    }
    const bool selfRetirePassed =
        retireQueued && !g_pHostedSelfRetireEvent &&
        g_HostedSelfRetireCalls == (retireCallsBefore + 1) &&
        g_HostedSelfRetireDestructions ==
            (retireDestructionsBefore + 1);

    HostedAdmissionRetireEvent *admissionEvent =
        new HostedAdmissionRetireEvent;
    HostedAdmissionRetireContext admissionContext(
        admissionEvent,
        static_cast<size_t>(
            g_HostedAdmissionRetireDestructions));
    g_pHostedAdmissionRetireContext = &admissionContext;
    __atomic_store_n(&g_EventAdmissionTarget, this, __ATOMIC_RELEASE);
    __atomic_store_n(
        &g_EventAdmissionHook, &hostedAdmissionRetireHook,
        __ATOMIC_RELEASE);
    const bool rejectedByConcurrentRetire = !sendEvent(admissionEvent);
    __atomic_store_n(
        &g_EventAdmissionHook,
        static_cast<EventAdmissionHook>(nullptr), __ATOMIC_RELEASE);
    __atomic_store_n(
        &g_EventAdmissionTarget, static_cast<Thread *>(nullptr),
        __ATOMIC_RELEASE);
    g_pHostedAdmissionRetireContext = nullptr;
    const bool admissionRetirePassed =
        rejectedByConcurrentRetire && admissionContext.calls == 1 &&
        admissionContext.destroyedInsideHook == 0 &&
        g_HostedAdmissionRetireDestructions ==
            (admissionContext.destructionsBefore + 1);

    return deliveryLeasePassed && nestedDeferralPassed &&
           selfRetirePassed && admissionRetirePassed;
}

bool Thread::runHostedEventShutdownRegression()
{
    if (Processor::information().getCurrentThread() != this)
    {
        return false;
    }

    HostedShutdownStableEvent stableEvent;
    HostedShutdownStableEvent racingEvent;
    HostedShutdownStableEvent postShutdownEvent;
    const size_t eventCallsBefore = g_HostedShutdownEventCalls;
    const size_t destructionsBefore =
        g_HostedShutdownEventDestructions;
    const size_t threadCallsBefore = g_HostedShutdownThreadCalls;

    Thread *target = new Thread(
        Scheduler::instance().getKernelProcess(), hostedShutdownThread,
        nullptr, nullptr, false, true, true);
    target->setName("hosted event-queue shutdown regression");

    const bool stableQueued = target->sendEvent(&stableEvent);
    HostedShutdownDeletableEvent *deletableEvent =
        new HostedShutdownDeletableEvent;
    const bool deletableQueued =
        stableQueued && target->sendEvent(deletableEvent);
    if (!deletableQueued)
    {
        delete deletableEvent;
    }

    __atomic_store_n(
        &g_EventAdmissionTarget, target, __ATOMIC_RELEASE);
    __atomic_store_n(
        &g_EventAdmissionHook,
        +[](Thread *admissionTarget) {
            admissionTarget->setUnwindState(Thread::TerminateThread);
            while (true)
            {
                {
                    LockGuard<Spinlock> guard(admissionTarget->m_Lock);
                    if (admissionTarget->m_bShutdown)
                    {
                        break;
                    }
                }
                Scheduler::instance().yield();
            }
        },
        __ATOMIC_RELEASE);
    const bool rejectedDuringShutdown = !target->sendEvent(&racingEvent);
    __atomic_store_n(
        &g_EventAdmissionHook,
        static_cast<EventAdmissionHook>(nullptr), __ATOMIC_RELEASE);
    __atomic_store_n(
        &g_EventAdmissionTarget, static_cast<Thread *>(nullptr),
        __ATOMIC_RELEASE);

    bool shutdownObserved = false;
    constexpr size_t ShutdownAttempts = 10000;
    for (size_t attempt = 0; attempt < ShutdownAttempts; ++attempt)
    {
        {
            LockGuard<Spinlock> guard(target->m_Lock);
            shutdownObserved = target->m_bShutdown;
        }
        if (shutdownObserved)
        {
            break;
        }
        Scheduler::instance().yield();
    }

    bool rejectedAfterShutdown = false;
    if (shutdownObserved)
    {
        rejectedAfterShutdown = !target->sendEvent(&postShutdownEvent);
        if (!rejectedAfterShutdown)
        {
            target->cullEvent(&postShutdownEvent);
        }
    }

    const bool joined = target->join();
    return stableQueued && deletableQueued && rejectedDuringShutdown &&
           shutdownObserved && rejectedAfterShutdown && joined &&
           stableEvent.pendingCount() == 0 &&
           racingEvent.pendingCount() == 0 &&
           postShutdownEvent.pendingCount() == 0 &&
           g_HostedShutdownEventCalls == eventCallsBefore &&
           g_HostedShutdownEventDestructions == (destructionsBefore + 1) &&
           g_HostedShutdownThreadCalls == threadCallsBefore;
}
#endif

void Thread::inhibitEvent(size_t eventNumber, bool bInhibit)
{
    LockGuard<Spinlock> guard(m_Lock);
    if (bInhibit)
        m_StateLevels[m_nStateLevel].m_InhibitMask->set(eventNumber);
    else
        m_StateLevels[m_nStateLevel].m_InhibitMask->clear(eventNumber);
}

uint64_t Thread::getSignalMask()
{
    LockGuard<Spinlock> guard(m_Lock);
    return m_StateLevels[m_nStateLevel].m_SignalMask;
}

void Thread::setSignalMask(uint64_t mask)
{
    LockGuard<Spinlock> guard(m_Lock);
    m_StateLevels[m_nStateLevel].m_SignalMask = mask;
}

void Thread::cullEvent(Event *pEvent)
{
    size_t removed = 0;
    {
        LockGuard<Spinlock> guard(m_Lock);

        for (List<Event *>::Iterator it = m_EventQueue.begin();
             it != m_EventQueue.end();)
        {
            if (*it == pEvent)
            {
                it = m_EventQueue.erase(it);
                ++removed;
            }
            else
            {
                ++it;
            }
        }
    }

    // The caller retains ownership of an exact-event cull. Account for every
    // enqueue independently, including duplicate enqueues of the same object.
    while (removed--)
    {
        pEvent->deregisterThread(this);
    }
}

void Thread::cullEvent(size_t eventNumber)
{
    Vector<Event *> deregisterEvents;

    {
        LockGuard<Spinlock> guard(m_Lock);

        for (List<Event *>::Iterator it = m_EventQueue.begin();
             it != m_EventQueue.end();)
        {
            if ((*it)->getNumber() == eventNumber)
            {
                Event *pEvent = *it;
                it = m_EventQueue.erase(it);
                deregisterEvents.pushBack(pEvent);
            }
            else
                ++it;
        }
    }

    // clean up events now that we're no longer locked
    for (auto it : deregisterEvents)
    {
        it->completeDelivery(this);
    }
}

Event::Delivery Thread::getNextEvent()
{
    Event *pResult = nullptr;

    {
        LockGuard<Spinlock> guard(m_Lock);

        if (
            __atomic_load_n(
                &m_EventDeferralDepth, __ATOMIC_ACQUIRE))
        {
            return Event::Delivery();
        }

        for (size_t i = 0; i < m_EventQueue.count(); i++)
        {
            Event *e = m_EventQueue.popFront();
            if (!e)
            {
                ERROR("A null event was in a thread's event queue!");
                continue;
            }

            size_t eventNumber = e->getNumber();
            bool signalInhibited =
                e->isSignalEvent() && eventNumber > 0 && eventNumber <= 64 &&
                (m_StateLevels[m_nStateLevel].m_SignalMask &
                 (static_cast<uint64_t>(1) << (eventNumber - 1)));
            if (m_StateLevels[m_nStateLevel].m_InhibitMask->test(eventNumber) ||
                signalInhibited ||
                (e->getSpecificNestingLevel() != ~0UL &&
                 e->getSpecificNestingLevel() != m_nStateLevel))
            {
                m_EventQueue.pushBack(e);
            }
            else
            {
                pResult = e;
                break;
            }
        }
    }

    return pResult ? Event::Delivery(pResult, this) : Event::Delivery();
}

bool Thread::hasEvents()
{
    LockGuard<Spinlock> guard(m_Lock);

    return hasEventsUnlocked();
}

bool Thread::hasEventsUnlocked()
{
    for (List<Event *>::Iterator it = m_EventQueue.begin();
         it != m_EventQueue.end(); ++it)
    {
        Event *event = *it;
        size_t eventNumber = event->getNumber();
        bool signalInhibited =
            event->isSignalEvent() && eventNumber > 0 && eventNumber <= 64 &&
            (m_StateLevels[m_nStateLevel].m_SignalMask &
             (static_cast<uint64_t>(1) << (eventNumber - 1)));
        if (!m_StateLevels[m_nStateLevel].m_InhibitMask->test(eventNumber) &&
            !signalInhibited &&
            (event->getSpecificNestingLevel() == ~0UL ||
             event->getSpecificNestingLevel() == m_nStateLevel))
        {
            return true;
        }
    }

    return false;
}

bool Thread::hasDeliverableEventsUnlocked()
{
    return !__atomic_load_n(
               &m_EventDeferralDepth, __ATOMIC_ACQUIRE) &&
           hasEventsUnlocked();
}

bool Thread::hasEvent(Event *pEvent)
{
    LockGuard<Spinlock> guard(m_Lock);

    for (List<Event *>::Iterator it = m_EventQueue.begin();
         it != m_EventQueue.end(); ++it)
    {
        if ((*it) == pEvent)
        {
            return true;
        }
    }

    return false;
}

bool Thread::hasEvent(size_t eventNumber)
{
    LockGuard<Spinlock> guard(m_Lock);

    for (List<Event *>::Iterator it = m_EventQueue.begin();
         it != m_EventQueue.end(); ++it)
    {
        if ((*it)->getNumber() == eventNumber)
        {
            return true;
        }
    }

    return false;
}

void Thread::unexpectedExit()
{
}

uintptr_t Thread::getTlsBase()
{
    if (!m_StateLevels[0].m_pKernelStack)
        return 0;

    // Solves a problem where threads are created pointing to different address
    // spaces than the process that creates them (for whatever reason). Because
    // this is usually only called right after the address space switch in
    // PerProcessorScheduler, the address space is set properly.
    if (!m_pTlsBase)
    {
        // Get ourselves some space.
        uintptr_t base = 0;
        if (m_pParent->getAddressSpace()->getDynamicStart())
            m_pParent->getDynamicSpaceAllocator().allocate(
                THREAD_TLS_SIZE, base);
        else
            m_pParent->getSpaceAllocator().allocate(THREAD_TLS_SIZE, base);

        if (!base)
        {
            // Failed to allocate space.
            NOTICE(
                "Thread [" << Dec << m_pParent->getId() << ":" << m_Id << Hex
                           << "]: failed to allocate TLS area.");
            return base;
        }

        // Map.
        physical_uintptr_t phys =
            PhysicalMemoryManager::instance().allocatePage();
        m_pParent->getAddressSpace()->map(
            phys, reinterpret_cast<void *>(base), VirtualAddressSpace::Write);

        // Set up our thread ID to start with in the TLS region, now that it's
        // actually mapped into the address space.
        m_pTlsBase = reinterpret_cast<void *>(base);
        uint32_t *tlsBase = reinterpret_cast<uint32_t *>(m_pTlsBase);
#if BITS_64
        *tlsBase = static_cast<uint32_t>(m_Id);
#else
        *tlsBase = m_Id;
#endif

#if VERBOSE_KERNEL
        NOTICE(
            "Thread [" << Dec << m_pParent->getId() << ":" << m_Id << Hex
                       << "]: allocated TLS area at " << m_pTlsBase << ".");
#endif
    }
    return reinterpret_cast<uintptr_t>(m_pTlsBase);
}

void Thread::resetTlsBase()
{
    m_pTlsBase = 0;
    m_bTlsBaseOverride = false;
    Processor::setTlsBase(getTlsBase());
}

void Thread::setTlsBase(uintptr_t base)
{
    /// \todo clean up old base
    m_bTlsBaseOverride = true;
    m_pTlsBase = reinterpret_cast<void *>(base);

    if (Processor::information().getCurrentThread() == this)
    {
        Processor::setTlsBase(getTlsBase());
    }

    // base[0] == base (for e.g. %fs:0 to get the address of %fs).
    // See the "ELF Handling For Thread-Local Storage" document for this
    // requirement (IA-32 section).
    uintptr_t *pBase = reinterpret_cast<uintptr_t *>(base);
    *pBase = base;
}

bool Thread::join()
{
    return joinInternal(false);
}

bool Thread::joinForCompletion()
{
    TerminationDeferral terminationDeferral;
    return joinInternal(true);
}

bool Thread::joinInternal(bool completion)
{
    Thread *pThisThread = Processor::information().getCurrentThread();
    if (pThisThread == this)
    {
        return false;
    }

    Process *pParent = nullptr;
    {
        auto guard = m_JoinWaiters.acquire();
        if (m_bDetached || m_bJoinClaimed)
        {
            return false;
        }
        pParent = m_pParent;
        if (!pParent->beginThreadJoin())
        {
            return false;
        }
        m_bJoinClaimed = true;
    }

    while (true)
    {
        bool reapable = false;
        {
            auto guard = m_JoinWaiters.acquire();
            if (m_bReapable)
            {
                reapable = true;
            }
            else
            {
                const uintptr_t returnAddress =
                    reinterpret_cast<uintptr_t>(
                        __builtin_return_address(0));
                WaitQueue::WakeReason reason =
                    completion
                        ? guard.waitForCompletion(
                              WaitQueue::Channel(), Thread::Joining,
                              returnAddress)
                        : guard.wait(
                              WaitQueue::Channel(), Thread::Joining,
                              returnAddress, &Thread::abandonJoin, this);
                if (
                    reason == WaitQueue::WakeReason::Unwinding ||
                    reason == WaitQueue::WakeReason::Terminating)
                {
                    if (completion)
                    {
                        continue;
                    }
                    {
                        auto claimGuard = m_JoinWaiters.acquire();
                        m_bJoinClaimed = false;
                    }
                    pParent->endThreadJoin();
                    return false;
                }
            }
        }

        if (!reapable)
        {
            continue;
        }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        JoinOperationHook hook =
            __atomic_load_n(&g_JoinOperationHook, __ATOMIC_ACQUIRE);
        if (hook)
        {
            hook(this, pParent);
        }
#endif

        // No new external inspector may enter once final retirement begins.
        // Existing inspectors finish before the target can be deleted.
        closeExternalLeaseAdmissionAndDrain();

        // Serialise the final ownership check and deletion with Process::kill.
        // Process exit retains every participant until Process destruction.
        bool processOwnsTarget = false;
        {
            RecursingLockGuard<Spinlock> processGuard(pParent->m_Lock);
            {
                auto claimGuard = m_JoinWaiters.acquire();
                if (!m_bReapable)
                {
                    continue;
                }
                if (m_bProcessExitOwned)
                {
                    m_bJoinClaimed = false;
                    processOwnsTarget = true;
                }
            }

            if (!processOwnsTarget)
            {
                // markReapable() runs only after the scheduler has switched
                // away from this stack. Holding the Process lock keeps its
                // thread vector stable.
                delete this;
            }
        }

        pParent->endThreadJoin();
        return !processOwnsTarget;
    }
}

bool Thread::beginExternalLease()
{
    LockGuard<Spinlock> guard(m_ExternalLeaseLock);
    if (m_bExternalLeaseAdmissionClosed)
    {
        return false;
    }

    ++m_nExternalLeases;
    return true;
}

void Thread::endExternalLease()
{
    bool wake = false;
    bool finishDetachedRetirement = false;
    {
        LockGuard<Spinlock> guard(m_ExternalLeaseLock);
        if (!m_nExternalLeases)
        {
            FATAL("Thread external lease underflow.");
        }

        --m_nExternalLeases;
        finishDetachedRetirement =
            !m_nExternalLeases && m_bExternalLeaseAdmissionClosed;
        if (finishDetachedRetirement)
        {
            m_bExternalLeaseReleaseInProgress = true;
        }
        else
        {
            wake = !m_nExternalLeases;
        }
    }

    if (wake)
    {
        m_ExternalLeaseWaiters.wakeAll(
            WaitQueue::WakeReason::Signalled,
            WaitQueue::Channel(this));
    }

    if (!finishDetachedRetirement)
    {
        return;
    }

    // Process::ThreadLease keeps the parent pinned until this method returns.
    // Serialising with Process teardown lets the final lease perform deferred
    // detached deletion after the scheduler has already switched off-stack.
    Process *parent = m_pParent;
    bool deleteNow = false;
    {
        RecursingLockGuard<Spinlock> processGuard(parent->m_Lock);
        {
            auto joinGuard = m_JoinWaiters.acquire();
            deleteNow =
                m_bReapable && m_bDetached && !m_bProcessExitOwned &&
                !m_bDetachedRetirementClaimed;
            if (deleteNow)
            {
                m_bDetachedRetirementClaimed = true;
            }
        }

        {
            LockGuard<Spinlock> leaseGuard(m_ExternalLeaseLock);
            m_bExternalLeaseReleaseInProgress = false;
        }

        if (!deleteNow)
        {
            // Keep the Process lock until the final queue access is complete.
            // Scheduler-side retirement takes the same lock, so it cannot
            // delete this Thread between clearing the handoff bit and waking
            // a completion waiter.
            m_ExternalLeaseWaiters.wakeAll(
                WaitQueue::WakeReason::Signalled,
                WaitQueue::Channel(this));
        }

        if (deleteNow)
        {
            delete this;
        }
    }
}

void Thread::closeExternalLeaseAdmission()
{
    LockGuard<Spinlock> guard(m_ExternalLeaseLock);
    m_bExternalLeaseAdmissionClosed = true;
}

void Thread::closeExternalLeaseAdmissionAndDrain()
{
    TerminationDeferral terminationDeferral;
    while (true)
    {
        auto guard = m_ExternalLeaseWaiters.acquire();
        {
            LockGuard<Spinlock> stateGuard(m_ExternalLeaseLock);
            m_bExternalLeaseAdmissionClosed = true;
            if (
                !m_nExternalLeases &&
                !m_bExternalLeaseReleaseInProgress)
            {
                return;
            }
        }

        const WaitQueue::WakeReason reason = guard.waitForCompletion(
            WaitQueue::Channel(this), Thread::Joining,
            reinterpret_cast<uintptr_t>(this));
        (void) reason;
    }
}

void Thread::abandonJoin(void *context)
{
    Thread *target = reinterpret_cast<Thread *>(context);
    Process *parent = target->m_pParent;
    {
        auto guard = target->m_JoinWaiters.acquire();
        target->m_bJoinClaimed = false;
    }
    parent->endThreadJoin();
}

bool Thread::detach()
{
    Process *pParent = m_pParent;
    if (!pParent->beginThreadJoin())
    {
        return false;
    }

    bool deleteNow = false;
    bool joinInProgress = false;
    {
        RecursingLockGuard<Spinlock> processGuard(pParent->m_Lock);
        {
            auto guard = m_JoinWaiters.acquire();
            if (m_bJoinClaimed)
            {
                ERROR(
                    "Thread::detach() called while other threads are "
                    "joining.");
                joinInProgress = true;
            }
            else
            {
                m_bDetached = true;
                deleteNow =
                    m_bReapable && !m_bProcessExitOwned &&
                    !m_bDetachedRetirementClaimed;
                if (deleteNow)
                {
                    m_bDetachedRetirementClaimed = true;
                }
            }
        }
    }

    if (joinInProgress)
    {
        pParent->endThreadJoin();
        return false;
    }

    if (deleteNow)
    {
        closeExternalLeaseAdmissionAndDrain();
        RecursingLockGuard<Spinlock> processGuard(pParent->m_Lock);
        {
            auto guard = m_JoinWaiters.acquire();
            deleteNow =
                deleteNow && m_bDetached && m_bReapable &&
                !m_bProcessExitOwned &&
                m_bDetachedRetirementClaimed;
        }
        if (deleteNow)
        {
            delete this;
        }
    }

    pParent->endThreadJoin();
    return true;
}

Thread::StateLevel::StateLevel()
    : m_State(), m_pKernelStack(0), m_pUserStack(0), m_pAuxillaryStack(0),
      m_InhibitMask(), m_SignalMask(0), m_Errno(0),
      m_InterruptionReason(NotInterrupted), m_bDispatchingWaitEvent(false)
{
    m_State = new SchedulerState;
    ByteSet(m_State, 0, sizeof(SchedulerState));
    m_InhibitMask = SharedPointer<ExtensibleBitmap>::allocate();
}

Thread::StateLevel::~StateLevel()
{
    delete m_State;
}

Thread::StateLevel::StateLevel(const Thread::StateLevel &s)
    : m_State(), m_pKernelStack(s.m_pKernelStack), m_pUserStack(s.m_pUserStack),
      m_pAuxillaryStack(s.m_pAuxillaryStack), m_InhibitMask(),
      m_SignalMask(s.m_SignalMask), m_Errno(s.m_Errno),
      m_InterruptionReason(s.m_InterruptionReason),
      m_bDispatchingWaitEvent(false)
{
    m_State = new SchedulerState(*(s.m_State));
    m_InhibitMask =
        SharedPointer<ExtensibleBitmap>::allocate(*(s.m_InhibitMask));
}

Thread::StateLevel &Thread::StateLevel::operator=(const Thread::StateLevel &s)
{
    m_State = new SchedulerState(*(s.m_State));
    m_InhibitMask =
        SharedPointer<ExtensibleBitmap>::allocate(*(s.m_InhibitMask));
    m_SignalMask = s.m_SignalMask;
    m_Errno = s.m_Errno;
    m_InterruptionReason = s.m_InterruptionReason;
    m_bDispatchingWaitEvent = false;
    m_pKernelStack = s.m_pKernelStack;
    return *this;
}

void Thread::markTimeoutInterruptedWait()
{
    const size_t interruptedLevel = m_nStateLevel ? m_nStateLevel - 1 : 0;
    m_StateLevels[interruptedLevel].m_InterruptionReason =
        InterruptedByTimeout;
}

void Thread::markSignalInterruptedWait()
{
    if (!m_nStateLevel)
    {
        return;
    }

    StateLevel &interrupted = m_StateLevels[m_nStateLevel - 1];
    if (interrupted.m_bDispatchingWaitEvent)
    {
        interrupted.m_InterruptionReason = InterruptedBySignal;
    }
}

bool Thread::eventsDeferred()
{
    return __atomic_load_n(
               &m_EventDeferralDepth, __ATOMIC_ACQUIRE) != 0;
}

bool Thread::getWaitDebugInfo(WaitDebugInfo &info)
{
    const size_t level = __atomic_load_n(&m_nStateLevel, __ATOMIC_ACQUIRE);
    if (level >= MAX_NESTED_EVENTS)
    {
        return false;
    }

    WaitQueue::Waiter &waiter = m_StateLevels[level].m_Waiter;
    WaitQueue *queue = waiter.loadQueue();
    if (!queue)
    {
        return false;
    }

    info.queue = queue;
    info.channelOwner = waiter.channel.owner;
    info.channelValue = waiter.channel.value;
    info.reason = waiter.loadReason();
    info.stateLevel = waiter.stateLevel;
    info.queued = waiter.isQueued();

    // A concurrent wake may unpublish this persistent record. Reject a torn
    // snapshot rather than taking the target lock, which may be frozen by the
    // kernel debugger.
    return waiter.loadQueue() == queue &&
           __atomic_load_n(&m_nStateLevel, __ATOMIC_ACQUIRE) == level;
}

void Thread::deferEvents()
{
    __atomic_add_fetch(
        &m_EventDeferralDepth, static_cast<size_t>(1),
        __ATOMIC_ACQ_REL);
}

void Thread::resumeEvents()
{
    const size_t depth =
        __atomic_load_n(&m_EventDeferralDepth, __ATOMIC_ACQUIRE);
    if (!depth)
    {
        FATAL("Unbalanced event-delivery deferral.");
    }
    __atomic_sub_fetch(
        &m_EventDeferralDepth, static_cast<size_t>(1),
        __ATOMIC_ACQ_REL);
}

void Thread::deferTermination()
{
    __atomic_add_fetch(
        &m_TerminationDeferralDepth, static_cast<size_t>(1),
        __ATOMIC_ACQ_REL);
}

void Thread::resumeTermination()
{
    const size_t depth = __atomic_load_n(
        &m_TerminationDeferralDepth, __ATOMIC_ACQUIRE);
    if (!depth)
    {
        FATAL("Unbalanced terminal-teardown deferral.");
    }
    __atomic_sub_fetch(
        &m_TerminationDeferralDepth, static_cast<size_t>(1),
        __ATOMIC_ACQ_REL);
}

void Thread::registerDeferredScope(
    DeferredScopeRecord &record, bool termination, bool events)
{
    if (
        record.armed || record.next ||
        record.defersTermination || record.defersEvents ||
        record.sequence || record.cleanup || record.context)
    {
        FATAL("Deferred scope registered more than once.");
    }

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);

    const size_t level =
        __atomic_load_n(&m_nStateLevel, __ATOMIC_ACQUIRE);
    const size_t sequence = __atomic_add_fetch(
        &m_NextStateCleanupSequence, static_cast<size_t>(1),
        __ATOMIC_ACQ_REL);
    if (!sequence)
    {
        FATAL("Thread state cleanup sequence exhausted.");
    }

    record.stateLevel = level;
    record.sequence = sequence;
    record.defersTermination = termination;
    record.defersEvents = events;
    record.cleanup = nullptr;
    record.context = nullptr;
    record.armed = true;

    if (termination)
    {
        deferTermination();
    }
    if (events)
    {
        deferEvents();
    }

    DeferredScopeRecord *head =
        __atomic_load_n(&m_pDeferredScopes[level], __ATOMIC_ACQUIRE);
    do
    {
        record.next = head;
    } while (!__atomic_compare_exchange_n(
        &m_pDeferredScopes[level], &head, &record, false,
        __ATOMIC_RELEASE, __ATOMIC_ACQUIRE));

    Processor::setInterrupts(interruptsWereEnabled);
}

void Thread::armStateCleanup(
    DeferredScopeRecord &record,
    DeferredScopeRecord::Cleanup cleanup, void *context)
{
    if (!cleanup)
    {
        FATAL("State cleanup armed without a callback.");
    }

    if (
        record.armed || record.next ||
        record.defersTermination || record.defersEvents ||
        record.sequence || record.cleanup || record.context)
    {
        FATAL("State cleanup record armed more than once.");
    }

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);

    const size_t level =
        __atomic_load_n(&m_nStateLevel, __ATOMIC_ACQUIRE);
    const size_t sequence = __atomic_add_fetch(
        &m_NextStateCleanupSequence, static_cast<size_t>(1),
        __ATOMIC_ACQ_REL);
    if (!sequence)
    {
        FATAL("Thread state cleanup sequence exhausted.");
    }

    record.stateLevel = level;
    record.sequence = sequence;
    record.cleanup = cleanup;
    record.context = context;
    record.armed = true;

    DeferredScopeRecord *head =
        __atomic_load_n(&m_pDeferredScopes[level], __ATOMIC_ACQUIRE);
    do
    {
        record.next = head;
    } while (!__atomic_compare_exchange_n(
        &m_pDeferredScopes[level], &head, &record, false,
        __ATOMIC_RELEASE, __ATOMIC_ACQUIRE));

    Processor::setInterrupts(interruptsWereEnabled);
}

void Thread::unregisterDeferredScope(DeferredScopeRecord &record)
{
    if (!record.armed || record.stateLevel >= MAX_NESTED_EVENTS)
    {
        FATAL("Deferred scope was not registered on this Thread.");
    }

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);

    DeferredScopeRecord *expected = &record;
    if (!__atomic_compare_exchange_n(
            &m_pDeferredScopes[record.stateLevel], &expected,
            record.next, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        FATAL("Deferred scopes were not released in LIFO order.");
    }

    if (record.defersTermination)
    {
        resumeTermination();
    }
    if (record.defersEvents)
    {
        resumeEvents();
    }
    record = DeferredScopeRecord();

    Processor::setInterrupts(interruptsWereEnabled);
}

void Thread::disarmStateCleanup(DeferredScopeRecord &record)
{
    if (!record.armed || record.stateLevel >= MAX_NESTED_EVENTS)
    {
        FATAL("State cleanup record was not armed on this Thread.");
    }

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);

    DeferredScopeRecord *expected = &record;
    if (!__atomic_compare_exchange_n(
            &m_pDeferredScopes[record.stateLevel], &expected,
            record.next, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        FATAL("State cleanup records were not disarmed in LIFO order.");
    }

    record = DeferredScopeRecord();

    Processor::setInterrupts(interruptsWereEnabled);
}

void Thread::moveDeferredScope(
    DeferredScopeRecord &from, DeferredScopeRecord &to)
{
    if (
        &from == &to || !from.armed ||
        from.stateLevel >= MAX_NESTED_EVENTS)
    {
        FATAL("Moved deferred scope was not registered.");
    }
    if (
        to.armed || to.next || to.defersTermination || to.defersEvents ||
        to.sequence || to.cleanup || to.context)
    {
        FATAL("Deferred scope move destination was already registered.");
    }

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);

    to = from;
    DeferredScopeRecord *expected = &from;
    if (!__atomic_compare_exchange_n(
            &m_pDeferredScopes[from.stateLevel], &expected, &to, false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        FATAL("Deferred scopes were not moved in LIFO order.");
    }
    from = DeferredScopeRecord();

    Processor::setInterrupts(interruptsWereEnabled);
}

void Thread::retireDeferredScopes(
    bool allStateLevels, size_t stateLevel)
{
    retireDeferredScopesMatching(
        allStateLevels, stateLevel, false, 0);
}

size_t Thread::stateCleanupCheckpoint()
{
    return __atomic_load_n(
        &m_NextStateCleanupSequence, __ATOMIC_ACQUIRE);
}

void Thread::retireDeferredScopesAfter(size_t checkpoint)
{
    retireDeferredScopesMatching(
        true, 0, true, checkpoint);
}

void Thread::retireDeferredScopesMatching(
    bool allStateLevels, size_t stateLevel,
    bool newerThanCheckpoint, size_t checkpoint)
{
    DeferredScopeRecord *retired = nullptr;
    DeferredScopeRecord *retiredTail = nullptr;

    if (!allStateLevels && stateLevel >= MAX_NESTED_EVENTS)
    {
        FATAL("State cleanup retirement has an invalid level.");
    }

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);

    while (true)
    {
        DeferredScopeRecord *candidate = nullptr;
        size_t candidateLevel = 0;

        for (size_t level = 0; level < MAX_NESTED_EVENTS; ++level)
        {
            if (
                !allStateLevels && level != stateLevel)
            {
                continue;
            }

            DeferredScopeRecord *head =
                __atomic_load_n(
                    &m_pDeferredScopes[level], __ATOMIC_ACQUIRE);
            if (
                !head ||
                (newerThanCheckpoint && head->sequence <= checkpoint))
            {
                continue;
            }
            if (
                !head->armed || head->stateLevel != level ||
                !head->sequence)
            {
                FATAL("Corrupt Thread state cleanup publication.");
            }
            if (!candidate || head->sequence > candidate->sequence)
            {
                candidate = head;
                candidateLevel = level;
            }
        }

        if (!candidate)
        {
            break;
        }

        DeferredScopeRecord *expected = candidate;
        if (!__atomic_compare_exchange_n(
                &m_pDeferredScopes[candidateLevel], &expected,
                candidate->next, false, __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE))
        {
            continue;
        }

        if (candidate->defersTermination)
        {
            resumeTermination();
        }
        if (candidate->defersEvents)
        {
            resumeEvents();
        }
        candidate->armed = false;
        candidate->next = nullptr;
        if (retiredTail)
        {
            retiredTail->next = candidate;
        }
        else
        {
            retired = candidate;
        }
        retiredTail = candidate;
    }

    Processor::setInterrupts(interruptsWereEnabled);

    while (retired)
    {
        DeferredScopeRecord *next = retired->next;
        DeferredScopeRecord::Cleanup cleanup = retired->cleanup;
        void *context = retired->context;
        *retired = DeferredScopeRecord();
        if (cleanup)
        {
            cleanup(context);
        }
        retired = next;
    }
}

void Thread::armAtomicStateCleanup(
    AtomicStateCleanupRecord &record,
    AtomicStateCleanupRecord::Cleanup cleanup, void *context)
{
    if (Processor::information().getCurrentThread() != this)
    {
        FATAL(
            "Interrupt/exception cleanup armed for a non-current Thread.");
    }
    armStateCleanup(record, cleanup, context);
}

void Thread::disarmAtomicStateCleanup(AtomicStateCleanupRecord &record)
{
    if (Processor::information().getCurrentThread() != this)
    {
        FATAL(
            "Interrupt/exception cleanup disarmed for a non-current Thread.");
    }
    disarmStateCleanup(record);
}

void Thread::setScheduler(class PerProcessorScheduler *pScheduler)
{
    m_pScheduler = pScheduler;
}

PerProcessorScheduler *Thread::getScheduler() const
{
    return m_pScheduler;
}

void Thread::cleanStateLevel(size_t level)
{
    if (
        __atomic_load_n(
            &m_pDeferredScopes[level], __ATOMIC_ACQUIRE))
    {
        FATAL(
            "Thread state stack freed with an armed cleanup record.");
    }

    if (m_StateLevels[level].m_Waiter.loadQueue())
    {
        FATAL("Thread state stack was cleaned while still in a wait queue.");
    }

    if (m_StateLevels[level].m_pKernelStack)
    {
        VirtualAddressSpace::getKernelAddressSpace().freeStack(
            m_StateLevels[level].m_pKernelStack);
        m_StateLevels[level].m_pKernelStack = 0;
    }
    else if (m_StateLevels[level].m_pAuxillaryStack)
    {
        VirtualAddressSpace::getKernelAddressSpace().freeStack(
            m_StateLevels[level].m_pAuxillaryStack);
        m_StateLevels[level].m_pAuxillaryStack = 0;
    }

    if (m_StateLevels[level].m_pUserStack && m_pParent)
    {
        // Can't use Processor::getCurrent.. as by the time we're called
        // we may have switched address spaces to allow the thread to die.
        m_pParent->getAddressSpace()->freeStack(
            m_StateLevels[level].m_pUserStack);
        m_StateLevels[level].m_pUserStack = 0;
    }

    m_StateLevels[level].m_InhibitMask.reset();
}

void Thread::setUnwindState(UnwindType ut)
{
    bool becameReady = false;
    bool queuedBeforeStart = false;
    PerProcessorScheduler *readyScheduler = nullptr;
    {
        LockGuard<Spinlock> guard(m_Lock);
        __atomic_store_n(&m_UnwindState, ut, __ATOMIC_RELEASE);
        queuedBeforeStart =
            m_Status == Created && ut == TerminateThread;
        if (ut != Continue)
        {
            const bool terminating = ut == TerminateThread;
            becameReady = interruptWaitUnlocked(
                terminating ? WaitQueue::WakeReason::Terminating
                            : WaitQueue::WakeReason::Unwinding,
                readyScheduler);
        }
    }

    if (becameReady)
    {
        assert(readyScheduler);
        readyScheduler->publishReadyFromWait(this);
    }
    else if (queuedBeforeStart)
    {
        Scheduler::instance().threadStatusChanged(this);
    }
}

Thread::UnwindType Thread::getUnwindState()
{
    return __atomic_load_n(&m_UnwindState, __ATOMIC_ACQUIRE);
}

bool Thread::interruptWaitUnlocked(
    WaitQueue::WakeReason reason,
    PerProcessorScheduler *&readyScheduler)
{
    readyScheduler = nullptr;
    WaitQueue::Waiter &waiter = m_StateLevels[m_nStateLevel].m_Waiter;
    if (
        !waiter.loadQueue() ||
        waiter.loadReason() != WaitQueue::WakeReason::Waiting)
    {
        return false;
    }

    waiter.storeReason(reason);

    if (m_Status == Sleeping)
    {
        m_Status = Ready;
        readyScheduler = waiter.scheduler;
        assert(readyScheduler);
        return true;
    }
    return false;
}

bool Thread::hasActiveWaitUnlocked() const
{
    return m_StateLevels[m_nStateLevel].m_Waiter.loadQueue() != nullptr;
}

bool Thread::activeWaitPendingUnlocked() const
{
    const WaitQueue::Waiter &waiter = m_StateLevels[m_nStateLevel].m_Waiter;
    return waiter.loadQueue() &&
           waiter.loadReason() == WaitQueue::WakeReason::Waiting;
}

bool Thread::markReapable()
{
    auto guard = m_JoinWaiters.acquire();
    m_bReapable = true;
    if (!m_bDetached)
    {
        guard.wakeAll();
    }

    bool externalLeasesDrained = false;
    {
        LockGuard<Spinlock> leaseGuard(m_ExternalLeaseLock);
        externalLeasesDrained =
            m_bExternalLeaseAdmissionClosed && !m_nExternalLeases &&
            !m_bExternalLeaseReleaseInProgress;
    }
    const bool deleteNow =
        m_bDetached && !m_bProcessExitOwned &&
        externalLeasesDrained && !m_bDetachedRetirementClaimed;
    if (deleteNow)
    {
        m_bDetachedRetirementClaimed = true;
    }
    return deleteNow;
}

#endif  // THREADS
