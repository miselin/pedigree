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

#ifndef KERNEL_MACHINE_X86_COMMON_RTC_H
#define KERNEL_MACHINE_X86_COMMON_RTC_H

#include "RtcAlarmQueue.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/IrqEventCounter.h"
#include "pedigree/kernel/machine/SplitIrqHandler.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/machine/TimerHandlerRegistry.h"
#include "pedigree/kernel/machine/types.h"
#include "pedigree/kernel/processor/IoPort.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/new"

class TimerHandler;

/** @addtogroup kernelmachinex86common
 * @{ */

/** Class for the Real-time clock / CMOS implementing the Timer interface */
class Rtc : public Timer, private SplitIrqHandler
{
  public:
    inline static Rtc &instance()
    {
        return m_Instance;
    }

    //
    // Timer interface
    //
    virtual bool registerHandler(TimerHandler *handler);
    virtual bool unregisterHandler(TimerHandler *handler);
    virtual void
    addAlarm(class Event *pEvent, size_t alarmSecs, size_t alarmUsecs = 0);
    virtual void removeAlarm(class Event *pEvent);
    virtual size_t removeAlarm(class Event *pEvent, bool bRetZero);
    virtual size_t getYear();
    virtual uint8_t getMonth();
    virtual uint8_t getDayOfMonth();
    virtual uint8_t getDayOfWeek();
    virtual uint8_t getHour();
    virtual uint8_t getMinute();
    virtual uint8_t getSecond();
    virtual uint64_t getNanosecond();
    virtual uint64_t getTickCount();
    virtual uint64_t getTickCountNano();

    /** Initialises the class
     *\return true, if successful, false otherwise */
    bool initialise1() INITIALISATION_ONLY;
    /** Calibrates the TSC while the RTC line remains masked. */
    bool initialise2() INITIALISATION_ONLY;
    /** Starts the RTC bottom half and enables periodic IRQ delivery. */
    bool initialise3();
    /** Synchronise the time/date with the hardware */
    virtual void synchronise(bool tohw = false);
    /** Uninitialises the class */
    void uninitialise();

  protected:
    /** The default constructor */
    Rtc() INITIALISATION_ONLY;
    /** The destructor */
    inline virtual ~Rtc()
    {
    }

  private:
    /** The copy-constructor
     *\note NOT implemented */
    Rtc(const Rtc &);
    /** The assignment operator
     *\note NOT implemented */
    Rtc &operator=(const Rtc &);

    HardIrqDisposition
    hardIrq(irq_id_t number, InterruptState &state, size_t &work) override;
    void threadedIrq(size_t work) override;
    bool quiesceIrqSources() override;
    void rearmIrqSources(size_t work) override;

    /** Applies one periodic event in ordinary thread context. */
    void processPeriodicTick(uint64_t delta);

    /** Atomically changes the RTC periodic-interrupt source and clears C. */
    void setPeriodicInterruptEnabled(bool enabled);

    /** Set the index register
     *\param[in] index the new index */
    void setIndexLocked(uint8_t index);
    /** Wait until the Update of the RTC entries in the CMOS is complete,
     * if we want to access one of those entries
     *\param[in] index the index we want to access afterwards */
    void waitForUpdateCompletion(uint8_t index);
    /** Dis/Enable the RTC updates
     *\param[in] index the index we want to access / have accessed
     *\param[in] enable Do we want to enable or disable? */
    void enableRtcUpdates(bool enable);
    uint8_t readLocked(uint8_t index);
    void writeLocked(uint8_t index, uint8_t value);
    /** Read the value in the CMOS at a specific index
     *\param[in] index the index
     *\return the value at the index */
    uint8_t read(uint8_t index);
    /** Write the value to the CMOS at a specific index
     *\param[in] index the index
     *\param[in] value the value */
    void write(uint8_t index, uint8_t value);

    /** Drains a sendEvent handoff owned by another processor. */
    void drainRemoteAlarmDispatch(class Event *pEvent, void *owner);

    /** The CMOS/Real-time Clock I/O port range */
    IoPort m_IoPort;

    /** The IRQ Identifier */
    irq_id_t m_IrqId;

    /** Index into the periodicIrqInfo table */
    size_t m_PeriodicIrqInfoIndex;

    /** Selects the alternating fractional-nanosecond periodic delta. */
    size_t m_PeriodicPhase;

    /** Hard-stage phase used to advance the authoritative alarm clock. */
    Atomic<size_t> m_CapturePhase;

    /** Delivered periodic IRQ callbacks preserved across worker coalescing. */
    IrqEventCounter m_PendingTicks;

    /** BCD mode? (otherwise in binary mode) */
    bool m_bBCD;

    /** The current year */
    size_t m_Year;
    /** The current month */
    uint8_t m_Month;
    /** The current day of month */
    uint8_t m_DayOfMonth;
    /** The current hour */
    uint8_t m_Hour;
    /** The current minute */
    uint8_t m_Minute;
    /** The current second */
    uint8_t m_Second;
    /** The current nanosecond */
    uint64_t m_Nanosecond;

    /** Current captured time used when callers create or remove alarms. */
    Atomic<uint64_t> m_TickCount;

    /** Bottom-half cursor used to deliver due alarms in event order. */
    uint64_t m_ProcessedTickCount;

    /** Holds information about the RTC periodic irq */
    struct periodicIrqInfo_t
    {
        /** The frequency */
        size_t Hz;
        /** Value that needs to be written to the CMOS register */
        uint8_t rateBits;
        /** Nanoseconds between two ticks */
        uint64_t ns[2];
    };

    /** Information about the RTC's periodic irq */
    static periodicIrqInfo_t periodicIrqInfo[12];

    /** The Rtc class instance */
    static Rtc m_Instance;

    /** Timer handlers and their callback lifetime state. */
    TimerHandlerRegistry m_HandlerRegistry;

    using Alarm = RtcAlarmQueue::Record;

    /** Intrusive alarm ownership; queue operations never allocate in IRQs. */
    RtcAlarmQueue m_AlarmQueue;
    /** Protects the alarm queue and the dispatch-ownership transition. */
    Spinlock m_Lock;

    /** Serialises the CMOS index/data register pair across processors. */
    Spinlock m_CmosLock;

    /** Tracks the number of nanoseconds per TSC tick. */
    uint64_t m_TscTicksPerNanosecond;

    /** Initial TSC value. */
    uint64_t m_Tsc0;
};

/** @} */

#endif
