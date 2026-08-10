/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef KERNEL_MACHINE_MACH_PC_LOCALAPICTLBSHOOTDOWN_H
#define KERNEL_MACHINE_MACH_PC_LOCALAPICTLBSHOOTDOWN_H

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/process/ExecutionContext.h"
#include "pedigree/kernel/processor/types.h"

/**
 * Serialises processor-control state and mutation-gate ownership.
 *
 * The phase bits are part of the same atomic word as the owning processor, so
 * no caller can observe a successful quiesce as ownerless between operations.
 */
class LocalApicProcessorControlOwner {
 public:
  static constexpr size_t MaxProcessors = 64;

  LocalApicProcessorControlOwner() : m_State(0) {}

  bool tryAcquire(size_t processor) {
    return processor < MaxProcessors &&
           m_State.compareAndSwap(0, processor + 1);
  }

  bool ownedBy(size_t processor) const {
    return processor < MaxProcessors &&
           (m_State.value() & ~PhaseMask) == processor + 1;
  }

  bool activeBy(size_t processor) const {
    return processor < MaxProcessors && m_State.value() == processor + 1;
  }

  bool markQuiesced(size_t processor) {
    return processor < MaxProcessors &&
           m_State.compareAndSwap(processor + 1, QuiescedBit | (processor + 1));
  }

  bool claimQuiesced(size_t processor) {
    return processor < MaxProcessors &&
           m_State.compareAndSwap(QuiescedBit | (processor + 1), processor + 1);
  }

  bool quiescedBy(size_t processor) const {
    return processor < MaxProcessors &&
           m_State.value() == (QuiescedBit | (processor + 1));
  }

  bool markTerminal(size_t processor) {
    return processor < MaxProcessors &&
           m_State.compareAndSwap(processor + 1, TerminalBit | (processor + 1));
  }

  bool claimTerminal(size_t processor) {
    return processor < MaxProcessors &&
           m_State.compareAndSwap(TerminalBit | (processor + 1), processor + 1);
  }

  bool terminalBy(size_t processor) const {
    return processor < MaxProcessors &&
           m_State.value() == (TerminalBit | (processor + 1));
  }

  bool release(size_t processor) {
    return processor < MaxProcessors &&
           m_State.compareAndSwap(processor + 1, 0);
  }

  bool owned() const {
    return m_State.value() != 0;
  }

 private:
  static constexpr size_t QuiescedBit = size_t(1) << (sizeof(size_t) * 8 - 1);
  static constexpr size_t TerminalBit = QuiescedBit >> 1;
  static constexpr size_t PhaseMask = QuiescedBit | TerminalBit;
  Atomic<size_t> m_State;
};

/**
 * Admission barrier spanning a page-table mutation and its shootdown.
 *
 * Closing first and then waiting for drain gives processor retirement a
 * stable point at which no CPU can be between changing a PTE and publishing
 * the corresponding invalidation.
 */
class LocalApicTlbMutationGate {
 public:
  LocalApicTlbMutationGate() : m_Closed(false), m_Active(0) {}

  bool tryEnter() {
    if (m_Closed) {
      return false;
    }

    m_Active += 1;
    if (m_Closed) {
      leave();
      return false;
    }
    return true;
  }

  bool leave() {
    size_t active = m_Active.value();
    while (active) {
      if (m_Active.compareAndSwap(active, active - 1)) {
        return true;
      }
      active = m_Active.value();
    }
    return false;
  }

  bool close() {
    return m_Closed.compareAndSwap(false, true);
  }

  bool reopen() {
    return !m_Active && m_Closed.compareAndSwap(true, false);
  }

  /**
   * Abandon a close before any processor-control state has changed.
   *
   * A debugger can interrupt the local processor while it owns an admission
   * lease. In that case drain cannot complete until the interrupted code is
   * resumed, so the failed quiesce attempt must restore admission while that
   * existing lease is still active.
   */
  bool cancelClose() {
    return m_Closed.compareAndSwap(true, false);
  }

  bool closed() const {
    return m_Closed;
  }

  bool drained() const {
    return !m_Active;
  }

  size_t active() const {
    return m_Active.value();
  }

 private:
  Atomic<bool> m_Closed;
  Atomic<size_t> m_Active;
};

/**
 * Allocation-free publication state for one synchronous TLB shootdown.
 *
 * A processor which is spinning with maskable interrupts disabled can service
 * the published transaction explicitly. Service leases keep a completed
 * generation from being reused while an old observer can still acknowledge
 * it.
 */
class LocalApicTlbShootdown {
 public:
  static constexpr size_t MaxProcessors = 64;

  struct Service {
    constexpr Service() : generation(0), address(0), processorMask(0) {}

    size_t generation;
    uintptr_t address;
    uint64_t processorMask;
  };

  LocalApicTlbShootdown()
      : m_Owner(false),
        m_Generation(0),
        m_NextGeneration(0),
        m_Address(0),
        m_Expected(0),
        m_Acknowledged(0),
        m_Servicing(0) {}

  static constexpr bool supportsContext(ExecutionContext context) {
    return context == ExecutionContext::WaitableThread ||
           context == ExecutionContext::AtomicThread;
  }

  static constexpr bool onlyCurrentProcessorServiceable(
      size_t processorCount, size_t terminalProcessors) {
    return processorCount && terminalProcessors >= (processorCount - 1);
  }

  bool tryAcquire() {
    return m_Owner.compareAndSwap(false, true);
  }

  bool publish(uintptr_t address, size_t processor, size_t processorCount) {
    uint64_t expected = 0;
    if (!m_Owner || m_Generation || m_Servicing || processor >= processorCount ||
        !processorMask(processorCount, expected)) {
      return false;
    }

    size_t generation = m_NextGeneration.value() + 1;
    if (!generation) {
      generation = 1;
    }

    m_Address = address;
    m_Expected = expected;
    m_Acknowledged = uint64_t(1) << processor;
    m_NextGeneration = generation;
    // Publishing the generation last makes the complete request visible to
    // interrupt and cooperative service paths in one step.
    m_Generation = generation;
    return true;
  }

  bool beginService(size_t processor, Service& service) {
    if (processor >= MaxProcessors) {
      return false;
    }

    const size_t generation = m_Generation.value();
    if (!generation) {
      return false;
    }
    const uint64_t processorBit = uint64_t(1) << processor;
    if (m_Acknowledged.value() & processorBit) {
      return false;
    }

    m_Servicing += 1;
    if (m_Generation.value() != generation) {
      m_Servicing -= 1;
      return false;
    }

    service.generation = generation;
    service.address = m_Address.value();
    service.processorMask = processorBit;
    return true;
  }

  bool finishService(Service& service) {
    if (!service.generation) {
      return false;
    }

    const bool current = m_Generation.value() == service.generation;
    if (current) {
      m_Acknowledged |= service.processorMask;
    }
    service = Service();
    m_Servicing -= 1;
    return current;
  }

  bool complete() const {
    const uint64_t expected = m_Expected.value();
    return m_Generation && expected && (m_Acknowledged.value() & expected) == expected;
  }

  void close() {
    m_Generation = 0;
  }

  bool drained() const {
    return !m_Servicing;
  }

  bool release() {
    if (m_Generation || m_Servicing) {
      return false;
    }
    return m_Owner.compareAndSwap(true, false);
  }

  bool owned() const {
    return m_Owner;
  }

  size_t generation() const {
    return m_Generation.value();
  }

  uintptr_t address() const {
    return m_Address.value();
  }

  uint64_t expectedMask() const {
    return m_Expected.value();
  }

  uint64_t acknowledgedMask() const {
    return m_Acknowledged.value();
  }

  size_t servicing() const {
    return m_Servicing.value();
  }

 private:
  static bool processorMask(size_t processorCount, uint64_t& mask) {
    if (!processorCount || processorCount > MaxProcessors) {
      return false;
    }
    mask = processorCount == MaxProcessors ? ~uint64_t(0)
                                           : (uint64_t(1) << processorCount) - 1;
    return true;
  }

  Atomic<bool> m_Owner;
  Atomic<size_t> m_Generation;
  Atomic<size_t> m_NextGeneration;
  Atomic<uintptr_t> m_Address;
  Atomic<uint64_t> m_Expected;
  Atomic<uint64_t> m_Acknowledged;
  Atomic<size_t> m_Servicing;
};

#endif
