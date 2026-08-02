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

#include "Rtc.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/core/SlamAllocator.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Serial.h"
#include "pedigree/kernel/machine/TimerHandler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

class Event;

// RTC frequency to set at startup - tradeoff between precision of timers
// against constant RTC noise.
/// \todo HPET, many timers could be described as just one-shots
#ifdef BOCHS
#define INITIAL_RTC_HZ 64
#else
#define INITIAL_RTC_HZ 512
#endif
#define BCD_TO_BIN8(x) (((((x) & 0xF0) >> 4) * 10) + ((x) & 0x0F))
#define BIN_TO_BCD8(x) ((((x) / 10) * 16) + ((x) % 10))

namespace
{
constexpr uint8_t RtcPeriodicInterruptEnable = 1U << 6;
constexpr uint8_t RtcAlarmInterruptEnable = 1U << 5;
constexpr uint8_t RtcUpdateEndedInterruptEnable = 1U << 4;
constexpr uint8_t RtcInterruptEnableMask = RtcPeriodicInterruptEnable |
                                           RtcAlarmInterruptEnable |
                                           RtcUpdateEndedInterruptEnable;
constexpr uint8_t RtcUpdateInhibit = 1U << 7;
constexpr uint8_t RtcInterruptRequested = 1U << 7;
constexpr uint8_t RtcPeriodicFlag = 1U << 6;
constexpr size_t RtcPeriodicWork = 1;
// MC146818A UIP can remain asserted for up to 2.228 ms. This margin also
// accommodates compatible devices, emulators, and the uncalibrated TSC used
// during initialise1().
constexpr Time::Timestamp RtcUpdateTimeout =
    25 * Time::Multiplier::Millisecond;
constexpr size_t RtcUpdateMaximumPolls = 1000000;
constexpr size_t RtcCalibrationMaximumPolls = 100000000;

uint64_t readOrderedTsc()
{
    uint32_t edx = 0;
    uint32_t eax = 0;
    asm volatile("lfence\nrdtsc" : "=d"(edx), "=a"(eax) : : "memory");
    return (static_cast<uint64_t>(edx) << 32U) | eax;
}
}  // namespace

Rtc::periodicIrqInfo_t Rtc::periodicIrqInfo[12] = {
    {4, 0x0e, {250000000ULL, 250000000ULL}},
    {8, 0x0d, {125000000ULL, 125000000ULL}},
    {16, 0x0c, {62500000ULL, 62500000ULL}},
    {32, 0x0b, {31250000ULL, 31250000ULL}},
    {64, 0x0a, {15625000ULL, 15625000ULL}},
    {128, 0x09, {7812500ULL, 7812500ULL}},
    {256, 0x08, {3906250ULL, 3906250ULL}},
    {512, 0x07, {1953125ULL, 1953125ULL}},
    {1024, 0x06, {976562ULL, 976563ULL}},
    {2048, 0x05, {488281ULL, 488281ULL}},
    {4096, 0x04, {244140ULL, 244141ULL}},
    {8192, 0x03, {122070ULL, 122070ULL}},
};

static uint8_t daysPerMonth[] = {31, 28, 31, 30, 31, 30,
                                 31, 31, 30, 31, 30, 31};

static uint64_t
addAlarmDuration(uint64_t deadline, size_t count, uint64_t multiplier)
{
    if (count > ((Time::Infinity - deadline) / multiplier))
    {
        return Time::Infinity;
    }
    return deadline + (count * multiplier);
}

Rtc Rtc::m_Instance;

static void *currentAlarmDispatchOwner()
{
    return static_cast<void *>(&Processor::information());
}

void Rtc::addAlarm(Event *pEvent, size_t alarmSecs, size_t alarmUsecs)
{
    Alarm *pAlarm = nullptr;
    m_Lock.acquire();
    pAlarm = m_AlarmQueue.takeReusable();
    m_Lock.release();

    if (!pAlarm)
    {
        pAlarm = new Alarm;
    }

    LockGuard<Spinlock> guard(m_Lock);

    // Figure out when to trigger the alarm.
    uint64_t target = addAlarmDuration(
        m_TickCount.value(), alarmSecs, Time::Multiplier::Second);
    target =
        addAlarmDuration(target, alarmUsecs, Time::Multiplier::Microsecond);
    pAlarm->prepare(
        pEvent, target, Processor::information().getCurrentThread());
    m_AlarmQueue.add(pAlarm);
}

void Rtc::drainRemoteAlarmDispatch(Event *pEvent, void *owner)
{
    Thread *current = Processor::information().getCurrentThread();
    const bool canYield = current && Processor::getInterrupts();
    TerminationDeferral terminationDeferral(canYield);
    uintptr_t previousDebugAddress = 0;
    Thread::DebugState previousDebugState = Thread::None;
    bool debuggingDrain = false;

    while (true)
    {
        m_Lock.acquire();
        const bool dispatching = m_AlarmQueue.hasRemoteInFlight(pEvent, owner);
        m_Lock.release();

        if (!dispatching)
        {
            break;
        }

        if (canYield)
        {
            if (!debuggingDrain)
            {
                previousDebugState =
                    current->getDebugState(previousDebugAddress);
                current->setDebugState(
                    Thread::CallbackDrain, reinterpret_cast<uintptr_t>(pEvent));
                debuggingDrain = true;
            }
            Scheduler::instance().yield();
        }
        else
        {
            Processor::pause();
        }
    }

    if (debuggingDrain)
    {
        current->setDebugState(previousDebugState, previousDebugAddress);
    }
}

void Rtc::removeAlarm(Event *pEvent)
{
    void *owner = currentAlarmDispatchOwner();
    bool remoteInFlight = false;
    bool selfDeferred = false;

    m_Lock.acquire();
    Alarm *reclaim = m_AlarmQueue.removeAllQueued(
        pEvent, owner, remoteInFlight, selfDeferred);
    m_AlarmQueue.recycleList(reclaim);
    m_Lock.release();

    (void) selfDeferred;
    if (remoteInFlight)
    {
        drainRemoteAlarmDispatch(pEvent, owner);
    }
}

size_t Rtc::removeAlarm(class Event *pEvent, bool bRetZero)
{
    void *owner = currentAlarmDispatchOwner();

    m_Lock.acquire();
    const uint64_t currTime = m_TickCount.value();
    RtcAlarmQueue::Removal removal = m_AlarmQueue.removeFirst(pEvent, owner);
    if (removal.record)
    {
        m_AlarmQueue.recycleList(removal.record);
    }
    m_Lock.release();

    if (removal.disposition ==
        RtcAlarmQueue::RemovalDisposition::RemoteInFlight)
    {
        drainRemoteAlarmDispatch(pEvent, owner);
        return 0;
    }
    if (removal.disposition != RtcAlarmQueue::RemovalDisposition::Removed ||
        bRetZero || removal.deadline < currTime)
    {
        return 0;
    }

    const uint64_t diff = removal.deadline - currTime;
    size_t ret = diff / Time::Multiplier::Second;
    if (diff % Time::Multiplier::Second)
    {
        ++ret;
    }
    return ret;
}

bool Rtc::registerHandler(TimerHandler *handler)
{
    return m_HandlerRegistry.registerHandler(handler);
}

bool Rtc::unregisterHandler(TimerHandler *handler)
{
    return m_HandlerRegistry.unregisterHandler(handler);
}
size_t Rtc::getYear()
{
    return m_Year;
}
uint8_t Rtc::getMonth()
{
    return m_Month;
}
uint8_t Rtc::getDayOfMonth()
{
    return m_DayOfMonth;
}
uint8_t Rtc::getDayOfWeek()
{
    static size_t monthnumbers[] = {0, 3, 3, 6, 1, 4, 6, 2, 5, 0, 3, 5};

    // Calculate day of week
    uint8_t dayOfWeek = m_DayOfMonth % 7;
    dayOfWeek += monthnumbers[m_Month - 1];
    dayOfWeek += ((m_Year % 100) + ((m_Year % 100) / 4)) % 7;
    dayOfWeek -= ((m_Year / 100) % 4 - 3) * 2;
    if (m_Month < 3)
        dayOfWeek--;
    dayOfWeek %= 7;

    return dayOfWeek;
}
uint8_t Rtc::getHour()
{
    return m_Hour;
}
uint8_t Rtc::getMinute()
{
    return m_Minute;
}
uint8_t Rtc::getSecond()
{
    return m_Second;
}
uint64_t Rtc::getNanosecond()
{
    return m_Nanosecond;
}
uint64_t Rtc::getTickCount()
{
    return getTickCountNano() / Time::Multiplier::Millisecond;
}
uint64_t Rtc::getTickCountNano()
{
    // Migration between the TSC sample and the per-CPU anchor would combine
    // unrelated clock domains. Keep only that bounded snapshot non-preemptible;
    // conversion and global publication do not depend on the current CPU.
    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    uint64_t anchorTsc = m_Tsc0;
    uint64_t anchorNanoseconds = 0;
    Processor::information().getTscClockAnchor(
        anchorTsc, anchorNanoseconds);
    const uint64_t tsc = readOrderedTsc();
    Processor::setInterrupts(interruptsWereEnabled);

    uint64_t candidate = PcTscClock::fromAnchor(
        tsc, anchorTsc, anchorNanoseconds, m_TscCalibration);
    const uint64_t coarseFloor = m_TickCount.value();
    if (candidate < coarseFloor)
    {
        candidate = coarseFloor;
    }

    return m_MonotonicTicks.publish(candidate);
}
bool Rtc::initialise1()
{
    NOTICE("Rtc::initialise1");

    // Allocate the I/O port range"CMOS"
    if (m_IoPort.allocate(0x70, 2) == false)
        return false;

    // No IRQ yet.
    m_IrqId = 0;

    // Initialise handlers.
    m_HandlerRegistry.reset();

    // Are the RTC values in the CMOS encoded in BCD (or binary)?
    uint8_t statusB = 0;
    if (!read(0x0B, statusB))
    {
        return false;
    }
    m_bBCD = (statusB & 0x04) != 0x04;

    if (!readHardwareClock())
    {
        ERROR("RTC: timed out reading the initial hardware clock");
        return false;
    }

    // Find the initial rtc rate
    uint8_t rateBits = 0x06;
    for (size_t i = 0; i < 12; i++)
        if (periodicIrqInfo[i].Hz == INITIAL_RTC_HZ)
        {
            m_PeriodicIrqInfoIndex = i;
            rateBits = periodicIrqInfo[i].rateBits;
            break;
        }

    // Set the Rate for the periodic IRQ
    uint8_t tmp = read(0x0A);
    write(0x0A, (tmp & 0xF0) | rateBits);

    return true;
}

bool Rtc::initialise2()
{
    NOTICE("Rtc::initialise2");

    // Calibrate against TSC without making pre-scheduler IRQ callbacks do
    // runtime policy work. IRQ8 is still masked because it has no handler;
    // register C remains pollable and clears each periodic occurrence.
    constexpr size_t CalibrationPeriods = 50;
    setPeriodicInterruptEnabled(true);
    size_t calibrationPolls = 0;
    while (!(read(0x0C) & RtcPeriodicFlag))
    {
        if (++calibrationPolls >= RtcCalibrationMaximumPolls)
        {
            setPeriodicInterruptEnabled(false);
            ERROR("RTC: timed out waiting for TSC calibration to start");
            return false;
        }
        Processor::pause();
    }

    const uint64_t tsc0 = readOrderedTsc();

    size_t periods = 0;
    while (periods < CalibrationPeriods)
    {
        if (read(0x0C) & RtcPeriodicFlag)
        {
            ++periods;
        }
        if (++calibrationPolls >= RtcCalibrationMaximumPolls)
        {
            setPeriodicInterruptEnabled(false);
            ERROR(
                "RTC: timed out collecting periodic TSC calibration samples");
            return false;
        }
        Processor::pause();
    }

    const uint64_t tsc1 = readOrderedTsc();
    setPeriodicInterruptEnabled(false);

    uint64_t elapsedNanoseconds = 0;
    for (size_t i = 0; i < CalibrationPeriods; ++i)
    {
        elapsedNanoseconds += periodicIrqInfo[m_PeriodicIrqInfoIndex].ns[i & 1];
    }
    if (tsc1 <= tsc0 || !elapsedNanoseconds)
    {
        ERROR("RTC: invalid TSC calibration interval");
        return false;
    }
    const uint64_t elapsedCycles = tsc1 - tsc0;
    m_TscCalibration =
        PcTscClock::Calibration(elapsedCycles, elapsedNanoseconds);
    NOTICE(
        "TSC calibration: " << elapsedCycles << " cycles / "
                            << elapsedNanoseconds << " ns");

    m_TickCount = 0;
    m_ProcessedTickCount = 0;
    m_Tsc0 = tsc1;
    m_MonotonicTicks.reset();
    Processor::information().initialiseTscClockAnchor(tsc1, 0);
    m_PeriodicPhase = 0;
    m_CapturePhase = 0;
    m_PendingTicks.reset();

    return true;
}

void Rtc::initialiseProcessorClock()
{
    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    ProcessorInformation &processor = Processor::information();
    const uint64_t tsc = readOrderedTsc();
    uint64_t nanoseconds = m_MonotonicTicks.value();
    const uint64_t coarseFloor = m_TickCount.value();
    if (nanoseconds < coarseFloor)
    {
        nanoseconds = coarseFloor;
    }
    processor.initialiseTscClockAnchor(tsc, nanoseconds);
    Processor::setInterrupts(interruptsWereEnabled);
}

bool Rtc::initialise3()
{
    if (!initialiseSplitIrq())
    {
        return false;
    }

    IrqManager &irqManager = *Machine::instance().getIrqManager();
    m_IrqId =
        registerIsaSplitIrq(irqManager, 8, IrqPolicy::levelHard());
    if (!m_IrqId)
    {
        if (!shutdownSplitIrq())
        {
            FATAL("RTC could not stop its unregistered bottom-half worker");
        }
        return false;
    }

    setPeriodicInterruptEnabled(true);
    return true;
}

void Rtc::synchronise(bool tohw)
{
    enableRtcUpdates(false);
    const bool success = tohw ? writeHardwareClock() : readHardwareClock();
    enableRtcUpdates(true);

    if (!success)
    {
        if (tohw)
        {
            ERROR("RTC: timed out writing the hardware clock");
        }
        else
        {
            ERROR("RTC: timed out reading the hardware clock");
        }
    }
}
void Rtc::uninitialise()
{
    if (!shutdownSplitIrq())
    {
        FATAL("RTC teardown could not drain its split IRQ worker");
    }
    m_IrqId = 0;

    synchronise(true);

    m_HandlerRegistry.reset();

    Alarm *reclaim = nullptr;
    Alarm *freeAlarms = nullptr;
    {
        LockGuard<Spinlock> guard(m_Lock);
        reclaim = m_AlarmQueue.detachActive();
        freeAlarms = m_AlarmQueue.detachFree();
    }
    while (reclaim)
    {
        Alarm *next = reclaim->next();
        delete reclaim;
        reclaim = next;
    }
    while (freeAlarms)
    {
        Alarm *next = freeAlarms->next();
        delete freeAlarms;
        freeAlarms = next;
    }

    // Free the I/O port range
    m_IoPort.free();
}

Rtc::Rtc()
    : SplitIrqHandler(MakeConstantString("RTC bottom half")), m_IoPort("CMOS"),
      m_IrqId(0), m_PeriodicIrqInfoIndex(0), m_PeriodicPhase(0),
      m_CapturePhase(0), m_PendingTicks(), m_bBCD(true), m_Year(1970),
      m_Month(0), m_DayOfMonth(0), m_Hour(0), m_Minute(0), m_Second(0),
      m_Nanosecond(0), m_TickCount(0), m_ProcessedTickCount(0),
      m_HandlerRegistry(), m_AlarmQueue(), m_Lock(false), m_CmosLock(false),
      m_TscCalibration(), m_Tsc0(0), m_MonotonicTicks()
{
}

extern size_t g_FreePages;
extern size_t g_AllocedPages;

SplitIrqHandler::HardIrqDisposition
Rtc::hardIrq(irq_id_t number, InterruptState &state, size_t &work)
{
    (void) number;
    (void) state;

    const uint8_t status = acknowledgeInterruptFromHardIrq();
    if (!(status & RtcInterruptRequested))
    {
        return HardIrqDisposition::Handled;
    }
    if (!(status & RtcPeriodicFlag))
    {
        return HardIrqDisposition::Handled;
    }

    // IRQ8 is serialized until EOI. Use one bounded RMW for each captured
    // value so a bottom-half reader cannot make the hard top spin.
    const size_t phase = (m_CapturePhase ^= 1) ^ 1;
    const uint64_t delta = periodicIrqInfo[m_PeriodicIrqInfoIndex].ns[phase];
    constexpr uint64_t MaximumTick = ~static_cast<uint64_t>(0);
    const uint64_t tickCount = m_TickCount.value();
    if (tickCount > (MaximumTick - delta))
    {
        FATAL_NOLOCK("RTC monotonic tick counter overflowed");
        return HardIrqDisposition::Handled;
    }
    m_TickCount += delta;

    if (!m_PendingTicks.recordFromInterrupt())
    {
        FATAL_NOLOCK("RTC periodic-event counter saturated");
        return HardIrqDisposition::Handled;
    }
    work = RtcPeriodicWork;
    return HardIrqDisposition::Deferred;
}

void Rtc::threadedIrq(size_t work)
{
    if (!(work & RtcPeriodicWork))
    {
        return;
    }

    size_t ticks = m_PendingTicks.takeAll();
    while (ticks--)
    {
        const uint64_t delta =
            periodicIrqInfo[m_PeriodicIrqInfoIndex].ns[m_PeriodicPhase];
        m_PeriodicPhase ^= 1;
        processPeriodicTick(delta);
    }
}

bool Rtc::quiesceIrqSources()
{
    setPeriodicInterruptEnabled(false);
    return true;
}

void Rtc::rearmIrqSources(size_t work)
{
    // Reading register C in hardIrq() already rearmed the RTC source.
    (void) work;
}

void Rtc::processPeriodicTick(uint64_t delta)
{
    const uint64_t prevTickCount = m_ProcessedTickCount;
    m_ProcessedTickCount += delta;
    if (m_ProcessedTickCount < prevTickCount)
    {
        WARNING("RTC: rolled over.");
        /// \todo figure out how best to handle this
    }

    // Calculate the new time/date
    m_Nanosecond += delta;

    // Claim one due alarm under the queue lock, then publish it without
    // carrying that lock into Event or allocator code. A remover which sees
    // m_bDispatching waits for this ownership handoff to finish before it
    // returns and permits the Event to be reclaimed.
    while (true)
    {
        m_Lock.acquire();
        Alarm *claimed = m_AlarmQueue.claimDue(
            m_ProcessedTickCount, currentAlarmDispatchOwner());
        m_Lock.release();

        if (!claimed)
        {
            break;
        }

        Thread *target = reinterpret_cast<Thread *>(claimed->target());
        Event *event = reinterpret_cast<Event *>(claimed->event());
        target->sendEvent(event);

        m_Lock.acquire();
        m_AlarmQueue.completeDispatch(claimed);
        m_Lock.release();
    }

    if (UNLIKELY(m_Nanosecond >= Time::Multiplier::Millisecond))
    {
        // Every millisecond, unblock any interrupts which were halted and halt
        // any which need to be halted.
        Machine::instance().getIrqManager()->tick();
    }

    if (UNLIKELY(m_Nanosecond >= Time::Multiplier::Second))
    {
        ++m_Second;
        m_Nanosecond -= Time::Multiplier::Second;

#if MEMORY_LOGGING_ENABLED
        Serial *pSerial = Machine::instance().getSerial(1);
        NormalStaticString memoryLogStr;
        memoryLogStr += "Heap: ";
        memoryLogStr += SlamAllocator::instance().heapPageCount() * 4;
        memoryLogStr += "K\tPages: ";
        memoryLogStr += (g_AllocedPages * 4096) / 1024;
        memoryLogStr += "K\t Free: ";
        memoryLogStr += (g_FreePages * 4096) / 1024;
        memoryLogStr += "K\n";

        pSerial->write_str(memoryLogStr);

        // Memory snapshot of current processes.
        for (size_t i = 0; i < Scheduler::instance().getNumProcesses(); ++i)
        {
            Scheduler::ProcessLease processLease;
            if (!Scheduler::instance().acquireProcess(processLease, i))
            {
                continue;
            }
            Process *pProcess = processLease.get();
            LargeStaticString processListStr;

            ssize_t heapK = pProcess->getHeapUsage() / 1024;
            ssize_t virtK = (pProcess->getVirtualPageCount() * 0x1000) / 1024;
            ssize_t physK = (pProcess->getPhysicalPageCount() * 0x1000) / 1024;
            ssize_t shrK = (pProcess->getSharedPageCount() * 0x1000) / 1024;

            processListStr.append("\tProcess #");
            processListStr.append(pProcess->getId(), 10);
            processListStr.append(" '");
            processListStr.append(pProcess->description());
            processListStr.append("' V=");
            processListStr.append(virtK, 10);
            processListStr.append("K P=");
            processListStr.append(physK, 10);
            processListStr.append("K S=");
            processListStr.append(shrK, 10);
            processListStr.append("K Heap=");
            processListStr.append(heapK, 10);
            processListStr.append("K\n");
            pSerial->write_str(processListStr);
        }
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
    m_HandlerRegistry.dispatch(delta);
}

void Rtc::setIndexLocked(uint8_t index)
{
    uint8_t idx = m_IoPort.read8(0);
    m_IoPort.write8((idx & 0x80) | (index & 0x7F), 0);
}

uint8_t Rtc::acknowledgeInterruptFromHardIrq()
{
    assert(!Processor::getInterrupts());

    // Register C is the device acknowledgement. Unlike an ordinary CMOS
    // read, this path must perform exactly one index/data transaction and
    // cannot wait for an update cycle to complete.
    LockGuard<Spinlock> guard(m_CmosLock);
    return readLocked(0x0C);
}

bool Rtc::waitForUpdateCompletion(uint8_t index)
{
    if (index > 9 && index != 0x32)
    {
        return true;
    }

    const Time::Timestamp started = Time::getTicks();
    size_t polls = 0;
    while (true)
    {
        bool updating = false;
        {
            LockGuard<Spinlock> guard(m_CmosLock);
            setIndexLocked(0x0A);
            updating = (m_IoPort.read8(1) & 0x80) != 0;
        }
        if (!updating)
        {
            return true;
        }
        if (++polls >= RtcUpdateMaximumPolls ||
            (Time::getTicks() - started) >= RtcUpdateTimeout)
        {
            ERROR(
                "RTC: timed out waiting for update completion before CMOS "
                "index "
                << index);
            return false;
        }
        Processor::pause();
    }
}
void Rtc::enableRtcUpdates(bool enable)
{
    LockGuard<Spinlock> guard(m_CmosLock);

    // Write the index
    setIndexLocked(0x0B);

    // Update the status register
    uint8_t statusB = m_IoPort.read8(1);
    statusB = enable ? static_cast<uint8_t>(statusB & ~RtcUpdateInhibit) :
                       static_cast<uint8_t>(statusB | RtcUpdateInhibit);
    m_IoPort.write8(statusB, 1);
}

uint8_t Rtc::readCmos(uint8_t index)
{
    return m_Instance.read(index);
}

uint8_t Rtc::read(uint8_t index)
{
    uint8_t value = 0xFF;
    (void) read(index, value);
    return value;
}

bool Rtc::read(uint8_t index, uint8_t &value)
{
    if (!waitForUpdateCompletion(index))
    {
        return false;
    }
    LockGuard<Spinlock> guard(m_CmosLock);
    value = readLocked(index);
    return true;
}

uint8_t Rtc::readLocked(uint8_t index)
{
    setIndexLocked(index);
    return m_IoPort.read8(1);
}

bool Rtc::write(uint8_t index, uint8_t value)
{
    if (!waitForUpdateCompletion(index))
    {
        return false;
    }

    LockGuard<Spinlock> guard(m_CmosLock);
    writeLocked(index, value);
    return true;
}

void Rtc::writeCmos(uint8_t index, uint8_t value)
{
    (void) m_Instance.write(index, value);
}

bool Rtc::readHardwareClock()
{
    uint8_t second = 0;
    uint8_t minute = 0;
    uint8_t hour = 0;
    uint8_t dayOfMonth = 0;
    uint8_t month = 0;
    uint8_t year = 0;
    uint8_t century = 0;
    if (!read(0x00, second) || !read(0x02, minute) ||
        !read(0x04, hour) || !read(0x07, dayOfMonth) ||
        !read(0x08, month) || !read(0x09, year) ||
        !read(0x32, century))
    {
        return false;
    }

    if (m_bBCD)
    {
        second = BCD_TO_BIN8(second);
        minute = BCD_TO_BIN8(minute);
        hour = BCD_TO_BIN8(hour);
        dayOfMonth = BCD_TO_BIN8(dayOfMonth);
        month = BCD_TO_BIN8(month);
        year = BCD_TO_BIN8(year);
        century = BCD_TO_BIN8(century);
    }

    m_Second = second;
    m_Minute = minute;
    m_Hour = hour;
    m_DayOfMonth = dayOfMonth;
    m_Month = month;
    m_Year = (century * 100) + year;
    return true;
}

bool Rtc::writeHardwareClock()
{
    const uint8_t second =
        m_bBCD ? BIN_TO_BCD8(m_Second) : m_Second;
    const uint8_t minute =
        m_bBCD ? BIN_TO_BCD8(m_Minute) : m_Minute;
    const uint8_t hour = m_bBCD ? BIN_TO_BCD8(m_Hour) : m_Hour;
    const uint8_t dayOfMonth =
        m_bBCD ? BIN_TO_BCD8(m_DayOfMonth) : m_DayOfMonth;
    const uint8_t month = m_bBCD ? BIN_TO_BCD8(m_Month) : m_Month;
    const uint8_t year =
        m_bBCD ? BIN_TO_BCD8(m_Year % 100) : m_Year % 100;
    const uint8_t century =
        m_bBCD ? BIN_TO_BCD8(m_Year / 100) : m_Year / 100;

    return write(0x00, second) && write(0x02, minute) &&
           write(0x04, hour) && write(0x07, dayOfMonth) &&
           write(0x08, month) && write(0x09, year) &&
           write(0x32, century);
}

void Rtc::writeLocked(uint8_t index, uint8_t value)
{
    setIndexLocked(index);
    m_IoPort.write8(value, 1);
}

void Rtc::setPeriodicInterruptEnabled(bool enabled)
{
    LockGuard<Spinlock> guard(m_CmosLock);
    const uint8_t status = readLocked(0x0B);
    const uint8_t quiescentStatus =
        static_cast<uint8_t>(status & ~RtcInterruptEnableMask);
    writeLocked(
        0x0B, enabled ? static_cast<uint8_t>(
                            quiescentStatus | RtcPeriodicInterruptEnable) :
                        quiescentStatus);

    // Some RTCs require register C to be cleared after control changes.
    (void) readLocked(0x0C);
}
