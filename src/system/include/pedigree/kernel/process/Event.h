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

#ifndef EVENT_H
#define EVENT_H
#include <config.h>

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/new"

class Thread;

/** The maximum size of one event, serialized. */
#define EVENT_LIMIT 4096
/** The maximum thread ID one can dispatch an Event to. */
#define EVENT_TID_MAX 255
/** The maximum number of times an event can fire while in an event handler
   (nesting levels). After this, the process will be terminated. */
#define MAX_NESTED_EVENTS 16

#define EVENT_MAGIC 0x8899AABBCCDDEEFFULL

/** The abstract base class for an asynchronous event. An event can hold any
   amount of information up to a hard maximum size of EVENT_LIMIT (usually 4096
   bytes). An event is serialized using the virtual serialize() function and
   sent to a recipient thread in either user or kernel mode, where it is
   unserialized. */
class EXPORTED_PUBLIC Event {
 public:
  /** Pins Event storage across one Thread::sendEvent admission attempt. */
  class SendLease {
   public:
    SendLease();
    SendLease(SendLease&& other);
    ~SendLease();

    SendLease& operator=(SendLease&& other);

    explicit operator bool() const {
      return m_pEvent != nullptr;
    }

   private:
    friend class Event;
    friend class Thread;

    explicit SendLease(Event* event);
    void reset();

    SendLease(const SendLease&) = delete;
    SendLease& operator=(const SendLease&) = delete;

    Event* m_pEvent;
  };

  /**
   * Keeps an Event alive while its owner removes every external source.
   *
   * Construction closes future delivery and registration admission. The
   * destructor completes retirement, so callbacks cannot re-arm a raw Event
   * pointer between source removal and deletion.
   */
  class Retirement {
   public:
    Retirement();
    Retirement(Retirement&& other);
    ~Retirement();

    Retirement& operator=(Retirement&& other);

   private:
    friend class Event;

    explicit Retirement(Event* event);
    void reset();

    Retirement(const Retirement&) = delete;
    Retirement& operator=(const Retirement&) = delete;

    Event* m_pEvent;
  };

  /**
   * Owns one event delivery after it has left a Thread's queue.
   *
   * The enqueue registration remains live until this object is destroyed,
   * so an Event owner can safely wait for an in-flight scheduler delivery.
   */
  class Delivery {
   public:
    Delivery();
    Delivery(Delivery&& other);
    ~Delivery();

    Delivery& operator=(Delivery&& other);

    Event* get() const {
      return m_pEvent;
    }

    Event* operator->() const {
      return m_pEvent;
    }

    explicit operator bool() const {
      return m_pEvent != nullptr;
    }

    /** Completes this delivery before the end of the enclosing scope. */
    void reset();

   private:
    friend class Event;
    friend class Thread;
    friend class PerProcessorScheduler;

    Delivery(Event* event, Thread* thread);
    void beginDispatch();

    Delivery(const Delivery&) = delete;
    Delivery& operator=(const Delivery&) = delete;

    Event* m_pEvent;
    Thread* m_pThread;
    Delivery* m_pPreviousActive;
    Delivery* m_pNextActive;
    bool m_bActive;
  };

  /** Constructs an Event object.
      \param handlerAddress The address of the handling function.
      \param isDeletable Can the object be deleted after map()? This is used
     for creating objects without worrying about destroying them. \param
     specificNestingLevel Is the event pinned to a specific nesting level? If
     this value is not ~0UL, then the event will only be fired if the current
     nesting level is \p specificNestingLevel . \note As can be surmised,
     handlerAddress is NOT reentrant. If you use this Event in multiple
            threads concurrently, you CANNOT change the handler address. */
  Event(uintptr_t handlerAddress, bool isDeletable, size_t specificNestingLevel = ~0UL);
  virtual ~Event();

  /** Retrieves the main trampoline memory address. */
  static uintptr_t getTrampoline();

  /** Retrieves the secondary trampoline memory address. */
  static uintptr_t getSecondaryTrampoline();

  /** Retrieves the event handler buffer memory address. */
  static uintptr_t getHandlerBuffer();

  /** Retrieves the target-page-rounded size of one event handler buffer. */
  static size_t getHandlerBufferSize();

  /** Retrieves the last handler buffer memory address. */
  static uintptr_t getLastHandlerBuffer();

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  /** Hosted-only seam for checking handler-buffer geometry. */
  static constexpr size_t getHostedHandlerBufferSize(size_t pageSize) {
    return pageSize ? handlerBufferSize(pageSize) : 0;
  }
#endif

  /** Returns true if the event is on the heap and can be deleted when
     handled. This is for creating fire-and-forget messages and not worrying
     about memory leaks. */
  virtual bool isDeletable();

  /** Given a buffer EVENT_LIMIT bytes long, take the variables
      present in this object and convert them to a binary form.

      It can be assumed that this function will only be called when
      the Event is about to be dispatched, and so the inhibited mask
      of the current Thread is available to be changed to what the
      event handler requires. This change will be undone when the
      handler completes.

      \param pBuffer The buffer to serialize to.
      \return The number of bytes serialized. */
  virtual size_t serialize(uint8_t* pBuffer) = 0;

  /** Given a serialized Event in binary form, attempt to unserialize into the
     given object. If this is impossible, return false.

      \note It is impossible to make static functions virtual, but it is
     <b>required</b> that all subclasses of Event implement this function
     statically. */
  static bool unserialize(uint8_t* pBuffer, Event& event);

  /** Given a serialized event, returns the type of that event (a constant
   * from eventNumbers.h). */
  static size_t getEventType(uint8_t* pBuffer);

  /** Returns the handler address. */
  uintptr_t getHandlerAddress() {
    return m_HandlerAddress;
  }

  /** Returns true if this event is subject to the thread's signal mask. */
  virtual bool isSignalEvent() const {
    return false;
  }

  /**
   * Returns the instance to enqueue for one delivery. Most events enqueue
   * themselves; signal dispositions return an independently owned snapshot.
   */
  virtual Event* cloneForDelivery() {
    return this;
  }

  /** Returns the specific nesting level, or ~0UL if there is none defined. */
  size_t getSpecificNestingLevel() {
    return m_NestingLevel;
  }

  /** Returns the event number / ID. */
  virtual size_t getNumber() = 0;

  Event(const Event& other);
  Event& operator=(const Event& other);

  /** Gets the number of queued or actively dispatched deliveries. */
  size_t pendingCount();

  /**
   * Pins this Event while an external notification source stores its
   * pointer. Returns an empty lease once retirement has begun.
   */
  bool tryAcquireRegistration(SendLease& registration);

  /**
   * Closes delivery admission and waits until every queued or active
   * delivery has drained. Calling this from this Event's own active kernel
   * handler is a contract violation; heap owners which can retire from a
   * handler must use retire().
   */
  virtual void waitForDeliveries();

  /**
   * Closes delivery admission and deletes this heap-owned Event after the
   * final queued or active delivery drains. The caller must discard its
   * pointer immediately.
   */
  void retire();

  /**
   * Begins retirement while keeping storage pinned so the owner can remove
   * raw pointers from external notification registries.
   */
  void beginRetirement(Retirement& retirement);

 protected:
  /** Handler address. */
  uintptr_t m_HandlerAddress;

  /** Can the object be deleted after map? */
  bool m_bIsDeletable;

  /** Specific nesting level, or ~0UL. */
  size_t m_NestingLevel;

  /** Magic number for verification. */
  uint64_t m_Magic;

  /** One entry per queued or actively dispatched delivery. */
  List<Thread*> m_Threads;

  /** Spinlock for controlling access to the thread list. */
  Spinlock m_Lock;

 private:
  friend class Thread;

  static constexpr size_t handlerBufferSize(size_t pageSize) {
    const size_t pageCount = (EVENT_LIMIT / pageSize) + ((EVENT_LIMIT % pageSize) ? 1 : 0);
    return pageCount * pageSize;
  }

  /** Admits and pins one sender before it touches any other Event state. */
  SendLease beginSend();
  void endSend();
  void finishRetirement();

  /** Registers one enqueue while delivery admission remains open. */
  bool registerThread(Thread* thread);

  /** Deregisters exactly one enqueue without transferring ownership. */
  void deregisterThread(Thread* thread);

  /**
   * Completes exactly one delivery. Deletable events are retired only after
   * their final queued or active delivery releases its registration.
   */
  void completeDelivery(Thread* thread);

  /** Tracks an Event::Delivery while its kernel callback is active. */
  void beginDispatch(Delivery* delivery);
  void endDispatch(Delivery* delivery);

  /** A completed delivery requested deletion once all leases drain. */
  bool m_DeleteWhenUnused;

  /** No delivery can register after close-and-drain begins. */
  bool m_DeliveriesClosed;

  /** Exactly one owner may execute the permanent close-and-drain. */
  bool m_DrainClaimed;

  /** sendEvent calls admitted before close-and-drain. */
  size_t m_SendersInFlight;

  /** Intrusive active-callback list; Delivery owns the link storage. */
  Delivery* m_pFirstActiveDelivery;

  /** Waiters for the final queued or active delivery to drain. */
  WaitQueue m_DeliveryWaiters;
};

#endif
