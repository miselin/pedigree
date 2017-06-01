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

#include <process/ConditionVariable.h>
#include <process/Mutex.h>
#include <process/Thread.h>
#include <processor/Processor.h>
#include <time/Time.h>
#include <Log.h>

ConditionVariable::ConditionVariable() : m_Lock(false), m_Waiters()
{
}

ConditionVariable::~ConditionVariable()
{
    broadcast();
}

bool ConditionVariable::wait(Mutex &mutex, Time::Timestamp timeout)
{
    if (mutex.getValue())
    {
        // Mutex must be acquired.
        WARNING("ConditionVariable::wait called without a locked mutex");
        return false;
    }

    Thread *me = Processor::information().getCurrentThread();

    m_Lock.acquire();
    m_Waiters.pushBack(me);

    void *alarmHandle = nullptr;
    if (timeout > 0)
    {
        alarmHandle = addAlarm(timeout);
    }

    // Safe now to release the mutex as we're about to sleep.
    mutex.release();

    while (true)
    {
        uintptr_t ra = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        me->setDebugState(Thread::CondWait, ra);
        Processor::information().getScheduler().sleep(&m_Lock);
        me->setDebugState(Thread::None, 0);

        if (timeout == 0)
        {
            break;
        }
        else if (me->wasInterrupted())
        {
            // Timeout
            removeAlarm(alarmHandle);
            return false;
        }
        else if (me->getUnwindState() != Thread::Continue)
        {
            removeAlarm(alarmHandle);
            return false;
        }

        me->setInterrupted(false);
    }

    // We just got woken by something, so let the caller check the condition.
    // They will re-enter if the condition is not met.
    return mutex.acquire();
}

void ConditionVariable::signal()
{
    LockGuard<Spinlock> guard(m_Lock);

    if (!m_Waiters.count())
    {
        return;
    }

    // Mark the next thread ready.
    Thread *pThread = m_Waiters.popFront();
    pThread->getLock().acquire();
    pThread->setStatus(Thread::Ready);
    pThread->getLock().release();
}

void ConditionVariable::broadcast()
{
    LockGuard<Spinlock> guard(m_Lock);

    for (auto pThread : m_Waiters)
    {
        pThread->getLock().acquire();
        pThread->setStatus(Thread::Ready);
        pThread->getLock().release();
    }

    m_Waiters.clear();
}
