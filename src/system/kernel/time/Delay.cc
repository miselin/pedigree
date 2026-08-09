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

#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/eventNumbers.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/new"

namespace Time
{
static void delayTimerFired(uint8_t *pBuffer)
{
    Processor::information()
        .getCurrentThread()
        ->markTimeoutInterruptedWait();
}

class DelayTimerEvent : public Event
{
  public:
    explicit DelayTimerEvent(
        Thread *owner, size_t nestingLevel = ~0UL);
    virtual ~DelayTimerEvent();

    virtual size_t serialize(uint8_t *pBuffer);
    static bool unserialize(uint8_t *pBuffer, DelayTimerEvent &event);
    virtual size_t getNumber();

    Thread *owner() const
    {
        return m_pOwner;
    }

  private:
    Thread *m_pOwner;
};

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
Atomic<size_t> g_HostedAlarmCreates(0);
Atomic<size_t> g_HostedAlarmDestroys(0);
#endif

bool delay(Timestamp nanoseconds)
{
    Thread *pThread = Processor::information().getCurrentThread();
    void *handle = addAlarm(nanoseconds);

    while (true)
    {
        if (!pThread->wasInterrupted())
            pThread->waitForEvent(&removeAlarm, handle);

        const Thread::InterruptionReason interruption =
            pThread->getInterruptionReason();
        if (interruption == Thread::InterruptedByTimeout)
        {
            removeAlarm(handle);
            pThread->clearInterruption();
            break;
        }
        else if (interruption == Thread::InterruptedBySignal)
        {
            removeAlarm(handle);
            return false;
        }
        else if (pThread->getUnwindState() != Thread::Continue)
        {
            removeAlarm(handle);
            return false;
        }

        pThread->clearInterruption();
    }

    return true;
}

void *addAlarm(Timestamp nanoseconds)
{
    Thread *pThread = Processor::information().getCurrentThread();
    Event *pEvent =
        new DelayTimerEvent(pThread, pThread->getStateLevel());
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    g_HostedAlarmCreates += 1;
#endif
    uint64_t usecs = nanoseconds / Multiplier::Microsecond;
    if (!usecs)
        ++usecs;  /// \todo perhaps change addAlarm to take ns.

    pThread->clearInterruption();
    Machine::instance().getTimer()->addAlarm(pEvent, 0, usecs);

    return pEvent;
}

void removeAlarm(void *handle)
{
    assert(handle != nullptr);

    DelayTimerEvent *pEvent =
        reinterpret_cast<DelayTimerEvent *>(handle);
    Machine::instance().getTimer()->removeAlarm(pEvent);

    // Handle a race condition where the timeout triggers but hasn't been
    // handled by the time we clean up the alarm. That leaves the deleted event
    // in this thread's event queue. We also do so after removing the alarm
    // from the Machine implementation so that we don't get new events added
    // after our cull.
    pEvent->owner()->cullEvent(pEvent);

    // A timer dispatch may already have admitted this event while the alarm
    // was being removed. Close admission and drain that lease before delete.
    pEvent->waitForDeliveries();
    delete pEvent;
}

DelayTimerEvent::DelayTimerEvent(
    Thread *owner, size_t nestingLevel)
    : Event(
          reinterpret_cast<uintptr_t>(&delayTimerFired), false, nestingLevel),
      m_pOwner(owner)
{
}
DelayTimerEvent::~DelayTimerEvent()
{
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    g_HostedAlarmDestroys += 1;
#endif
}

size_t DelayTimerEvent::serialize(uint8_t *pBuffer)
{
    return 0;
}

bool DelayTimerEvent::unserialize(uint8_t *pBuffer, DelayTimerEvent &event)
{
    return true;
}

size_t DelayTimerEvent::getNumber()
{
    return EventNumbers::DelayTimer;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
size_t getHostedAlarmCreateCount()
{
    return g_HostedAlarmCreates;
}

size_t getHostedAlarmDestroyCount()
{
    return g_HostedAlarmDestroys;
}
#endif
}  // namespace Time
