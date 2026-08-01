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

#include "Timer.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/TimerHandler.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/machine/Serial.h"

#include "pedigree/kernel/core/SlamAllocator.h"

#include <stdio.h>
#include <errno.h>

// Millisecond interval (tick every ms)
#define INTERVAL 1000000

using namespace __pedigree_hosted;

// Set by PhysicalMemoryManager.
extern size_t g_FreePages;
extern size_t g_AllocedPages;

static uint64_t addAlarmDuration(
    uint64_t deadline, size_t count, uint64_t multiplier)
{
    if (count > ((Time::Infinity - deadline) / multiplier))
    {
        return Time::Infinity;
    }
    return deadline + (count * multiplier);
}

HostedTimer HostedTimer::m_Instance;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
HostedTimer::HandlerPinHook HostedTimer::m_HandlerPinHook = nullptr;
#endif

uint8_t daysPerMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

HostedTimer::~HostedTimer()
{
    uninitialise();
}

void HostedTimer::addAlarm(Event *pEvent, size_t alarmSecs, size_t alarmUsecs)
{
    LockGuard<Spinlock> guard(m_AlarmLock);
    uint64_t deadline = getTickCountNano();
    deadline = addAlarmDuration(
        deadline, alarmSecs, Time::Multiplier::Second);
    deadline = addAlarmDuration(
        deadline, alarmUsecs, Time::Multiplier::Microsecond);
    Alarm *pAlarm = new Alarm(
        pEvent, deadline,
        Processor::information().getCurrentThread());
    m_Alarms.pushBack(pAlarm);
}

void HostedTimer::removeAlarm(Event *pEvent)
{
    LockGuard<Spinlock> guard(m_AlarmLock);
    for (List<Alarm *>::Iterator it = m_Alarms.begin(); it != m_Alarms.end();
         ++it)
    {
        if ((*it)->m_pEvent == pEvent)
        {
            Alarm *alarm = *it;
            m_Alarms.erase(it);
            delete alarm;
            return;
        }
    }
}

size_t HostedTimer::removeAlarm(class Event *pEvent, bool bRetZero)
{
    LockGuard<Spinlock> guard(m_AlarmLock);
    const uint64_t currTime = getTickCountNano();

    for (List<Alarm *>::Iterator it = m_Alarms.begin(); it != m_Alarms.end();
         ++it)
    {
        if ((*it)->m_pEvent == pEvent)
        {
            Alarm *alarm = *it;
            size_t ret = 0;
            if (!bRetZero)
            {
                size_t alarmEndTime = alarm->m_Time;

                // Is it later than the end of the alarm?
                if (alarmEndTime < currTime)
                    ret = 0;
                else
                {
                    const uint64_t diff = alarmEndTime - currTime;
                    ret = diff / Time::Multiplier::Second;
                    if (diff % Time::Multiplier::Second)
                    {
                        ++ret;
                    }
                }
            }

            m_Alarms.erase(it);
            delete alarm;
            return ret;
        }
    }

    return 0;
}

bool HostedTimer::registerHandler(TimerHandler *handler)
{
    if (!handler)
    {
        return false;
    }

    LockGuard<Spinlock> guard(m_HandlerLock);
    for (size_t i = 0; i < MAX_TIMER_HANDLERS; ++i)
    {
        if (m_Handlers[i].handler == handler)
        {
            if (
                !m_Handlers[i].enabled &&
                m_Handlers[i].deferredRemoval &&
                !m_Handlers[i].drainers)
            {
                // A new user arrived before a callback's deferred
                // self-removal completed.
                m_Handlers[i].enabled = true;
                m_Handlers[i].deferredRemoval = false;
                return true;
            }
            return false;
        }
    }

    for (size_t i = 0; i < MAX_TIMER_HANDLERS; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        if (!slot.handler)
        {
            assert(!slot.inFlight);
            assert(!slot.drainers);
            assert(!slot.dispatches);
            slot.handler = handler;
            slot.enabled = true;
            slot.deferredRemoval = false;
            return true;
        }
    }

    return false;
}

bool HostedTimer::unregisterHandler(TimerHandler *handler)
{
    if (!handler)
    {
        return false;
    }

    Thread *current = Processor::information().getCurrentThread();
    const bool canYield = current && Processor::getInterrupts();

    m_HandlerLock.acquire();

    HandlerSlot *slot = nullptr;
    for (size_t i = 0; i < MAX_TIMER_HANDLERS; ++i)
    {
        if (m_Handlers[i].handler == handler)
        {
            slot = &m_Handlers[i];
            break;
        }
    }

    if (!slot)
    {
        m_HandlerLock.release();
        return false;
    }

    bool callbackContext = false;
    bool selfUnregister = false;
    for (size_t i = 0; i < MAX_TIMER_HANDLERS; ++i)
    {
        for (
            HandlerDispatch *dispatch = m_Handlers[i].dispatches; dispatch;
            dispatch = dispatch->next)
        {
            if (dispatch->thread == current)
            {
                callbackContext = true;
                selfUnregister |= &m_Handlers[i] == slot;
            }
        }
    }

    if (selfUnregister)
    {
        // The synchronous ownership barrier cannot include the callback which
        // is currently making the request. Reject that contract explicitly,
        // but retire the legacy callback once it returns.
        slot->enabled = false;
        if (!slot->drainers)
        {
            slot->deferredRemoval = true;
        }
        m_HandlerLock.release();
        return false;
    }

    if (callbackContext && slot->inFlight)
    {
        // A timer callback must not block on another callback's pin.
        m_HandlerLock.release();
        return false;
    }

    // Stop new callbacks before waiting for callbacks which already own a pin.
    slot->enabled = false;
    slot->deferredRemoval = false;
    ++slot->drainers;
    if (!slot->inFlight)
    {
        --slot->drainers;
        if (!slot->drainers)
        {
            slot->handler = nullptr;
        }
        m_HandlerLock.release();
        return true;
    }
    m_HandlerLock.release();

    if (canYield)
    {
        TerminationDeferral terminationDeferral;
        while (true)
        {
            auto guard = slot->drainWaiters.acquire();
            m_HandlerLock.acquire();
            if (!slot->inFlight)
            {
                assert(slot->drainers);
                --slot->drainers;
                if (!slot->drainers)
                {
                    slot->handler = nullptr;
                }
                m_HandlerLock.release();
                return true;
            }
            m_HandlerLock.release();

            const WaitQueue::WakeReason reason =
                guard.waitForCompletion(
                    WaitQueue::Channel(slot), Thread::CallbackDrain,
                    reinterpret_cast<uintptr_t>(handler));
            (void) reason;
        }
    }

    while (true)
    {
        Processor::pause();
        m_HandlerLock.acquire();
        if (!slot->inFlight)
        {
            assert(slot->drainers);
            --slot->drainers;
            if (!slot->drainers)
            {
                slot->handler = nullptr;
            }
            m_HandlerLock.release();
            return true;
        }
        m_HandlerLock.release();
    }
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void HostedTimer::setHandlerPinHook(HandlerPinHook hook)
{
    __atomic_store_n(&m_HandlerPinHook, hook, __ATOMIC_RELEASE);
}
#endif

size_t HostedTimer::getYear()
{
    return m_Year;
}

uint8_t HostedTimer::getMonth()
{
    return m_Month;
}

uint8_t HostedTimer::getDayOfMonth()
{
    return m_DayOfMonth;
}

uint8_t HostedTimer::getDayOfWeek()
{
    return m_DayOfWeek;
}

uint8_t HostedTimer::getHour()
{
    return m_Hour;
}

uint8_t HostedTimer::getMinute()
{
    return m_Minute;
}

uint8_t HostedTimer::getSecond()
{
    return m_Second;
}

uint64_t HostedTimer::getNanosecond()
{
    return m_Nanosecond;
}

uint64_t HostedTimer::getTickCount()
{
    return getTickCountNano() / Time::Multiplier::Millisecond;
}

uint64_t HostedTimer::getTickCountNano()
{
    struct timespec tv;
    clock_gettime(CLOCK_MONOTONIC, &tv);
    return (tv.tv_sec * 1000000000ULL) + (tv.tv_nsec);
}

bool HostedTimer::initialise()
{
    assert(!m_bInitialized);

    synchronise();

    {
        LockGuard<Spinlock> guard(m_HandlerLock);
        for (size_t i = 0; i < MAX_TIMER_HANDLERS; ++i)
        {
            assert(!m_Handlers[i].inFlight);
            assert(!m_Handlers[i].drainers);
            assert(!m_Handlers[i].dispatches);
            m_Handlers[i].handler = nullptr;
            m_Handlers[i].enabled = false;
            m_Handlers[i].deferredRemoval = false;
        }
    }

    struct sigevent sv;
    ByteSet(&sv, 0, sizeof(sv));
    sv.sigev_notify = SIGEV_SIGNAL;
    sv.sigev_signo = SIGUSR1;
    sv.sigev_value.sival_ptr = this;
    int r = timer_create(CLOCK_MONOTONIC, &sv, &m_Timer);
    if (r != 0)
    {
        /// \todo error message or something
        return false;
    }

    struct itimerspec interval;
    ByteSet(&interval, 0, sizeof(interval));
    interval.it_interval.tv_nsec = INTERVAL;
    interval.it_value.tv_nsec = INTERVAL;
    r = timer_settime(m_Timer, 0, &interval, 0);
    if (r != 0)
    {
        timer_delete(m_Timer);
        return false;
    }

    IrqManager &irqManager = *Machine::instance().getIrqManager();
    m_IrqId = irqManager.registerIsaIrqHandler(0, this);
    if (m_IrqId == 0)
    {
        timer_delete(m_Timer);
        return false;
    }

    m_bInitialized = true;
    return true;
}

void HostedTimer::synchronise(bool tohw)
{
    if (tohw)
        return;

    struct timespec tv;
    clock_gettime(CLOCK_REALTIME, &tv);
    struct tm conv;
    struct tm *t = gmtime_r(&tv.tv_sec, &conv);
    assert (t != NULL);

    m_Nanosecond = tv.tv_nsec;
    m_Second = t->tm_sec;
    m_Minute = t->tm_min;
    m_Hour = t->tm_hour;
    m_DayOfMonth = t->tm_mday;
    m_Month = t->tm_mon;
    m_Year = t->tm_year + 1900;  // Years since 1900.
    m_DayOfWeek = t->tm_wday;
}

void HostedTimer::uninitialise()
{
    if (!m_bInitialized)
    {
        return;
    }
    m_bInitialized = false;

    synchronise();

    timer_delete(m_Timer);

    // Unregister the irq
    if (m_IrqId)
    {
        IrqManager &irqManager = *Machine::instance().getIrqManager();
        if (!irqManager.unregisterHandler(m_IrqId, this))
        {
            FATAL(
                "HostedTimer teardown could not synchronously unregister its "
                "IRQ callback");
        }
        m_IrqId = 0;
    }

    {
        LockGuard<Spinlock> guard(m_AlarmLock);
        for (
            List<Alarm *>::Iterator it = m_Alarms.begin();
            it != m_Alarms.end(); ++it)
        {
            delete *it;
        }
        m_Alarms.clear();
    }

    {
        LockGuard<Spinlock> guard(m_HandlerLock);
        for (size_t i = 0; i < MAX_TIMER_HANDLERS; ++i)
        {
            assert(!m_Handlers[i].inFlight);
            assert(!m_Handlers[i].drainers);
            assert(!m_Handlers[i].dispatches);
            m_Handlers[i].handler = nullptr;
            m_Handlers[i].enabled = false;
            m_Handlers[i].deferredRemoval = false;
        }
    }
}

HostedTimer::HostedTimer()
    : m_Year(0), m_Month(0), m_DayOfMonth(0), m_DayOfWeek(0), m_Hour(0),
      m_Minute(0), m_Second(0), m_Nanosecond(0), m_IrqId(0), m_Handlers(),
      m_HandlerLock(false), m_Alarms(), m_AlarmLock(false)
{
}

bool HostedTimer::irq(irq_id_t number, InterruptState &state)
{
    // Should we handle this?
    uint64_t opaque = state.getRegister(0);
    if (opaque != reinterpret_cast<uint64_t>(this))
    {
        return false;
    }

    // Update tick count.
    uint64_t delta = INTERVAL;

    // Calculate the new time/date
    m_Nanosecond += delta;

    // Check for alarms.
    m_AlarmLock.acquire();
    uint64_t tickCount = getTickCountNano();
    while (true)
    {
        bool bDispatched = false;
        for (List<Alarm *>::Iterator it = m_Alarms.begin();
             it != m_Alarms.end(); it++)
        {
            Alarm *pA = *it;
            if (pA->m_Time <= tickCount)
            {
                m_Alarms.erase(it);

                // sendEvent can synchronously schedule the target. Its timeout
                // cleanup may call removeAlarm before this IRQ frame resumes,
                // so do not carry the alarm lock across that handoff. Hosted
                // execution is single-core, and sendEvent registers the Event
                // before it can make the target runnable.
                m_AlarmLock.release();
                pA->m_pThread->sendEvent(pA->m_pEvent);
                delete pA;
                m_AlarmLock.acquire();
                bDispatched = true;
                break;
            }
        }
        if (!bDispatched)
            break;
    }
    m_AlarmLock.release();

    if (UNLIKELY(m_Nanosecond >= 1000000ULL))
    {
        // Every millisecond, unblock any interrupts which were halted and halt
        // any which need to be halted.
        Machine::instance().getIrqManager()->tick();
    }

    if (UNLIKELY(m_Nanosecond >= 1000000000ULL))
    {
        ++m_Second;
        m_Nanosecond -= 1000000000ULL;

#if MEMORY_LOGGING_ENABLED
        Serial *pSerial = Machine::instance().getSerial(1);
        NormalStaticString str;
        str += "Heap: ";
        str += SlamAllocator::instance().heapPageCount() * 4;
        str += "K\tPages: ";
        str += (g_AllocedPages * 4096) / 1024;
        str += "K\t Free: ";
        str += (g_FreePages * 4096) / 1024;
        str += "K\n";

        pSerial->write_str(str);
#endif

        if (UNLIKELY(m_Second == 60))
        {
            ++m_Minute;
            m_Second = 0;

            if (UNLIKELY(m_Minute == 60))
            {
                ++m_Hour;
                m_Minute = 0;

                if (UNLIKELY(m_Hour == 24))
                {
                    ++m_DayOfMonth;
                    m_Hour = 0;

                    // Are we in a leap year
                    bool isLeap = ((m_Year % 4) == 0) & (((m_Year % 100) != 0) |
                                                         ((m_Year % 400) == 0));

                    if (UNLIKELY(
                            ((m_DayOfMonth > daysPerMonth[m_Month - 1]) &&
                             ((m_Month != 2) || isLeap == false)) ||
                            (m_DayOfMonth > (daysPerMonth[m_Month - 1] + 1))))
                    {
                        ++m_Month;
                        m_DayOfMonth = 1;

                        if (UNLIKELY(m_Month > 12))
                        {
                            ++m_Year;
                            m_Month = 1;
                        }
                    }
                }
            }
        }
    }

    // Pin each callback under the registry lock, but never carry that lock
    // across handler code. unregisterHandler disables future pins and drains
    // any callback which has already committed here.
    for (size_t i = 0; i < MAX_TIMER_HANDLERS; ++i)
    {
        HandlerDispatch dispatch = {
            Processor::information().getCurrentThread(), nullptr};
        TimerHandler *handler = nullptr;

        m_HandlerLock.acquire();
        HandlerSlot &slot = m_Handlers[i];
        if (slot.handler && slot.enabled)
        {
            handler = slot.handler;
            ++slot.inFlight;
            dispatch.next = slot.dispatches;
            slot.dispatches = &dispatch;
        }
        m_HandlerLock.release();

        if (!handler)
        {
            continue;
        }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        HandlerPinHook hook =
            __atomic_load_n(&m_HandlerPinHook, __ATOMIC_ACQUIRE);
        if (hook)
        {
            hook(handler);
        }
#endif

        // Timer delta is in nanoseconds.
        handler->timer(delta, state);

        bool wakeDrainers = false;
        m_HandlerLock.acquire();
        HandlerDispatch **link = &slot.dispatches;
        while (*link && *link != &dispatch)
        {
            link = &(*link)->next;
        }
        assert(*link == &dispatch);
        *link = dispatch.next;
        assert(slot.inFlight);
        --slot.inFlight;
        wakeDrainers = !slot.inFlight && slot.drainers;
        if (!slot.inFlight && slot.deferredRemoval)
        {
            slot.handler = nullptr;
            slot.deferredRemoval = false;
        }
        m_HandlerLock.release();

        if (wakeDrainers)
        {
            slot.drainWaiters.wakeAll(
                WaitQueue::WakeReason::Signalled,
                WaitQueue::Channel(&slot));
        }
    }

    return true;
}
