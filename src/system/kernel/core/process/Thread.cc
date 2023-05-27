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

    // If we've been given a user stack pointer, we are a user mode thread.
    bool bUserMode = true;
    void *requestedStack = pStack;
    if (pStack == 0)
    {
        bUserMode = false;
        VirtualAddressSpace::Stack *kernelStack =
            m_StateLevels[0].m_pAuxillaryStack =
                m_StateLevels[0].m_pKernelStack;
        m_StateLevels[0].m_pKernelStack =
            0;  // No kernel stack if kernel mode thread - causes bug on PPC

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

    if (delayedStart)
    {
        m_Status = Sleeping;
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
    // NO! No kernel stack for kernel-mode threads. On PPC, causes bug!
    // m_pKernelStack =
    // VirtualAddressSpace::getKernelAddressSpace().allocateStack();

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

    m_Id = m_pParent->addThread(this);

    // SyscallState variant has to be called from the parent thread, so this is
    // OK to do.
    Thread *pCurrent = Processor::information().getCurrentThread();
    if (pCurrent->m_bTlsBaseOverride)
    {
        // Override our TLS base too (but this will be in the copied address
        // space).
        m_bTlsBaseOverride = true;
        m_pTlsBase = pCurrent->m_pTlsBase;
    }

    m_Lock.acquire();

    if (delayedStart)
    {
        m_Status = Sleeping;
    }

    // Now we are ready to go into the scheduler.
    ProcessorThreadAllocator::instance().addThread(this, state);
}

Thread::~Thread()
{
    if (InputManager::instance().removeCallbackByThread(this))
    {
        WARNING("A thread is being removed, but it never removed itself from "
                "InputManager.");
        WARNING(
            "This warning indicates an application or kernel module is buggy!");
    }

    // Before removing from the scheduler, terminate if needed.
    if (!m_bRemovingRequests)
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
    // We are now removing requests from this thread - deny any other thread
    // from doing so, as that may invalidate our iterators.
    m_bRemovingRequests = true;

    if (m_PendingRequests.count())
    {
        for (List<RequestQueue::Request *>::Iterator it =
                 m_PendingRequests.begin();
             it != m_PendingRequests.end();)
        {
            RequestQueue::Request *pReq = *it;
            RequestQueue *pQueue = pReq->owner;

            if (!pQueue)
            {
                ERROR("Thread::shutdown: request in pending requests list has "
                      "no owner!");
                ++it;
                continue;
            }

            // Halt the owning RequestQueue while we tweak this request.
            pReq->owner->halt();

            // During the halt, we may have lost a request. Check.
            if (!pQueue->isRequestValid(pReq))
            {
                // Resume queue and skip this request - it's dead.
                // Async items are run in their own thread, parented to the
                // kernel. So, for this to happen, a non-async request
                // succeeded, and may or may not have cleaned up.
                /// \todo identify a way to make cleanup work here.
                pQueue->resume();
                ++it;
                continue;
            }

            // Check for an already completed request. If we called addRequest,
            // the request will not have been destroyed as the RequestQueue is
            // expecting the calling thread to handle it.
            if (pReq->bCompleted)
            {
                // Only destroy if the refcount allows us to - other threads may
                // be also referencing this request (as RequestQueue has dedup).
                if (pReq->refcnt <= 1)
                    delete pReq;
                else
                {
                    pReq->refcnt--;

                    // Ensure the RequestQueue is not referencing us - we're
                    // dying.
                    if (pReq->pThread == this)
                        pReq->pThread = 0;
                }
            }
            else
            {
                // Not completed yet and the queue is halted. If there's more
                // than one thread waiting on the request, we can just decrease
                // the refcount and carry on. Otherwise, we can kill off the
                // request.
                if (pReq->refcnt > 1)
                {
                    pReq->refcnt--;
                    if (pReq->pThread == this)
                        pReq->pThread = 0;
                }
                else
                {
                    // Terminate.
                    pReq->bReject = true;
                    pReq->pThread = 0;
                    pReq->mutex.release();
                }
            }

            // Allow the queue to resume operation now.
            pQueue->resume();

            // Remove the request from our internal list.
            it = m_PendingRequests.erase(it);
        }
    }

    reportWakeup(WokenBecauseTerminating);

    // Notify any waiters on this thread.
    if (m_pWaiter)
    {
        m_pWaiter->getLock().acquire();
        m_pWaiter->setStatus(Thread::Ready);
        m_pWaiter->getLock().release();
    }

    // Mark us as waiting for a join if we aren't detached. This ensures that
    // join will not block waiting for this thread if it is called after this
    // point.
    m_ConcurrencyLock.acquire();
    if (!m_bDetached)
    {
        m_Status = AwaitingJoin;
    }
    m_ConcurrencyLock.release();
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
        // Wipe out any pending events that currently exist.
        for (List<Event *>::Iterator it = m_EventQueue.begin();
             it != m_EventQueue.end(); ++it)
        {
            Event *pEvent = *it;
            if (pEvent->isDeletable())
            {
                delete pEvent;
            }
        }

        m_EventQueue.clear();

        // Notify parent process we have become a zombie.
        // We do this here to avoid an amazing race between calling
        // notifyWaiters and scheduling a process into the Zombie state that can
        // cause some processes to simply never be reaped.
        if (m_pParent)
        {
            m_pParent->notifyWaiters();
        }
    }

    if (m_Status == Thread::Ready && previousStatus != Thread::Running)
    {
        /// \todo provide a way to report this in Thread::setStatus API
        reportWakeupUnlocked(Unknown);
    }

    if (m_pScheduler)
    {
        m_pScheduler->threadStatusChanged(this);
    }
}

SchedulerState &Thread::state()
{
    return *(m_StateLevels[m_nStateLevel].m_State);
}

SchedulerState &Thread::pushState()
{
    if ((m_nStateLevel + 1) >= MAX_NESTED_EVENTS)
    {
        ERROR("Thread: Max nested events!");
        /// \todo Take some action here - possibly kill the thread?
        return *(m_StateLevels[MAX_NESTED_EVENTS - 1].m_State);
    }
    m_nStateLevel++;
    // NOTICE("New state level: " << m_nStateLevel << "...");
    m_StateLevels[m_nStateLevel].m_InhibitMask =
        m_StateLevels[m_nStateLevel - 1].m_InhibitMask;

    allocateStackAtLevel(m_nStateLevel);

    setKernelStack();

    return *(m_StateLevels[m_nStateLevel - 1].m_State);
}

void Thread::popState(bool clean)
{
    size_t origStateLevel = m_nStateLevel;

    if (m_nStateLevel == 0)
    {
        ERROR("Thread: Potential error: popStack() called with state level 0!");
        ERROR("Thread: (ignore this if longjmp has been called)");
        return;
    }
    m_nStateLevel--;

    setKernelStack();

    if (clean)
    {
        cleanStateLevel(origStateLevel);
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
    return m_nStateLevel;
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
    if (m_StateLevels[m_nStateLevel].m_pKernelStack)
    {
        uintptr_t stack = reinterpret_cast<uintptr_t>(
            m_StateLevels[m_nStateLevel].m_pKernelStack->getTop());
        Processor::information().setKernelStack(stack);
    }
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
    // Check that we aren't already a zombie (can't receive events if so).
    if (m_Status == Zombie)
    {
        WARNING("Thread: dropping event as we are a zombie");
        return false;
    }

    /// \todo we should be checking inhibits HERE! so we don't wake a thread
    /// that has inhibited the sent event

    // Only need the lock to adjust the queue of events.
    m_Lock.acquire();
    m_EventQueue.pushBack(pEvent);
    m_Lock.release();

    pEvent->registerThread(this);

    if (m_Status == Sleeping)
    {
        if (m_bInterruptible)
        {
            reportWakeup(WokenByEvent);

            // Interrupt the sleeping thread, there's an event firing
            m_Status = Ready;

            // Notify the scheduler that we're now ready, so we get put into the
            // scheduling algorithm's ready queue.
            Scheduler::instance().threadStatusChanged(this);
        }
        else
        {
            WARNING("Thread: not immediately waking up from event as we're not "
                    "interruptible");
        }
    }

    return true;
}

void Thread::inhibitEvent(size_t eventNumber, bool bInhibit)
{
    LockGuard<Spinlock> guard(m_Lock);
    if (bInhibit)
        m_StateLevels[m_nStateLevel].m_InhibitMask->set(eventNumber);
    else
        m_StateLevels[m_nStateLevel].m_InhibitMask->clear(eventNumber);
}

void Thread::cullEvent(Event *pEvent)
{
    bool bDelete = false;
    {
        LockGuard<Spinlock> guard(m_Lock);

        for (List<Event *>::Iterator it = m_EventQueue.begin();
             it != m_EventQueue.end();)
        {
            if (*it == pEvent)
            {
                if ((*it)->isDeletable())
                {
                    bDelete = true;
                }
                it = m_EventQueue.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    pEvent->deregisterThread(this);

    // Delete last to avoid double frees.
    if (bDelete)
    {
        delete pEvent;
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
        it->deregisterThread(this);
        if (it->isDeletable())
            delete it;
    }
}

Event *Thread::getNextEvent()
{
    Event *pResult = nullptr;

    if (!m_bInterruptible)
    {
        // No events if we're not interruptible
        return nullptr;
    }

    {
        LockGuard<Spinlock> guard(m_Lock);

        for (size_t i = 0; i < m_EventQueue.count(); i++)
        {
            Event *e = m_EventQueue.popFront();
            if (!e)
            {
                ERROR("A null event was in a thread's event queue!");
                continue;
            }

            if (m_StateLevels[m_nStateLevel].m_InhibitMask->test(
                    e->getNumber()) ||
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

    if (pResult)
    {
        // de-register thread outside of the Thread lock to avoid Event/Thread
        // lock dependencies by accident
        pResult->deregisterThread(this);
        return pResult;
    }

    return 0;
}

bool Thread::hasEvents()
{
    LockGuard<Spinlock> guard(m_Lock);

    return m_EventQueue.count() != 0;
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

void Thread::addRequest(RequestQueue::Request *req)
{
    if (m_bRemovingRequests)
        return;

    m_PendingRequests.pushBack(req);
}

void Thread::removeRequest(RequestQueue::Request *req)
{
    if (m_bRemovingRequests)
        return;

    for (List<RequestQueue::Request *>::Iterator it = m_PendingRequests.begin();
         it != m_PendingRequests.end(); it++)
    {
        if (req == *it)
        {
            m_PendingRequests.erase(it);
            return;
        }
    }
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
    Thread *pThisThread = Processor::information().getCurrentThread();

    m_ConcurrencyLock.acquire();

    // Can't join a detached thread.
    if (m_bDetached)
    {
        m_ConcurrencyLock.release();
        return false;
    }

    // Check thread state. Perhaps the join is just a matter of terminating this
    // thread, as it has died.
    if (m_Status != AwaitingJoin)
    {
        if (m_pWaiter)
        {
            // Another thread is already join()ing.
            m_ConcurrencyLock.release();
            return false;
        }

        m_pWaiter = pThisThread;
        pThisThread->setDebugState(
            Joining, reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
        m_ConcurrencyLock.release();

        while (1)
        {
            Processor::information().getScheduler().sleep(0);
            if (!(pThisThread->wasInterrupted() ||
                  pThisThread->getUnwindState() != Thread::Continue))
                break;
        }

        pThisThread->setDebugState(None, 0);
    }
    else
    {
        m_ConcurrencyLock.release();
    }

    // Thread has terminated, we may now clean up.
    delete this;
    return true;
}

bool Thread::detach()
{
    if (m_Status == AwaitingJoin)
    {
        WARNING("Thread::detach() called on a thread that has already exited.");
        return join();
    }
    else
    {
        LockGuard<Spinlock> guard(m_ConcurrencyLock);

        if (m_pWaiter)
        {
            ERROR("Thread::detach() called while other threads are joining.");
            return false;
        }

        m_bDetached = true;
        return true;
    }
}

Thread::StateLevel::StateLevel()
    : m_State(), m_pKernelStack(0), m_pUserStack(0), m_pAuxillaryStack(0),
      m_InhibitMask(), m_pBlockingThread(0)
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
      m_pBlockingThread(s.m_pBlockingThread)
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
    m_pKernelStack = s.m_pKernelStack;
    return *this;
}

bool Thread::isInterruptible()
{
    return m_bInterruptible;
}

void Thread::setInterruptible(bool state)
{
    LockGuard<Spinlock> guard(m_Lock);
    m_bInterruptible = state;
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

void Thread::addWakeupWatcher(WakeReason *watcher)
{
    LockGuard<Spinlock> guard(m_Lock);

    m_WakeWatchers.pushBack(watcher);
}

void Thread::removeWakeupWatcher(WakeReason *watcher)
{
    LockGuard<Spinlock> guard(m_Lock);

    for (auto it = m_WakeWatchers.begin(); it != m_WakeWatchers.end();)
    {
        if ((*it) == watcher)
        {
            it = m_WakeWatchers.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void Thread::reportWakeup(WakeReason reason)
{
    LockGuard<Spinlock> guard(m_Lock);

    reportWakeupUnlocked(reason);
}

void Thread::reportWakeupUnlocked(WakeReason reason)
{
    for (auto it = m_WakeWatchers.begin(); it != m_WakeWatchers.end(); ++it)
    {
        *(*it) = reason;
    }

    m_WakeWatchers.clear();
}

#endif  // THREADS
