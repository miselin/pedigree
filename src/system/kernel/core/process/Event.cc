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

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/utility.h"

Event::Event(uintptr_t handlerAddress, bool isDeletable, size_t specificNestingLevel)
    : m_HandlerAddress(handlerAddress),
      m_bIsDeletable(isDeletable),
      m_NestingLevel(specificNestingLevel),
      m_Magic(EVENT_MAGIC),
      m_Threads(),
      m_Lock(false),
      m_DeleteWhenUnused(false),
      m_DeliveriesClosed(false),
      m_DrainClaimed(false),
      m_SendersInFlight(0),
      m_pFirstActiveDelivery(nullptr),
      m_DeliveryWaiters() {}

Event::SendLease::SendLease() : m_pEvent(nullptr) {}

Event::SendLease::SendLease(Event* event) : m_pEvent(event) {}

Event::SendLease::SendLease(SendLease&& other) : m_pEvent(other.m_pEvent) {
  other.m_pEvent = nullptr;
}

Event::SendLease::~SendLease() {
  reset();
}

Event::SendLease& Event::SendLease::operator=(SendLease&& other) {
  if (this != &other) {
    reset();
    m_pEvent = other.m_pEvent;
    other.m_pEvent = nullptr;
  }
  return *this;
}

void Event::SendLease::reset() {
  Event* event = m_pEvent;
  m_pEvent = nullptr;
  if (event) {
    event->endSend();
  }
}

Event::Retirement::Retirement() : m_pEvent(nullptr) {}

Event::Retirement::Retirement(Event* event) : m_pEvent(event) {}

Event::Retirement::Retirement(Retirement&& other) : m_pEvent(other.m_pEvent) {
  other.m_pEvent = nullptr;
}

Event::Retirement::~Retirement() {
  reset();
}

Event::Retirement& Event::Retirement::operator=(Retirement&& other) {
  if (this != &other) {
    reset();
    m_pEvent = other.m_pEvent;
    other.m_pEvent = nullptr;
  }
  return *this;
}

void Event::Retirement::reset() {
  Event* event = m_pEvent;
  m_pEvent = nullptr;
  if (event) {
    event->finishRetirement();
  }
}

Event::Delivery::Delivery()
    : m_pEvent(nullptr),
      m_pThread(nullptr),
      m_pPreviousActive(nullptr),
      m_pNextActive(nullptr),
      m_bActive(false) {}

Event::Delivery::Delivery(Event* event, Thread* thread)
    : m_pEvent(event),
      m_pThread(thread),
      m_pPreviousActive(nullptr),
      m_pNextActive(nullptr),
      m_bActive(false) {}

Event::Delivery::Delivery(Delivery&& other)
    : m_pEvent(other.m_pEvent),
      m_pThread(other.m_pThread),
      m_pPreviousActive(nullptr),
      m_pNextActive(nullptr),
      m_bActive(false) {
  if (other.m_bActive) {
    FATAL("Moving an active Event delivery.");
  }
  other.m_pEvent = nullptr;
  other.m_pThread = nullptr;
}

Event::Delivery::~Delivery() {
  reset();
}

Event::Delivery& Event::Delivery::operator=(Delivery&& other) {
  if (this != &other) {
    if (other.m_bActive) {
      FATAL("Moving an active Event delivery.");
    }
    reset();
    m_pEvent = other.m_pEvent;
    m_pThread = other.m_pThread;
    other.m_pEvent = nullptr;
    other.m_pThread = nullptr;
  }
  return *this;
}

void Event::Delivery::beginDispatch() {
  if (m_pEvent) {
    m_pEvent->beginDispatch(this);
  }
}

void Event::Delivery::reset() {
  Event* event = m_pEvent;
  Thread* thread = m_pThread;
  if (event) {
    if (m_bActive) {
      event->endDispatch(this);
    }
    m_pEvent = nullptr;
    m_pThread = nullptr;
    event->completeDelivery(thread);
  }
}

Event::~Event() {
  EMIT_IF(THREADS) {
    if (m_DeliveryWaiters.waiterCount()) {
      FATAL("Deleting an Event while delivery waiters are live.");
    }

    LockGuard<Spinlock> guard(m_Lock);

    if (m_SendersInFlight) {
      FATAL("Deleting an Event with admitted senders.");
    }

    if (m_pFirstActiveDelivery) {
      FATAL("Deleting an Event with an active kernel delivery.");
    }

    if (m_Threads.count()) {
      ERROR("UNSAFE EVENT DELETION");
      for (auto it : m_Threads) {
        ERROR(" => Pending delivery to thread " << it << " (" << it->getParent()->getId() << ":"
                                                << it->getId() << ").");
      }
      FATAL("Unsafe event deletion: " << m_Threads.count() << " threads reference it!");

      m_Threads.clear();
    }
  }
}

uintptr_t Event::getTrampoline() {
  EMIT_IF(THREADS) {
    return VirtualAddressSpace::getKernelAddressSpace().getKernelEventBlockStart();
  }

  return 0;
}

uintptr_t Event::getSecondaryTrampoline() {
  return getTrampoline() + 0x100;
}

uintptr_t Event::getHandlerBuffer() {
  return getTrampoline() + 0x1000;
}

uintptr_t Event::getLastHandlerBuffer() {
  return getHandlerBuffer() + ((EVENT_TID_MAX * MAX_NESTED_EVENTS) * EVENT_LIMIT);
}

bool Event::isDeletable() {
  return m_bIsDeletable;
}

bool Event::unserialize(uint8_t* pBuffer, Event& event) {
  ERROR("Event::unserialize is abstract, should never be called.");
  return false;
}

size_t Event::getEventType(uint8_t* pBuffer) {
  void* alignedBuffer = ASSUME_ALIGNMENT(pBuffer, sizeof(size_t));
  size_t* pBufferSize_t = reinterpret_cast<size_t*>(alignedBuffer);
  return pBufferSize_t[0];
}

Event::Event(const Event& other)
    : Event(other.m_HandlerAddress, other.m_bIsDeletable, other.m_NestingLevel) {
  ConstexprLockGuard<Spinlock, THREADS> guard(m_Lock);
  m_Threads.clear();
}

Event& Event::operator=(const Event& other) {
  if (this == &other) {
    return *this;
  }

  ConstexprLockGuard<Spinlock, THREADS> guard(m_Lock);
  if (m_Threads.count()) {
    FATAL("Cannot replace an Event while deliveries are live.");
  }
  if (m_DrainClaimed || m_SendersInFlight || m_pFirstActiveDelivery) {
    FATAL("Cannot replace an Event while its lifetime is owned.");
  }

  m_HandlerAddress = other.m_HandlerAddress;
  m_bIsDeletable = other.m_bIsDeletable;
  m_NestingLevel = other.m_NestingLevel;
  m_DeleteWhenUnused = false;
  m_DeliveriesClosed = false;
  m_DrainClaimed = false;
  m_SendersInFlight = 0;
  return *this;
}

Event::SendLease Event::beginSend() {
  auto deliveryGuard = m_DeliveryWaiters.acquire();
  LockGuard<Spinlock> guard(m_Lock);
  if (m_DeliveriesClosed) {
    return SendLease();
  }
  ++m_SendersInFlight;
  return SendLease(this);
}

void Event::endSend() {
  bool drained = false;
  bool deleteNow = false;
  {
    auto deliveryGuard = m_DeliveryWaiters.acquire();
    {
      LockGuard<Spinlock> guard(m_Lock);
      assert(m_SendersInFlight);
      --m_SendersInFlight;
      drained = !m_SendersInFlight && !m_Threads.count();
      deleteNow = drained && m_DeleteWhenUnused;
    }
    if (drained) {
      deliveryGuard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(this));
    }
  }

  if (deleteNow) {
    delete this;
  }
}

bool Event::registerThread(Thread* thread) {
  LockGuard<Spinlock> guard(m_Lock);
  if (m_DeliveriesClosed) {
    return false;
  }
  m_Threads.pushBack(thread);
  return true;
}

void Event::deregisterThread(Thread* thread) {
  bool finalDelivery = false;
  bool deleteNow = false;
  {
    auto deliveryGuard = m_DeliveryWaiters.acquire();
    {
      LockGuard<Spinlock> guard(m_Lock);

      for (List<Thread*>::Iterator it = m_Threads.begin(); it != m_Threads.end(); ++it) {
        if (*it == thread) {
          m_Threads.erase(it);
          finalDelivery = !m_Threads.count() && !m_SendersInFlight;
          break;
        }
      }
      deleteNow = finalDelivery && m_DeleteWhenUnused;
    }
    if (finalDelivery) {
      deliveryGuard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(this));
    }
  }

  if (deleteNow) {
    delete this;
  }
}

void Event::completeDelivery(Thread* thread) {
  bool deleteNow = false;
  {
    auto deliveryGuard = m_DeliveryWaiters.acquire();
    bool finalDelivery = false;
    {
      LockGuard<Spinlock> guard(m_Lock);

      bool found = false;
      for (List<Thread*>::Iterator it = m_Threads.begin(); it != m_Threads.end(); ++it) {
        if (*it == thread) {
          m_Threads.erase(it);
          found = true;
          break;
        }
      }

      if (!found) {
        FATAL("Completing an event delivery with no registration.");
      }

      finalDelivery = !m_Threads.count() && !m_SendersInFlight;
      if (m_bIsDeletable) {
        m_DeleteWhenUnused = true;
      }
      deleteNow = finalDelivery && m_DeleteWhenUnused;
    }

    if (finalDelivery) {
      deliveryGuard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(this));
    }
  }

  if (deleteNow) {
    delete this;
  }
}

size_t Event::pendingCount() {
  LockGuard<Spinlock> guard(m_Lock);

  return m_Threads.count();
}

bool Event::tryAcquireRegistration(SendLease& registration) {
  registration = beginSend();
  return static_cast<bool>(registration);
}

void Event::beginDispatch(Delivery* delivery) {
  LockGuard<Spinlock> guard(m_Lock);
  if (delivery->m_bActive || delivery->m_pEvent != this) {
    FATAL("Invalid Event delivery activation.");
  }

  delivery->m_pPreviousActive = nullptr;
  delivery->m_pNextActive = m_pFirstActiveDelivery;
  if (m_pFirstActiveDelivery) {
    m_pFirstActiveDelivery->m_pPreviousActive = delivery;
  }
  m_pFirstActiveDelivery = delivery;
  delivery->m_bActive = true;
}

void Event::endDispatch(Delivery* delivery) {
  LockGuard<Spinlock> guard(m_Lock);
  if (!delivery->m_bActive || delivery->m_pEvent != this) {
    FATAL("Invalid Event delivery deactivation.");
  }

  if (delivery->m_pPreviousActive) {
    delivery->m_pPreviousActive->m_pNextActive = delivery->m_pNextActive;
  } else {
    assert(m_pFirstActiveDelivery == delivery);
    m_pFirstActiveDelivery = delivery->m_pNextActive;
  }
  if (delivery->m_pNextActive) {
    delivery->m_pNextActive->m_pPreviousActive = delivery->m_pPreviousActive;
  }

  delivery->m_pPreviousActive = nullptr;
  delivery->m_pNextActive = nullptr;
  delivery->m_bActive = false;
}

void Event::waitForDeliveries() {
  // no-op if no threads
  EMIT_IF(!THREADS) {
    return;
  }

  if (m_bIsDeletable) {
    FATAL("Cannot wait on a self-deleting Event.");
  }

  bool claimed = false;
  while (true) {
    auto guard = m_DeliveryWaiters.acquire();
    {
      LockGuard<Spinlock> deliveryGuard(m_Lock);
      if (!claimed) {
        if (m_DrainClaimed) {
          FATAL(
              "Event delivery drain attempted by more than one "
              "owner.");
        }
        m_DrainClaimed = true;
        claimed = true;
      }
      m_DeliveriesClosed = true;
      Thread* current = Processor::information().getCurrentThread();
      for (Delivery* delivery = m_pFirstActiveDelivery; delivery;
           delivery = delivery->m_pNextActive) {
        if (delivery->m_pThread == current) {
          FATAL(
              "Event handler attempted to drain its own active "
              "delivery; use Event::retire for heap ownership.");
        }
      }
      if (!m_Threads.count() && !m_SendersInFlight) {
        return;
      }
    }

    const WaitQueue::WakeReason wakeReason =
        guard.waitForCompletion(WaitQueue::Channel(this), Thread::EventWait,
                                reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    (void)wakeReason;
  }
}

void Event::retire() {
  Retirement retirement;
  beginRetirement(retirement);
}

void Event::beginRetirement(Retirement& retirement) {
  {
    auto guard = m_DeliveryWaiters.acquire();
    LockGuard<Spinlock> deliveryGuard(m_Lock);
    if (m_bIsDeletable) {
      FATAL("Explicitly retiring an already self-deleting Event.");
    }
    if (m_DrainClaimed) {
      FATAL("Cannot retire an Event whose drain is already owned.");
    }
    m_DrainClaimed = true;
    m_DeliveriesClosed = true;
    ++m_SendersInFlight;
  }

  retirement = Retirement(this);
}

void Event::finishRetirement() {
  {
    LockGuard<Spinlock> deliveryGuard(m_Lock);
    if (!m_DrainClaimed || !m_DeliveriesClosed || !m_SendersInFlight) {
      FATAL("Completing an Event retirement without ownership.");
    }
    m_DeleteWhenUnused = true;
  }

  // The retirement pin prevents a concurrent final delivery from deleting
  // the Event before this handoff completes.
  endSend();
}
