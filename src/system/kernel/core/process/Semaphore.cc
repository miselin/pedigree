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
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

namespace
{
constexpr size_t SemaphoreMagic = 0xdeadbaba;
constexpr size_t MutexStateTag = 1;
constexpr size_t MutexUnlocked = MutexStateTag;

size_t loadState(const size_t *state)
{
    return __atomic_load_n(state, __ATOMIC_ACQUIRE);
}

bool isMutexState(size_t state)
{
    return (state & MutexStateTag) == MutexStateTag;
}

size_t currentMutexOwner()
{
    Thread *thread = Processor::information().getCurrentThread();
    uintptr_t owner = thread
                          ? reinterpret_cast<uintptr_t>(thread)
                          : reinterpret_cast<uintptr_t>(
                                &Processor::information());
    assert((owner & MutexStateTag) == 0);
    return owner | MutexStateTag;
}

void destroyTimeoutEvent(Thread *thread, Event *event)
{
    if (!event)
    {
        return;
    }

    // Timer removal is the publication barrier; culling then covers a timeout
    // that was already queued while another wake reason won the wait.
    Machine::instance().getTimer()->removeAlarm(event);
    thread->cullEvent(event);
    delete event;
}

struct SemaphoreTimeoutAbandonment
{
    Thread *thread;
    Event *event;
};

void abandonSemaphoreWait(void *context)
{
    SemaphoreTimeoutAbandonment *abandonment =
        reinterpret_cast<SemaphoreTimeoutAbandonment *>(context);
    destroyTimeoutEvent(abandonment->thread, abandonment->event);
    abandonment->event = nullptr;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
Semaphore::MutexTransitionHook g_MutexTransitionHook = nullptr;
Atomic<size_t> g_SemaphoreTimeoutCreates(0);
Atomic<size_t> g_SemaphoreTimeoutDestroys(0);

void observeMutexTransition(Semaphore::MutexTransitionWindow window)
{
    Semaphore::MutexTransitionHook hook =
        __atomic_load_n(&g_MutexTransitionHook, __ATOMIC_ACQUIRE);
    if (hook)
    {
        hook(window);
    }
}
#endif
}  // namespace

static void interruptSemaphore(uint8_t *pBuffer)
{
    EMIT_IF(THREADS)
    {
        Processor::information()
            .getCurrentThread()
            ->markTimeoutInterruptedWait();
    }
}

Semaphore::SemaphoreEvent::SemaphoreEvent(size_t nestingLevel)
    : Event(
          reinterpret_cast<uintptr_t>(&interruptSemaphore),
          false /* Not deletable */, nestingLevel)
{
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    g_SemaphoreTimeoutCreates += 1;
#endif
}

Semaphore::SemaphoreEvent::~SemaphoreEvent()
{
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    g_SemaphoreTimeoutDestroys += 1;
#endif
}

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
    : magic(SemaphoreMagic), m_Counter(nInitialValue), m_Waiters(),
      m_bCanInterrupt(canInterrupt)
{
    assert(magic == SemaphoreMagic);
}

Semaphore::~Semaphore()
{
    assert(magic == SemaphoreMagic);
}

Semaphore::SemaphoreResult
Semaphore::acquireWithResult(
    size_t n, size_t timeoutSecs, size_t timeoutUsecs, bool deferTerminal)
{
    size_t state = loadState(&magic);
    assert(state == SemaphoreMagic || isMutexState(state));
    if (isMutexState(state))
    {
        if (n != 1)
        {
            FATAL("Mutex acquisition must request exactly one item");
        }
        if (state == currentMutexOwner())
        {
            FATAL("Recursive Mutex acquisition");
        }
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
        Thread *pThread = Processor::information().getCurrentThread();
        pThread->clearInterruption();
        const bool scopedTerminationDeferral =
            pThread->isTerminationDeferred();
        const bool terminalWaitDeferred =
            deferTerminal || scopedTerminationDeferral;

        // If we have a timeout, create the event and register it.
        Event *pEvent = 0;
        if (timeoutSecs || timeoutUsecs)
        {
            pEvent = new SemaphoreEvent(pThread->getStateLevel());
            Machine::instance().getTimer()->addAlarm(
                pEvent, timeoutSecs, timeoutUsecs);
        }
        SemaphoreTimeoutAbandonment timeoutAbandonment = {
            pThread, pEvent};

        SemaphoreResult result = SemaphoreResult::withValue(true);
        while (true)
        {
            if (
                scopedTerminationDeferral && !deferTerminal &&
                m_bCanInterrupt &&
                pThread->getUnwindState() != Thread::Continue)
            {
                destroyTimeoutEvent(pThread, pEvent);
                return SemaphoreResult::withError(Interrupted);
            }

            // The timeout handler can run before this thread reaches
            // WaitQueue::wait(). Keep its state as a predicate rather than
            // clearing the only evidence that the deadline expired.
            if (
                pThread->getInterruptionReason() ==
                Thread::InterruptedByTimeout)
            {
                destroyTimeoutEvent(pThread, pEvent);
                pThread->clearInterruption();
                return SemaphoreResult::withError(TimedOut);
            }

            if (tryAcquire(n))
            {
                destroyTimeoutEvent(pThread, pEvent);

                return result;
            }

            auto guard = m_Waiters.acquire();

            // The predicate is checked while serialised with release(). The
            // WaitQueue publishes a persistent wait record before dropping this
            // guard, closing both wake-before-enrol and wake-before-sleep gaps.
            if (tryAcquire(n))
            {
                destroyTimeoutEvent(pThread, pEvent);
                return result;
            }

            WaitQueue::WakeReason wakeReason =
                terminalWaitDeferred
                    ? guard.waitForCompletion(
                          WaitQueue::Channel(this), Thread::SemWait,
                          reinterpret_cast<uintptr_t>(
                              __builtin_return_address(0)))
                    : guard.wait(
                          WaitQueue::Channel(this), Thread::SemWait,
                          reinterpret_cast<uintptr_t>(
                              __builtin_return_address(0)),
                          pEvent ? &abandonSemaphoreWait : nullptr,
                          &timeoutAbandonment);

            // Event delivery can follow an ordinary wake which already won
            // waiter.reason. The per-wait marker remains authoritative.
            const Thread::InterruptionReason interruption =
                pThread->getInterruptionReason();
            if (
                (wakeReason == WaitQueue::WakeReason::Event ||
                 wakeReason == WaitQueue::WakeReason::Unwinding ||
                 wakeReason == WaitQueue::WakeReason::Terminating ||
                 interruption != Thread::NotInterrupted))
            {
                if (
                    terminalWaitDeferred &&
                    (wakeReason == WaitQueue::WakeReason::Unwinding ||
                     wakeReason == WaitQueue::WakeReason::Terminating))
                {
                    if (deferTerminal || !m_bCanInterrupt)
                    {
                        continue;
                    }

                    destroyTimeoutEvent(pThread, pEvent);
                    return SemaphoreResult::withError(Interrupted);
                }

                if (interruption == Thread::InterruptedByTimeout)
                {
                    result = SemaphoreResult::withError(TimedOut);
                    pThread->clearInterruption();
                }
                else if (
                    !m_bCanInterrupt && !deferTerminal &&
                    (wakeReason == WaitQueue::WakeReason::Event ||
                     interruption == Thread::InterruptedBySignal))
                {
                    // A Mutex wait is not an interruptible operation. The
                    // event was delivered, but must not leak into the next
                    // unrelated interruptible wait. The interruption marker
                    // also matters when an ordinary release won waiter.reason
                    // just before the event was dispatched.
                    pThread->clearInterruption();
                    continue;
                }
                else
                {
                    result = SemaphoreResult::withError(Interrupted);

                    // Callers such as lwIP expose only a boolean/timeout
                    // result. Keep the signal reason on the Thread so the
                    // syscall boundary can still distinguish EINTR.
                    if (interruption != Thread::InterruptedBySignal)
                    {
                        pThread->clearInterruption();
                    }
                }

                destroyTimeoutEvent(pThread, pEvent);
                return result;
            }
        }
    }
}

bool Semaphore::acquire(
    size_t n, size_t timeoutSecs, size_t timeoutUsecs)
{
    SemaphoreResult result =
        acquireWithResult(n, timeoutSecs, timeoutUsecs);
    return result.hasValue() && result.value();
}

bool Semaphore::acquireWithError(
    size_t n, size_t timeoutSecs, size_t timeoutUsecs,
    SemaphoreError &error)
{
    SemaphoreResult result =
        acquireWithResult(n, timeoutSecs, timeoutUsecs);
    if (result.hasError())
    {
        error = result.error();
        return false;
    }

    error = NoError;
    return result.value();
}

bool Semaphore::acquireForCompletion(
    size_t n, size_t timeoutSecs, size_t timeoutUsecs)
{
    EMIT_IF(!THREADS)
    {
        return acquire(n, timeoutSecs, timeoutUsecs);
    }
    else
    {
        Thread *thread = Processor::information().getCurrentThread();
        const bool hasTimeout = timeoutSecs || timeoutUsecs;
        const Time::Timestamp started = Time::getTicks();

        Time::Timestamp timeout = 0;
        if (hasTimeout)
        {
            const Time::Timestamp maximum = Time::Infinity - 1;
            if (timeoutSecs > (maximum / Time::Multiplier::Second))
            {
                timeout = maximum;
            }
            else
            {
                timeout =
                    timeoutSecs * Time::Multiplier::Second;
                const Time::Timestamp microseconds =
                    timeoutUsecs > (maximum / Time::Multiplier::Microsecond)
                        ? maximum
                        : timeoutUsecs * Time::Multiplier::Microsecond;
                timeout =
                    microseconds > (maximum - timeout)
                        ? maximum
                        : timeout + microseconds;
            }
        }

        Thread::InterruptionReason retainedInterruption =
            thread ? thread->getInterruptionReason()
                   : Thread::NotInterrupted;
        if (
            thread &&
            retainedInterruption != Thread::NotInterrupted)
        {
            thread->clearInterruption();
        }

        size_t remainingSecs = timeoutSecs;
        size_t remainingUsecs = timeoutUsecs;
        if (hasTimeout)
        {
            const Time::Timestamp remainingMicroseconds =
                (timeout / Time::Multiplier::Microsecond) +
                ((timeout % Time::Multiplier::Microsecond) ? 1 : 0);
            remainingSecs =
                remainingMicroseconds /
                (Time::Multiplier::Second / Time::Multiplier::Microsecond);
            remainingUsecs =
                remainingMicroseconds %
                (Time::Multiplier::Second / Time::Multiplier::Microsecond);
        }

        while (true)
        {
            SemaphoreResult result = acquireWithResult(
                n, remainingSecs, remainingUsecs, true);
            const bool acquired =
                result.hasValue() && result.value();
            const SemaphoreError error =
                result.hasError() ? result.error() : NoError;
            if (acquired)
            {
                if (
                    thread &&
                    retainedInterruption != Thread::NotInterrupted)
                {
                    thread->setInterruptionReason(
                        retainedInterruption);
                }
                return true;
            }

            if (error == TimedOut)
            {
                if (!hasTimeout)
                {
                    // This semaphore owns no alarm, so the timeout marker
                    // belongs to an outer wait whose mutex/lifetime state we
                    // are reacquiring. Retain it and finish ownership.
                    retainedInterruption =
                        Thread::InterruptedByTimeout;
                    continue;
                }
                if (
                    thread &&
                    retainedInterruption != Thread::NotInterrupted)
                {
                    thread->setInterruptionReason(
                        retainedInterruption);
                }
                return false;
            }

            if (
                error != Interrupted || !thread ||
                thread->getInterruptionReason() !=
                    Thread::InterruptedBySignal)
            {
                return false;
            }

            if (retainedInterruption != Thread::InterruptedByTimeout)
            {
                retainedInterruption = Thread::InterruptedBySignal;
            }
            thread->clearInterruption();

            if (!hasTimeout)
            {
                continue;
            }

            const Time::Timestamp elapsed =
                Time::getTicks() - started;
            if (elapsed >= timeout)
            {
                thread->setInterruptionReason(
                    retainedInterruption);
                return false;
            }

            const Time::Timestamp remaining = timeout - elapsed;
            const Time::Timestamp remainingMicroseconds =
                (remaining / Time::Multiplier::Microsecond) +
                ((remaining % Time::Multiplier::Microsecond) ? 1 : 0);
            remainingSecs =
                remainingMicroseconds /
                (Time::Multiplier::Second / Time::Multiplier::Microsecond);
            remainingUsecs =
                remainingMicroseconds %
                (Time::Multiplier::Second / Time::Multiplier::Microsecond);
        }
    }
}

bool Semaphore::tryAcquire(size_t n)
{
    const bool mutex = isMutexState(loadState(&magic));
    if (mutex && n != 1)
    {
        ERROR("Mutex acquisition must request exactly one item");
        return false;
    }

    if (mutex)
    {
        EnsureInterrupts interrupts(false);
        ssize_t value = m_Counter;

        if ((value - static_cast<ssize_t>(n)) < 0)
        {
            return false;
        }
        if (!m_Counter.compareAndSwap(value, value - n))
        {
            return false;
        }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        observeMutexTransition(MutexCounterAcquired);
#endif

        if (
            !__sync_bool_compare_and_swap(
                &magic, MutexUnlocked, currentMutexOwner()))
        {
            // The binary count and ownership record must move together. Put
            // the item back if a corrupt ownership state is ever observed.
            m_Counter += n;
            ERROR("Mutex acquired with stale ownership state");
            return false;
        }
    }
    else
    {
        // Counting Semaphore transitions remain independent of Mutex
        // ownership and do not alter interrupt state.
        ssize_t value = m_Counter;
        if ((value - static_cast<ssize_t>(n)) < 0)
        {
            return false;
        }
        if (!m_Counter.compareAndSwap(value, value - n))
        {
            return false;
        }
    }

    EMIT_IF(STRICT_LOCK_ORDERING)
    {
        // TODO LockManager::acquired(*this);
    }
    return true;
}

void Semaphore::release(size_t n)
{
    size_t state = loadState(&magic);
    assert(state == SemaphoreMagic || isMutexState(state));

    if (isMutexState(state))
    {
        if (n != 1)
        {
            ERROR("Mutex release must return exactly one item");
            return;
        }

        {
            EnsureInterrupts interrupts(false);
            const size_t owner = currentMutexOwner();
            if (!__sync_bool_compare_and_swap(&magic, owner, MutexUnlocked))
            {
                ERROR("Mutex release attempted by a non-owner");
                return;
            }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
            observeMutexTransition(MutexOwnerReleased);
#endif

            if (!m_Counter.compareAndSwap(0, 1))
            {
                // Do not allow a double release to turn a mutex into a
                // counting semaphore. Restore ownership so the actual owner
                // can recover.
                __sync_bool_compare_and_swap(
                    &magic, MutexUnlocked, owner);
                ERROR("Mutex release found an invalid binary count");
                return;
            }
        }
    }
    else
    {
        m_Counter += n;
    }

    EMIT_IF(THREADS)
    {
        // Waiters can request different counts, so wake all and let each retry
        // the counter predicate under the queue guard.
        m_Waiters.wakeAll(
            WaitQueue::WakeReason::Signalled, WaitQueue::Channel(this));
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

const void *Semaphore::getDebugMutexOwner() const
{
    const size_t state = loadState(&magic);
    if (!isMutexState(state) || state == MutexUnlocked)
    {
        return nullptr;
    }

    return reinterpret_cast<const void *>(state & ~MutexStateTag);
}

void Semaphore::initialiseMutex(bool locked)
{
    assert(loadState(&magic) == SemaphoreMagic);
    magic = locked ? currentMutexOwner() : MutexUnlocked;
}

void Semaphore::destroyMutex()
{
    if (loadState(&magic) != MutexUnlocked || m_Counter != 1)
    {
        FATAL("Destroying a locked or corrupt Mutex");
    }

    magic = SemaphoreMagic;
}

bool Semaphore::mutexOwnedByCurrentThread() const
{
    return loadState(&magic) == currentMutexOwner();
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void Semaphore::setMutexTransitionHook(MutexTransitionHook hook)
{
    __atomic_store_n(&g_MutexTransitionHook, hook, __ATOMIC_RELEASE);
}

size_t Semaphore::getHostedTimeoutCreateCount()
{
    return g_SemaphoreTimeoutCreates;
}

size_t Semaphore::getHostedTimeoutDestroyCount()
{
    return g_SemaphoreTimeoutDestroys;
}
#endif
