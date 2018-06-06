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
    Processor::information().getCurrentThread()->setInterrupted(true);
}

class DelayTimerEvent : public Event
{
  public:
    DelayTimerEvent();
    virtual ~DelayTimerEvent();

    virtual size_t serialize(uint8_t *pBuffer);
    static bool unserialize(uint8_t *pBuffer, DelayTimerEvent &event);
    virtual size_t getNumber();
};

bool delay(Timestamp nanoseconds)
{
    Thread *pThread = Processor::information().getCurrentThread();
    void *handle = addAlarm(nanoseconds);

    /// \todo possible race condition for very short alarm times
    while (true)
    {
        if (!pThread->wasInterrupted())
            Processor::information().getScheduler().sleep(0);

        if (pThread->wasInterrupted())
        {
            removeAlarm(handle);
            break;
        }
        else if (pThread->getUnwindState() != Thread::Continue)
        {
            removeAlarm(handle);
            return false;
        }

        pThread->setInterrupted(false);
    }

    return true;
}

void *addAlarm(Timestamp nanoseconds)
{
    Event *pEvent = new DelayTimerEvent();
    uint64_t usecs = nanoseconds / Multiplier::Microsecond;
    if (!usecs)
        ++usecs;  /// \todo perhaps change addAlarm to take ns.

    Thread *pThread = Processor::information().getCurrentThread();
    pThread->setInterrupted(false);
    Machine::instance().getTimer()->addAlarm(pEvent, 0, usecs);

    return pEvent;
}

void removeAlarm(void *handle)
{
    assert(handle != nullptr);

    Thread *pThread = Processor::information().getCurrentThread();
    Event *pEvent = reinterpret_cast<Event *>(handle);
    Machine::instance().getTimer()->removeAlarm(pEvent);

    // Handle a race condition where the timeout triggers but hasn't been
    // handled by the time we clean up the alarm. That leaves the deleted event
    // in this thread's event queue. We also do so after removing the alarm
    // from the Machine implementation so that we don't get new events added
    // after our cull.
    pThread->cullEvent(pEvent);

    delete pEvent;
}

DelayTimerEvent::DelayTimerEvent()
    : Event(reinterpret_cast<uintptr_t>(&delayTimerFired), false)
{
}
DelayTimerEvent::~DelayTimerEvent()
{
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
}  // namespace Time
