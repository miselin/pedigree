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
#include "pedigree/kernel/machine/Serial.h"
#include "pedigree/kernel/machine/TimerHandler.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/state.h"

#include "pedigree/kernel/core/SlamAllocator.h"

#include <errno.h>
#include <stdio.h>

// Millisecond interval (tick every ms)
#define INTERVAL 1000000

using namespace __pedigree_hosted;

// Set by PhysicalMemoryManager.
extern size_t g_FreePages;
extern size_t g_AllocedPages;

static uint64_t
addAlarmDuration(uint64_t deadline, size_t count, uint64_t multiplier)
{
    if (count > ((Time::Infinity - deadline) / multiplier))
    {
        return Time::Infinity;
    }
    return deadline + (count * multiplier);
}

HostedTimer HostedTimer::m_Instance;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
HostedTimer::AlarmSendAdmissionHook HostedTimer::m_AlarmSendAdmissionHook =
    nullptr;

bool HostedTimer::setSignalIntervalForTest(uint64_t nanoseconds)
{
    if (!m_Instance.m_bInitialized || !nanoseconds)
    {
        return false;
    }

    struct itimerspec interval;
    ByteSet(&interval, 0, sizeof(interval));
    interval.it_interval.tv_sec =
        nanoseconds / Time::Multiplier::Second;
    interval.it_interval.tv_nsec =
        nanoseconds % Time::Multiplier::Second;
    interval.it_value = interval.it_interval;
    return timer_settime(m_Instance.m_Timer, 0, &interval, nullptr) == 0;
}
#endif

HostedTimer::~HostedTimer()
{
    uninitialise();
}

void HostedTimer::addAlarm(Event *pEvent, size_t alarmSecs, size_t alarmUsecs)
{
    LockGuard<Spinlock> guard(m_AlarmLock);
    uint64_t deadline = getTickCountNano();
    deadline = addAlarmDuration(deadline, alarmSecs, Time::Multiplier::Second);
    deadline =
        addAlarmDuration(deadline, alarmUsecs, Time::Multiplier::Microsecond);
    Alarm *pAlarm = new Alarm(
        pEvent, deadline, Processor::information().getCurrentThread());
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

bool HostedTimer::dispatchHandlerForTest(TimerHandler *handler, uint64_t delta)
{
    return m_Instance.m_HandlerRegistry.dispatch(delta, handler);
}

size_t HostedTimer::activeDispatchCountForTest(TimerHandler *handler)
{
    return m_Instance.m_HandlerRegistry.activeDispatchCountForTest(handler);
}

size_t HostedTimer::claimedDispatchCountForTest()
{
    return m_Instance.m_HandlerRegistry.claimedDispatchCountForTest();
}

void HostedTimer::setAlarmSendAdmissionHookForTest(
    AlarmSendAdmissionHook hook)
{
    __atomic_store_n(&m_AlarmSendAdmissionHook, hook, __ATOMIC_RELEASE);
}

bool HostedTimer::alarmLockHeldForTest()
{
    return m_Instance.m_AlarmLock.acquired();
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

bool HostedTimer::initialise1()
{
    assert(!m_bInitialized && !m_bPrepared);

    synchronise();

    m_HandlerRegistry.reset();

    struct sigevent sv;
    ByteSet(&sv, 0, sizeof(sv));
    const uintptr_t executionThreadId = Processor::hostedExecutionThreadId();
    if (!executionThreadId)
    {
        return false;
    }
    sv.sigev_notify = SIGEV_THREAD_ID;
    sv.sigev_signo = SIGUSR1;
    sv.sigev_value.sival_ptr = this;
    sv._sigev_un._tid =
        static_cast<decltype(sv._sigev_un._tid)>(executionThreadId);
    int r = timer_create(CLOCK_MONOTONIC, &sv, &m_Timer);
    if (r != 0)
    {
        /// \todo error message or something
        return false;
    }

    m_bPrepared = true;
    return true;
}

bool HostedTimer::initialise3()
{
    if (!m_bPrepared || m_bInitialized)
    {
        return false;
    }

    m_PendingExpirations.reset();
    if (!initialiseSplitIrq())
    {
        timer_delete(m_Timer);
        m_bPrepared = false;
        return false;
    }

    IrqManager &irqManager = *Machine::instance().getIrqManager();
    m_IrqId =
        registerIsaSplitIrq(irqManager, 0, IrqPolicy::syntheticHard());
    if (!m_IrqId)
    {
        if (!shutdownSplitIrq())
        {
            FATAL(
                "HostedTimer could not stop its unregistered bottom-half "
                "worker");
        }
        timer_delete(m_Timer);
        m_bPrepared = false;
        return false;
    }

    struct itimerspec interval;
    ByteSet(&interval, 0, sizeof(interval));
    interval.it_interval.tv_nsec = INTERVAL;
    interval.it_value.tv_nsec = INTERVAL;
    const int r = timer_settime(m_Timer, 0, &interval, 0);
    if (r != 0)
    {
        if (!shutdownSplitIrq())
        {
            FATAL("HostedTimer could not stop after timer arming failed");
        }
        m_IrqId = 0;
        timer_delete(m_Timer);
        m_bPrepared = false;
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
    assert(t != NULL);

    m_Nanosecond = tv.tv_nsec;
    m_Second = t->tm_sec;
    m_Minute = t->tm_min;
    m_Hour = t->tm_hour;
    m_DayOfMonth = t->tm_mday;
    m_Month = t->tm_mon + 1;
    m_Year = t->tm_year + 1900;  // Years since 1900.
    m_DayOfWeek = t->tm_wday;
}

void HostedTimer::uninitialise()
{
    if (!m_bPrepared)
    {
        return;
    }

    if (m_bInitialized)
    {
        if (!shutdownSplitIrq())
        {
            FATAL("HostedTimer teardown could not drain its split IRQ worker");
        }
        m_IrqId = 0;
        m_bInitialized = false;
    }

    timer_delete(m_Timer);
    m_bPrepared = false;

    synchronise();

    {
        LockGuard<Spinlock> guard(m_AlarmLock);
        for (List<Alarm *>::Iterator it = m_Alarms.begin();
             it != m_Alarms.end(); ++it)
        {
            delete *it;
        }
        m_Alarms.clear();
    }

    m_HandlerRegistry.reset();
}

HostedTimer::HostedTimer()
    : SplitIrqHandler(MakeConstantString("Hosted timer bottom half")),
      m_Year(0), m_Month(0), m_DayOfMonth(0), m_DayOfWeek(0), m_Hour(0),
      m_Minute(0), m_Second(0), m_Nanosecond(0), m_Timer(), m_IrqId(0),
      m_PendingExpirations(), m_HandlerRegistry(), m_Alarms(),
      m_AlarmLock(false)
{
}

SplitIrqHandler::HardStageDisposition
HostedTimer::hardIrq(irq_id_t number, InterruptState &state, size_t &work)
{
    const siginfo_t *signalInfo =
        reinterpret_cast<const siginfo_t *>(state.getRegister(1));
    if (
        number != 0 || state.getInterruptNumber() != SIGUSR1 || !signalInfo ||
        signalInfo->si_signo != SIGUSR1 || signalInfo->si_code != SI_TIMER ||
        signalInfo->si_value.sival_ptr != this)
    {
        return HardStageDisposition::NotHandled;
    }

    if (signalInfo->si_overrun < 0)
    {
        FATAL_NOLOCK("HostedTimer received invalid POSIX timer metadata");
        return HardStageDisposition::Handled;
    }

    const size_t expirations = static_cast<size_t>(signalInfo->si_overrun) + 1;
    if (!m_PendingExpirations.recordFromInterrupt(expirations))
    {
        FATAL_NOLOCK("HostedTimer expiration counter saturated");
        return HardStageDisposition::Handled;
    }

    work = 1;
    return HardStageDisposition::Deferred;
}

void HostedTimer::threadedIrq(size_t work)
{
    if (!work)
    {
        return;
    }

    const size_t expirations = m_PendingExpirations.takeAll();
    if (!expirations)
    {
        return;
    }

    constexpr uint64_t MaximumDelta = ~static_cast<uint64_t>(0);
    if (expirations > (MaximumDelta / INTERVAL))
    {
        FATAL("HostedTimer elapsed-time batch overflowed");
        return;
    }
    const uint64_t delta = static_cast<uint64_t>(expirations) * INTERVAL;
    processTimerBatch(delta);
}

bool HostedTimer::quiesceIrqSources()
{
    struct itimerspec disarmed;
    ByteSet(&disarmed, 0, sizeof(disarmed));
    return timer_settime(m_Timer, 0, &disarmed, nullptr) == 0;
}

void HostedTimer::rearmIrqSources(size_t work)
{
    // The POSIX periodic timer remains armed after each delivered signal.
    (void) work;
}

void HostedTimer::processTimerBatch(uint64_t delta)
{
    const bool elapsedSecond =
        delta >= Time::Multiplier::Second ||
        m_Nanosecond >= (Time::Multiplier::Second - delta);
    if (elapsedSecond)
    {
        synchronise();
    }
    else
    {
        m_Nanosecond += delta;
    }

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
                // Cancellation must observe either a queued alarm it can
                // remove or an Event whose send admission has completed.
                // Thread::sendEvent only publishes a ready target; it does not
                // run that target synchronously, so this lock can cover the
                // complete ownership handoff without a self-drain path.
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
                AlarmSendAdmissionHook hook = __atomic_load_n(
                    &m_AlarmSendAdmissionHook, __ATOMIC_ACQUIRE);
                if (hook)
                {
                    hook(pA->m_pEvent);
                }
#endif
                pA->m_pThread->sendEvent(pA->m_pEvent);
                m_Alarms.erase(it);
                delete pA;
                bDispatched = true;
                break;
            }
        }
        if (!bDispatched)
            break;
    }
    m_AlarmLock.release();

    // Each batch represents at least one millisecond. Apply any interrupt
    // mitigation policy once after all elapsed time has been captured.
    Machine::instance().getIrqManager()->tick();

    if (UNLIKELY(elapsedSecond))
    {
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
    }

    // Timer delta is in nanoseconds.
    m_HandlerRegistry.dispatch(delta);
}
