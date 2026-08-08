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

#include "TickSource.h"

#include "pedigree/kernel/utilities/utility.h"

#include <errno.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace
{
static_assert(
    __atomic_always_lock_free(sizeof(size_t), nullptr),
    "hosted tick expiration accounting must remain signal-safe");
static_assert(
    __atomic_always_lock_free(sizeof(bool), nullptr),
    "hosted tick state publication must remain signal-safe");

timespec nanosecondsToTimespec(uint64_t nanoseconds)
{
    timespec result;
    result.tv_sec = nanoseconds / 1000000000ULL;
    result.tv_nsec = nanoseconds % 1000000000ULL;
    return result;
}

#if !defined(__linux__)
uint64_t monotonicNanoseconds()
{
    timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return 0;
    }
    return (static_cast<uint64_t>(now.tv_sec) * 1000000000ULL) +
           static_cast<uint64_t>(now.tv_nsec);
}

int waitFor(
    pthread_cond_t *condition, pthread_mutex_t *mutex, uint64_t nanoseconds)
{
    timespec delay = nanosecondsToTimespec(nanoseconds);
#if defined(__APPLE__)
    return pthread_cond_timedwait_relative_np(condition, mutex, &delay);
#else
    timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
    {
        return errno;
    }
    deadline.tv_sec += delay.tv_sec;
    deadline.tv_nsec += delay.tv_nsec;
    if (deadline.tv_nsec >= 1000000000L)
    {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(condition, mutex, &deadline);
#endif
}
#endif
}  // namespace

HostedTickSource::HostedTickSource()
#if defined(__linux__)
    : m_Timer(),
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      m_InjectedExpirations(0),
#endif
      m_Signal(0), m_Owner(nullptr), m_Prepared(false)
#else
    : m_TargetThread(), m_HelperThread(), m_Mutex(), m_Condition(),
      m_IntervalNanoseconds(0), m_PendingExpirations(0), m_Generation(0),
      m_Armed(false), m_Delivering(false), m_Stop(false), m_Failed(false),
      m_HelperStarted(false), m_Signal(0), m_Owner(nullptr), m_Prepared(false)
#endif
{
}

HostedTickSource::~HostedTickSource()
{
    destroy();
}

bool HostedTickSource::prepare(int signal, void *owner)
{
    if (__atomic_load_n(&m_Prepared, __ATOMIC_ACQUIRE) || signal <= 0 ||
        !owner)
    {
        return false;
    }

    m_Signal = signal;
    m_Owner = owner;

#if defined(__linux__)
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    __atomic_store_n(
        &m_InjectedExpirations, static_cast<size_t>(0), __ATOMIC_RELEASE);
#endif
    const long executionThreadId = ::syscall(SYS_gettid);
    if (executionThreadId <= 0)
    {
        return false;
    }

    sigevent event;
    ByteSet(&event, 0, sizeof(event));
    event.sigev_notify = SIGEV_THREAD_ID;
    event.sigev_signo = signal;
    event.sigev_value.sival_ptr = owner;
    event._sigev_un._tid =
        static_cast<decltype(event._sigev_un._tid)>(executionThreadId);
    if (timer_create(CLOCK_MONOTONIC, &event, &m_Timer) != 0)
    {
        return false;
    }
#else
    m_TargetThread = pthread_self();
    m_IntervalNanoseconds = 0;
    m_PendingExpirations = 0;
    m_Generation = 0;
    m_Armed = false;
    m_Delivering = false;
    m_Stop = false;
    m_Failed = false;
    if (pthread_mutex_init(&m_Mutex, nullptr) != 0)
    {
        return false;
    }
    if (pthread_cond_init(&m_Condition, nullptr) != 0)
    {
        pthread_mutex_destroy(&m_Mutex);
        return false;
    }
    if (pthread_create(&m_HelperThread, nullptr, helperEntry, this) != 0)
    {
        pthread_cond_destroy(&m_Condition);
        pthread_mutex_destroy(&m_Mutex);
        return false;
    }
    m_HelperStarted = true;
#endif

    __atomic_store_n(&m_Prepared, true, __ATOMIC_RELEASE);
    return true;
}

bool HostedTickSource::arm(uint64_t intervalNanoseconds)
{
    if (!__atomic_load_n(&m_Prepared, __ATOMIC_ACQUIRE) ||
        !intervalNanoseconds)
    {
        return false;
    }

#if defined(__linux__)
    itimerspec interval;
    ByteSet(&interval, 0, sizeof(interval));
    interval.it_interval = nanosecondsToTimespec(intervalNanoseconds);
    interval.it_value = interval.it_interval;
    return timer_settime(m_Timer, 0, &interval, nullptr) == 0;
#else
    pthread_mutex_lock(&m_Mutex);
    if (__atomic_load_n(&m_Failed, __ATOMIC_ACQUIRE))
    {
        pthread_mutex_unlock(&m_Mutex);
        return false;
    }
    m_IntervalNanoseconds = intervalNanoseconds;
    m_Armed = true;
    ++m_Generation;
    pthread_cond_broadcast(&m_Condition);
    pthread_mutex_unlock(&m_Mutex);
    return true;
#endif
}

bool HostedTickSource::disarm()
{
    if (!__atomic_load_n(&m_Prepared, __ATOMIC_ACQUIRE))
    {
        return true;
    }

#if defined(__linux__)
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    __atomic_store_n(
        &m_InjectedExpirations, static_cast<size_t>(0), __ATOMIC_RELEASE);
#endif
    itimerspec disarmed;
    ByteSet(&disarmed, 0, sizeof(disarmed));
    return timer_settime(m_Timer, 0, &disarmed, nullptr) == 0;
#else
    pthread_mutex_lock(&m_Mutex);
    m_Armed = false;
    ++m_Generation;
    pthread_cond_broadcast(&m_Condition);
    while (m_Delivering)
    {
        pthread_cond_wait(&m_Condition, &m_Mutex);
    }
    __atomic_store_n(
        &m_PendingExpirations, static_cast<size_t>(0), __ATOMIC_RELEASE);
    const bool success = !__atomic_load_n(&m_Failed, __ATOMIC_ACQUIRE);
    pthread_mutex_unlock(&m_Mutex);
    return success;
#endif
}

void HostedTickSource::destroy()
{
    if (!__atomic_load_n(&m_Prepared, __ATOMIC_ACQUIRE))
    {
        return;
    }

#if defined(__linux__)
    timer_delete(m_Timer);
#else
    disarm();
    pthread_mutex_lock(&m_Mutex);
    m_Stop = true;
    pthread_cond_broadcast(&m_Condition);
    pthread_mutex_unlock(&m_Mutex);
    if (m_HelperStarted)
    {
        pthread_join(m_HelperThread, nullptr);
        m_HelperStarted = false;
    }
    pthread_cond_destroy(&m_Condition);
    pthread_mutex_destroy(&m_Mutex);
#endif

    __atomic_store_n(&m_Prepared, false, __ATOMIC_RELEASE);
    m_Signal = 0;
    m_Owner = nullptr;
}

HostedTickSource::TakeResult HostedTickSource::takeExpirations(
    const siginfo_t *info, size_t &expirations)
{
    expirations = 0;
    if (!__atomic_load_n(&m_Prepared, __ATOMIC_ACQUIRE) || !info ||
        info->si_signo != m_Signal)
    {
        return TakeResult::NotSource;
    }

#if defined(__linux__)
    if (info->si_code != SI_TIMER || info->si_value.sival_ptr != m_Owner)
    {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        expirations = __atomic_exchange_n(
            &m_InjectedExpirations, static_cast<size_t>(0),
            __ATOMIC_ACQ_REL);
        if (expirations)
        {
            return TakeResult::Expirations;
        }
#endif
        return TakeResult::NotSource;
    }
    if (info->si_overrun < 0)
    {
        return TakeResult::Invalid;
    }
    expirations = static_cast<size_t>(info->si_overrun) + 1;
#else
    if (__atomic_load_n(&m_Failed, __ATOMIC_ACQUIRE))
    {
        return TakeResult::Invalid;
    }
    expirations = __atomic_exchange_n(
        &m_PendingExpirations, static_cast<size_t>(0), __ATOMIC_ACQ_REL);
#endif
    return TakeResult::Expirations;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
bool HostedTickSource::queueExpirationForTest()
{
    if (!__atomic_load_n(&m_Prepared, __ATOMIC_ACQUIRE))
    {
        return false;
    }

#if defined(__linux__)
    size_t pending =
        __atomic_load_n(&m_InjectedExpirations, __ATOMIC_ACQUIRE);
    while (true)
    {
        if (pending == ~static_cast<size_t>(0))
        {
            return false;
        }
        if (__atomic_compare_exchange_n(
                &m_InjectedExpirations, &pending, pending + 1, false,
                __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
        {
            break;
        }
    }
    if (pthread_kill(pthread_self(), m_Signal) == 0)
    {
        return true;
    }
    __atomic_fetch_sub(&m_InjectedExpirations, 1, __ATOMIC_ACQ_REL);
    return false;
#else
    if (!recordExpirations(1))
    {
        return false;
    }
    if (pthread_kill(m_TargetThread, m_Signal) == 0)
    {
        return true;
    }
    __atomic_fetch_sub(&m_PendingExpirations, 1, __ATOMIC_ACQ_REL);
    return false;
#endif
}
#endif

#if !defined(__linux__)
void *HostedTickSource::helperEntry(void *source)
{
    static_cast<HostedTickSource *>(source)->helper();
    return nullptr;
}

bool HostedTickSource::recordExpirations(size_t expirations)
{
    size_t pending =
        __atomic_load_n(&m_PendingExpirations, __ATOMIC_ACQUIRE);
    while (true)
    {
        if (expirations > (~static_cast<size_t>(0) - pending))
        {
            return false;
        }
        if (__atomic_compare_exchange_n(
                &m_PendingExpirations, &pending, pending + expirations, false,
                __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
        {
            return true;
        }
    }
}

void HostedTickSource::helper()
{
    pthread_mutex_lock(&m_Mutex);
    while (!m_Stop)
    {
        while (!m_Stop && !m_Armed)
        {
            pthread_cond_wait(&m_Condition, &m_Mutex);
        }
        if (m_Stop)
        {
            break;
        }

        const size_t generation = m_Generation;
        const uint64_t interval = m_IntervalNanoseconds;
        uint64_t deadline = monotonicNanoseconds() + interval;
        while (!m_Stop && m_Armed && generation == m_Generation)
        {
            const uint64_t now = monotonicNanoseconds();
            if (now < deadline)
            {
                waitFor(&m_Condition, &m_Mutex, deadline - now);
                continue;
            }

            const uint64_t elapsed = now - deadline;
            const uint64_t count64 = 1 + (elapsed / interval);
            if (count64 > static_cast<uint64_t>(~static_cast<size_t>(0)))
            {
                __atomic_store_n(&m_Failed, true, __ATOMIC_RELEASE);
                m_Armed = false;
                break;
            }
            deadline += count64 * interval;
            m_Delivering = true;
            pthread_mutex_unlock(&m_Mutex);

            const bool recorded = recordExpirations(
                static_cast<size_t>(count64));
            const int delivered =
                recorded ? pthread_kill(m_TargetThread, m_Signal) : EOVERFLOW;

            pthread_mutex_lock(&m_Mutex);
            if (delivered != 0)
            {
                __atomic_store_n(&m_Failed, true, __ATOMIC_RELEASE);
                m_Armed = false;
            }
            m_Delivering = false;
            pthread_cond_broadcast(&m_Condition);
        }
    }
    pthread_mutex_unlock(&m_Mutex);
}
#endif
