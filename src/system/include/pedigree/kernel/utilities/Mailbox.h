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

#ifndef PEDIGREE_KERNEL_UTILITIES_MAILBOX_H
#define PEDIGREE_KERNEL_UTILITIES_MAILBOX_H
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/RingQueue.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/new"

#include <config.h>

#if THREADS
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#endif

namespace MailboxWait {
enum WaitType { Reading, Writing };
}

struct MailboxNotifications {
  void changed(bool, bool) {}
  void closed() {}
};

/**
 * Blocking bounded queue. Publication, waiter enrolment and close admission
 * share one mutex. Notifications run under that mutex; the default policy
 * compiles away and carries no readiness or subscriber state.
 */
template <class T, size_t preallocatedSize = 0, class Notifications = MailboxNotifications>
class EXPORTED_PUBLIC Mailbox {
 protected:
  // Owns admission and the mutex together. Condition waits temporarily release
  // the mutex, but close() must still drain the admitted operation.
  class ActiveOperation {
   public:
    explicit ActiveOperation(Mailbox& buffer)
        : m_TerminationDeferral(), m_Buffer(buffer.beginOperation() ? &buffer : nullptr) {}

    ~ActiveOperation() {
      if (m_Buffer) {
        m_Buffer->endOperation();
      }
    }

    explicit operator bool() const {
      return m_Buffer != nullptr;
    }

   private:
    ActiveOperation(const ActiveOperation&) = delete;
    ActiveOperation& operator=(const ActiveOperation&) = delete;

    TerminationDeferral m_TerminationDeferral;
    Mailbox* m_Buffer;
  };

 public:
  enum Error {
    NoError,

    // Mailbox is empty and a zero timeout was specified
    Empty,

    // A nonblocking operation could not acquire the lock or found no space
    WouldBlock,

    // ConditionVariable failure modes
    TimedOut,
    Interrupted,
    ThreadTerminating,

    // Mailbox has closed and will not admit another operation.
    Closed,
  };

  explicit Mailbox(size_t ringSize) : m_Ring(ringSize) {}

  /// Destructor - closes the ring and drains every admitted operation.
  ~Mailbox() {
    close();
  }

  /**
   * Stop admitting operations, wake every blocked operation and monitor,
   * and wait for already-admitted operations to retire.
   */
  void close() {
    TerminationDeferral terminationDeferral;
    m_Lock.acquire();
    if (!m_Closing) {
      m_Closing = true;
      m_WriteClosed = true;
      if (m_ReadWaiters) {
        m_ReadCondition.broadcast();
      }
      if (m_WriteWaiters) {
        m_WriteCondition.broadcast();
      }
      m_Notifications.closed();
    }

    while (m_ActiveOperations) {
      m_DrainCondition.waitForCompletion(m_Lock);
    }
    m_Lock.release();
  }

  /**
   * Remove one object after close() has drained all admitted operations.
   *
   * Pointer queues use this to retire payload ownership which the generic
   * container cannot infer. No producer or consumer can race this path once
   * close() has returned.
   */
  MUST_USE_RESULT bool takeAfterClose(T& out) {
    out = T();
    LockGuard<Mutex> guard(m_Lock);
    if (!m_Closing || m_ActiveOperations || !m_Ring.count()) {
      return false;
    }

    popLocked(&out, 1);
    return true;
  }

  /// Writes one object, waiting for space up to the supplied timeout.
  Error write(const T& obj, Time::Timestamp& timeout) {
    ActiveOperation operation(*this);
    if (!operation) {
      return Closed;
    }

    const Error error = waitForLocked(MailboxWait::Writing, timeout);
    if (error == NoError) {
      pushLocked(&obj, 1);
    }
    return error;
  }

  Error write(const T& obj) {
    Time::Timestamp timeout = Time::Infinity;
    return write(obj, timeout);
  }

  /**
   * Closes write admission after appending one final object. Readers can
   * drain the objects already present, including the final object.
   */
  bool closeWritesWithFinal(const T& obj) {
    ActiveOperation operation(*this);
    if (!operation) {
      return false;
    }

    if (m_WriteClosed) {
      return false;
    }

    m_WriteClosed = true;
    m_FinalPending = true;
    if (m_WriteWaiters) {
      m_WriteCondition.broadcast();
    }
    while (!m_Closing && m_Ring.count() >= m_Ring.capacity()) {
      ++m_WriteWaiters;
      m_WriteCondition.waitForCompletion(m_Lock);
      --m_WriteWaiters;
    }

    if (m_Closing) {
      return false;
    }

    pushLocked(&obj, 1);
    m_FinalPending = false;
    if (m_ReadWaiters) {
      m_ReadCondition.broadcast();
    }
    return true;
  }

  /**
   * Atomically writes one object without waiting for lock ownership or space.
   * A WouldBlock result leaves the ring unchanged.
   */
  Error tryWrite(const T& obj) {
#if THREADS
    // Waking a blocked reader enters scheduler locks and is not IRQ-safe.
    if (Processor::inDeviceHardIrq()) {
      return WouldBlock;
    }
#endif

    TerminationDeferral terminationDeferral;
    if (!m_Lock.tryAcquire()) {
      return WouldBlock;
    }

    if (m_Closing || m_WriteClosed) {
      m_Lock.release();
      return Closed;
    }

    if (m_Ring.count() >= m_Ring.capacity()) {
      m_Lock.release();
      return WouldBlock;
    }

    pushLocked(&obj, 1);
    m_Lock.release();
    return NoError;
  }

  /// Publishes each available chunk before waiting for more space.
  size_t write(const T* obj, size_t n, Time::Timestamp& timeout) {
    ActiveOperation operation(*this);
    if (!operation) {
      return 0;
    }

    if (n > m_Ring.capacity()) {
      n = m_Ring.capacity();
    }

    size_t written = 0;
    while (written < n) {
      if (waitForLocked(MailboxWait::Writing, timeout) != NoError) {
        break;
      }
      written += pushLocked(obj + written, n - written);
    }
    return written;
  }

  size_t write(const T* obj, size_t n) {
    Time::Timestamp timeout = Time::Infinity;
    return write(obj, n, timeout);
  }

  /**
   * Read one object from the ring buffer.
   *
   * On success, \p out contains the removed object and \p error is NoError.
   * On failure, \p out is reset to T() and \p error describes the failure.
   * The internal mutex is reacquired after a wait and released before this
   * function returns; no lock or reference into the ring escapes to the
   * caller. The Mailbox must outlive any blocked read.
   */
  MUST_USE_RESULT bool read(T& out, Time::Timestamp& timeout, Error& error) {
    ActiveOperation operation(*this);
    if (!operation) {
      out = T();
      error = Closed;
      return false;
    }

    out = T();
    if (!timeout && !m_Ring.count() && !readsClosed()) {
      error = Empty;
      return false;
    }
    error = waitForLocked(MailboxWait::Reading, timeout);
    if (error != NoError) {
      return false;
    }
    popLocked(&out, 1);
    return true;
  }

  MUST_USE_RESULT bool read(T& out, Error& error) {
    Time::Timestamp timeout = 0;
    return read(out, timeout, error);
  }

  /// Removes each available chunk before waiting for more data.
  size_t read(T* out, size_t n, Time::Timestamp& timeout) {
    ActiveOperation operation(*this);
    if (!operation) {
      return 0;
    }

    if (n > m_Ring.capacity()) {
      n = m_Ring.capacity();
    }

    size_t read = 0;
    while (read < n && timeout > 0) {
      if (waitForLocked(MailboxWait::Reading, timeout) != NoError) {
        out[read] = T();
        break;
      }
      read += popLocked(out + read, n - read);
    }
    return read;
  }

  size_t read(T* out, size_t n) {
    Time::Timestamp timeout = Time::Infinity;
    return read(out, n, timeout);
  }

  /// dataReady - is data ready for reading from the ring buffer?
  bool dataReady() {
    ActiveOperation operation(*this);
    if (!operation) {
      return false;
    }

    return m_Ring.count() > 0;
  }

  /// canWrite - is it possible to write to the ring buffer without blocking?
  bool canWrite() {
    ActiveOperation operation(*this);
    if (!operation) {
      return false;
    }

    return !m_Closing && !m_WriteClosed && m_Ring.count() < m_Ring.capacity();
  }

  /// waitFor - block until the given condition is true (readable/writeable)
  MUST_USE_RESULT bool waitFor(MailboxWait::WaitType wait, Time::Timestamp& timeout, Error& error) {
    error = NoError;
    ActiveOperation operation(*this);
    if (!operation) {
      error = Closed;
      return false;
    }

    error = waitForLocked(wait, timeout);
    return error == NoError;
  }

  bool waitFor(MailboxWait::WaitType wait, Time::Timestamp& timeout) {
    Error error = NoError;
    return waitFor(wait, timeout, error);
  }

  bool waitFor(MailboxWait::WaitType wait) {
    Time::Timestamp timeout = Time::Infinity;
    return waitFor(wait, timeout);
  }

 private:
  Error waitForLocked(MailboxWait::WaitType direction, Time::Timestamp& timeout) {
    const bool writing = direction == MailboxWait::Writing;
    while (true) {
      if (writing ? (m_Closing || m_WriteClosed) : readsClosed()) {
        return Closed;
      }
      if (writing ? m_Ring.count() < m_Ring.capacity() : m_Ring.count() != 0) {
        return NoError;
      }

      ConditionVariable::Error error = ConditionVariable::NoError;
      if (!waitForChange(writing ? m_WriteCondition : m_ReadCondition,
                         writing ? m_WriteWaiters : m_ReadWaiters, timeout, error)) {
        return errorFromConditionVariable(error);
      }
    }
  }

  size_t pushLocked(const T* items, size_t count) {
    const bool wasEmpty = !m_Ring.count();
    const size_t written = m_Ring.push(items, count);
    if (written) {
      m_Notifications.changed(wasEmpty, false);
      if (m_ReadWaiters) {
        if (written == 1) {
          m_ReadCondition.signal();
        } else {
          m_ReadCondition.broadcast();
        }
      }
    }
    return written;
  }

  size_t popLocked(T* items, size_t count) {
    const bool wasFull = m_Ring.count() == m_Ring.capacity();
    const size_t read = m_Ring.pop(items, count);
    if (read) {
      m_Notifications.changed(false, wasFull);
      if (m_WriteWaiters) {
        if (read == 1) {
          m_WriteCondition.signal();
        } else {
          m_WriteCondition.broadcast();
        }
      }
    }
    return read;
  }

  bool waitForChange(ConditionVariable& condition, size_t& waiters, Time::Timestamp& timeout,
                     ConditionVariable::Error& error) {
    ++waiters;
    const bool result = condition.wait(m_Lock, timeout, error);
    --waiters;
    return result;
  }

  bool beginOperation() {
    m_Lock.acquire();
    if (m_Closing) {
      m_Lock.release();
      return false;
    }

    ++m_ActiveOperations;
    return true;
  }

  void endOperation() {
    assert(m_ActiveOperations);
    --m_ActiveOperations;
    if (m_Closing && !m_ActiveOperations) {
      m_DrainCondition.broadcast();
    }
    m_Lock.release();
  }

  Error errorFromConditionVariable(ConditionVariable::Error err) {
    switch (err) {
      case ConditionVariable::TimedOut:
        return TimedOut;
      case ConditionVariable::Interrupted:
#if THREADS
        // ConditionVariable consumes the detailed reason as part of
        // its result contract. Preserve it for callers, such as
        // lwIP, whose compatibility API collapses all failures to a
        // timeout sentinel.
        Processor::information().getCurrentThread()->setInterruptionReason(
            Thread::InterruptedBySignal);
#endif
        return Interrupted;
      case ConditionVariable::TerminationDeferred:
        return ThreadTerminating;
      default:
        FATAL("invalid ConditionVariable::Error enum value for Mailbox");
    }

    return NoError;
  }

 protected:
  bool readsClosed() const {
    return m_Closing || (m_WriteClosed && !m_FinalPending && !m_Ring.count());
  }

  RingQueue<T, preallocatedSize> m_Ring;
  [[no_unique_address]] Notifications m_Notifications;
  bool m_Closing = false;
  bool m_WriteClosed = false;
  // Readers must not observe EOF while the final writer is waiting for space.
  bool m_FinalPending = false;

 private:
  Mutex m_Lock;
  ConditionVariable m_WriteCondition;
  ConditionVariable m_ReadCondition;
  ConditionVariable m_DrainCondition;
  size_t m_ActiveOperations = 0;
  // Protected by m_Lock, including enrolment and return from condition waits.
  size_t m_ReadWaiters = 0;
  size_t m_WriteWaiters = 0;
};

#endif
