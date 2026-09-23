/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"

OperationBarrier::Lease::Lease() : m_Barrier(nullptr) {}

OperationBarrier::Lease::Lease(OperationBarrier* barrier) : m_Barrier(barrier) {}

OperationBarrier::Lease::Lease(Lease&& other) noexcept : m_Barrier(other.m_Barrier) {
  other.m_Barrier = nullptr;
}

OperationBarrier::Lease::~Lease() {
  reset();
}

OperationBarrier::Lease& OperationBarrier::Lease::operator=(
    OperationBarrier::Lease&& other) noexcept {
  if (this != &other) {
    reset();
    m_Barrier = other.m_Barrier;
    other.m_Barrier = nullptr;
  }
  return *this;
}

void OperationBarrier::Lease::reset() {
  if (m_Barrier) {
    OperationBarrier* barrier = m_Barrier;
    m_Barrier = nullptr;
    barrier->leave();
  }
}

OperationBarrier::OperationBarrier() : m_Waiters(), m_State(0) {}

OperationBarrier::~OperationBarrier() {
  auto guard = m_Waiters.acquire();
  if (__atomic_load_n(&m_State, __ATOMIC_ACQUIRE) != Closed) {
    panic("OperationBarrier destroyed before close-and-drain completed.");
  }
}

bool OperationBarrier::tryEnter() {
  size_t state = __atomic_load_n(&m_State, __ATOMIC_RELAXED);
  do {
    if (state & Closed) {
      return false;
    }
    if (state == CountMask) {
      panic("OperationBarrier operation count overflow.");
    }
  } while (!__atomic_compare_exchange_n(&m_State, &state, state + 1, true, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED));
  return true;
}

bool OperationBarrier::tryAcquire(Lease& lease) {
  if (!tryEnter()) {
    lease = Lease();
    return false;
  }

  lease = Lease(this);
  return true;
}

void OperationBarrier::leave() {
  size_t state = __atomic_load_n(&m_State, __ATOMIC_RELAXED);
  do {
    if (!(state & CountMask)) {
      panic("OperationBarrier operation count underflow.");
    }
    if (state == (Closed | 1)) {
      // Publish the final closed release under the queue lock, so a drainer
      // cannot destroy the barrier while we still have to wake its waiters.
      auto guard = m_Waiters.acquire();
      __atomic_fetch_sub(&m_State, size_t{1}, __ATOMIC_ACQ_REL);
      guard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(this));
      return;
    }
  } while (!__atomic_compare_exchange_n(&m_State, &state, state - 1, true, __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED));
  // A concurrent close can now drain and destroy us; do not touch the barrier.
}

void OperationBarrier::close() {
  __atomic_fetch_or(&m_State, Closed, __ATOMIC_ACQ_REL);
}

bool OperationBarrier::tryCloseIfIdle() {
  auto guard = m_Waiters.acquire();
  size_t state = 0;
  return __atomic_compare_exchange_n(&m_State, &state, Closed, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE) ||
         state == Closed;
}

void OperationBarrier::wait() {
  TerminationDeferral terminationDeferral;
  while (true) {
    auto guard = m_Waiters.acquire();
    const size_t state = __atomic_load_n(&m_State, __ATOMIC_ACQUIRE);
    if (!(state & Closed)) {
      panic("OperationBarrier::wait called before close.");
    }
    if (!(state & CountMask)) {
      return;
    }

    const WaitQueue::WakeReason reason =
        guard.waitForCompletion(WaitQueue::Channel(this), Thread::CallbackDrain,
                                reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    (void)reason;
  }
}

void OperationBarrier::closeAndWait() {
  close();
  wait();
}

bool OperationBarrier::isOpen() {
  return !(__atomic_load_n(&m_State, __ATOMIC_ACQUIRE) & Closed);
}

bool OperationBarrier::isClosedAndDrained() {
  auto guard = m_Waiters.acquire();
  return __atomic_load_n(&m_State, __ATOMIC_ACQUIRE) == Closed;
}
