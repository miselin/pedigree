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
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/SchedulerTimer.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/RoundRobin.h"
#include "pedigree/kernel/process/SchedulingAlgorithm.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/utilities/utility.h"
#include "pedigree/kernel/debugger/commands/LocksCommand.h"

PerProcessorScheduler::PerProcessorScheduler()
    : m_pSchedulingAlgorithm(0), m_NewThreadDataLock(false),
      m_NewThreadDataCondition(), m_NewThreadData(), m_pIdleThread(0)
#if ARM_BEAGLE
      ,
      m_TickCount(0)
#endif
{
}

PerProcessorScheduler::~PerProcessorScheduler()
{
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

int PerProcessorScheduler::processorAddThread(void *instance)
{
    PerProcessorScheduler *pInstance =
        reinterpret_cast<PerProcessorScheduler *>(instance);
    pInstance->m_NewThreadDataLock.acquire();
    while (true)
    {
        if (!pInstance->m_NewThreadData.count())
        {
            /// \todo handle result
            pInstance->m_NewThreadDataCondition.wait(
                pInstance->m_NewThreadDataLock);
            continue;
        }

        void *p = pInstance->m_NewThreadData.popFront();

        newThreadData *pData = reinterpret_cast<newThreadData *>(p);

        if (pInstance != &Processor::information().getScheduler())
        {
            FATAL(
                "instance "
                << instance
                << " does not match current scheduler in processorAddThread!");
        }

        // Only add thread if it's in a valid status for adding. Otherwise we
        // need to spin. Yes - this is NOT efficient. Threads with delayed start
        // should not do much between creation and starting.
        if (!(pData->pThread->getStatus() == Thread::Running ||
              pData->pThread->getStatus() == Thread::Ready))
        {
            pInstance->m_NewThreadData.pushBack(p);
            pInstance->schedule();  // yield
            continue;
        }

        pData->pThread->setCpuId(Processor::id());
        pData->pThread->m_Lock.acquire();
        if (pData->useSyscallState)
        {
            pInstance->addThread(pData->pThread, pData->state);
        }
        else
        {
            pInstance->addThread(
                pData->pThread, pData->pStartFunction, pData->pParam,
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

    Thread *pAddThread = new Thread(
        pThread->getParent(), processorAddThread,
        reinterpret_cast<void *>(this), 0, false, true);
    pAddThread->setName("PerProcessorScheduler thread add worker");
    pAddThread->detach();
}

void PerProcessorScheduler::schedule(
    Thread::Status nextStatus, Thread *pNewThread, Spinlock *pLock)
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

    // Now attempt to get another thread to run.
    // This will also get the lock for the returned thread.
    Thread *pNextThread;
    if (!pNewThread)
    {
        pNextThread = m_pSchedulingAlgorithm->getNext(pCurrentThread);
        if (pNextThread == 0)
        {
            bool needsIdle = false;

            // If we're supposed to be sleeping, this isn't a good place to be
            if (nextStatus != Thread::Ready)
            {
                needsIdle = true;
            }
            else
            {
                if (pCurrentThread->getScheduler() == this)
                {
                    // Nothing to switch to, but we aren't sleeping. Just
                    // return.
                    pCurrentThread->getLock().release();
                    Processor::setInterrupts(bWasInterrupts);
                    return;
                }
                else
                {
                    // Current thread is switching cores, and no other thread
                    // was available. So we have to go idle.
                    needsIdle = true;
                }
            }

            if (needsIdle)
            {
                if (m_pIdleThread == 0)
                {
                    FATAL("No idle thread available, and the current thread is "
                          "leaving the ready state!");
                }
                else
                {
                    pNextThread = m_pIdleThread;
                }
            }
        }
    }
    else
    {
        pNextThread = pNewThread;
    }

    if (pNextThread == pNewThread)
        WARNING("scheduler: next thread IS new thread");

    if (pNextThread != pCurrentThread)
        pNextThread->getLock().acquire();

    // Now neither thread can be moved, we're safe to switch.
    if (pCurrentThread != m_pIdleThread)
        pCurrentThread->setStatus(nextStatus);
    pNextThread->setStatus(Thread::Running);
    Processor::information().setCurrentThread(pNextThread);

    // Should *never* happen
    if (pLock &&
        (pNextThread->getStateLevel() == reinterpret_cast<uintptr_t>(pLock)))
        FATAL(
            "STATE LEVEL = LOCK PASSED TO SCHEDULER: "
            << pNextThread->getStateLevel() << "/"
            << reinterpret_cast<uintptr_t>(pLock) << "!");

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

    // We'll release the current thread's lock when we reschedule, so for now
    // we just lie to the lock checker.
    EMIT_IF(TRACK_LOCKS)
    {
        g_LocksCommand.lockReleased(&pCurrentThread->getLock());
    }

    if (pLock)
    {
        // We cannot call ->release() here, because this lock was grabbed
        // before we disabled interrupts, so it may re-enable interrupts.
        // And that would be a very bad thing.
        //
        // We instead store the interrupt state of the spinlock, and manually
        // unlock it.
        if (pLock->m_bInterrupts)
            bWasInterrupts = true;
        pLock->exit();
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
        Processor::setInterrupts(bWasInterrupts);
        checkEventState(0);
    }
    else
    {
        // NOTICE_NOLOCK("calling saveState [schedule]");
        if (Processor::saveState(pCurrentThread->state()))
        {
            // Just context-restored, return.

            // Return to previous interrupt state.
            Processor::setInterrupts(bWasInterrupts);

            // Check the event state - we don't have a user mode stack available
            // to us, so pass zero and don't execute user-mode event handlers.
            checkEventState(0);

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

    if (!pThread->isInterruptible())
    {
        // Cannot check for any events - we aren't allowed to handle them.
        Processor::setInterrupts(bWasInterrupts);
        return;
    }

    Event *pEvent = pThread->getNextEvent();
    if (!pEvent)
    {
        Processor::setInterrupts(bWasInterrupts);
        return;
    }

    uintptr_t handlerAddress = pEvent->getHandlerAddress();

    // Simple heuristic for whether to launch the event handler in kernel or
    // user mode - is the handler address mapped kernel or user mode?
    VirtualAddressSpace &va = Processor::information().getVirtualAddressSpace();
    if (!va.isMapped(reinterpret_cast<void *>(handlerAddress)))
    {
        ERROR_NOLOCK(
            "checkEventState: Handler address " << handlerAddress
                                                << " not mapped!");
        if (pEvent->isDeletable())
            delete pEvent;
        Processor::setInterrupts(bWasInterrupts);
        return;
    }

    SchedulerState &oldState = pThread->pushState();

    physical_uintptr_t page;
    size_t flags;
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
                Processor::setInterrupts(bWasInterrupts);
                return;
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

    pEvent->serialize(reinterpret_cast<uint8_t *>(addr));

    EMIT_IF(!SYSTEM_REQUIRES_ATOMIC_CONTEXT_SWITCH)
    {
        if (Processor::saveState(oldState))
        {
            // Just context-restored.
            Processor::setInterrupts(bWasInterrupts);
            return;
        }
    }

    if (pEvent->isDeletable())
        delete pEvent;

    if (flags & VirtualAddressSpace::KernelMode)
    {
        void (*fn)(size_t) = reinterpret_cast<void (*)(size_t)>(handlerAddress);
        fn(addr);
        pThread->popState();

        Processor::setInterrupts(bWasInterrupts);
        return;
    }
    else if (userStack != 0)
    {
        pThread->getParent()->trackTime(false);
        pThread->getParent()->recordTime(true);
        EMIT_IF(SYSTEM_REQUIRES_ATOMIC_CONTEXT_SWITCH)
        {
            Processor::saveAndJumpUser(
                bWasInterrupts, oldState, 0, Event::getTrampoline(), userStack,
                handlerAddress, addr);
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
    pThread->popState(false);  // can't safely clean, we're on the stack

    Processor::restoreState(pThread->state());
    // Not reached.
}

void PerProcessorScheduler::addThread(
    Thread *pThread, Thread::ThreadStartFunc pStartFunction, void *pParam,
    bool bUsermode, void *pStack)
{
    // Handle wrong CPU, and handle thread not yet ready to schedule.
    if (this != &Processor::information().getScheduler() ||
        pThread->getStatus() == Thread::Sleeping)
    {
        NOTICE("wrong cpu => this=" << this << " sched=" << &Processor::information().getScheduler());
        newThreadData *pData = new newThreadData;
        pData->pThread = pThread;
        pData->pStartFunction = pStartFunction;
        pData->pParam = pParam;
        pData->bUsermode = bUsermode;
        pData->pStack = pStack;
        pData->useSyscallState = false;

        m_NewThreadDataLock.acquire();
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
    pThread->getLock().m_Atom.m_Atom = 1;
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
        pThread->getStatus() == Thread::Sleeping)
    {
        newThreadData *pData = new newThreadData;
        pData->pThread = pThread;
        pData->useSyscallState = true;
        pData->state = state;

        pThread->m_Lock.release();

        m_NewThreadDataLock.acquire();
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
        if (!g_LocksCommand.checkSchedule())
        {
            FATAL("Lock checker disallowed this reschedule.");
        }
        if (pLock)
        {
            g_LocksCommand.lockReleased(pLock);
            if (!g_LocksCommand.checkSchedule())
            {
                FATAL("Lock checker disallowed this reschedule.");
            }
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
    if (pThread->detached())
    {
        delete pThread;
    }
}

void PerProcessorScheduler::removeThread(Thread *pThread)
{
    m_pSchedulingAlgorithm->removeThread(pThread);
}

void PerProcessorScheduler::sleep(Spinlock *pLock)
{
    // Before sleeping, check for any pending events, and process them.
    // Looping ensures any events that come in while we're processing an
    // event still get handled.
    Thread *pThread = Processor::information().getCurrentThread();
    if (pThread->hasEvents())
    {
        // We're about to handle an event, so release the lock (as the schedule
        // would have done that had we not handled an event).
        if (pLock)
            pLock->release();

        checkEventState(0);

        // We handled some events, so abort the sleep. The caller should now go
        // ahead and retry the previous operation it tried before it slept and
        // perhaps try and sleep again.
        return;
    }

    // Now we can happily sleep.
    schedule(Thread::Sleeping, 0, pLock);
}

void PerProcessorScheduler::timer(uint64_t delta, InterruptState &state)
{
#if ARM_BEAGLE  // Timer at 1 tick per ms, we want to run every 100 ms
    m_TickCount++;
    if ((m_TickCount % 100) == 0)
    {
#endif
        schedule();

        // Check if the thread should exit.
        Thread *pThread = Processor::information().getCurrentThread();
        if (pThread->getUnwindState() == Thread::Exit)
            pThread->getParent()->getSubsystem()->exit(0);
#if ARM_BEAGLE
    }
#endif
}

void PerProcessorScheduler::threadStatusChanged(Thread *pThread)
{
    m_pSchedulingAlgorithm->threadStatusChanged(pThread);
}

void PerProcessorScheduler::setIdle(Thread *pThread)
{
    m_pIdleThread = pThread;
}
