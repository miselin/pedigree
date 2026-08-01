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
    return m_HandlerRegistry.registerHandler(handler);
}

bool HostedTimer::unregisterHandler(TimerHandler *handler)
{
    return m_HandlerRegistry.unregisterHandler(handler);
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void HostedTimer::setHandlerPinHook(HandlerPinHook hook)
{
    m_Instance.m_HandlerRegistry.setHandlerPinHook(hook);
}

void HostedTimer::setHandlerPrePinHook(HandlerPrePinHook hook)
{
    m_Instance.m_HandlerRegistry.setHandlerPrePinHook(hook);
}

void HostedTimer::setHandlerHazardClaimHook(HandlerHazardClaimHook hook)
{
    m_Instance.m_HandlerRegistry.setHandlerHazardClaimHook(hook);
}

void HostedTimer::setHandlerAtomicDrainHook(HandlerAtomicDrainHook hook)
{
    m_Instance.m_HandlerRegistry.setHandlerAtomicDrainHook(hook);
}

void HostedTimer::withHandlerMutationLockForTest(HandlerMutationLockHook hook)
{
    m_Instance.m_HandlerRegistry.withMutationLockForTest(hook);
}

bool HostedTimer::dispatchHandlerForTest(
    TimerHandler *handler, uint64_t delta, InterruptState &state)
{
    return m_Instance.m_HandlerRegistry.dispatch(delta, state, handler);
}

size_t HostedTimer::activeDispatchCountForTest(TimerHandler *handler)
{
    return m_Instance.m_HandlerRegistry.activeDispatchCountForTest(handler);
}

size_t HostedTimer::claimedDispatchCountForTest()
{
    return m_Instance.m_HandlerRegistry.claimedDispatchCountForTest();
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

    m_HandlerRegistry.reset();

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
    m_IrqId = irqManager.registerHardIsaIrqHandler(0, this);
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

    m_HandlerRegistry.reset();
}

HostedTimer::HostedTimer()
    : m_Year(0), m_Month(0), m_DayOfMonth(0), m_DayOfWeek(0), m_Hour(0),
      m_Minute(0), m_Second(0), m_Nanosecond(0), m_IrqId(0),
      m_HandlerRegistry(), m_Alarms(), m_AlarmLock(false)
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

    // Timer delta is in nanoseconds.
    m_HandlerRegistry.dispatch(delta, state);

    return true;
}
