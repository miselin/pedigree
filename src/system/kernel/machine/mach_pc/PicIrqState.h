/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_MACHINE_MACH_PC_PICIRQSTATE_H
#define PEDIGREE_KERNEL_MACHINE_MACH_PC_PICIRQSTATE_H

#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/assert.h"

/**
 * Software ownership state for the dual 8259 PIC.
 *
 * The caller serialises every operation with controller-state ownership and
 * writes the returned master/slave masks to hardware. Registration counts are
 * updated independently of the callback registry's drain latency, so a new
 * handler cannot be hidden by the final accounting step of an older
 * unregister.
 */
class PicIrqState {
 public:
  static constexpr size_t LineCount = 16;
  static constexpr size_t CascadeIrq = 2;

  enum class TriggerMode : uint8_t {
    Unconfigured,
    Level,
    Edge,
  };

  PicIrqState() : m_Mask(0) {
    for (size_t i = 0; i < LineCount; ++i) {
      m_TriggerModes[i] = TriggerMode::Unconfigured;
      m_ControllerAck[i] = IrqControllerAck::AfterHardStage;
      m_HardHandlerCounts[i] = 0;
      m_ThreadedHandlerCounts[i] = 0;
      m_DispatchGenerations[i] = 0;
      m_AcknowledgedGenerations[i] = 0;
      m_AcknowledgementPending[i] = false;
      m_ThreadedPending[i] = false;
      m_TransitionPending[i] = false;
      m_RequestedEnabled[i] = true;
      m_SchedulerOwned[i] = false;
    }
  }

  bool canRegister(size_t irq, const IrqPolicy& policy, IrqDelivery delivery) const {
    if (irq >= LineCount || irq == CascadeIrq || m_SchedulerOwned[irq] ||
        (delivery != IrqDelivery::Hard && delivery != IrqDelivery::Threaded)) {
      return false;
    }

    if ((delivery == IrqDelivery::Hard && !policy.validForHard()) ||
        (delivery == IrqDelivery::Threaded && !policy.validForThreaded())) {
      return false;
    }

    const TriggerMode requested =
        policy.trigger() == IrqTrigger::Edge ? TriggerMode::Edge : TriggerMode::Level;
    return m_TriggerModes[irq] == TriggerMode::Unconfigured ||
           (m_TriggerModes[irq] == requested && m_ControllerAck[irq] == policy.controllerAck());
  }

  bool canRegister(size_t irq, const IrqPolicy& policy) const {
    return canRegister(irq, policy, legacyDelivery(policy));
  }

  bool canRegisterScheduler(size_t irq, const IrqPolicy& policy) const {
    return irq == 0 && !m_SchedulerOwned[irq] && !handlerCount(irq) &&
           policy == IrqPolicy::edgeHard();
  }

  void schedulerRegistered(size_t irq, const IrqPolicy& policy) {
    assert(canRegisterScheduler(irq, policy));
    handlerRegistered(irq, policy, IrqDelivery::Hard);
    m_SchedulerOwned[irq] = true;
  }

  void schedulerUnregistered(size_t irq) {
    assert(irq < LineCount && m_SchedulerOwned[irq]);
    m_SchedulerOwned[irq] = false;
    handlerUnregistered(irq, IrqDelivery::Hard);
  }

  bool schedulerRegistered(size_t irq) const {
    assert(irq < LineCount);
    return m_SchedulerOwned[irq];
  }

  void handlerRegistered(size_t irq, const IrqPolicy& policy, IrqDelivery delivery) {
    assert(canRegister(irq, policy, delivery));
    const bool firstHandler = handlerCount(irq) == 0;
    if (m_TriggerModes[irq] == TriggerMode::Unconfigured) {
      m_TriggerModes[irq] =
          policy.trigger() == IrqTrigger::Edge ? TriggerMode::Edge : TriggerMode::Level;
      m_ControllerAck[irq] = policy.controllerAck();
    }
    if (delivery == IrqDelivery::Hard) {
      ++m_HardHandlerCounts[irq];
    } else {
      ++m_ThreadedHandlerCounts[irq];
    }
    if (firstHandler) {
      m_AcknowledgementPending[irq] = false;
      m_ThreadedPending[irq] = false;
      m_AcknowledgedGenerations[irq] = m_DispatchGenerations[irq];
      m_RequestedEnabled[irq] = true;
      rebuildMask();
    }
  }

  void handlerRegistered(size_t irq, const IrqPolicy& policy) {
    handlerRegistered(irq, policy, legacyDelivery(policy));
  }

  void handlerUnregistered(size_t irq, IrqDelivery delivery) {
    assert(irq < LineCount);
    assert(delivery == IrqDelivery::Hard || delivery == IrqDelivery::Threaded);
    if (delivery == IrqDelivery::Hard) {
      assert(m_HardHandlerCounts[irq]);
      --m_HardHandlerCounts[irq];
    } else {
      assert(m_ThreadedHandlerCounts[irq]);
      --m_ThreadedHandlerCounts[irq];
      if (!m_ThreadedHandlerCounts[irq] && m_ThreadedPending[irq]) {
        m_ThreadedPending[irq] = false;
        rebuildMask();
      }
    }

    if (!handlerCount(irq)) {
      m_AcknowledgementPending[irq] = false;
      m_ThreadedPending[irq] = false;
      m_AcknowledgedGenerations[irq] = m_DispatchGenerations[irq];
      m_RequestedEnabled[irq] = false;
      m_TriggerModes[irq] = TriggerMode::Unconfigured;
      m_ControllerAck[irq] = IrqControllerAck::AfterHardStage;
      rebuildMask();
    }
  }

  void handlerUnregistered(size_t irq) {
    assert(irq < LineCount);
    assert(!m_HardHandlerCounts[irq] || !m_ThreadedHandlerCounts[irq]);
    handlerUnregistered(irq,
                        m_ThreadedHandlerCounts[irq] ? IrqDelivery::Threaded : IrqDelivery::Hard);
  }

  size_t beginDispatch(size_t irq) {
    assert(irq < LineCount);
    size_t generation = ++m_DispatchGenerations[irq];
    if (!generation) {
      generation = ++m_DispatchGenerations[irq];
    }
    return generation;
  }

  /**
   * Completes a dispatch without losing an acknowledgement which raced the
   * handler return. An acknowledgement covers every dispatch admitted
   * before it observed the line.
   */
  void completeDispatch(size_t irq, size_t dispatchGeneration, bool needsAcknowledgement) {
    assert(irq < LineCount);
    if (!needsAcknowledgement || !handlerCount(irq) ||
        generationReached(m_AcknowledgedGenerations[irq], dispatchGeneration)) {
      return;
    }

    m_AcknowledgementPending[irq] = true;
    rebuildMask();
  }

  bool acknowledge(size_t irq) {
    assert(irq < LineCount);
    if (!handlerCount(irq)) {
      return false;
    }

    m_AcknowledgedGenerations[irq] = m_DispatchGenerations[irq];
    if (m_AcknowledgementPending[irq]) {
      m_AcknowledgementPending[irq] = false;
      rebuildMask();
    }
    return true;
  }

  /** Applies a policy-requested mask until the bottom half completes. */
  void beginThreadedDispatch(size_t irq) {
    assert(irq < LineCount);
    if (lineRelease(irq) == IrqLineRelease::AfterThreadedCompletion) {
      m_ThreadedPending[irq] = true;
      rebuildMask();
    }
  }

  /**
   * Completes the latest threaded batch. A stale batch cannot reopen a line
   * which has since delivered another occurrence.
   */
  bool completeThreadedDispatch(size_t irq, size_t dispatchGeneration, bool allowRearm) {
    assert(irq < LineCount);
    if (m_DispatchGenerations[irq] != dispatchGeneration) {
      return false;
    }

    if (lineRelease(irq) == IrqLineRelease::AfterThreadedCompletion &&
        (allowRearm || !handlerCount(irq))) {
      m_ThreadedPending[irq] = false;
      rebuildMask();
    }
    return true;
  }

  bool threadedPending(size_t irq) const {
    assert(irq < LineCount);
    return m_ThreadedPending[irq];
  }

  bool acknowledgementPending(size_t irq) const {
    assert(irq < LineCount);
    return m_AcknowledgementPending[irq];
  }

  void beginLineTransition(size_t irq) {
    assert(irq < LineCount);
    m_TransitionPending[irq] = true;
    rebuildMask();
  }

  void finishLineTransition(size_t irq) {
    assert(irq < LineCount);
    m_TransitionPending[irq] = false;
    rebuildMask();
  }

  bool lineTransitionPending(size_t irq) const {
    assert(irq < LineCount);
    return m_TransitionPending[irq];
  }

  size_t handlerCount(size_t irq) const {
    assert(irq < LineCount);
    return m_HardHandlerCounts[irq] + m_ThreadedHandlerCounts[irq];
  }

  size_t hardHandlerCount(size_t irq) const {
    assert(irq < LineCount);
    return m_HardHandlerCounts[irq];
  }

  size_t threadedHandlerCount(size_t irq) const {
    assert(irq < LineCount);
    return m_ThreadedHandlerCounts[irq];
  }

  IrqDelivery delivery(size_t irq) const {
    assert(irq < LineCount);
    const bool hard = m_HardHandlerCounts[irq] != 0;
    const bool threaded = m_ThreadedHandlerCounts[irq] != 0;
    if (hard && threaded) {
      return IrqDelivery::Mixed;
    }
    if (hard) {
      return IrqDelivery::Hard;
    }
    if (threaded) {
      return IrqDelivery::Threaded;
    }
    return IrqDelivery::None;
  }

  bool edgeTriggered(size_t irq) const {
    assert(irq < LineCount);
    return m_TriggerModes[irq] == TriggerMode::Edge;
  }

  IrqTrigger trigger(size_t irq) const {
    assert(irq < LineCount);
    return m_TriggerModes[irq] == TriggerMode::Edge ? IrqTrigger::Edge : IrqTrigger::Level;
  }

  IrqControllerAck controllerAck(size_t irq) const {
    assert(irq < LineCount);
    return m_ControllerAck[irq];
  }

  IrqLineRelease lineRelease(size_t irq) const {
    assert(irq < LineCount);
    return m_TriggerModes[irq] == TriggerMode::Level && m_ThreadedHandlerCounts[irq]
               ? IrqLineRelease::AfterThreadedCompletion
               : IrqLineRelease::AfterHardStage;
  }

  bool enabled(size_t irq) const {
    assert(irq < LineCount);
    return (m_Mask & bit(irq)) == 0;
  }

  bool requestedEnabled(size_t irq) const {
    assert(irq < LineCount);
    return m_RequestedEnabled[irq];
  }

  size_t dispatchGeneration(size_t irq) const {
    assert(irq < LineCount);
    return m_DispatchGenerations[irq];
  }

  size_t acknowledgedGeneration(size_t irq) const {
    assert(irq < LineCount);
    return m_AcknowledgedGenerations[irq];
  }

  void setEnabled(size_t irq, bool enabled) {
    assert(irq < LineCount);
    m_RequestedEnabled[irq] = enabled;
    rebuildMask();
  }

  void setAllEnabled(bool enabled) {
    for (size_t i = 0; i < LineCount; ++i) {
      m_RequestedEnabled[i] = enabled;
    }

    // IRQ2 is the cascade input and must remain available when device
    // lines are masked, or every slave IRQ becomes unreachable.
    if (!enabled) {
      m_RequestedEnabled[2] = true;
    }
    rebuildMask();
  }

  uint16_t mask() const {
    return m_Mask;
  }

  uint8_t masterMask() const {
    return static_cast<uint8_t>(m_Mask & 0xFF);
  }

  uint8_t slaveMask() const {
    return static_cast<uint8_t>(m_Mask >> 8);
  }

 private:
  static IrqDelivery legacyDelivery(const IrqPolicy& policy) {
    if (policy.lineRelease() == IrqLineRelease::AfterThreadedCompletion ||
        (policy.trigger() == IrqTrigger::Edge &&
         policy.controllerAck() == IrqControllerAck::AfterHardStage)) {
      return IrqDelivery::Threaded;
    }
    return IrqDelivery::Hard;
  }

  static uint16_t bit(size_t irq) {
    return static_cast<uint16_t>(static_cast<uint16_t>(1U) << irq);
  }

  static bool generationReached(size_t current, size_t target) {
    return static_cast<intptr_t>(current - target) >= 0;
  }

  void rebuildMask() {
    uint16_t mask = 0;
    for (size_t i = 0; i < LineCount; ++i) {
      if (!m_RequestedEnabled[i] || m_AcknowledgementPending[i] || m_ThreadedPending[i] ||
          m_TransitionPending[i]) {
        mask |= bit(i);
      }
    }

    // The master IRQ2 bit represents both a direct IRQ2 source and every
    // slave line, so a live slave always wins over a direct-line mask.
    if ((mask & static_cast<uint16_t>(0xFF00)) != static_cast<uint16_t>(0xFF00)) {
      mask &= static_cast<uint16_t>(~bit(2));
    }
    m_Mask = mask;
  }

  uint16_t m_Mask;
  TriggerMode m_TriggerModes[LineCount];
  IrqControllerAck m_ControllerAck[LineCount];
  size_t m_HardHandlerCounts[LineCount];
  size_t m_ThreadedHandlerCounts[LineCount];
  size_t m_DispatchGenerations[LineCount];
  size_t m_AcknowledgedGenerations[LineCount];
  bool m_AcknowledgementPending[LineCount];
  bool m_ThreadedPending[LineCount];
  bool m_TransitionPending[LineCount];
  bool m_RequestedEnabled[LineCount];
  bool m_SchedulerOwned[LineCount];
};

struct PicContentionLineResult {
  bool terminalWork;
  bool schedulerDrop;
  bool quarantine;
  bool invalidateThreaded;
  bool maskChanged;
  size_t threadedOccurrences;
  size_t unhandledOccurrences;
};

/** Resolves deferred controller work without touching the 8259 hardware. */
inline PicContentionLineResult resolvePicContentionLine(PicIrqState& state, size_t irq,
                                                        size_t realEntries, size_t deferredTails,
                                                        size_t deferredTailEois) {
  assert(irq < PicIrqState::LineCount);

  PicContentionLineResult result = {};
  const uint16_t maskBefore = state.mask();
  const bool hasTailAction = deferredTails || deferredTailEois;
  result.terminalWork = realEntries || hasTailAction;
  if (!result.terminalWork) {
    return result;
  }

  if (irq == 0 && state.schedulerRegistered(0) && !hasTailAction) {
    result.schedulerDrop = true;
    result.unhandledOccurrences = realEntries;
    for (size_t occurrence = 0; occurrence < realEntries; ++occurrence) {
      const size_t generation = state.beginDispatch(irq);
      state.acknowledge(irq);
      state.completeDispatch(irq, generation, false);
    }
    result.maskChanged = state.mask() != maskBefore;
    return result;
  }

  const IrqDelivery delivery = state.delivery(irq);
  if (realEntries && !hasTailAction && delivery == IrqDelivery::Threaded) {
    // The gate retained a current-lifetime occurrence, not an interrupted
    // frame. A threaded-only line can therefore run its ordinary
    // admission/publication sequence when the owner drains this batch.
    result.threadedOccurrences = realEntries;
    return result;
  }

  result.quarantine = true;
  result.unhandledOccurrences = realEntries + deferredTails;
  result.invalidateThreaded = delivery == IrqDelivery::Threaded || delivery == IrqDelivery::Mixed;
  const size_t generation = state.beginDispatch(irq);
  state.completeDispatch(irq, generation, true);
  result.maskChanged = state.mask() != maskBefore;
  return result;
}

/**
 * Lock-free ownership handoff for access to the shared 8259 controllers.
 *
 * Producers first account their deferred action, then publish Pending before
 * attempting to become Owner. The current owner can therefore clear Pending
 * and retain ownership in one compare-and-swap, while release can only succeed
 * from an exactly idle Owner state.
 */
class PicControllerStateGate {
 public:
  static constexpr size_t LineCount = PicIrqState::LineCount;
  static constexpr size_t CountBits = 12;
  static constexpr size_t CountMask = (static_cast<size_t>(1) << CountBits) - 1;
  static constexpr size_t LifetimeMask = static_cast<size_t>(~static_cast<size_t>(0)) >> CountBits;
  static constexpr size_t TransitionLifetime = LifetimeMask;

  static_assert(sizeof(size_t) >= sizeof(uint64_t),
                "PIC lifetime-qualified handoff requires lock-free 64-bit words");
  static_assert(__atomic_always_lock_free(sizeof(size_t), nullptr),
                "PIC controller handoff words must be lock-free");

  struct PendingActions {
    PendingActions() {
      clear();
    }

    void clear() {
      for (size_t i = 0; i < LineCount; ++i) {
        entry[i] = 0;
        tail[i] = 0;
        tailEoi[i] = 0;
        staleEntry[i] = 0;
        staleTail[i] = 0;
        staleTailEoi[i] = 0;
      }
    }

    bool empty() const {
      for (size_t i = 0; i < LineCount; ++i) {
        if (entry[i] || tail[i] || tailEoi[i] || staleEntry[i] || staleTail[i] || staleTailEoi[i]) {
          return false;
        }
      }
      return true;
    }

    size_t entry[LineCount];
    size_t tail[LineCount];
    size_t tailEoi[LineCount];
    size_t staleEntry[LineCount];
    size_t staleTail[LineCount];
    size_t staleTailEoi[LineCount];
  };

  PicControllerStateGate() : m_State(Clean) {
    for (size_t i = 0; i < LineCount; ++i) {
      m_LifetimeGenerations[i] = 1;
      m_LineLifetimes[i] = 1;
      m_EntryCounts[i] = tagged(1, 0);
      m_TailNoEoiCounts[i] = tagged(1, 0);
      m_TailEoiCounts[i] = tagged(1, 0);
      m_StaleEntryCounts[i] = 0;
      m_StaleTailNoEoiCounts[i] = 0;
      m_StaleTailEoiCounts[i] = 0;
    }
  }

  /** Acquires ownership only when no work is already pending. */
  bool tryAcquireClean() {
    size_t expected = Clean;
    return __atomic_compare_exchange_n(&m_State, &expected, Owner, false, __ATOMIC_ACQUIRE,
                                       __ATOMIC_RELAXED);
  }

  /** Claims an unowned clean or pending gate in one attempt. */
  bool tryClaim() {
    const size_t state = __atomic_load_n(&m_State, __ATOMIC_ACQUIRE);
    if (state & Owner) {
      return false;
    }

    size_t expected = state;
    return __atomic_compare_exchange_n(&m_State, &expected, state | Owner, false, __ATOMIC_ACQ_REL,
                                       __ATOMIC_RELAXED);
  }

  size_t currentLifetime(size_t irq) const {
    assert(irq < LineCount);
    return __atomic_load_n(&m_LineLifetimes[irq], __ATOMIC_ACQUIRE);
  }

  /** Prevents in-flight old work from qualifying against a changing line. */
  void beginLineTransition(size_t irq) {
    assert(irq < LineCount);
    replaceLifetime(irq, TransitionLifetime);
  }

  /** Publishes a fresh handler-set lifetime after a line transition. */
  size_t finishLineTransition(size_t irq) {
    assert(irq < LineCount);
    size_t lifetime = ++m_LifetimeGenerations[irq];
    lifetime &= LifetimeMask;
    if (!lifetime || lifetime == TransitionLifetime) {
      lifetime = 1;
      m_LifetimeGenerations[irq] = lifetime;
    }
    replaceLifetime(irq, lifetime);
    return lifetime;
  }

  /** Queues controller-entry work and elects an owner if one is needed. */
  bool queueEntry(size_t irq, size_t lifetime) {
    assert(irq < LineCount);
    queueTagged(m_EntryCounts[irq], m_StaleEntryCounts[irq], lifetime);
    publishPending(irq == 0);
    return tryClaim();
  }

  /** Queues controller-tail work, preserving its EOI obligation. */
  bool queueTail(size_t irq, size_t lifetime, bool owesEoi) {
    assert(irq < LineCount);
    if (owesEoi) {
      queueTagged(m_TailEoiCounts[irq], m_StaleTailEoiCounts[irq], lifetime);
    } else {
      queueTagged(m_TailNoEoiCounts[irq], m_StaleTailNoEoiCounts[irq], lifetime);
    }
    publishPending(irq == 0);
    return tryClaim();
  }

  /** Announces a complete tail record published in separate fixed storage. */
  bool queueTailRecord(size_t irq) {
    assert(irq < LineCount);
    publishPending(irq == 0);
    return tryClaim();
  }

  /** Atomically claims the published batch while retaining ownership. */
  bool takePending(PendingActions& actions) {
    actions.clear();
    size_t expected = __atomic_load_n(&m_State, __ATOMIC_ACQUIRE);
    do {
      if ((expected & (Owner | Pending)) != (Owner | Pending)) {
        return false;
      }
    } while (!__atomic_compare_exchange_n(&m_State, &expected, Owner, false, __ATOMIC_ACQ_REL,
                                          __ATOMIC_ACQUIRE));

    for (size_t i = 0; i < LineCount; ++i) {
      const size_t lifetime = currentLifetime(i);
      takeTagged(m_EntryCounts[i], m_StaleEntryCounts[i], lifetime, actions.entry[i],
                 actions.staleEntry[i]);

      size_t tailNoEoi = 0;
      size_t staleTailNoEoi = 0;
      takeTagged(m_TailNoEoiCounts[i], m_StaleTailNoEoiCounts[i], lifetime, tailNoEoi,
                 staleTailNoEoi);
      takeTagged(m_TailEoiCounts[i], m_StaleTailEoiCounts[i], lifetime, actions.tailEoi[i],
                 actions.staleTailEoi[i]);
      actions.tail[i] = tailNoEoi + actions.tailEoi[i];
      actions.staleTail[i] = staleTailNoEoi + actions.staleTailEoi[i];
    }
    return true;
  }

  /** Releases ownership only if no producer has published pending work. */
  bool releaseIfIdle() {
    size_t expected = Owner;
    return __atomic_compare_exchange_n(&m_State, &expected, Clean, false, __ATOMIC_RELEASE,
                                       __ATOMIC_RELAXED);
  }

  bool hasPending() const {
    return (__atomic_load_n(&m_State, __ATOMIC_ACQUIRE) & Pending) != 0;
  }

  bool urgentPending() const {
    return (__atomic_load_n(&m_State, __ATOMIC_ACQUIRE) & Urgent) != 0;
  }

  /** Keeps ownership discoverable while a thread-context continuation wakes. */
  void requestContinuation() {
    publishPending(false);
  }

  /** Hands only non-urgent pending work to a guaranteed continuation. */
  bool relinquishOwnerForContinuation() {
    size_t expected = Owner | Pending;
    return __atomic_compare_exchange_n(&m_State, &expected, Pending, false, __ATOMIC_RELEASE,
                                       __ATOMIC_ACQUIRE);
  }

 private:
  static constexpr size_t Clean = 0;
  static constexpr size_t Owner = 1U << 0;
  static constexpr size_t Pending = 1U << 1;
  static constexpr size_t Urgent = 1U << 2;

  static size_t tagged(size_t lifetime, size_t count) {
    return ((lifetime & LifetimeMask) << CountBits) | (count & CountMask);
  }

  static size_t taggedLifetime(size_t value) {
    return value >> CountBits;
  }

  static size_t taggedCount(size_t value) {
    return value & CountMask;
  }

  static void addStale(size_t& counter, size_t count) {
    if (count) {
      __atomic_fetch_add(&counter, count, __ATOMIC_RELAXED);
    }
  }

  static void queueTagged(size_t& currentCounter, size_t& staleCounter, size_t lifetime) {
    const size_t qualifiedLifetime = lifetime & LifetimeMask;
    if (!qualifiedLifetime || qualifiedLifetime == TransitionLifetime) {
      addStale(staleCounter, 1);
      return;
    }

    size_t expected = __atomic_load_n(&currentCounter, __ATOMIC_ACQUIRE);
    const size_t count = taggedCount(expected);
    if (taggedLifetime(expected) != qualifiedLifetime || count == CountMask ||
        !__atomic_compare_exchange_n(&currentCounter, &expected,
                                     tagged(qualifiedLifetime, count + 1), false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
      // A lifecycle boundary or competing publication wins over
      // software quarantine. The stale path still preserves hardware
      // mask/EOI obligations without touching the replacement lifetime.
      addStale(staleCounter, 1);
    }
  }

  static void takeTagged(size_t& currentCounter, size_t& staleCounter, size_t lifetime,
                         size_t& current, size_t& stale) {
    const size_t value =
        __atomic_exchange_n(&currentCounter, tagged(lifetime, 0), __ATOMIC_ACQ_REL);
    const size_t count = taggedCount(value);
    if (taggedLifetime(value) == lifetime) {
      current = count;
    } else {
      stale = count;
    }
    stale += __atomic_exchange_n(&staleCounter, static_cast<size_t>(0), __ATOMIC_ACQ_REL);
  }

  void replaceLifetime(size_t irq, size_t lifetime) {
    size_t moved = 0;
    size_t value = __atomic_exchange_n(&m_EntryCounts[irq], tagged(lifetime, 0), __ATOMIC_ACQ_REL);
    moved += taggedCount(value);
    addStale(m_StaleEntryCounts[irq], taggedCount(value));

    value = __atomic_exchange_n(&m_TailNoEoiCounts[irq], tagged(lifetime, 0), __ATOMIC_ACQ_REL);
    moved += taggedCount(value);
    addStale(m_StaleTailNoEoiCounts[irq], taggedCount(value));

    value = __atomic_exchange_n(&m_TailEoiCounts[irq], tagged(lifetime, 0), __ATOMIC_ACQ_REL);
    moved += taggedCount(value);
    addStale(m_StaleTailEoiCounts[irq], taggedCount(value));

    __atomic_store_n(&m_LineLifetimes[irq], lifetime, __ATOMIC_RELEASE);
    if (moved) {
      publishPending(irq == 0);
    }
  }

  void publishPending(bool urgent) {
    __atomic_fetch_or(&m_State, Pending | (urgent ? Urgent : 0), __ATOMIC_RELEASE);
  }

  size_t m_State;
  size_t m_LifetimeGenerations[LineCount];
  size_t m_LineLifetimes[LineCount];
  size_t m_EntryCounts[LineCount];
  size_t m_TailNoEoiCounts[LineCount];
  size_t m_TailEoiCounts[LineCount];
  size_t m_StaleEntryCounts[LineCount];
  size_t m_StaleTailNoEoiCounts[LineCount];
  size_t m_StaleTailEoiCounts[LineCount];

  PicControllerStateGate(const PicControllerStateGate&) = delete;
  PicControllerStateGate& operator=(const PicControllerStateGate&) = delete;
};

/** Complete post-callback state for one hard interrupt occurrence. */
struct PicHardTailRecord {
  uint8_t irq;
  size_t controllerLifetime;
  size_t dispatchGeneration;
  size_t hardStageGeneration;
  size_t threadedCookie;
  IrqControllerAck controllerAck;
  HardIrqDisposition hardDisposition;
  bool hasThreadedStage;
  bool threadedPublished;
  bool admitted;
};

/** The controller and line state against which a hard tail is finalized. */
struct PicHardTailCurrentState {
  size_t controllerLifetime;
  size_t dispatchGeneration;
  size_t hardStageGeneration;
  size_t threadedCookie;
  size_t threadedDispatchGeneration;
  size_t hardHandlerCount;
  IrqDelivery delivery;
  bool hardLineQuarantined;
};

enum class PicHardTailThreadedAction : uint8_t {
  None,
  Publish,
  MissingPublication,
  Quiesced,
  Stale,
};

/** Pure lifecycle decision shared by the PIC finalizer and hosted tests. */
struct PicHardTailPlan {
  bool controllerLifetimeCurrent;
  bool hardStageLifetimeCurrent;
  bool threadedLifetimeCurrent;
  HardIrqDisposition effectiveHardDisposition;
  PicHardTailThreadedAction threadedAction;
};

inline PicHardTailPlan resolvePicHardTail(const PicHardTailRecord& record,
                                          const PicHardTailCurrentState& current) {
  PicHardTailPlan plan = {};
  plan.controllerLifetimeCurrent = record.controllerLifetime == current.controllerLifetime;
  plan.hardStageLifetimeCurrent = record.hardStageGeneration == current.hardStageGeneration;
  plan.threadedLifetimeCurrent =
      record.hasThreadedStage && record.threadedCookie == current.threadedCookie &&
      record.dispatchGeneration == current.threadedDispatchGeneration &&
      (current.delivery == IrqDelivery::Threaded || current.delivery == IrqDelivery::Mixed);
  plan.effectiveHardDisposition =
      !plan.hardStageLifetimeCurrent ||
              (record.hasThreadedStage && current.delivery == IrqDelivery::Threaded) ||
              (record.hardDisposition == HardIrqDisposition::KeepMasked &&
               !current.hardLineQuarantined)
          ? HardIrqDisposition::Handled
          : record.hardDisposition;

  if (!record.hasThreadedStage) {
    plan.threadedAction = PicHardTailThreadedAction::None;
  } else if (plan.threadedLifetimeCurrent) {
    plan.threadedAction = record.threadedPublished ? PicHardTailThreadedAction::Publish
                                                   : PicHardTailThreadedAction::MissingPublication;
  } else if (record.dispatchGeneration == current.dispatchGeneration && current.hardHandlerCount &&
             (current.delivery == IrqDelivery::Hard || current.delivery == IrqDelivery::Mixed)) {
    plan.threadedAction = PicHardTailThreadedAction::Quiesced;
  } else {
    plan.threadedAction = PicHardTailThreadedAction::Stale;
  }
  return plan;
}

struct PicHardTailDoorbellResult {
  bool published;
  bool invalidateStagedDispatch;
  bool quarantine;
  bool completeDispatch;
};

/** Resolves the terminal state of an already-staged threaded publication. */
inline PicHardTailDoorbellResult resolvePicHardTailDoorbell(bool staged, bool accepted) {
  PicHardTailDoorbellResult result = {};
  result.published = staged && accepted;
  result.invalidateStagedDispatch = staged && !accepted;
  result.quarantine = result.invalidateStagedDispatch;
  result.completeDispatch = !result.published;
  return result;
}

/** Enforces a stale physical mask before an owed post-hard-stage EOI. */
class PicHardTailTerminalSequence {
 public:
  PicHardTailTerminalSequence(bool temporaryMaskRequired, IrqControllerAck controllerAck)
      : m_TemporaryMaskRequired(temporaryMaskRequired),
        m_TemporaryMaskApplied(false),
        m_ControllerAck(controllerAck),
        m_Acknowledged(false) {}

  template <typename Sink>
  void applyTemporaryMask(Sink&& sink) {
    if (!m_TemporaryMaskRequired) {
      return;
    }
    sink();
    m_TemporaryMaskApplied = true;
  }

  template <typename Sink>
  void acknowledge(Sink&& sink) {
    assert(!m_Acknowledged);
    if (m_ControllerAck == IrqControllerAck::AfterHardStage) {
      assert(!m_TemporaryMaskRequired || m_TemporaryMaskApplied);
      sink();
    }
    m_Acknowledged = true;
  }

 private:
  bool m_TemporaryMaskRequired;
  bool m_TemporaryMaskApplied;
  IrqControllerAck m_ControllerAck;
  bool m_Acknowledged;
};

/**
 * One lock-free terminal record per 8259 input.
 *
 * A line which still owes EOI cannot deliver another occurrence. A line
 * acknowledged before its hard stage still cannot produce a second rich tail
 * while the first is pending: its next entry sees controller ownership and is
 * reduced to the counted fail-closed path. The caller keeps that counted path
 * as an overflow fallback rather than waiting here.
 */
class PicHardTailQueue {
 public:
  static constexpr size_t LineCount = PicIrqState::LineCount;

  PicHardTailQueue() : m_States(), m_Records() {
    for (size_t irq = 0; irq < LineCount; ++irq) {
      __atomic_store_n(&m_States[irq], Empty, __ATOMIC_RELAXED);
    }
  }

  bool publish(uint8_t irq, const PicHardTailRecord& record) {
    if (irq >= LineCount || record.irq != irq) {
      return false;
    }

    size_t expected = Empty;
    if (!__atomic_compare_exchange_n(&m_States[irq], &expected, Claimed, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
      return false;
    }
    m_Records[irq] = record;
    __atomic_store_n(&m_States[irq], Ready, __ATOMIC_RELEASE);
    return true;
  }

  bool take(uint8_t irq, PicHardTailRecord& record) {
    if (irq >= LineCount) {
      return false;
    }

    size_t expected = Ready;
    if (!__atomic_compare_exchange_n(&m_States[irq], &expected, Claimed, false, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED)) {
      return false;
    }
    record = m_Records[irq];
    return true;
  }

  void complete(uint8_t irq) {
    assert(irq < LineCount);
    size_t expected = Claimed;
    const bool completed = __atomic_compare_exchange_n(&m_States[irq], &expected, Empty, false,
                                                       __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    assert(completed);
  }

  /** Keeps the slot claimed until all terminal actions have returned. */
  template <typename Finalizer>
  bool consume(uint8_t irq, Finalizer&& finalizer) {
    PicHardTailRecord record = {};
    if (!take(irq, record)) {
      return false;
    }
    finalizer(record);
    complete(irq);
    return true;
  }

  bool pending(uint8_t irq) const {
    return irq < LineCount && __atomic_load_n(&m_States[irq], __ATOMIC_ACQUIRE) == Ready;
  }

 private:
  static constexpr size_t Empty = 0;
  static constexpr size_t Claimed = 1;
  static constexpr size_t Ready = 2;

  size_t m_States[LineCount];
  PicHardTailRecord m_Records[LineCount];

  PicHardTailQueue(const PicHardTailQueue&) = delete;
  PicHardTailQueue& operator=(const PicHardTailQueue&) = delete;
};

inline uint16_t effectivePicMask(uint16_t canonical, uint16_t temporary) {
  return canonical | temporary;
}

/** Keeps an AP from exposing a replacement lifetime before BSP publication. */
inline uint16_t preserveUnsafePicUnmasks(uint16_t applied, uint16_t canonical, uint16_t temporary,
                                         uint16_t deferredTransitions,
                                         bool canPublishBeforeRelease) {
  if (!canPublishBeforeRelease) {
    const uint16_t newlyUnmasked =
        static_cast<uint16_t>(applied & ~effectivePicMask(canonical, temporary));
    temporary |= static_cast<uint16_t>(newlyUnmasked & deferredTransitions);
  }
  return temporary;
}

/** Physical overrides outside a lifetime transition can be restored by owner. */
inline uint16_t restorablePicTemporaryMask(uint16_t temporary, uint16_t deferredTransitions) {
  return static_cast<uint16_t>(temporary & ~deferredTransitions);
}

inline uint16_t cancelDeferredPicLineTransition(uint16_t deferred, size_t irq) {
  assert(irq < PicIrqState::LineCount);
  return static_cast<uint16_t>(deferred & ~static_cast<uint16_t>(1U << irq));
}

/** Selects physical-only masks for work which must not mutate a line owner. */
inline uint16_t temporaryPicMaskForDeferredWork(
    uint16_t existing, const size_t (&realEntries)[PicIrqState::LineCount],
    const PicControllerStateGate::PendingActions& actions,
    const size_t (&spuriousEntries)[PicIrqState::LineCount]) {
  uint16_t result = existing;
  for (size_t irq = 0; irq < PicIrqState::LineCount; ++irq) {
    if (realEntries[irq] || actions.tailEoi[irq] || spuriousEntries[irq]) {
      result |= static_cast<uint16_t>(1U << irq);
    }
  }
  return result;
}

enum class PicControllerWriteTarget : uint8_t {
  MasterCommand,
  MasterMask,
  SlaveCommand,
  SlaveMask,
};

template <typename Sink>
void emitPicMaskWrites(uint16_t mask, Sink&& sink) {
  sink(PicControllerWriteTarget::MasterMask, static_cast<uint8_t>(mask & 0xFF));
  sink(PicControllerWriteTarget::SlaveMask, static_cast<uint8_t>(mask >> 8));
}

/** Emits one deferred batch in the only safe terminal hardware order. */
template <typename Sink>
void emitPicContentionWrites(const PicIrqState& state, bool writeMask,
                             const size_t (&realEntries)[PicIrqState::LineCount],
                             const PicControllerStateGate::PendingActions& pending,
                             size_t spuriousCascadeEois, Sink&& sink) {
  if (writeMask) {
    sink(PicControllerWriteTarget::MasterMask, state.masterMask());
    sink(PicControllerWriteTarget::SlaveMask, state.slaveMask());
  }

  for (size_t irq = 0; irq < PicIrqState::LineCount; ++irq) {
    const size_t owedEois = realEntries[irq] + pending.tailEoi[irq];
    for (size_t occurrence = 0; occurrence < owedEois; ++occurrence) {
      if (irq > 7) {
        sink(PicControllerWriteTarget::SlaveCommand, static_cast<uint8_t>(0x60 + (irq - 8)));
        sink(PicControllerWriteTarget::MasterCommand, 0x62);
      } else {
        sink(PicControllerWriteTarget::MasterCommand, static_cast<uint8_t>(0x60 + irq));
      }
    }
  }

  for (size_t occurrence = 0; occurrence < spuriousCascadeEois; ++occurrence) {
    sink(PicControllerWriteTarget::MasterCommand, 0x62);
  }
}

#endif
