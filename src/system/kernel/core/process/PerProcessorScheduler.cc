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

#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Trace.h"
#include "pedigree/kernel/machine/SchedulerTimer.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/RoundRobin.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/SchedulingAlgorithm.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/utilities/utility.h"
#include "pedigree/kernel/debugger/commands/LocksCommand.h"
#if HOSTED
#include "pedigree/kernel/processor/hosted/Processor.h"
#endif

#define VERBOSE_SCHEDULER 0

PerProcessorScheduler::PerProcessorScheduler()
    : m_pSchedulingAlgorithm(0), m_NewThreadDataLock(),
      m_NewThreadDataCondition(), m_NewThreadData(),
      m_DelayedNewThreadData(), m_NewThreadAdmissionOpen(false),
      m_StopNewThreadWorker(false), m_NewThreadWorker(),
      m_IrqWorkDoorbell(0), m_pIdleThread(0)
{
}

PerProcessorScheduler::~PerProcessorScheduler()
{
    stopNewThreadWorker();

    SchedulerTimer *pTimer = Machine::instance().getSchedulerTimer();
    if (!pTimer)
    {
        panic("No scheduler timer present.");
    }
    Machine::instance().getSchedulerTimer()->removeHandler(this);
}

struct newThreadData
{
    Thread *pThread;
    Thread::ThreadStartFunc pStartFunction;
    void *pParam;
    bool bUsermode;
    void *pStack;
    SyscallState state;
    bool useSyscallState;
};

void PerProcessorScheduler::startNewThreadWorker(Process *pParent)
{
    m_NewThreadDataLock.acquire();
    if (
        m_NewThreadWorker || m_NewThreadData.count() ||
        m_DelayedNewThreadData.count())
    {
        m_NewThreadDataLock.release();
        FATAL("Per-processor thread add worker started with live state.");
    }
    m_StopNewThreadWorker = false;
    m_NewThreadAdmissionOpen = true;
    m_NewThreadDataLock.release();

    Thread *pAddThread = new Thread(
        pParent, processorAddThread, reinterpret_cast<void *>(this), 0,
        false, true);
    pAddThread->setName("PerProcessorScheduler thread add worker");
    m_NewThreadWorker.adopt(pAddThread);
}

void PerProcessorScheduler::stopNewThreadWorker()
{
    m_NewThreadDataLock.acquire();
    m_NewThreadAdmissionOpen = false;
    m_StopNewThreadWorker = true;
    while (m_DelayedNewThreadData.count())
    {
        m_NewThreadData.pushBack(m_DelayedNewThreadData.popFront());
    }
    const bool pending = m_NewThreadData.count();
    m_NewThreadDataLock.release();

    if (!m_NewThreadWorker)
    {
        if (pending)
        {
            FATAL("Per-processor thread add queue has no worker.");
        }
        return;
    }

    m_NewThreadDataCondition.broadcast();
    m_NewThreadWorker.join();

    m_NewThreadDataLock.acquire();
    const bool drained =
        !m_NewThreadData.count() && !m_DelayedNewThreadData.count();
    m_NewThreadDataLock.release();
    if (!drained)
    {
        FATAL("Per-processor thread add worker stopped before draining.");
    }
}

int PerProcessorScheduler::processorAddThread(void *instance)
{
    PerProcessorScheduler *pInstance =
        reinterpret_cast<PerProcessorScheduler *>(instance);
    while (true)
    {
        pInstance->m_NewThreadDataLock.acquire();
        while (!pInstance->m_NewThreadData.count())
        {
            if (pInstance->m_StopNewThreadWorker)
            {
                pInstance->m_NewThreadDataLock.release();
                return 0;
            }

            pInstance->m_NewThreadDataCondition.waitForCompletion(
                pInstance->m_NewThreadDataLock);
        }

        void *p = pInstance->m_NewThreadData.popFront();
        pInstance->m_NewThreadDataLock.release();

        newThreadData *pData = reinterpret_cast<newThreadData *>(p);

        if (pInstance != &Processor::information().getScheduler())
        {
            FATAL(
                "instance "
                << instance
                << " does not match current scheduler in processorAddThread!");
        }

        Thread *pThread = pData->pThread;
        pThread->m_Lock.acquire();
        const bool retireBeforeStart =
            pThread->getUnwindState() == Thread::TerminateThread;
        if (
            pThread->m_Status == Thread::Created &&
            pThread->m_bStartRequested && !retireBeforeStart)
        {
            pThread->m_bStartRequested = false;
            pThread->m_Status = Thread::Ready;
        }

        const bool runnable =
            pThread->m_Status == Thread::Running ||
            pThread->m_Status == Thread::Ready;
        if (retireBeforeStart)
        {
            pThread->m_Lock.release();
            // This thread has never owned a running stack. The add worker owns
            // the last queued reference and can complete its off-stack exit.
            delete pData;
            pThread->shutdown();
            deleteThread(pThread);
            continue;
        }

        if (!runnable)
        {
            if (pThread->m_Status != Thread::Created)
            {
                pThread->m_Lock.release();
                FATAL(
                    "Per-processor add worker cannot park an already "
                    "scheduled thread.");
            }

            // State changes take m_Lock before publishing through
            // threadStatusChanged(). Holding it until the parked record is
            // visible closes the final lost-wakeup window.
            pInstance->m_NewThreadDataLock.acquire();
            const bool stopping = pInstance->m_StopNewThreadWorker;
            if (!stopping)
            {
                pInstance->m_DelayedNewThreadData.pushBack(p);
            }
            pInstance->m_NewThreadDataLock.release();
            pThread->m_Lock.release();
            if (!stopping)
            {
                continue;
            }

            pThread->setUnwindState(Thread::TerminateThread);
            delete pData;
            pThread->shutdown();
            deleteThread(pThread);
            continue;
        }

        pThread->setCpuId(Processor::id());
        if (pData->useSyscallState)
        {
            pInstance->addThread(pThread, pData->state);
        }
        else
        {
            pInstance->addThread(
                pThread, pData->pStartFunction, pData->pParam,
                pData->bUsermode, pData->pStack);
        }
        delete pData;
    }
}

void PerProcessorScheduler::initialise(Thread *pThread)
{
    m_pSchedulingAlgorithm = new RoundRobin();

    pThread->setStatus(Thread::Running);
    pThread->setCpuId(Processor::id());
    Processor::information().setCurrentThread(pThread);

    m_pSchedulingAlgorithm->addThread(pThread);
    Processor::information().setKernelStack(
        reinterpret_cast<uintptr_t>(pThread->getKernelStack()));
    Processor::setTlsBase(pThread->getTlsBase());

    SchedulerTimer *pTimer = Machine::instance().getSchedulerTimer();
    if (!pTimer)
    {
        panic("No scheduler timer present.");
    }
    Machine::instance().getSchedulerTimer()->registerHandler(this);

    startNewThreadWorker(pThread->getParent());
}

void PerProcessorScheduler::schedule(Thread::Status nextStatus)
{
    bool bWasInterrupts = Processor::getInterrupts();
    Processor::setInterrupts(false);

    Thread *pCurrentThread = Processor::information().getCurrentThread();
    if (!pCurrentThread)
    {
        FATAL("Missing a current thread in PerProcessorScheduler::schedule!");
    }

    // Grab the current thread's lock.
    pCurrentThread->getLock().acquire();

    bool dispatchEvent = false;
    if (nextStatus == Thread::Sleeping)
    {
        if (!pCurrentThread->hasActiveWaitUnlocked())
        {
            FATAL("Scheduler refused a sleep without an active WaitQueue.");
        }

        // The wait record is published before blockCurrent(). A wake in that
        // window changes it away from Waiting, so it is impossible to commit a
        // stale Sleeping transition.
        dispatchEvent = pCurrentThread->hasDeliverableEventsUnlocked();
        if (dispatchEvent)
        {
            PerProcessorScheduler *readyScheduler = nullptr;
            pCurrentThread->interruptWaitUnlocked(
                WaitQueue::WakeReason::Event, readyScheduler);
        }
        if (
            !pCurrentThread->activeWaitPendingUnlocked() || dispatchEvent)
        {
            pCurrentThread->getLock().release();
            Processor::setInterrupts(bWasInterrupts);
            return;
        }
    }

    // Now attempt to get another thread to run.
    // This will also get the lock for the returned thread.
    Thread *pNextThread = m_pSchedulingAlgorithm->getNext(pCurrentThread);
    if (pNextThread == 0)
    {
        // No other thread in the scheduler - take a round trip through the
        // idle thread before we schedule back to the yielding thread.
        // In most cases a thread is yielding either because it needs to
        // sleep to wait for something or because it has no work currently,
        // so simply switching back to it makes no sense (and causes us to
        // spin tightly rather than halting for an interrupt or other event)
        if (m_pIdleThread == 0)
        {
            // The scheduler is still bootstrapping, so spinning is the only
            // available fallback.
            pCurrentThread->getLock().release();
            Processor::setInterrupts(bWasInterrupts);
            return;
        }
        else
        {
            pNextThread = m_pIdleThread;
        }
    }

    // The idle fallback can select an already-running idle thread. Saving and
    // restoring the same hosted context does not yield and can strand the
    // add-thread worker indefinitely, so treat that selection as a no-op.
    if (pNextThread == pCurrentThread)
    {
        pCurrentThread->getLock().release();
        Processor::setInterrupts(bWasInterrupts);
        return;
    }

    pNextThread->getLock().acquire();

#if VERBOSE_SCHEDULER
    NOTICE_NOLOCK("schedule: " << pCurrentThread << " -> " << pNextThread << " -- " << pCurrentThread->getName() << " -> " << pNextThread->getName());
#endif

    // Now neither thread can be moved, we're safe to switch.
    if (pCurrentThread != m_pIdleThread)
        pCurrentThread->setStatus(nextStatus);
    pNextThread->setStatus(Thread::Running);
    Processor::information().setCurrentThread(pNextThread);

    // Load the new kernel stack into the TSS, and the new TLS base and switch
    // address spaces
    Processor::information().setKernelStack(
        reinterpret_cast<uintptr_t>(pNextThread->getKernelStack()));
    Processor::switchAddressSpace(*pNextThread->getParent()->getAddressSpace());
    Processor::setTlsBase(pNextThread->getTlsBase());

    // Update times.
    pCurrentThread->getParent()->trackTime(false);
    pNextThread->getParent()->recordTime(false);

    pNextThread->getLock().release();

    // The real switch releases the old current thread's lock after changing
    // stacks, so retire that deferred release from the lock checker now.
    EMIT_IF(TRACK_LOCKS)
    {
        g_LocksCommand.lockReleased(&pCurrentThread->getLock());
    }

    EMIT_IF(TRACK_LOCKS)
    {
        if (!g_LocksCommand.checkSchedule())
        {
            FATAL("Lock checker disallowed this reschedule.");
        }
    }

    EMIT_IF(SYSTEM_REQUIRES_ATOMIC_CONTEXT_SWITCH)
    {
        pCurrentThread->getLock().unwind();
        Processor::switchState(
            bWasInterrupts, pCurrentThread->state(), pNextThread->state(),
            &pCurrentThread->getLock().m_Atom.m_Atom);
        const bool waitOwnsEventDispatch =
            pCurrentThread->hasActiveWaitUnlocked();
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        Processor::notifyHostedContextSwitchStage(
            ProcessorBase::HostedContextSwitchStage::
                SchedulerBookkeepingComplete);
        Processor::notifyHostedContextSwitchStage(
            ProcessorBase::HostedContextSwitchStage::
                SchedulerRestoringInterrupts);
#endif
        Processor::setInterrupts(bWasInterrupts);
        if (!waitOwnsEventDispatch)
        {
            checkEventState(0);
        }
    }
    else
    {
        // NOTICE_NOLOCK("calling saveState [schedule]");
        if (Processor::saveState(pCurrentThread->state()))
        {
            // Just context-restored, return.

            // A resumed WaitQueue must retire its outer wait record before an
            // event handler can enter another blocking operation. WaitQueue
            // performs the event check immediately after that retirement.
            const bool waitOwnsEventDispatch =
                pCurrentThread->hasActiveWaitUnlocked();

            // Return to previous interrupt state.
            Processor::setInterrupts(bWasInterrupts);
            if (!waitOwnsEventDispatch)
            {
                // We don't have a user-mode stack available here, so pass zero
                // and don't execute user-mode event handlers.
                checkEventState(0);
            }

            return;
        }

        // Restore context, releasing the old thread's lock when we've switched
        // stacks.
        pCurrentThread->getLock().unwind();
        Processor::restoreState(
            pNextThread->state(), &pCurrentThread->getLock().m_Atom.m_Atom);
        // Not reached.
    }
}

void PerProcessorScheduler::checkEventState(uintptr_t userStack)
{
    bool bWasInterrupts = Processor::getInterrupts();
    Processor::setInterrupts(false);

    size_t pageSz = PhysicalMemoryManager::getPageSize();

    Thread *pThread = Processor::information().getCurrentThread();
    if (!pThread)
    {
        Processor::setInterrupts(bWasInterrupts);
        return;
    }

    if (pThread->getScheduler() != this)
    {
        // Wrong scheduler - don't try to run an event for this thread.
        Processor::setInterrupts(bWasInterrupts);
        return;
    }

    if (pThread->eventsDeferred())
    {
        // Cannot check for any events - we aren't allowed to handle them.
        Processor::setInterrupts(bWasInterrupts);
        return;
    }

    Event::Delivery eventDelivery = pThread->getNextEvent();
    Event *pEvent = eventDelivery.get();
    if (!eventDelivery)
    {
        Processor::setInterrupts(bWasInterrupts);
        return;
    }

    uintptr_t handlerAddress = pEvent->getHandlerAddress();

    // Simple heuristic for whether to launch the event handler in kernel or
    // user mode - is the handler address mapped kernel or user mode?
    VirtualAddressSpace &va = Processor::information().getVirtualAddressSpace();
    EMIT_IF(!HOSTED)
    {
        if (!va.isMapped(reinterpret_cast<void *>(handlerAddress)))
        {
            ERROR_NOLOCK(
                "checkEventState: Handler address " << Hex << handlerAddress
                                                    << " not mapped!");
            Processor::setInterrupts(bWasInterrupts);
            return;
        }
    }

    SchedulerState *oldState = pThread->pushState();
    if (!oldState)
    {
        // Keep the event pending until an outer handler unwinds and makes a
        // state slot available.
        pThread->sendEvent(pEvent);
        Processor::setInterrupts(bWasInterrupts);
        return;
    }

    physical_uintptr_t page;
    size_t flags;
    EMIT_IF(HOSTED)
    {
        flags = VirtualAddressSpace::KernelMode;
    }
    else
    {
        va.getMapping(reinterpret_cast<void *>(handlerAddress), page, flags);
        if (!(flags & VirtualAddressSpace::KernelMode))
        {
            if (userStack != 0)
                va.getMapping(
                    reinterpret_cast<void *>(userStack - pageSz), page, flags);
            if (userStack == 0 || (flags & VirtualAddressSpace::KernelMode))
            {
                VirtualAddressSpace::Stack *stateStack =
                    pThread->getStateUserStack();
                if (!stateStack)
                {
                    stateStack = va.allocateStack();
                    pThread->setStateUserStack(stateStack);
                }
                else
                {
                    // Verify that the stack is mapped
                    if (!va.isMapped(adjust_pointer(stateStack->getTop(), -pageSz)))
                    {
                        /// \todo This is a quickfix for a bigger problem. I imagine
                        ///       it has something to do with calling execve
                        ///       directly without fork, meaning the memory is
                        ///       cleaned up but the state level stack information
                        ///       is *not*.
                        stateStack = va.allocateStack();
                        pThread->setStateUserStack(stateStack);
                    }
                }

                userStack = reinterpret_cast<uintptr_t>(stateStack->getTop());
            }
            else
            {
                va.getMapping(reinterpret_cast<void *>(userStack), page, flags);
                if (flags & VirtualAddressSpace::KernelMode)
                {
                    NOTICE_NOLOCK(
                        "User stack for event in checkEventState is the kernel's!");
                    pThread->sendEvent(pEvent);
                    pThread->popState();
                    Processor::setInterrupts(bWasInterrupts);
                    return;
                }
            }
        }
    }

    // The address of the serialize buffer is determined by the thread ID and
    // the nesting level.
    uintptr_t addr =
        Event::getHandlerBuffer() + (pThread->getId() * MAX_NESTED_EVENTS +
                                     (pThread->getStateLevel() - 1)) *
                                        PhysicalMemoryManager::getPageSize();

    // Ensure the page is mapped.
    if (!va.isMapped(reinterpret_cast<void *>(addr)))
    {
        physical_uintptr_t p = PhysicalMemoryManager::instance().allocatePage();
        if (!p)
        {
            panic("checkEventState: Out of memory!");
        }
        va.map(p, reinterpret_cast<void *>(addr), VirtualAddressSpace::Write);
    }

    const bool deletableEvent = pEvent->isDeletable();
    if (!deletableEvent && (flags & VirtualAddressSpace::KernelMode))
    {
        eventDelivery.beginDispatch();
    }
    pEvent->serialize(reinterpret_cast<uint8_t *>(addr));

    // Fire-and-forget events are fully represented by their serialized data.
    // Stable kernel events retain the lease through their callback because
    // that callback may intentionally refer back to the original object.
    if (deletableEvent)
    {
        eventDelivery.reset();
    }

    EMIT_IF(!SYSTEM_REQUIRES_ATOMIC_CONTEXT_SWITCH)
    {
        if (Processor::saveState(*oldState))
        {
            // Just context-restored.
            Processor::setInterrupts(bWasInterrupts);
            return;
        }
    }

    if (flags & VirtualAddressSpace::KernelMode)
    {
#if HOSTED
        // Hosted state levels use distinct signal/scheduler stacks. Run the
        // handler on the selected level so an interrupt cannot save a
        // level-one frame that is physically still on level zero.
        callOnStack(
            reinterpret_cast<uintptr_t>(pThread->getKernelStack()),
            handlerAddress, addr);
#else
        void (*fn)(size_t) =
            reinterpret_cast<void (*)(size_t)>(handlerAddress);
        fn(addr);
#endif

        eventDelivery.reset();
        pThread->popState();
        Processor::setInterrupts(bWasInterrupts);
        return;
    }
    else if (userStack != 0)
    {
        // User delivery consumes only the serialized representation.
        eventDelivery.reset();
        pThread->getParent()->trackTime(false);
        pThread->getParent()->recordTime(true);
        EMIT_IF(SYSTEM_REQUIRES_ATOMIC_CONTEXT_SWITCH)
        {
            Processor::saveAndJumpUser(
                bWasInterrupts, *oldState, 0, Event::getTrampoline(), userStack,
                handlerAddress, addr);
            Processor::setInterrupts(bWasInterrupts);
        }
        else
        {
            Processor::jumpUser(
                0, Event::getTrampoline(), userStack, handlerAddress, addr);
            // Not reached.
        }
    }
}

void PerProcessorScheduler::eventHandlerReturned()
{
    Processor::setInterrupts(false);

    Thread *pThread = Processor::information().getCurrentThread();
    pThread->abandonCurrentState(false);

    Processor::restoreState(pThread->state());
    // Not reached.
}

void PerProcessorScheduler::addThread(
    Thread *pThread, Thread::ThreadStartFunc pStartFunction, void *pParam,
    bool bUsermode, void *pStack)
{
    // Handle wrong CPU, and handle thread not yet ready to schedule.
    if (this != &Processor::information().getScheduler() ||
        pThread->getStatus() == Thread::Created)
    {
        newThreadData *pData = new newThreadData;
        pData->pThread = pThread;
        pData->pStartFunction = pStartFunction;
        pData->pParam = pParam;
        pData->bUsermode = bUsermode;
        pData->pStack = pStack;
        pData->useSyscallState = false;

        m_NewThreadDataLock.acquire();
        if (!m_NewThreadAdmissionOpen)
        {
            m_NewThreadDataLock.release();
            pThread->m_Lock.release();
            delete pData;
            FATAL("Thread admitted after its per-processor worker stopped.");
        }
        m_NewThreadData.pushBack(pData);
        m_NewThreadDataLock.release();

        m_NewThreadDataCondition.signal();

        pThread->m_Lock.release();
        return;
    }

    pThread->setCpuId(Processor::id());
    pThread->setScheduler(this);

    bool bWasInterrupts = Processor::getInterrupts();
    Processor::setInterrupts(false);

    // We assume here that pThread's lock is already taken.

    Thread *pCurrentThread = Processor::information().getCurrentThread();

    // Grab the current thread's lock.
    pCurrentThread->getLock().acquire();

    m_pSchedulingAlgorithm->addThread(pThread);

    // Now neither thread can be moved, we're safe to switch.
    if (pCurrentThread != m_pIdleThread)
    {
        pCurrentThread->setStatus(Thread::Ready);
    }
    pThread->setStatus(Thread::Running);
    Processor::information().setCurrentThread(pThread);
    void *kernelStack = pThread->getKernelStack();
    Processor::information().setKernelStack(
        reinterpret_cast<uintptr_t>(kernelStack));
    Processor::switchAddressSpace(*pThread->getParent()->getAddressSpace());
    Processor::setTlsBase(pThread->getTlsBase());

    // This thread is safe from being moved as its status is now "running".
    // It is worth noting that we can't just call exit() here, as the lock is
    // not necessarily actually taken.
    if (pThread->getLock().m_bInterrupts)
        bWasInterrupts = true;
    bool bWas = pThread->getLock().acquired();
    pThread->getLock().unwind();
    pThread->getLock().m_Atom = true;
    EMIT_IF(TRACK_LOCKS)
    {
        // Satisfy the lock checker; we're releasing these out of order, so make
        // sure the checker sees them unlocked in order.
        g_LocksCommand.lockReleased(&pCurrentThread->getLock());
        if (bWas)
        {
            // Lock was in fact locked before.
            g_LocksCommand.lockReleased(&pThread->getLock());
        }
        if (!g_LocksCommand.checkSchedule())
        {
            FATAL("Lock checker disallowed this reschedule.");
        }
    }

    EMIT_IF(SYSTEM_REQUIRES_ATOMIC_CONTEXT_SWITCH)
    {
        pCurrentThread->getLock().unwind();
        if (bUsermode)
        {
            Processor::saveAndJumpUser(
                bWasInterrupts, pCurrentThread->state(),
                &pCurrentThread->getLock().m_Atom.m_Atom,
                reinterpret_cast<uintptr_t>(pStartFunction),
                reinterpret_cast<uintptr_t>(pStack),
                reinterpret_cast<uintptr_t>(pParam));
        }
        else
        {
            Processor::saveAndJumpKernel(
                bWasInterrupts, pCurrentThread->state(),
                &pCurrentThread->getLock().m_Atom.m_Atom,
                reinterpret_cast<uintptr_t>(pStartFunction),
                reinterpret_cast<uintptr_t>(pStack),
                reinterpret_cast<uintptr_t>(pParam));
        }
        Processor::setInterrupts(bWasInterrupts);
    }
    else
    {
        if (Processor::saveState(pCurrentThread->state()))
        {
            // Just context-restored.
            if (bWasInterrupts)
                Processor::setInterrupts(true);
            return;
        }

        pCurrentThread->getLock().unwind();
        if (bUsermode)
        {
            pCurrentThread->getParent()->recordTime(true);
            Processor::jumpUser(
                &pCurrentThread->getLock().m_Atom.m_Atom,
                reinterpret_cast<uintptr_t>(pStartFunction),
                reinterpret_cast<uintptr_t>(pStack),
                reinterpret_cast<uintptr_t>(pParam));
        }
        else
        {
            pCurrentThread->getParent()->recordTime(false);
            Processor::jumpKernel(
                &pCurrentThread->getLock().m_Atom.m_Atom,
                reinterpret_cast<uintptr_t>(pStartFunction),
                reinterpret_cast<uintptr_t>(pStack),
                reinterpret_cast<uintptr_t>(pParam));
        }
    }
}

void PerProcessorScheduler::addThread(Thread *pThread, SyscallState &state)
{
    // Handle wrong CPU, and handle thread not yet ready to schedule.
    if (this != &Processor::information().getScheduler() ||
        pThread->getStatus() == Thread::Created)
    {
        newThreadData *pData = new newThreadData;
        pData->pThread = pThread;
        pData->useSyscallState = true;
        pData->state = state;

        pThread->m_Lock.release();

        m_NewThreadDataLock.acquire();
        if (!m_NewThreadAdmissionOpen)
        {
            m_NewThreadDataLock.release();
            delete pData;
            FATAL("Thread admitted after its per-processor worker stopped.");
        }
        m_NewThreadData.pushBack(pData);
        m_NewThreadDataLock.release();

        m_NewThreadDataCondition.signal();
        return;
    }

    pThread->setCpuId(Processor::id());
    pThread->setScheduler(this);

    bool bWasInterrupts = Processor::getInterrupts();
    Processor::setInterrupts(false);

    // We assume here that pThread's lock is already taken.

    Thread *pCurrentThread = Processor::information().getCurrentThread();

    // Grab the current thread's lock.
    pCurrentThread->getLock().acquire();

    m_pSchedulingAlgorithm->addThread(pThread);

    // Now neither thread can be moved, we're safe to switch.

    if (pCurrentThread != m_pIdleThread)
    {
        pCurrentThread->setStatus(Thread::Ready);
    }
    pThread->setStatus(Thread::Running);
    Processor::information().setCurrentThread(pThread);
    void *kernelStack = pThread->getKernelStack();
    Processor::information().setKernelStack(
        reinterpret_cast<uintptr_t>(kernelStack));
    Processor::switchAddressSpace(*pThread->getParent()->getAddressSpace());
    Processor::setTlsBase(pThread->getTlsBase());

    // This thread is safe from being moved as its status is now "running".
    // It is worth noting that we can't just call exit() here, as the lock is
    // not necessarily actually taken.
    if (pThread->getLock().m_bInterrupts)
        bWasInterrupts = true;
    bool bWas = pThread->getLock().acquired();
    pThread->getLock().unwind();
    pThread->getLock().m_Atom.m_Atom = 1;
    EMIT_IF(TRACK_LOCKS)
    {
        g_LocksCommand.lockReleased(&pCurrentThread->getLock());
        if (bWas)
        {
            // We unlocked the lock, so track that unlock.
            g_LocksCommand.lockReleased(&pThread->getLock());
        }
        if (!g_LocksCommand.checkSchedule())
        {
            FATAL("Lock checker disallowed this reschedule.");
        }
    }

    // Copy the SyscallState into this thread's kernel stack.
    uintptr_t kStack = reinterpret_cast<uintptr_t>(pThread->getKernelStack());
    kStack -= sizeof(SyscallState);
    MemoryCopy(
        reinterpret_cast<void *>(kStack), reinterpret_cast<void *>(&state),
        sizeof(SyscallState));

    // Grab a reference to the stack in the form of a full SyscallState.
    SyscallState &newState = *reinterpret_cast<SyscallState *>(kStack);

    pCurrentThread->getParent()->trackTime(false);
    pThread->getParent()->recordTime(false);

    EMIT_IF(SYSTEM_REQUIRES_ATOMIC_CONTEXT_SWITCH)
    {
        pCurrentThread->getLock().unwind();
        NOTICE("restoring (new) syscall state");
        Processor::switchState(
            bWasInterrupts, pCurrentThread->state(), newState,
            &pCurrentThread->getLock().m_Atom.m_Atom);
    }
    else
    {
        if (Processor::saveState(pCurrentThread->state()))
        {
            // Just context-restored.
            if (bWasInterrupts)
                Processor::setInterrupts(true);
            return;
        }

        pCurrentThread->getLock().unwind();
        Processor::restoreState(newState, &pCurrentThread->getLock().m_Atom.m_Atom);
    }
}

void PerProcessorScheduler::killCurrentThread(Spinlock *pLock)
{
    Thread *pThread = Processor::information().getCurrentThread();

    // No C++ destructors run after this call. Retire stack-owned lifetime
    // records while their abandoned stack is still mapped.
    pThread->retireDeferredScopes(true);

    // Start shutting down the current thread while we can still schedule it.
    pThread->shutdown();

    Processor::setInterrupts(false);

    // Removing the current thread. Grab its lock.
    pThread->getLock().acquire();

    // If we're tracking locks, don't pollute the results. Yes, we've kept
    // this lock held, but it no longer matters.
    EMIT_IF(TRACK_LOCKS)
    {
        g_LocksCommand.lockReleased(&pThread->getLock());
        if (pLock)
        {
            g_LocksCommand.lockReleased(pLock);
        }
        if (!g_LocksCommand.checkSchedule())
        {
            FATAL("Lock checker disallowed this reschedule.");
        }
    }

    // Get another thread ready to schedule.
    // This will also get the lock for the returned thread.
    Thread *pNextThread = m_pSchedulingAlgorithm->getNext(pThread);

    if (pNextThread == 0 && m_pIdleThread == 0)
    {
        // Nothing to switch to, we're in a VERY bad situation.
        panic("Attempting to kill only thread on this processor!");
    }
    else if (pNextThread == 0)
    {
        pNextThread = m_pIdleThread;
    }

    if (pNextThread != pThread)
        pNextThread->getLock().acquire();

    pNextThread->setStatus(Thread::Running);
    Processor::information().setCurrentThread(pNextThread);
    void *kernelStack = pNextThread->getKernelStack();
    Processor::information().setKernelStack(
        reinterpret_cast<uintptr_t>(kernelStack));
    Processor::switchAddressSpace(*pNextThread->getParent()->getAddressSpace());
    Processor::setTlsBase(pNextThread->getTlsBase());

    pNextThread->getLock().exit();

    // Pass in the lock atom we were given if possible, as the caller wants an
    // atomic release (i.e. once the thread is no longer able to be scheduled).
    deleteThreadThenRestoreState(
        pThread, pNextThread->state(), pLock ? &pLock->m_Atom.m_Atom : 0);
}

void PerProcessorScheduler::deleteThread(Thread *pThread)
{
    Process *pProcess = pThread->getParent();
    // This runs on a temporary handoff stack before the replacement Thread
    // has retired any WaitQueue record it resumed from. Blocking here would
    // try to enrol that Thread in two queues at once. Close admission now;
    // joins drain in ordinary thread context, while the final external lease
    // completes deferred detached deletion.
    pThread->closeExternalLeaseAdmission();
    bool deleteTarget = false;
    bool completesProcessExit = false;
    bool wakeExitOwner = false;
    {
        RecursingLockGuard<Spinlock> processGuard(pProcess->m_Lock);
        deleteTarget = pThread->markReapable();
        completesProcessExit =
            pProcess->terminatingThreadReapable(pThread, wakeExitOwner);

        if (deleteTarget)
        {
            delete pThread;
        }
    }

    // killCurrentThread() keeps this lock closed until execution has left the
    // target stack. Release it only after all outer Process locks have
    // unwound: making another same-core thread runnable under those locks can
    // otherwise resume straight into a conflicting terminal operation.
    if (!deleteTarget)
    {
        pThread->getLock().unwind();
        pThread->getLock().m_Atom.m_Atom = 1;
    }

    // Process-exit progress is a predicate update under m_Lock followed by an
    // out-of-lock notification. Keeping the WaitQueue wake outside m_Lock
    // prevents a resumed owner from acquiring the queue under an outer lock.
    if (wakeExitOwner)
    {
        pProcess->m_TerminationWaiters.wakeAll();
    }

    if (!completesProcessExit)
    {
        return;
    }

    // This is the final Process access: publication can wake a reaper that
    // destroys both the Process and its retained, reapable Thread objects.
    pProcess->publishTermination();
}

void PerProcessorScheduler::removeThread(Thread *pThread)
{
    m_pSchedulingAlgorithm->removeThread(pThread);
}

void PerProcessorScheduler::blockCurrent()
{
    schedule(Thread::Sleeping);
}

void PerProcessorScheduler::publishReadyFromWait(Thread *pThread)
{
    assert(pThread);
    assert(pThread->getScheduler() == this);
    assert(pThread->getStatus() == Thread::Ready);
    m_pSchedulingAlgorithm->threadStatusChanged(pThread);
}

void PerProcessorScheduler::timer(uint64_t delta, InterruptState &state)
{
    // Hard IRQ publication only changes an atomic work predicate. Consume the
    // reschedule request at the scheduler interrupt, where a context switch is
    // already required and no arbitrary device IRQ frame is suspended.
    m_IrqWorkDoorbell.compareAndSwap(1, 0);
    schedule();

    // Check if the thread should exit.
    Thread *pThread = Processor::information().getCurrentThread();
    const Thread::UnwindType unwindState = pThread->getUnwindState();
    if (unwindState == Thread::TerminateThread)
    {
        // A kernel-mode timer can interrupt code while it owns arbitrary
        // locks. Defer to a WaitQueue/syscall boundary in that case.
        if (!state.kernelMode())
        {
            killCurrentThread();
        }
        return;
    }
    if (unwindState == Thread::Exit)
        pThread->getParent()->getSubsystem()->exit(0);
}

void PerProcessorScheduler::threadStatusChanged(Thread *pThread)
{
    bool wakeWorker = false;
    // Only Created threads can be parked in the add-worker predicate. Avoid
    // taking a sleeping mutex from ordinary scheduling and interrupt paths.
    if (pThread->getStatus() == Thread::Created)
    {
        m_NewThreadDataLock.acquire();
        for (
            List<void *>::Iterator it = m_DelayedNewThreadData.begin();
            it != m_DelayedNewThreadData.end();)
        {
            newThreadData *pData =
                reinterpret_cast<newThreadData *>(*it);
            if (pData->pThread == pThread)
            {
                void *p = *it;
                it = m_DelayedNewThreadData.erase(it);
                m_NewThreadData.pushBack(p);
                wakeWorker = true;
            }
            else
            {
                ++it;
            }
        }
        m_NewThreadDataLock.release();
    }

    if (wakeWorker)
    {
        m_NewThreadDataCondition.signal();
    }
    m_pSchedulingAlgorithm->threadStatusChanged(pThread);
}

void PerProcessorScheduler::ringIrqWorkDoorbell()
{
    m_IrqWorkDoorbell = 1;
}

void PerProcessorScheduler::serviceIrqWorkDoorbell()
{
    if (!m_pSchedulingAlgorithm ||
        !Processor::information().getCurrentThread())
    {
        return;
    }

    // One bounded claim is enough: a racing ring remains set for the next
    // scheduler tick, while the predicate-backed worker stays scheduler-
    // visible in the meantime.
    if (m_IrqWorkDoorbell.compareAndSwap(1, 0))
    {
        schedule();
    }
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
namespace
{
int hostedNewThreadWorkerEntry(void *parameter)
{
    Atomic<size_t> *calls =
        reinterpret_cast<Atomic<size_t> *>(parameter);
    *calls += 1;
    return 0;
}
}  // namespace

bool PerProcessorScheduler::currentIrqWorkDoorbellPendingForTest()
{
    return Processor::information()
               .getScheduler()
               .m_IrqWorkDoorbell.value() != 0;
}

void PerProcessorScheduler::serviceCurrentIrqWorkDoorbellForTest()
{
    Processor::information().getScheduler().serviceIrqWorkDoorbell();
}

bool PerProcessorScheduler::runHostedNewThreadWorkerRegressions()
{
    if (this != &Processor::information().getScheduler() || !m_NewThreadWorker)
    {
        ERROR(
            "HOSTED-WAIT-TEST: per-processor worker regression requires "
            "the active scheduler");
        return false;
    }

    constexpr size_t Attempts = 10000;
    auto isParked = [this](Thread *target) {
        bool found = false;
        m_NewThreadDataLock.acquire();
        for (
            List<void *>::Iterator it = m_DelayedNewThreadData.begin();
            it != m_DelayedNewThreadData.end(); ++it)
        {
            newThreadData *pData =
                reinterpret_cast<newThreadData *>(*it);
            if (pData->pThread == target)
            {
                found = true;
                break;
            }
        }
        m_NewThreadDataLock.release();
        return found;
    };
    auto waitUntilParked = [&isParked, Attempts](Thread *target) {
        for (size_t attempt = 0; attempt < Attempts; ++attempt)
        {
            if (isParked(target))
            {
                return true;
            }
            Scheduler::instance().yield();
        }
        return false;
    };
    auto check = [](bool condition, const char *message) {
        if (!condition)
        {
            ERROR("HOSTED-WAIT-TEST: " << message);
        }
        return condition;
    };

    Process *kernelProcess =
        Processor::information().getCurrentThread()->getParent();
    bool passed = true;

    Atomic<size_t> delayedCalls(0);
    Thread *delayed = new Thread(
        kernelProcess, hostedNewThreadWorkerEntry, &delayedCalls, nullptr,
        false, true, true);
    delayed->setName("hosted delayed add-worker target");
    const bool delayedParked = waitUntilParked(delayed);
    for (size_t attempt = 0; attempt < 64; ++attempt)
    {
        Scheduler::instance().yield();
    }
    const bool stayedDormant =
        delayedParked && isParked(delayed) && delayedCalls == 0;
    const bool started = delayed->start();
    const bool delayedJoined = delayed->joinForCompletion();
    const bool delayedPassed = check(
        stayedDormant && started && delayedJoined && delayedCalls == 1,
        "delayed add-worker target did not remain parked until its single "
        "start publication");
    passed &= delayedPassed;
    if (delayedPassed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS perprocessor-delayed-start-wake");
    }

    Atomic<size_t> terminatedCalls(0);
    Thread *terminated = new Thread(
        kernelProcess, hostedNewThreadWorkerEntry, &terminatedCalls, nullptr,
        false, true, true);
    terminated->setName("hosted terminated add-worker target");
    const bool terminatedParked = waitUntilParked(terminated);
    terminated->setUnwindState(Thread::TerminateThread);
    const bool terminatedJoined = terminated->joinForCompletion();
    const bool terminatedPassed = check(
        terminatedParked && terminatedJoined && terminatedCalls == 0,
        "terminate-before-start did not retire the parked add-worker target");
    passed &= terminatedPassed;
    if (terminatedPassed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS perprocessor-terminate-before-start");
    }

    Atomic<size_t> teardownCalls(0);
    Thread *teardown = new Thread(
        kernelProcess, hostedNewThreadWorkerEntry, &teardownCalls, nullptr,
        false, true, true);
    teardown->setName("hosted add-worker teardown target");
    const bool teardownParked = waitUntilParked(teardown);
    stopNewThreadWorker();
    const bool teardownJoined = teardown->joinForCompletion();

    m_NewThreadDataLock.acquire();
    const bool teardownDrained =
        !m_NewThreadAdmissionOpen && m_StopNewThreadWorker &&
        !m_NewThreadData.count() && !m_DelayedNewThreadData.count();
    m_NewThreadDataLock.release();
    const bool workerJoined = !m_NewThreadWorker;

    // A joined worker cannot be hiding on the condition variable or retain a
    // detached reference to this scheduler's queue state.
    m_NewThreadDataCondition.broadcast();
    for (size_t attempt = 0; attempt < 64; ++attempt)
    {
        Scheduler::instance().yield();
    }

    const bool teardownPassed = check(
        teardownParked && teardownJoined && teardownCalls == 0 &&
            teardownDrained && workerJoined,
        "owned add worker did not drain and join with pending parked work");
    passed &= teardownPassed;
    if (teardownPassed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS perprocessor-worker-teardown");
    }

    startNewThreadWorker(kernelProcess);

    Atomic<size_t> restartCalls(0);
    Thread *restart = new Thread(
        kernelProcess, hostedNewThreadWorkerEntry, &restartCalls, nullptr,
        false, true, true);
    restart->setName("hosted restarted add-worker target");
    const bool restartParked = waitUntilParked(restart);
    const bool restartStarted = restart->start();
    bool restartReapable = false;
    for (size_t attempt = 0; attempt < Attempts; ++attempt)
    {
        if (restart->isReapableForHostedTest())
        {
            restartReapable = true;
            break;
        }
        Scheduler::instance().yield();
    }
    const bool restartJoined =
        restartReapable && restart->joinForCompletion();
    const bool restartPassed = check(
        restartParked && restartStarted && restartJoined && restartCalls == 1,
        "replacement add worker did not process a fresh delayed admission");
    passed &= restartPassed;
    if (restartPassed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS perprocessor-worker-restart");
    }

    return passed;
}
#endif

void PerProcessorScheduler::setIdle(Thread *pThread)
{
    m_pIdleThread = pThread;
}
