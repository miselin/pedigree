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

#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/eventNumbers.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

static void interruptSemaphore(uint8_t *pBuffer)
{
    EMIT_IF(THREADS)
    {
        Processor::information().getCurrentThread()->setInterrupted(true);
    }
}

Semaphore::SemaphoreEvent::SemaphoreEvent()
    : Event(
          reinterpret_cast<uintptr_t>(&interruptSemaphore),
          false /* Not deletable */)
{
}

Semaphore::SemaphoreEvent::~SemaphoreEvent() = default;

size_t Semaphore::SemaphoreEvent::serialize(uint8_t *pBuffer)
{
    return 0;
}

bool Semaphore::SemaphoreEvent::unserialize(
    uint8_t *pBuffer, Semaphore::SemaphoreEvent &event)
{
    return true;
}

size_t Semaphore::SemaphoreEvent::getNumber()
{
    return EventNumbers::Interrupt;
}

Semaphore::Semaphore(size_t nInitialValue, bool canInterrupt)
    : magic(0xdeadbaba), m_Counter(nInitialValue), m_BeingModified(false),
      m_Queue(), m_bCanInterrupt(canInterrupt)
{
    assert(magic == 0xdeadbaba);
}

Semaphore::~Semaphore()
{
    assert(magic == 0xdeadbaba);
    m_Queue.clear();
}

void Semaphore::removeThread(Thread *pThread)
{
    m_BeingModified.acquire();
    for (List<Thread *>::Iterator it = m_Queue.begin(); it != m_Queue.end();
         ++it)
    {
        if ((*it) == pThread)
        {
            m_Queue.erase(it);
            break;  /// \note Assume no other pointers that match
        }
    }
    m_BeingModified.release();
}

Semaphore::SemaphoreResult
Semaphore::acquireWithResult(size_t n, size_t timeoutSecs, size_t timeoutUsecs)
{
    if (magic != 0xdeadbaba)
    {
        NOTICE(magic);
        assert(false);
    }

    // Spin 10 times in the case that the lock is about to be released on
    // multiprocessor systems, and just once for uniprocessor systems, so we don't
    // go through the rigmarole of creating a timeout event if the lock is
    // available.
    EMIT_IF(MULTIPROCESSOR)
    {
        for (int i = 0; i < 10; i++)
        {
            if (tryAcquire(n))
                return SemaphoreResult::withValue(true);
        }
    }

    if (tryAcquire(n))
        return SemaphoreResult::withValue(true);

    EMIT_IF(!THREADS)
    {
        // failed to tryAcquire - no point doing anything else here.
        return SemaphoreResult::withError(TimedOut);
    }
    else
    {
        // If we have a timeout, create the event and register it.
        Event *pEvent = 0;
        if (timeoutSecs || timeoutUsecs)
        {
            pEvent = new SemaphoreEvent();
            Machine::instance().getTimer()->addAlarm(
                pEvent, timeoutSecs, timeoutUsecs);
        }

        SemaphoreResult result = SemaphoreResult::withValue(true);
        while (true)
        {
            Thread *pThread = Processor::information().getCurrentThread();
            if (tryAcquire(n))
            {
                if (pEvent)
                {
                    Machine::instance().getTimer()->removeAlarm(pEvent);
                    delete pEvent;
                }

                removeThread(pThread);
                return result;
            }

            m_BeingModified.acquire();
            bool bWasInterrupts = m_BeingModified.interrupts();

            // To avoid a race condition, check again here after we've disabled
            // interrupts. This stops the condition where the lock is released after
            // tryAcquire returns false, but before we grab the "being modified"
            // lock, which means the lock could be released by this point!
            if (tryAcquire(n))
            {
                if (pEvent)
                {
                    Machine::instance().getTimer()->removeAlarm(pEvent);
                    delete pEvent;
                }
                m_BeingModified.release();
                removeThread(pThread);
                return result;
            }

            m_Queue.pushBack(pThread);

            Thread::WakeReason wakeReason = Thread::NotWoken;

            pThread->setInterrupted(false);
            pThread->setUnwindState(Thread::Continue);
            pThread->setDebugState(
                Thread::SemWait,
                reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
            pThread->addWakeupWatcher(&wakeReason);
            Processor::information().getScheduler().sleep(&m_BeingModified);
            pThread->setDebugState(Thread::None, 0);
            pThread->removeWakeupWatcher(&wakeReason);  // sanity removal

            // Either acquired or interrupted, either way, we don't need to be woken
            // again
            removeThread(pThread);

            // Why were we woken?
            bool bState = true;
            if (m_bCanInterrupt && wakeReason != Thread::NotWoken)
            {
                // don't care about unknown wakeups for this
                if (wakeReason != Thread::Unknown)
                {
                    if (pThread->wasInterrupted())
                    {
                        // Timed out!
                        result = SemaphoreResult::withError(TimedOut);
                    }
                    else
                    {
                        // Interrupted by some other source (e.g. an event).
                        result = SemaphoreResult::withError(Interrupted);
                    }

                    if (pEvent)
                    {
                        Machine::instance().getTimer()->removeAlarm(pEvent);
                        delete pEvent;
                    }

                    bState = false;
                }
            }

            // Restore interrupt state. If we are woken by an event (eg, timeout,
            // user input, etc), we seem to fail to restore the correct interrupt
            // state when we unwind the event state. This is not great - it leaves
            // us in a weird state where interrupts are disabled, and means any
            // further iterations of the acquire loop will believe that interrupts
            // were entirely disabled. A side-effect of acquiring a Semaphore should
            // not be that interrupts are disabled, unless they were disabled before
            // acquire() was called!
            Processor::setInterrupts(bWasInterrupts);

            if (!bState)
            {
                return result;
            }
        }
    }
}

bool Semaphore::tryAcquire(size_t n)
{
    ssize_t value = m_Counter;

    if ((value - static_cast<ssize_t>(n)) < 0)
        return false;
    if (m_Counter.compareAndSwap(value, value - n))
    {
        EMIT_IF(STRICT_LOCK_ORDERING)
        {
            // TODO LockManager::acquired(*this);
        }
        return true;
    }
    return false;
}

void Semaphore::release(size_t n)
{
    assert(magic == 0xdeadbaba);
    m_Counter += n;

    EMIT_IF(THREADS)
    {
        m_BeingModified.acquire();
        if (m_Queue.count())
        {
            // Because we can't wake a suspended thread, and we pop each thread from
            // the queue, we need to push them back on the queue later.
            List<Thread *> stillPendingThreads;

            // We should wake threads without holding the modification lock, so this
            // list holds the threads that we need to wake.
            List<Thread *> wakeupThreads;

            while (m_Queue.count() != 0)
            {
                Thread *pThread = m_Queue.popFront();
                if (!pThread)
                {
                    WARNING("Null thread in a Semaphore thread queue");
                    continue;
                }
                else if (!Scheduler::instance().threadInSchedule(pThread))
                {
                    WARNING("A thread that was to be woken by a Semaphore is no "
                            "longer in the scheduler");
                    continue;
                }
                else if (pThread->getStatus() != Thread::Sleeping)
                {
                    // Don't wake up a thread that isn't sleeping (perhaps
                    // suspended?)
                    if (pThread->getStatus() == Thread::Zombie)
                        WARNING(
                            "Semaphore has a zombie thread in its thread queue");
                    else
                        stillPendingThreads.pushBack(pThread);
                    continue;
                }

                wakeupThreads.pushBack(pThread);
            }

            if (stillPendingThreads.count())
            {
                for (List<Thread *>::Iterator it = stillPendingThreads.begin();
                     it != stillPendingThreads.end(); ++it)
                {
                    Thread *pThread = *it;
                    m_Queue.pushBack(pThread);
                }
            }

            m_BeingModified.release();

            if (wakeupThreads.count())
            {
                for (List<Thread *>::Iterator it = wakeupThreads.begin();
                     it != wakeupThreads.end(); ++it)
                {
                    Thread *pThread = *it;
                    pThread->getLock().acquire();
                    pThread->setStatus(Thread::Ready);
                    pThread->getLock().release();
                }
            }
        }
        else
        {
            m_BeingModified.release();
        }
    }

    EMIT_IF(STRICT_LOCK_ORDERING)
    {
        // TODO LockManager::released(*this);
    }
}

ssize_t Semaphore::getValue()
{
    return static_cast<ssize_t>(m_Counter);
}
