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

OperationBarrier::OperationBarrier() : m_Waiters(), m_Open(true), m_ActiveOperations(0) {}

OperationBarrier::~OperationBarrier() {
  auto guard = m_Waiters.acquire();
  if (m_Open || m_ActiveOperations) {
    panic("OperationBarrier destroyed before close-and-drain completed.");
  }
}

bool OperationBarrier::tryEnter() {
  auto guard = m_Waiters.acquire();
  if (!m_Open) {
    return false;
  }

  ++m_ActiveOperations;
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
  auto guard = m_Waiters.acquire();
  if (!m_ActiveOperations) {
    panic("OperationBarrier operation count underflow.");
  }
  --m_ActiveOperations;
  if (!m_Open && !m_ActiveOperations) {
    guard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(this));
  }
}

void OperationBarrier::close() {
  auto guard = m_Waiters.acquire();
  m_Open = false;
}

void OperationBarrier::wait() {
  TerminationDeferral terminationDeferral;
  while (true) {
    auto guard = m_Waiters.acquire();
    if (m_Open) {
      panic("OperationBarrier::wait called before close.");
    }
    if (!m_ActiveOperations) {
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
  auto guard = m_Waiters.acquire();
  return m_Open;
}

bool OperationBarrier::isClosedAndDrained() {
  auto guard = m_Waiters.acquire();
  return !m_Open && !m_ActiveOperations;
}
