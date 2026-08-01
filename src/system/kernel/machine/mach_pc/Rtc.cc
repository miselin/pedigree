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
#define BCD_TO_BIN8(x) (((((x) &0xF0) >> 4) * 10) + ((x) &0x0F))
#define BIN_TO_BCD8(x) ((((x) / 10) * 16) + ((x) % 10))

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

static uint64_t addAlarmDuration(
    uint64_t deadline, size_t count, uint64_t multiplier)
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
        m_TickCount, alarmSecs, Time::Multiplier::Second);
    target = addAlarmDuration(
        target, alarmUsecs, Time::Multiplier::Microsecond);
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
        const bool dispatching =
            m_AlarmQueue.hasRemoteInFlight(pEvent, owner);
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
    const uint64_t currTime = m_TickCount;
    RtcAlarmQueue::Removal removal =
        m_AlarmQueue.removeFirst(pEvent, owner);
    if (removal.record)
    {
        m_AlarmQueue.recycleList(removal.record);
    }
    m_Lock.release();

    if (
        removal.disposition ==
        RtcAlarmQueue::RemovalDisposition::RemoteInFlight)
    {
        drainRemoteAlarmDispatch(pEvent, owner);
        return 0;
    }
    if (
        removal.disposition != RtcAlarmQueue::RemovalDisposition::Removed ||
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
    uint32_t edx, eax;
    asm volatile("rdtsc" : "=d"(edx), "=a"(eax)::"memory");

    uint64_t tsc;
    tsc = (static_cast<uint64_t>(edx) << 32UL) | eax;

    // calculate # ns since startup
    uint64_t ns = (tsc - m_Tsc0) / m_TscTicksPerNanosecond;

    return ns;
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
    m_bBCD = (read(0x0B) & 0x04) != 0x04;

    // Read the time and date
    if (m_bBCD == true)
    {
        m_Second = BCD_TO_BIN8(read(0x00));
        m_Minute = BCD_TO_BIN8(read(0x02));
        m_Hour = BCD_TO_BIN8(read(0x04));
        m_DayOfMonth = BCD_TO_BIN8(read(0x07));
        m_Month = BCD_TO_BIN8(read(0x08));
        m_Year = BCD_TO_BIN8(read(0x32)) * 100 + BCD_TO_BIN8(read(0x09));
    }
    else
    {
        m_Second = read(0x00);
        m_Minute = read(0x02);
        m_Hour = read(0x04);
        m_DayOfMonth = read(0x07);
        m_Month = read(0x08);
        m_Year = read(0x32) * 100 + read(0x09);
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

    // Register the irq
    IrqManager &irqManager = *Machine::instance().getIrqManager();
    m_IrqId = irqManager.registerIsaIrqHandler(8, this);
    if (m_IrqId == 0)
        return false;

    // Activate the IRQ
    uint8_t statusb = read(0x0B);
    write(0x0B, statusb | 0x40);
    read(0x0C);  // Some RTC chips need the interrupt status to be cleared after
                 // changing the control register.

    bool wasInterrupts = Processor::getInterrupts();
    Processor::setInterrupts(true);

    // Calibrate against TSC (assumes constant TSC - need to check CPUID!)
    uint64_t tsc0, tsc1;

    uint32_t edx, eax;
    asm volatile("rdtsc" : "=d"(edx), "=a"(eax)::"memory");
    tsc0 = (static_cast<uint64_t>(edx) << 32UL) | eax;

    m_TickCount = 0;

    // Burn some cycles.
    for (size_t i = 0; i < 50; ++i)
    {
        Processor::haltUntilInterrupt();
    }

    asm volatile("rdtsc" : "=d"(edx), "=a"(eax)::"memory");
    tsc1 = (static_cast<uint64_t>(edx) << 32UL) | eax;

    uint64_t diff = tsc1 - tsc0;
    m_TscTicksPerNanosecond = diff / m_TickCount;
    NOTICE("TSC ticks/ns: " << m_TscTicksPerNanosecond);

    m_Tsc0 = tsc1;

    Processor::setInterrupts(wasInterrupts);

    return true;
}

void Rtc::synchronise(bool tohw)
{
    enableRtcUpdates(false);

    if (tohw)
    {
        // Write the time and date back
        if (m_bBCD == true)
        {
            write(0x00, BIN_TO_BCD8(m_Second));
            write(0x02, BIN_TO_BCD8(m_Minute));
            write(0x04, BIN_TO_BCD8(m_Hour));
            write(0x07, BIN_TO_BCD8(m_DayOfMonth));
            write(0x08, BIN_TO_BCD8(m_Month));
            write(0x09, BIN_TO_BCD8(m_Year % 100));
            write(0x32, BIN_TO_BCD8(m_Year / 100));
        }
        else
        {
            write(0x00, m_Second);
            write(0x02, m_Minute);
            write(0x04, m_Hour);
            write(0x07, m_DayOfMonth);
            write(0x08, m_Month);
            write(0x09, m_Year % 100);
            write(0x32, m_Year / 100);
        }
    }
    else
    {
        // Read the time and date
        if (m_bBCD == true)
        {
            m_Second = BCD_TO_BIN8(read(0x00));
            m_Minute = BCD_TO_BIN8(read(0x02));
            m_Hour = BCD_TO_BIN8(read(0x04));
            m_DayOfMonth = BCD_TO_BIN8(read(0x07));
            m_Month = BCD_TO_BIN8(read(0x08));
            m_Year = BCD_TO_BIN8(read(0x32)) * 100 + BCD_TO_BIN8(read(0x09));
        }
        else
        {
            m_Second = read(0x00);
            m_Minute = read(0x02);
            m_Hour = read(0x04);
            m_DayOfMonth = read(0x07);
            m_Month = read(0x08);
            m_Year = read(0x32) * 100 + read(0x09);
        }
    }

    enableRtcUpdates(true);
}
void Rtc::uninitialise()
{
    // Deactivate the IRQ
    uint8_t statusb = read(0x0B);
    write(0x0B, statusb & ~0x40);

    synchronise(true);

    // Unregister the irq
    IrqManager &irqManager = *Machine::instance().getIrqManager();
    if (!irqManager.unregisterHandler(m_IrqId, this))
    {
        FATAL(
            "RTC teardown could not synchronously unregister its IRQ "
            "callback");
    }
    m_IrqId = 0;

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
    : m_IoPort("CMOS"), m_IrqId(0), m_PeriodicIrqInfoIndex(0), m_bBCD(true),
      m_Year(1970), m_Month(0), m_DayOfMonth(0), m_Hour(0), m_Minute(0),
      m_Second(0), m_Nanosecond(0), m_TickCount(0), m_HandlerRegistry(),
      m_AlarmQueue(), m_Lock(false)
{
}

extern size_t g_FreePages;
extern size_t g_AllocedPages;

bool Rtc::irq(irq_id_t number, InterruptState &state)
{
    static size_t index = 0;
    // Update the Tick Count
    uint64_t delta = periodicIrqInfo[m_PeriodicIrqInfoIndex].ns[index];
    index = (index == 0) ? 1 : 0;
    uint64_t prevTickCount = m_TickCount;
    m_TickCount += delta;
    if (m_TickCount < prevTickCount)
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
            m_TickCount, currentAlarmDispatchOwner());
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

    // Acknowledging the IRQ (within the CMOS)
    read(0x0C);

    // Timer delta is in nanoseconds.
    m_HandlerRegistry.dispatch(delta, state);

    return true;
}

void Rtc::setIndex(uint8_t index)
{
    uint8_t idx = m_IoPort.read8(0);
    m_IoPort.write8((idx & 0x80) | (index & 0x7F), 0);
}
void Rtc::waitForUpdateCompletion(uint8_t index)
{
    if (index <= 9 || index == 50)
    {
        setIndex(0x0A);
        while ((m_IoPort.read8(1) & 0x80) == 0x80)
            ;
    }
}
void Rtc::enableRtcUpdates(bool enable)
{
    // Write the index
    setIndex(0x0B);

    // Update the status register
    uint8_t statusA = m_IoPort.read8(1);
    m_IoPort.write8((statusA & 0x80) | (enable ? 0 : (1 << 7)), 1);
}
uint8_t Rtc::read(uint8_t index)
{
    // Wait until the RTC Update is completed
    waitForUpdateCompletion(index);

    // Write the index
    setIndex(index);

    // Read the data at that index
    return m_IoPort.read8(1);
}
void Rtc::write(uint8_t index, uint8_t value)
{
    // Wait until the RTC Update is completed
    waitForUpdateCompletion(index);

    // Write the index
    setIndex(index);

    // Write the data to that index
    m_IoPort.write8(value, 1);
}
