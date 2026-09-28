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

#ifndef RINGBUFFER_H
#define RINGBUFFER_H
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/utilities/BufferMonitors.h"
#include "pedigree/kernel/utilities/Mailbox.h"

namespace RingBufferWait = MailboxWait;

struct RingBufferNotifications {
  void changed(bool becameReadable, bool becameWritable) {
    if (becameReadable) {
      readableGeneration += 1;
    }
    if (becameWritable) {
      writableGeneration += 1;
    }
    monitors.notify();
  }

  void closed() {
    monitors.notify();
  }

  Atomic<uint64_t> readableGeneration{0};
  Atomic<uint64_t> writableGeneration{0};
  BufferMonitors monitors;
};

/** Blocking queue with one-shot readiness subscriptions and edge generations. */
template <class T, size_t preallocatedSize = 0>
class EXPORTED_PUBLIC RingBuffer : public Mailbox<T, preallocatedSize, RingBufferNotifications> {
  using Base = Mailbox<T, preallocatedSize, RingBufferNotifications>;

 public:
  explicit RingBuffer(size_t ringSize) : Base(ringSize) {}

  uint64_t readableGeneration() const {
    return this->m_Notifications.readableGeneration.value();
  }

  uint64_t writableGeneration() const {
    return this->m_Notifications.writableGeneration.value();
  }

  /**
   * \brief monitor - add a new Event to be fired when something happens
   *
   * This could be a read or a write event; after receiving the event be
   * sure to call dataReady() and/or canWrite() to determine the state
   * of the buffer.
   *
   * Do not assume that an event means both a read and write will not
   * block. In fact, never assume an event means either will not block.
   * You may need to re-subscribe to the event if something else reads
   * or writes to the ring buffer between the event trigger and your
   * handling.
   */
  bool monitor(Thread* pThread, Event* pEvent) {
    typename Base::ActiveOperation operation(*this);
    if (!operation) {
      Event::SendLease registration;
      if (pEvent->tryAcquireRegistration(registration)) {
        EMIT_IF(THREADS) {
          pThread->sendEvent(pEvent);
        }
      }
      return false;
    }

    if (this->readsClosed()) {
      Event::SendLease registration;
      if (pEvent->tryAcquireRegistration(registration)) {
        EMIT_IF(THREADS) {
          pThread->sendEvent(pEvent);
        }
      }
      return false;
    }

    return this->m_Notifications.monitors.add(pThread, pEvent);
  }

  /// Add a Semaphore to be signaled when readiness changes or the ring closes.
  bool monitor(Semaphore* pSemaphore) {
    typename Base::ActiveOperation operation(*this);
    if (!operation) {
      EMIT_IF(THREADS) {
        pSemaphore->release();
      }
      return false;
    }

    if (this->readsClosed()) {
      EMIT_IF(THREADS) {
        pSemaphore->release();
      }
      return false;
    }

    this->m_Notifications.monitors.add(pSemaphore);
    return true;
  }

  /// Cull all monitor targets pointing to \p pThread.
  void cullMonitorTargets(Thread* pThread) {
    typename Base::ActiveOperation operation(*this);
    if (!operation) {
      return;
    }

    this->m_Notifications.monitors.cull(pThread);
  }

  /// Cull all monitor targets pointing to \p pSemaphore.
  void cullMonitorTargets(Semaphore* pSemaphore) {
    typename Base::ActiveOperation operation(*this);
    if (!operation) {
      return;
    }

    this->m_Notifications.monitors.cull(pSemaphore);
  }

  /// Cull all monitor targets pointing to \p pEvent.
  void cullMonitorTargets(Event* pEvent) {
    typename Base::ActiveOperation operation(*this);
    if (!operation) {
      return;
    }

    this->m_Notifications.monitors.cull(pEvent);
  }
};

extern template class RingBuffer<char>;   // IWYU pragma: keep
extern template class RingBuffer<void*>;  // IWYU pragma: keep

#endif
