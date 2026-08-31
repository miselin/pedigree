/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef USB_HCD_PORTCHANGEREQUEST_H
#define USB_HCD_PORTCHANGEREQUEST_H
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/utilities/RequestQueue.h"

#include <config.h>

namespace UsbHcd {
constexpr size_t EhciRootPortCount = 15;
constexpr size_t OhciRootPortCount = 15;
constexpr size_t UhciRootPortCount = 8;

inline bool validEhciRootPortCount(size_t count) {
  return count <= EhciRootPortCount;
}

inline bool validOhciRootPortCount(size_t count) {
  return count && count <= OhciRootPortCount;
}

inline bool validUhciRootPortCount(size_t count) {
  return count <= UhciRootPortCount;
}

/**
 * Builds a read/modify/write value without echoing unrelated W1C bits.
 */
template <typename T>
inline T selectiveW1cValue(T status, T changeMask, T acknowledgeMask) {
  return static_cast<T>((status & ~changeMask) | (acknowledgeMask & changeMask));
}

/**
 * Port publications waiting for their controller-level status W1C.
 *
 * Controllers access this only while holding their IRQ serialization lock.
 */
class DeferredPortChanges {
 public:
  DeferredPortChanges() {
    for (size_t i = 0; i < EhciRootPortCount; ++i) {
      m_Generations[i] = 0;
    }
  }

  void defer(size_t port, size_t generation) {
    if (port < EhciRootPortCount && m_Generations[port] < generation) {
      m_Generations[port] = generation;
    }
  }

  size_t release(size_t port) {
    if (port >= EhciRootPortCount) {
      return 0;
    }

    const size_t generation = m_Generations[port];
    m_Generations[port] = 0;
    return generation;
  }

  bool empty() const {
    for (size_t i = 0; i < EhciRootPortCount; ++i) {
      if (m_Generations[i]) {
        return false;
      }
    }
    return true;
  }

 private:
  size_t m_Generations[EhciRootPortCount];
};

/**
 * One preallocated RequestQueue publication slot for one root-hub port.
 *
 * configure() fixes the queue payload before the interrupt source is enabled.
 * Each observe() returns a generation that the controller acknowledges only
 * after its hardware W1C has been flushed. Workers cannot sample the port
 * before that generation is acknowledged. Further observations coalesce into
 * one latest-generation follow-up publication without waiting for the current
 * worker to retire its token.
 */
class PortChangeRequest {
 public:
  enum class Result {
    Accepted,
    Coalesced,
    TokenBusy,
    QueueStopped,
    QueueFull,
    InvalidPriority,
  };

  struct Observation {
    Observation(Result result, size_t generation) : result(result), generation(generation) {}

    Result result;
    size_t generation;
  };

  class Completion {
   public:
    Completion(PortChangeRequest& request, size_t generation)
        : m_Request(request),
          m_Generation(generation),
          m_Ready(request.waitUntilAcknowledged(generation)) {}

    ~Completion() {
      m_Request.consume(m_Generation);
    }

    explicit operator bool() const {
      return m_Ready;
    }

   private:
    NOT_COPYABLE_OR_ASSIGNABLE(Completion);
    PortChangeRequest& m_Request;
    size_t m_Generation;
    bool m_Ready;
  };

  PortChangeRequest()
      : m_Request(requestReleased, this),
        m_Queue(nullptr),
        m_Priority(0),
        m_Configured(0),
        m_Observed(0),
        m_Acknowledged(0),
        m_Consumed(0),
        m_Stopping(0),
        m_ReleaseFailures(0),
        m_AcknowledgementWaiters() {
    for (size_t i = 0; i < 7; ++i) {
      m_Parameters[i] = 0;
    }
  }

  bool configure(RequestQueue& queue, size_t priority, uint64_t p1 = 0, uint64_t p2 = 0,
                 uint64_t p3 = 0, uint64_t p4 = 0, uint64_t p5 = 0, uint64_t p6 = 0,
                 uint64_t p7 = 0) {
    if (m_Configured || priority >= REQUEST_QUEUE_NUM_PRIORITIES || !m_Request.isAvailable()) {
      return false;
    }

    m_Queue = &queue;
    m_Priority = priority;
    m_Parameters[0] = p1;
    m_Parameters[1] = p2;
    m_Parameters[2] = p3;
    m_Parameters[3] = p4;
    m_Parameters[4] = p5;
    m_Parameters[5] = p6;
    m_Parameters[6] = p7;
    m_Configured = 1;
    return true;
  }

  Observation observe() {
#if !THREADS
    return Observation(Result::QueueStopped, 0);
#else
    if (!m_Configured || m_Stopping) {
      return Observation(Result::QueueStopped, 0);
    }

    const size_t generation = (m_Observed += 1);
    auto result = publishGeneration(generation, true);
    return Observation(result, generation);
#endif
  }

  void acknowledge(size_t generation) {
#if THREADS
    auto guard = m_AcknowledgementWaiters.acquire();
#endif
    advance(m_Acknowledged, generation);
#if THREADS
    guard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(this));
#endif
  }

  /**
   * Releases workers waiting for hardware acknowledgement and suppresses
   * follow-up publication.
   *
   * The controller must first close and drain the interrupt or timer source
   * that calls observe(). No observe() may overlap or follow this call.
   */
  void stopAfterQuiesce() {
#if THREADS
    auto guard = m_AcknowledgementWaiters.acquire();
#endif
    m_Stopping = 1;
#if THREADS
    guard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(this));
#endif
  }

  void cancel(size_t generation) {
    consume(generation);
  }

  bool isIdle() const {
    return m_Request.isAvailable() && (m_Stopping || m_Consumed >= m_Observed);
  }

  size_t observedGeneration() const {
    return m_Observed;
  }

  static bool canAcknowledge(Result result) {
    return result == Result::Accepted || result == Result::Coalesced;
  }

  bool hasPublicationFailure() const {
    return static_cast<size_t>(m_ReleaseFailures) != 0;
  }

 private:
  static void advance(Atomic<size_t>& value, size_t target) {
    while (value < target) {
      const size_t current = value;
      if (current >= target || value.compareAndSwap(current, target)) {
        return;
      }
    }
  }

  static Result translate(RequestQueue::PreallocatedPublishResult result) {
    using QueueResult = RequestQueue::PreallocatedPublishResult;
    switch (result) {
      case QueueResult::Accepted:
        return Result::Accepted;
      case QueueResult::QueueStopped:
        return Result::QueueStopped;
      case QueueResult::QueueFull:
        return Result::QueueFull;
      case QueueResult::InvalidPriority:
        return Result::InvalidPriority;
      case QueueResult::TokenBusy:
        return Result::TokenBusy;
    }
    return Result::TokenBusy;
  }

  static void requestReleased(void* context) {
    reinterpret_cast<PortChangeRequest*>(context)->released();
  }

  void released() {
#if THREADS
    // stopAfterQuiesce must either precede this publication or wait until
    // it is fully visible to RequestQueue teardown.
    auto guard = m_AcknowledgementWaiters.acquire();
#endif
    if (m_Stopping) {
      return;
    }

    const size_t observed = m_Observed;
    if (observed <= m_Consumed) {
      return;
    }

    const Result result = publishGeneration(observed, false);
    if (result != Result::Accepted && result != Result::Coalesced) {
      m_ReleaseFailures += 1;
    }
  }

  Result publishGeneration(size_t generation, bool tryIdle) {
    auto result = m_Queue->republishPreallocatedWhileReleasing(
        m_Request, m_Priority, m_Parameters[0], m_Parameters[1], m_Parameters[2], m_Parameters[3],
        m_Parameters[4], m_Parameters[5], m_Parameters[6], generation);
    if (tryIdle && result == RequestQueue::PreallocatedPublishResult::TokenBusy) {
      result = m_Queue->publishPreallocated(m_Request, m_Priority, m_Parameters[0], m_Parameters[1],
                                            m_Parameters[2], m_Parameters[3], m_Parameters[4],
                                            m_Parameters[5], m_Parameters[6], generation);
    }

    if (result == RequestQueue::PreallocatedPublishResult::TokenBusy) {
      return Result::Coalesced;
    }
    return translate(result);
  }

  bool waitUntilAcknowledged(size_t generation) {
#if THREADS
    while (true) {
      auto guard = m_AcknowledgementWaiters.acquire();
      if (m_Stopping) {
        return false;
      }
      if (m_Acknowledged >= generation) {
        return true;
      }

      const WaitQueue::WakeReason reason = guard.waitForCompletion(
          WaitQueue::Channel(this), Thread::CallbackDrain, reinterpret_cast<uintptr_t>(this));
      (void)reason;
    }
#else
    return !m_Stopping && m_Acknowledged >= generation;
#endif
  }

  void consume(size_t generation) {
    advance(m_Consumed, generation);
  }

  RequestQueue::PreallocatedRequest m_Request;
  RequestQueue* m_Queue;
  size_t m_Priority;
  uint64_t m_Parameters[7];
  Atomic<size_t> m_Configured;
  Atomic<size_t> m_Observed;
  Atomic<size_t> m_Acknowledged;
  Atomic<size_t> m_Consumed;
  Atomic<size_t> m_Stopping;
  Atomic<size_t> m_ReleaseFailures;
  WaitQueue m_AcknowledgementWaiters;
};
}  // namespace UsbHcd

#endif
