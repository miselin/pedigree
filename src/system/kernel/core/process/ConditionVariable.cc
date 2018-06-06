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

#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/utility.h"

ConditionVariable::ConditionVariable() : m_Lock(false), m_Waiters()
{
}

ConditionVariable::~ConditionVariable()
{
    broadcast();
}

ConditionVariable::WaitResult ConditionVariable::wait(Mutex &mutex)
{
    Time::Timestamp zero = Time::Infinity;
    return wait(mutex, zero);
}

ConditionVariable::WaitResult
ConditionVariable::wait(Mutex &mutex, Time::Timestamp &timeout)
{
    Time::Timestamp startTime = Time::getTimeNanoseconds();

    if (mutex.getValue())
    {
        // Mutex must be acquired.
        WARNING("ConditionVariable::wait called without a locked mutex");
        return Result<bool, Error>::withError(MutexNotLocked);
    }

    Thread *me = Processor::information().getCurrentThread();

    m_Lock.acquire();
    m_Waiters.pushBack(me);

    void *alarmHandle = nullptr;
    if (timeout != Time::Infinity)
    {
        alarmHandle = Time::addAlarm(timeout);
    }

    // Safe now to release the mutex as we're about to sleep.
    mutex.release();

    uintptr_t ra = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    me->setDebugState(Thread::CondWait, ra);
    Processor::information().getScheduler().sleep(&m_Lock);
    me->setDebugState(Thread::None, 0);

    bool interrupted = me->wasInterrupted();

    // Woken up by something. Remove any alarm we have pending as we're
    // finishing our wait now.
    if (alarmHandle)
    {
        Time::removeAlarm(alarmHandle);
    }

    me->setInterrupted(false);

    Error err = NoError;

    bool r = false;
    if (interrupted)
    {
        // Timeout.
        err = TimedOut;
    }
    else if (me->getUnwindState() != Thread::Continue)
    {
        // Thread needs to quit.
        err = ThreadTerminating;
    }
    else
    {
        // We just got woken by something! Time to re-check the condition.
        /// \todo this is actually buggy as it won't respect the timeout
        r = mutex.acquire();
        if (!r)
        {
            err = MutexNotAcquired;
        }
    }

    Time::Timestamp endTime = Time::getTimeNanoseconds();

    // Update timeout value to suit. We want to be able to make consecutive
    // calls to wait() without changing the timeout value to allow for wakeups
    // with an unchanged time limit.
    if (timeout != Time::Infinity)
    {
        Time::Timestamp timeConsumed = endTime - startTime;
        if (timeConsumed >= timeout)
        {
            timeout = 0;
        }
        else
        {
            timeout -= timeConsumed;
        }
    }

    if (err != NoError)
    {
        // Remove us from the waiter list (error condition)
        // This is important as for things like timeouts we would otherwise
        // keep the reference in m_Waiters and it would never be cleaned up
        // until a broadcast() finally happens.
        LockGuard<Spinlock> guard(m_Lock);
        for (auto it = m_Waiters.begin(); it != m_Waiters.end();)
        {
            if ((*it) == me)
            {
                it = m_Waiters.erase(it);
            }
            else
            {
                ++it;
            }
        }
        return Result<bool, Error>::withError(err);
    }
    else
    {
        return Result<bool, Error>::withValue(r);
    }
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
