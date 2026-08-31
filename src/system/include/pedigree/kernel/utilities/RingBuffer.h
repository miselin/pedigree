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
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/new"

#include <config.h>

#if THREADS
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#endif

/// \todo rewrite this in the same way as TcpBuffer!

namespace RingBufferWait {
enum WaitType { Reading, Writing };
}

/**
 * \brief Utility class to provide a ring buffer.
 *
 * Using this class provides safety in accessing the ring buffer as well as
 * the ability to check (with and without blocking) whether the buffer can
 * be read or written to at this time.
 *
 * The idea of the waitFor function is to provide a way for applications
 * desiring integration with a select()-style interface to block until the
 * condition is met.
 */
template <class T, size_t preallocatedSize = 0>
class EXPORTED_PUBLIC RingBuffer {
 private:
  class ActiveOperation {
   public:
    explicit ActiveOperation(RingBuffer& buffer)
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
    RingBuffer* m_Buffer;
  };

 public:
  enum Error {
    NoError,

    // RingBuffer is empty and a zero timeout was specified
    Empty,

    // A nonblocking operation could not acquire the lock or found no space
    WouldBlock,

    // ConditionVariable failure modes
    TimedOut,
    Interrupted,
    ThreadTerminating,

    // RingBuffer has closed and will not admit another operation.
    Closed,
  };

  RingBuffer();  // Not implemented, use RingBuffer(size_t)

  /// Constructor - pass in the desired size of the ring buffer.
  RingBuffer(size_t ringSize)
      : m_RingSize(ringSize),
        m_WriteCondition(),
        m_ReadCondition(),
        m_Ring(),
        m_PreallocatedRing(),
        m_RingRead(0),
        m_RingWrite(0),
        m_RingCount(0),
        m_Lock(),
        m_DrainCondition(),
        m_Closing(false),
        m_WriteClosed(false),
        m_ActiveOperations(0) {
    assert(!preallocatedSize || ringSize <= preallocatedSize);
  }

  /// Destructor - closes the ring and drains every admitted operation.
  ~RingBuffer() {
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
      m_ReadCondition.broadcast();
      m_WriteCondition.broadcast();
      notifyMonitorsLocked();
    }

    while (m_ActiveOperations) {
      m_DrainCondition.waitForCompletion(m_Lock);
    }
    m_Lock.release();
  }

  /// write - write a byte to the ring buffer.
  Error write(const T& obj, Time::Timestamp& timeout) {
    ActiveOperation operation(*this);
    if (!operation) {
      return Closed;
    }

    m_Lock.acquire();
    while (true) {
      if (m_Closing || m_WriteClosed) {
        m_Lock.release();
        return Closed;
      }

      // Wait for room in the buffer if we're full.
      if (ringCountLocked() >= m_RingSize) {
        ConditionVariable::Error error = ConditionVariable::NoError;
        if (!m_WriteCondition.wait(m_Lock, timeout, error)) {
          if (ConditionVariable::mutexAcquired(error)) {
            m_Lock.release();
          }
          return errorFromConditionVariable(error);
        }

        continue;
      }

      pushBackLocked(obj);
      break;
    }

    m_Lock.release();

    notifyMonitors();

    // Signal readers waiting for objects to read.
    m_ReadCondition.signal();

    return NoError;
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

    TerminationDeferral terminationDeferral;
    m_Lock.acquire();
    if (m_WriteClosed) {
      m_Lock.release();
      return false;
    }

    m_WriteClosed = true;
    m_WriteCondition.broadcast();
    while (!m_Closing && ringCountLocked() >= m_RingSize) {
      m_WriteCondition.waitForCompletion(m_Lock);
    }

    if (m_Closing) {
      m_Lock.release();
      return false;
    }

    pushBackLocked(obj);
    notifyMonitorsLocked();
    m_ReadCondition.broadcast();
    m_Lock.release();
    return true;
  }

  /**
   * Atomically writes one object without waiting for lock ownership or space.
   * A WouldBlock result leaves the ring unchanged.
   */
  Error tryWrite(const T& obj) {
#if THREADS
    // A failed ISR post is part of lwIP's mailbox contract. Reaching the
    // normal notification path would enter WaitQueue and scheduler locks.
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

    if (ringCountLocked() >= m_RingSize) {
      m_Lock.release();
      return WouldBlock;
    }

    pushBackLocked(obj);
    notifyMonitorsLocked();
    m_ReadCondition.signal();
    m_Lock.release();
    return NoError;
  }

  /// write - write the given number of objects to the ring buffer.
  size_t write(const T* obj, size_t n, Time::Timestamp& timeout) {
    ActiveOperation operation(*this);
    if (!operation) {
      return 0;
    }

    if (n > m_RingSize)
      n = m_RingSize;

    size_t i;
    for (i = 0; i < n; ++i) {
      if (write(obj[i], timeout) != NoError) {
        /// \todo this hides the error from a caller
        break;
      }
    }

    return i;  // return actual count written
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
   * caller. The RingBuffer must outlive any blocked read.
   */
  MUST_USE_RESULT bool read(T& out, Time::Timestamp& timeout, Error& error) {
    out = T();
    error = NoError;

    ActiveOperation operation(*this);
    if (!operation) {
      error = Closed;
      return false;
    }

    m_Lock.acquire();
    if (m_Closing) {
      m_Lock.release();
      error = Closed;
      return false;
    }

    if (timeout == 0) {
      if (!ringCountLocked()) {
        m_Lock.release();
        error = m_WriteClosed ? Closed : Empty;
        return false;
      }
    }

    while (true) {
      if (m_Closing || (m_WriteClosed && !ringCountLocked())) {
        m_Lock.release();
        error = Closed;
        return false;
      }

      // Wait for room in the buffer if we're full.
      if (ringCountLocked() == 0) {
        ConditionVariable::Error conditionError = ConditionVariable::NoError;
        if (!m_ReadCondition.wait(m_Lock, timeout, conditionError)) {
          if (ConditionVariable::mutexAcquired(conditionError)) {
            m_Lock.release();
          }
          error = errorFromConditionVariable(conditionError);
          return false;
        }

        continue;
      }

      out = popFrontLocked();
      break;
    }

    m_Lock.release();

    notifyMonitors();

    // Signal writers that may be waiting for buffer space.
    m_WriteCondition.signal();

    return true;
  }

  MUST_USE_RESULT bool read(T& out, Error& error) {
    Time::Timestamp timeout = 0;
    return read(out, timeout, error);
  }

  /// read - read up to the given number of objects from the ring buffer
  size_t read(T* out, size_t n, Time::Timestamp& timeout) {
    ActiveOperation operation(*this);
    if (!operation) {
      return 0;
    }

    if (n > m_RingSize)
      n = m_RingSize;

    size_t i;
    for (i = 0; i < n && timeout > 0; ++i) {
      Error error = NoError;
      if (!read(out[i], timeout, error)) {
        return i;
      }
    }

    return i;
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

    LockGuard<Mutex> guard(m_Lock);
    return ringCountLocked() > 0;
  }

  /// canWrite - is it possible to write to the ring buffer without blocking?
  bool canWrite() {
    ActiveOperation operation(*this);
    if (!operation) {
      return false;
    }

    LockGuard<Mutex> guard(m_Lock);
    return !m_Closing && !m_WriteClosed && ringCountLocked() < m_RingSize;
  }

  /// waitFor - block until the given condition is true (readable/writeable)
  MUST_USE_RESULT bool waitFor(RingBufferWait::WaitType wait, Time::Timestamp& timeout,
                               Error& error) {
    error = NoError;
    ActiveOperation operation(*this);
    if (!operation) {
      error = Closed;
      return false;
    }

    m_Lock.acquire();
    if (wait == RingBufferWait::Writing) {
      while (true) {
        if (m_Closing || m_WriteClosed) {
          m_Lock.release();
          error = Closed;
          return false;
        }

        if (ringCountLocked() < m_RingSize) {
          m_Lock.release();
          return true;
        }

        ConditionVariable::Error conditionError = ConditionVariable::NoError;
        if (!m_WriteCondition.wait(m_Lock, timeout, conditionError)) {
          if (ConditionVariable::mutexAcquired(conditionError)) {
            m_Lock.release();
          }
          error = errorFromConditionVariable(conditionError);
          return false;
        }
      }
    } else {
      while (true) {
        if (m_Closing || (m_WriteClosed && !ringCountLocked())) {
          m_Lock.release();
          error = Closed;
          return false;
        }

        if (ringCountLocked()) {
          m_Lock.release();
          return true;
        }

        ConditionVariable::Error conditionError = ConditionVariable::NoError;
        if (!m_ReadCondition.wait(m_Lock, timeout, conditionError)) {
          if (ConditionVariable::mutexAcquired(conditionError)) {
            m_Lock.release();
          }
          error = errorFromConditionVariable(conditionError);
          return false;
        }
      }
    }

    m_Lock.release();
    return false;
  }

  bool waitFor(RingBufferWait::WaitType wait, Time::Timestamp& timeout) {
    Error error = NoError;
    return waitFor(wait, timeout, error);
  }

  bool waitFor(RingBufferWait::WaitType wait) {
    Time::Timestamp timeout = Time::Infinity;
    return waitFor(wait, timeout);
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
    ActiveOperation operation(*this);
    if (!operation) {
      Event::SendLease registration;
      if (pEvent->tryAcquireRegistration(registration)) {
        EMIT_IF(THREADS) {
          pThread->sendEvent(pEvent);
        }
      }
      return false;
    }

    LockGuard<Mutex> guard(m_Lock);
    Event::SendLease registration;
    if (!pEvent->tryAcquireRegistration(registration)) {
      return false;
    }

    if (m_Closing || (m_WriteClosed && !ringCountLocked())) {
      EMIT_IF(THREADS) {
        pThread->sendEvent(pEvent);
      }
      return false;
    }

    m_MonitorTargets.pushBack(new MonitorTarget(pThread, pEvent, pedigree_std::move(registration)));
    return true;
  }

  /// Add a Semaphore to be signaled when readiness changes or the ring closes.
  bool monitor(Semaphore* pSemaphore) {
    ActiveOperation operation(*this);
    if (!operation) {
      EMIT_IF(THREADS) {
        pSemaphore->release();
      }
      return false;
    }

    LockGuard<Mutex> guard(m_Lock);
    if (m_Closing || (m_WriteClosed && !ringCountLocked())) {
      EMIT_IF(THREADS) {
        pSemaphore->release();
      }
      return false;
    }

    m_MonitorTargets.pushBack(new MonitorTarget(pSemaphore));
    return true;
  }

  /// Cull all monitor targets pointing to \p pThread.
  void cullMonitorTargets(Thread* pThread) {
    ActiveOperation operation(*this);
    if (!operation) {
      return;
    }

    LockGuard<Mutex> guard(m_Lock);
    typename List<MonitorTarget*>::Iterator it = m_MonitorTargets.begin();
    while (it != m_MonitorTargets.end()) {
      MonitorTarget* pMT = *it;
      if (pMT->pThread == pThread) {
        delete pMT;
        it = m_MonitorTargets.erase(it);
      } else {
        ++it;
      }
    }
  }

  /// Cull all monitor targets pointing to \p pSemaphore.
  void cullMonitorTargets(Semaphore* pSemaphore) {
    ActiveOperation operation(*this);
    if (!operation) {
      return;
    }

    LockGuard<Mutex> guard(m_Lock);
    typename List<MonitorTarget*>::Iterator it = m_MonitorTargets.begin();
    while (it != m_MonitorTargets.end()) {
      MonitorTarget* pMT = *it;
      if (pMT->pSemaphore == pSemaphore) {
        delete pMT;
        it = m_MonitorTargets.erase(it);
      } else {
        ++it;
      }
    }
  }

  /// Cull all monitor targets pointing to \p pEvent.
  void cullMonitorTargets(Event* pEvent) {
    ActiveOperation operation(*this);
    if (!operation) {
      return;
    }

    LockGuard<Mutex> guard(m_Lock);
    typename List<MonitorTarget*>::Iterator it = m_MonitorTargets.begin();
    while (it != m_MonitorTargets.end()) {
      MonitorTarget* pMT = *it;
      if (pMT->pEvent == pEvent) {
        delete pMT;
        it = m_MonitorTargets.erase(it);
      } else {
        ++it;
      }
    }
  }

 private:
  size_t ringCountLocked() const {
    if (preallocatedSize) {
      return m_RingCount;
    }
    return m_Ring.count();
  }

  void pushBackLocked(const T& obj) {
    assert(ringCountLocked() < m_RingSize);
    if (preallocatedSize) {
      m_PreallocatedRing[m_RingWrite] = obj;
      m_RingWrite = (m_RingWrite + 1) % m_RingSize;
      ++m_RingCount;
    } else {
      m_Ring.pushBack(obj);
    }
  }

  T popFrontLocked() {
    assert(ringCountLocked());
    if (preallocatedSize) {
      T obj = m_PreallocatedRing[m_RingRead];
      m_RingRead = (m_RingRead + 1) % m_RingSize;
      --m_RingCount;
      return obj;
    }
    return m_Ring.popFront();
  }

  /// Trigger event for threads waiting on us.
  void notifyMonitors() {
    LockGuard<Mutex> guard(m_Lock);
    notifyMonitorsLocked();
  }

  void notifyMonitorsLocked() {
    for (typename List<MonitorTarget*>::Iterator it = m_MonitorTargets.begin();
         it != m_MonitorTargets.end(); it++) {
      MonitorTarget* pMT = *it;

      if (pMT->pThread) {
        EMIT_IF(THREADS) {
          pMT->pThread->sendEvent(pMT->pEvent);
        }
      } else if (pMT->pSemaphore) {
        EMIT_IF(THREADS) {
          pMT->pSemaphore->release();
        }
      }
      delete pMT;
    }
    m_MonitorTargets.clear();
  }

  bool beginOperation() {
    LockGuard<Mutex> guard(m_Lock);
    if (m_Closing) {
      return false;
    }

    ++m_ActiveOperations;
    return true;
  }

  void endOperation() {
    LockGuard<Mutex> guard(m_Lock);
    assert(m_ActiveOperations);
    --m_ActiveOperations;
    if (m_Closing && !m_ActiveOperations) {
      m_DrainCondition.broadcast();
    }
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
        FATAL("invalid ConditionVariable::Error enum value for RingBuffer");
    }

    return NoError;
  }

  size_t m_RingSize;

  ConditionVariable m_WriteCondition;
  ConditionVariable m_ReadCondition;

  List<T> m_Ring;
  T m_PreallocatedRing[preallocatedSize ? preallocatedSize : 1];
  size_t m_RingRead;
  size_t m_RingWrite;
  size_t m_RingCount;

  Mutex m_Lock;

  ConditionVariable m_DrainCondition;
  bool m_Closing;
  bool m_WriteClosed;
  size_t m_ActiveOperations;

  struct MonitorTarget {
    MonitorTarget(Thread* pT, Event* pE, Event::SendLease registration)
        : pThread(pT),
          pEvent(pE),
          pSemaphore(nullptr),
          eventRegistration(pedigree_std::move(registration)) {}

    explicit MonitorTarget(Semaphore* pS)
        : pThread(nullptr), pEvent(nullptr), pSemaphore(pS), eventRegistration() {}

    Thread* pThread;
    Event* pEvent;
    Semaphore* pSemaphore;
    Event::SendLease eventRegistration;
  };

  List<MonitorTarget*> m_MonitorTargets;
};

extern template class RingBuffer<char>;   // IWYU pragma: keep
extern template class RingBuffer<void*>;  // IWYU pragma: keep

#endif
