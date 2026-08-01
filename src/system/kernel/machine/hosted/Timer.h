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

#ifndef KERNEL_MACHINE_HOSTED_COMMON_TIMER_H
#define KERNEL_MACHINE_HOSTED_COMMON_TIMER_H

#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/SchedulerTimer.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/machine/TimerHandlerRegistry.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/utilities/List.h"

namespace __pedigree_hosted
{
#include <signal.h>
#include <time.h>
}  // namespace __pedigree_hosted

/** @addtogroup kernelmachinehosted
 * @{ */

class HostedTimer : public Timer, private IrqHandler
{
  public:
    inline static HostedTimer &instance()
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
     *\return true, if successfull, false otherwise */
    bool initialise() INITIALISATION_ONLY;
    /** Synchronise the time/date with the hardware */
    virtual void synchronise(bool tohw = false);
    /** Uninitialises the class */
    void uninitialise();

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    using HandlerPinHook = TimerHandlerRegistry::HandlerPinHook;
    using HandlerPrePinHook = TimerHandlerRegistry::HandlerPrePinHook;
    using HandlerHazardClaimHook = TimerHandlerRegistry::HandlerHazardClaimHook;
    using HandlerAtomicDrainHook =
        TimerHandlerRegistry::HandlerAtomicDrainHook;
    using HandlerMutationLockHook = TimerHandlerRegistry::MutationLockHook;

    /** Installs a deterministic observer after a handler has been pinned. */
    static EXPORTED_PUBLIC void setHandlerPinHook(HandlerPinHook hook);
    /** Installs a deterministic observer before a handler pin commits. */
    static EXPORTED_PUBLIC void setHandlerPrePinHook(HandlerPrePinHook hook);
    /** Installs an observer after a hazard token is claimed but before pin. */
    static EXPORTED_PUBLIC void
    setHandlerHazardClaimHook(HandlerHazardClaimHook hook);
    /** Installs an observer after an atomic caller closes admission. */
    static EXPORTED_PUBLIC void
    setHandlerAtomicDrainHook(HandlerAtomicDrainHook hook);
    /** Runs a test callback while the registry writer lock is held. */
    static EXPORTED_PUBLIC void
    withHandlerMutationLockForTest(HandlerMutationLockHook hook);
    /** Dispatches one handler through the production registry path. */
    static EXPORTED_PUBLIC bool dispatchHandlerForTest(
        TimerHandler *handler, uint64_t delta, InterruptState &state);
    /** Returns committed callback hazards for one handler. */
    static EXPORTED_PUBLIC size_t
    activeDispatchCountForTest(TimerHandler *handler);
    /** Returns all claimed callback-hazard records, including partial pins. */
    static EXPORTED_PUBLIC size_t claimedDispatchCountForTest();
#endif

  protected:
    /** The default constructor */
    HostedTimer() INITIALISATION_ONLY;
    /** The destructor */
    virtual ~HostedTimer();

  private:
    /** The copy-constructor
     *\note NOT implemented */
    HostedTimer(const HostedTimer &);
    /** The assignment operator
     *\note NOT implemented */
    HostedTimer &operator=(const HostedTimer &);

    //
    // IrqHandler interface
    //
    virtual bool irq(irq_id_t number, InterruptState &state);

    /** The current year */
    size_t m_Year;
    /** The current month */
    uint8_t m_Month;
    /** The current day of month */
    uint8_t m_DayOfMonth;
    /** The current day of week */
    uint8_t m_DayOfWeek;
    /** The current hour */
    uint8_t m_Hour;
    /** The current minute */
    uint8_t m_Minute;
    /** The current second */
    uint8_t m_Second;
    /** The current nanosecond */
    uint64_t m_Nanosecond;

    /** Tick source. */
    __pedigree_hosted::timer_t m_Timer;

    /** Registered handler. */
    irq_id_t m_IrqId;

    /** The HostedTimer class instance */
    static HostedTimer m_Instance;

    /** Timer handlers and their callback lifetime state. */
    TimerHandlerRegistry m_HandlerRegistry;

    /** Alarm structure. */
    class Alarm
    {
      public:
        Alarm(class Event *pEvent, size_t time, class Thread *pThread)
            : m_pEvent(pEvent), m_Time(time), m_pThread(pThread)
        {
        }
        class Event *m_pEvent;
        size_t m_Time;
        class Thread *m_pThread;

      private:
        Alarm(const Alarm &);
        Alarm &operator=(const Alarm &);
    };

    /** List of alarms. */
    List<Alarm *> m_Alarms;

    /** Alarm modification lock. */
    Spinlock m_AlarmLock;

    /** Are we init-ed? */
    bool m_bInitialized = false;
};

/** @} */

#endif
