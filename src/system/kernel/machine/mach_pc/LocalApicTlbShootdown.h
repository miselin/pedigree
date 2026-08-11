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
    return processor < MaxProcessors && m_State.compareAndSwap(0, processor + 1);
  }

  bool ownedBy(size_t processor) const {
    return processor < MaxProcessors && (m_State.value() & ~PhaseMask) == processor + 1;
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

  /** Adopt a completed reversible pause for irreversible terminal control. */
  bool claimAnyQuiesced(size_t processor) {
    if (processor >= MaxProcessors) {
      return false;
    }

    size_t state = m_State.value();
    while ((state & QuiescedBit) && !(state & TerminalBit)) {
      if (m_State.compareAndSwap(state, processor + 1)) {
        return true;
      }
      state = m_State.value();
    }
    return false;
  }

  /** Adopt an operation which returned only after its last barrier access. */
  bool claimAnyFailed(size_t processor) {
    if (processor >= MaxProcessors) {
      return false;
    }

    size_t state = m_State.value();
    while ((state & FailedBit) && !(state & (QuiescedBit | TerminalBit))) {
      if (m_State.compareAndSwap(state, processor + 1)) {
        return true;
      }
      state = m_State.value();
    }
    return false;
  }

  bool quiescedBy(size_t processor) const {
    return processor < MaxProcessors && m_State.value() == (QuiescedBit | (processor + 1));
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
    return processor < MaxProcessors && m_State.value() == (TerminalBit | (processor + 1));
  }

  bool markFailed(size_t processor) {
    return processor < MaxProcessors &&
           m_State.compareAndSwap(processor + 1, FailedBit | (processor + 1));
  }

  bool failedBy(size_t processor) const {
    return processor < MaxProcessors && m_State.value() == (FailedBit | (processor + 1));
  }

  bool release(size_t processor) {
    return processor < MaxProcessors && m_State.compareAndSwap(processor + 1, 0);
  }

  bool owned() const {
    return m_State.value() != 0;
  }

 private:
  static constexpr size_t QuiescedBit = size_t(1) << (sizeof(size_t) * 8 - 1);
  static constexpr size_t TerminalBit = QuiescedBit >> 1;
  static constexpr size_t FailedBit = TerminalBit >> 1;
  static constexpr size_t PhaseMask = QuiescedBit | TerminalBit | FailedBit;
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
  enum class State : size_t { Open, ReversibleClosed, TerminalClosed };

  LocalApicTlbMutationGate() : m_State(static_cast<size_t>(State::Open)), m_Active(0) {}

  bool tryEnter() {
    if (state() != State::Open) {
      return false;
    }

    m_Active += 1;
    if (state() != State::Open) {
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

  bool closeReversible() {
    return m_State.compareAndSwap(static_cast<size_t>(State::Open),
                                  static_cast<size_t>(State::ReversibleClosed));
  }

  /** Permanently reject new mapping mutations. This transition is idempotent. */
  bool closeTerminal() {
    size_t current = m_State.value();
    while (current != static_cast<size_t>(State::TerminalClosed)) {
      if (m_State.compareAndSwap(current, static_cast<size_t>(State::TerminalClosed))) {
        return true;
      }
      current = m_State.value();
    }
    return true;
  }

  bool reopen() {
    return !m_Active && m_State.compareAndSwap(static_cast<size_t>(State::ReversibleClosed),
                                               static_cast<size_t>(State::Open));
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
    return m_State.compareAndSwap(static_cast<size_t>(State::ReversibleClosed),
                                  static_cast<size_t>(State::Open));
  }

  bool closed() const {
    return state() != State::Open;
  }

  bool reversibleClosed() const {
    return state() == State::ReversibleClosed;
  }

  bool terminalClosed() const {
    return state() == State::TerminalClosed;
  }

  State state() const {
    return static_cast<State>(m_State.value());
  }

  bool drained() const {
    return !m_Active;
  }

  size_t active() const {
    return m_Active.value();
  }

 private:
  Atomic<size_t> m_State;
  Atomic<size_t> m_Active;
};

/** Records the first mapper which must coordinate irreversible TLB failure. */
class LocalApicTlbTerminalFailure {
 public:
  static constexpr size_t MaxProcessors = 64;

  LocalApicTlbTerminalFailure() : m_State(0) {}

  bool elect(size_t processor, size_t reason) {
    if (processor >= MaxProcessors) {
      return false;
    }

    const size_t proposed = ((reason + 1) << ProcessorBits) | (processor + 1);
    if (m_State.compareAndSwap(0, proposed)) {
      return true;
    }
    return coordinator(processor);
  }

  bool active() const {
    return m_State.value() != 0;
  }

  bool coordinator(size_t processor) const {
    return processor < MaxProcessors && (m_State.value() & ProcessorMask) == processor + 1;
  }

  size_t reason() const {
    const size_t state = m_State.value();
    return state ? (state >> ProcessorBits) - 1 : 0;
  }

 private:
  static constexpr size_t ProcessorBits = 8;
  static constexpr size_t ProcessorMask = (size_t(1) << ProcessorBits) - 1;
  Atomic<size_t> m_State;
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
    constexpr Service()
        : generation(0),
          address(0),
          processor(MaxProcessors),
          servingToken(0),
          acknowledgedToken(0) {}

    size_t generation;
    uintptr_t address;
    size_t processor;
    size_t servingToken;
    size_t acknowledgedToken;
  };

  LocalApicTlbShootdown()
      : m_Owner(false), m_Generation(0), m_NextGeneration(0), m_Address(0), m_Expected(0) {}

  static constexpr bool supportsContext(ExecutionContext context) {
    return context == ExecutionContext::WaitableThread || context == ExecutionContext::AtomicThread;
  }

  static constexpr bool onlyCurrentProcessorServiceable(size_t processorCount,
                                                        size_t terminalProcessors) {
    return processorCount && terminalProcessors >= (processorCount - 1);
  }

  bool tryAcquire() {
    return m_Owner.compareAndSwap(false, true);
  }

  bool publish(uintptr_t address, size_t processor, size_t processorCount) {
    uint64_t expected = 0;
    if (!m_Owner || m_Generation || processor >= processorCount ||
        !processorMask(processorCount, expected)) {
      return false;
    }

    const size_t initiatorState = m_ProcessorState[processor].value();
    if (isServing(initiatorState)) {
      return false;
    }

    size_t generation = 0;
    if (!selectNextGeneration(generation)) {
      return false;
    }

    m_Address = address;
    m_Expected = expected;
    m_ProcessorState[processor] = acknowledgedToken(generation);
    m_NextGeneration = generation;
    // Publishing the generation last makes the complete request visible to
    // interrupt and cooperative service paths in one step.
    m_Generation = generation;
    return true;
  }

  bool beginService(size_t processor, Service& service) {
    if (processor >= MaxProcessors || service.generation) {
      return false;
    }

    const size_t generation = m_Generation.value();
    if (!generation) {
      return false;
    }

    const size_t serving = servingToken(generation);
    const size_t acknowledged = acknowledgedToken(generation);
    const size_t previous = m_ProcessorState[processor].value();
    if (isServing(previous) || previous == acknowledged ||
        !m_ProcessorState[processor].compareAndSwap(previous, serving)) {
      return false;
    }

    // Copy the request before revalidating its last-published generation. A
    // publisher may replace the shared address while an older reader retires;
    // that reader either copied the matching address or rejects below.
    const uintptr_t address = m_Address.value();
    return revalidateServiceClaim(processor, generation, address, serving, acknowledged, service);
  }

  bool finishService(Service& service) {
    if (!service.generation || service.processor >= MaxProcessors) {
      return false;
    }

    // Retirement and acknowledgement are one atomic transition. The owner
    // can therefore never observe the final ACK while the service lease is
    // still live.
    const bool acknowledged = m_ProcessorState[service.processor].compareAndSwap(
        service.servingToken, service.acknowledgedToken);
    const bool current = acknowledged && m_Generation.value() == service.generation;
    service = Service();
    return current;
  }

  bool complete() const {
    const size_t generation = m_Generation.value();
    const uint64_t expected = m_Expected.value();
    return generation && expected && allAcknowledged(generation, expected) &&
           m_Generation.value() == generation;
  }

  void close() {
    m_Generation = 0;
  }

  bool drained() const {
    for (size_t processor = 0; processor < MaxProcessors; ++processor) {
      if (isServing(m_ProcessorState[processor].value())) {
        return false;
      }
    }
    return true;
  }

  bool release() {
    if (m_Generation || !drained()) {
      return false;
    }
    return m_Owner.compareAndSwap(true, false);
  }

  bool owned() const {
    return m_Owner;
  }

  bool retainedClosed() const {
    return m_Owner && !m_Generation && drained();
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
    const size_t generation = m_Generation.value();
    return generation ? acknowledgedMask(generation) : 0;
  }

  size_t servicing() const {
    size_t count = 0;
    for (size_t processor = 0; processor < MaxProcessors; ++processor) {
      if (isServing(m_ProcessorState[processor].value())) {
        ++count;
      }
    }
    return count;
  }

 private:
  friend class LocalApicTlbShootdownTestPeer;

  static constexpr size_t ServingBit = 1;
  static constexpr size_t MaxGeneration = ~size_t(0) >> 1;

  static constexpr bool isServing(size_t token) {
    return token & ServingBit;
  }

  static constexpr size_t acknowledgedToken(size_t generation) {
    return generation << 1;
  }

  static constexpr size_t servingToken(size_t generation) {
    return acknowledgedToken(generation) | ServingBit;
  }

  bool revalidateServiceClaim(size_t processor, size_t generation, uintptr_t address,
                              size_t serving, size_t acknowledged, Service& service) {
    if (m_Generation.value() != generation) {
      m_ProcessorState[processor].compareAndSwap(serving, acknowledged);
      return false;
    }

    service.generation = generation;
    service.address = address;
    service.processor = processor;
    service.servingToken = serving;
    service.acknowledgedToken = acknowledged;
    return true;
  }

  bool selectNextGeneration(size_t& generation) const {
    size_t candidate = m_NextGeneration.value();
    for (size_t attempt = 0; attempt <= MaxProcessors; ++attempt) {
      candidate = candidate == MaxGeneration ? 1 : candidate + 1;
      bool present = false;
      for (size_t processor = 0; processor < MaxProcessors; ++processor) {
        if ((m_ProcessorState[processor].value() >> 1) == candidate) {
          present = true;
          break;
        }
      }
      if (!present) {
        generation = candidate;
        return true;
      }
    }
    return false;
  }

  uint64_t acknowledgedMask(size_t generation) const {
    uint64_t acknowledged = 0;
    const size_t token = acknowledgedToken(generation);
    for (size_t processor = 0; processor < MaxProcessors; ++processor) {
      if (m_ProcessorState[processor].value() == token) {
        acknowledged |= uint64_t(1) << processor;
      }
    }
    return acknowledged;
  }

  bool allAcknowledged(size_t generation, uint64_t expected) const {
    const size_t token = acknowledgedToken(generation);
    for (size_t processor = 0; expected; ++processor, expected >>= 1) {
      if ((expected & 1) && m_ProcessorState[processor].value() != token) {
        return false;
      }
    }
    return true;
  }

  static bool processorMask(size_t processorCount, uint64_t& mask) {
    if (!processorCount || processorCount > MaxProcessors) {
      return false;
    }
    mask = processorCount == MaxProcessors ? ~uint64_t(0) : (uint64_t(1) << processorCount) - 1;
    return true;
  }

  Atomic<bool> m_Owner;
  Atomic<size_t> m_Generation;
  Atomic<size_t> m_NextGeneration;
  Atomic<uintptr_t> m_Address;
  Atomic<uint64_t> m_Expected;
  Atomic<size_t> m_ProcessorState[MaxProcessors];
};

#endif
