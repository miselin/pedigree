/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_MACHINE_IRQHANDLERREGISTRY_H
#define PEDIGREE_KERNEL_MACHINE_IRQHANDLERREGISTRY_H
#include <config.h>

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/process/AtomicStateCleanup.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"

class HardIrqHandler;
class IrqHandler;
class IrqHandlerBase;
class Thread;
enum class HardIrqDisposition;

/**
 * Allocation-free IRQ callback registry with synchronous removal.
 *
 * Dispatch never takes the registry's writer lock. It pins an atomically
 * published slot and revalidates that publication before entering the
 * callback. Removal first closes admission and then drains callbacks which
 * committed their pins before that transition. Callback release only clears
 * atomic hazard state; a synchronous unregistering thread owns any cooperative
 * scheduling needed while it rechecks the drain predicate.
 */
class EXPORTED_PUBLIC IrqHandlerRegistry {
 public:
  struct ThreadedDispatchResult {
    bool handled;
    bool allowRearm;
  };

  struct AdmissionCutoff {
    size_t epoch;
    size_t occurrenceEpoch;
    size_t readerToken;
  };

  struct MixedAdmissionCutoffs {
    AdmissionCutoff hard;
    AdmissionCutoff threaded;
  };

  enum class UnregisterResult {
    Completed,
    Deferred,
    Rejected,
    NotFound,
  };

  enum class LineMode {
    Empty,
    Threaded,
    HardOnly,
    Mixed,
  };

  /** One coherent, detached view of a physical line's admitted handlers. */
  struct LineConfiguration {
    LineConfiguration()
        : mutationGeneration(0),
          handlerCount(0),
          mode(LineMode::Empty),
          policyConfigured(false),
          trigger(IrqTrigger::Edge),
          controllerAck(IrqControllerAck::None),
          lineRelease(IrqLineRelease::AfterHardStage) {}

    size_t mutationGeneration;
    size_t handlerCount;
    LineMode mode;
    bool policyConfigured;
    IrqTrigger trigger;
    IrqControllerAck controllerAck;
    IrqLineRelease lineRelease;
  };

  IrqHandlerRegistry();

  /** Publishes an ordinary thread-context handler. */
  bool registerThreadedHandler(uint8_t irq, IrqHandler* handler);

  /** Publishes a typed thread-context handler and line policy. */
  bool registerThreadedHandler(uint8_t irq, IrqHandler* handler, const IrqPolicy& policy);

  /** Publishes an explicit hard-IRQ handler. */
  bool registerHardHandler(uint8_t irq, HardIrqHandler* handler);

  /** Publishes a typed hard-IRQ handler and line policy. */
  bool registerHardHandler(uint8_t irq, HardIrqHandler* handler, const IrqPolicy& policy);

  /**
   * Stops future callbacks and drains callbacks already in progress.
   *
   * Self-removal cannot synchronously drain its own callback and is deferred
   * until that callback returns. Atomic contexts reject a removal which
   * would otherwise have to wait. A synchronous caller must not retain a
   * resource which the active handler needs to finish.
   *
   * The device source must be masked or quiesced before this call. Removal
   * closes and drains registry admission; it cannot stop hardware from
   * raising a new occurrence.
   */
  UnregisterResult unregisterHandler(uint8_t irq, IrqHandlerBase* handler);

  /**
   * Also reports the exact delivery of the slot whose admission was closed.
   *
   * `removedDelivery` is Empty when no matching slot was found and is never
   * Mixed: each registry slot retains one private delivery bit.
   */
  UnregisterResult unregisterHandler(uint8_t irq, IrqHandlerBase* handler,
                                     LineMode& removedDelivery);

  /**
   * Dispatches all enabled handlers for an IRQ.
   *
   * Returns true if at least one callback was admitted. `disposition`
   * aggregates callback results, with KeepMasked dominating Handled and
   * Handled dominating NotHandled.
   */
  bool dispatchHard(uint8_t irq, InterruptState& state, HardIrqDisposition& disposition,
                    HardIrqHandler* onlyHandler = nullptr, size_t dispatchGeneration = 0);
  bool dispatchHard(uint8_t irq, InterruptState& state, HardIrqDisposition& disposition,
                    HardIrqHandler* onlyHandler, size_t dispatchGeneration,
                    AdmissionCutoff admissionCutoff);

  /** Reports a sticky KeepMasked result from any live hard source. */
  bool hardLineQuarantined(uint8_t irq) const;

  /**
   * Records the exact threaded-handler publications admitted for one
   * physical occurrence.
   *
   * The nonzero generation is supplied again by the line worker. Repeated
   * occurrences may coalesce to the newest generation, but a handler which
   * was registered after this call is never admitted retroactively.
   * A controller serializes publication and invalidation for each physical
   * line; worker consumption remains concurrent.
   */
  bool publishThreadedDispatch(uint8_t irq, size_t dispatchGeneration);
  bool publishThreadedDispatch(uint8_t irq, size_t dispatchGeneration,
                               AdmissionCutoff admissionCutoff);

  /**
   * Captures one occurrence cutoff before controller delivery is sampled.
   * A false return leaves `cutoff` empty and transfers no lease.
   */
  bool captureAdmissionCutoff(uint8_t irq, AdmissionCutoff& cutoff);

  /**
   * Captures independent hard and threaded leases for one occurrence.
   *
   * Each cutoff must be consumed by its dispatch operation or released
   * separately. Both leases have the same occurrence and admission epochs.
   * A false return leaves both cutoffs empty and transfers no leases.
   */
  bool captureMixedAdmissionCutoffs(uint8_t irq, MixedAdmissionCutoffs& cutoffs);

  /** Releases a cutoff which was not consumed by a dispatch operation. */
  void releaseAdmissionCutoff(AdmissionCutoff admissionCutoff);

  /** Invalidates queued threaded work from an ended controller lifetime. */
  void invalidateThreadedLine(uint8_t irq, size_t throughGeneration);

  /**
   * Invalidates a generation in bounded hard-IRQ context.
   *
   * The controller serialises this with publication for the physical line.
   * Token and tombstone cleanup is deferred to workers or full line
   * invalidation.
   */
  void invalidateThreadedGenerationFromInterrupt(uint8_t irq, size_t throughGeneration);

  /**
   * Dispatches handlers admitted by publishThreadedDispatch() without
   * exposing interrupted state.
   *
   * Returns true when at least one callback was admitted and writes the
   * detached callback outcome to `result`.
   *
   * A Quiesced callback permits line rearm but remains distinct from a
   * callback which actually handled the occurrence.
   */
  bool dispatchThreaded(uint8_t irq, size_t dispatchGeneration, ThreadedDispatchResult& result,
                        IrqHandler* onlyHandler = nullptr);

  size_t handlerCount(uint8_t irq);

  /** Returns the delivery type of an enabled physical line. */
  LineMode lineMode(uint8_t irq);

  /**
   * Takes a bounded lock-free snapshot of one line's admitted configuration.
   *
   * A false return means a concurrent mutation outlived both bounded
   * attempts; callers may leave observational state stale and try again
   * later.
   */
  bool snapshotLineConfiguration(uint8_t irq, LineConfiguration& configuration) const;

  /**
   * Lock-free debugger query for committed hard callbacks on one line.
   *
   * Returns the active count and writes the exact controller generation only
   * when one callback is active. Multiple callbacks make the generation
   * ambiguous and write zero.
   */
  size_t hardDispatchState(uint8_t irq, size_t& exactGeneration) const;

  /**
   * Lock-free debugger query for committed threaded callbacks on one line.
   *
   * The committed pin includes the bounded setup and cleanup immediately
   * around the virtual call. Returns the active count and writes the exact
   * handler identity only when one pin is active. Multiple callbacks make the
   * identity ambiguous and write zero.
   */
  size_t threadedDispatchState(uint8_t irq, uintptr_t& exactHandlerIdentity) const;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  enum class HandlerHazardStage {
    BeforeClaim,
    Claimed,
    Committed,
    Released,
    RetirementBoundaryPublished,
    BeforeClaimFinalization,
    FinalizationContended,
    BeforeActionMutationPin,
    QuiescedObserved,
    BeforePendingExchange,
    PendingExchanged,
    BeforeQuiescedExchange,
    QuiescedExchanged,
  };

  enum class OccurrenceCaptureStage {
    BankZeroClaimed,
    BankOneClaimed,
    EpochSampled,
    UnusedBankReleased,
  };

  using HandlerPinHook = void (*)(IrqHandlerBase*);
  using HandlerPrePinHook = void (*)(IrqHandlerBase*);
  using HandlerHazardHook = void (*)(IrqHandlerBase*, HandlerHazardStage);
  using DispatchAbandonHook = void (*)(void*, bool);
  using MutationLockHook = void (*)();
  using OccurrenceCaptureHook = void (*)(IrqHandlerRegistry*, uint8_t, OccurrenceCaptureStage,
                                         size_t);

  void setHandlerPinHook(HandlerPinHook hook);
  void setHandlerPrePinHook(HandlerPrePinHook hook);
  void setHandlerHazardHook(HandlerHazardHook hook);
  void setDispatchAbandonHook(DispatchAbandonHook hook);
  void setOccurrenceCaptureHookForTest(OccurrenceCaptureHook hook);
  void withMutationLockForTest(MutationLockHook hook);
  void withMutationEpochForTest(MutationLockHook hook);
  size_t activeDispatchCountForTest(IrqHandlerBase* handler);
  size_t claimedDispatchCountForOwnerForTest(void* owner);
  bool containsHandlerForTest(uint8_t irq, IrqHandlerBase* handler);
  size_t tombstoneCountForTest(uint8_t irq) const;
  size_t threadedActionMutationWriterCountForTest() const;
  bool setThreadedActionLanesForTest(IrqHandlerBase* handler, size_t pendingGeneration,
                                     size_t claimedGeneration, size_t quiescedGeneration);
  bool consumeThreadedQuiescedForTest(IrqHandlerBase* handler, size_t generation);
  bool publishControllerQuiescedForTest(IrqHandlerBase* handler, uint8_t irq, size_t generation);
#endif

 private:
  enum class Delivery : size_t {
    Threaded,
    HardOnly,
  };

  static constexpr size_t MaxHandlerSlots = 64;
  static constexpr size_t MaxActiveDispatches = 64;
  static constexpr size_t IrqCount = 256;
  // Bucket collisions delay tombstone reuse but cannot admit a stale slot.
  static constexpr size_t GraceBucketCount = 16;
  static constexpr uint8_t InvalidIrq = 0xFF;

  enum class SlotMode : size_t {
    Empty = 0,
    Enabled,
    Draining,
    Cancelling,
    Closed,
    Retiring,
    Tombstone,
  };

  enum class QuiescedLane : size_t {
    Controller,
    Callback,
    Retirement,
    Count,
  };

  static constexpr size_t QuiescedLaneCount = static_cast<size_t>(QuiescedLane::Count);

  static constexpr size_t ModeBits = 3;
  static constexpr size_t ModeMask = (1 << ModeBits) - 1;
  static constexpr size_t DeliveryBits = 1;
  static constexpr size_t DeliveryShift = ModeBits;
  static constexpr size_t DeliveryMask = ((1 << DeliveryBits) - 1) << DeliveryShift;
  static constexpr size_t IrqShift = DeliveryShift + DeliveryBits;
  static constexpr size_t IrqMask = 0xFF << IrqShift;
  static constexpr size_t GenerationShift = IrqShift + 8;
  static constexpr size_t MaximumPublicationGeneration = ~static_cast<size_t>(0) >> GenerationShift;

  static constexpr size_t PolicyValid = 1U << 0;
  static constexpr size_t PolicyTriggerShift = 1;
  static constexpr size_t PolicyControllerAckShift = 3;
  static constexpr size_t PolicyLineReleaseShift = 5;
  static constexpr size_t PolicyTriggerMask = 3U << PolicyTriggerShift;
  static constexpr size_t PolicyControllerAckMask = 3U << PolicyControllerAckShift;
  static constexpr size_t PolicyLineReleaseMask = 1U << PolicyLineReleaseShift;
  static constexpr size_t PolicyMixedCompatibilityMask =
      PolicyTriggerMask | PolicyControllerAckMask;
  static constexpr size_t LineSnapshotAttempts = 2;

  struct HandlerSlot;

  struct ActiveDispatch {
    ActiveDispatch()
        : token(nullptr),
          generation(0),
          owner(nullptr),
          admittedPublication(0),
          controllerGeneration(0),
          slot(nullptr),
          callback(0) {}

    void* token;
    size_t generation;
    void* owner;
    size_t admittedPublication;
    size_t controllerGeneration;
    HandlerSlot* slot;
    size_t callback;
  };

  struct HandlerSlot {
    HandlerSlot()
        : handler(nullptr),
          publication(0),
          policy(0),
          admissionEpoch(0),
          pendingThreadedGeneration(0),
          claimedThreadedGeneration(0),
          quiescedThreadedGenerations(),
          retirementEpoch(0),
          hardHandoffState(0),
          finalizationGate(0) {}

    IrqHandlerBase* handler;
    size_t publication;
    size_t policy;
    size_t admissionEpoch;
    size_t pendingThreadedGeneration;
    size_t claimedThreadedGeneration;
    size_t quiescedThreadedGenerations[QuiescedLaneCount];
    size_t retirementEpoch;
    /** Generation in the high bits; KeepMasked is sticky in bit zero. */
    size_t hardHandoffState;
    size_t finalizationGate;
  };

  struct DispatchCleanup {
    DispatchCleanup(IrqHandlerRegistry* registry, HandlerSlot* handlerSlot, void* dispatchOwner,
                    size_t admittedPublication, bool isCallback = true)
        : registry(registry),
          slot(handlerSlot),
          owner(dispatchOwner),
          publication(admittedPublication),
          callback(isCallback),
          previousDeviceHardIrqDepth(0),
          restoreDeviceHardIrqDepth(false),
          previousInterruptState(false),
          restoreInterruptState(false),
          cleanup() {}

    IrqHandlerRegistry* registry;
    HandlerSlot* slot;
    void* owner;
    size_t publication;
    bool callback;
    size_t previousDeviceHardIrqDepth;
    bool restoreDeviceHardIrqDepth;
    bool previousInterruptState;
    bool restoreInterruptState;
    AtomicStateCleanupRecord cleanup;
  };

  struct AdmissionCutoffCleanup {
    AdmissionCutoffCleanup(IrqHandlerRegistry* owner, AdmissionCutoff admissionCutoff)
        : registry(owner), cutoff(admissionCutoff), thread(nullptr), ownsCutoff(false), cleanup() {}

    IrqHandlerRegistry* registry;
    AdmissionCutoff cutoff;
    Thread* thread;
    bool ownsCutoff;
    AtomicStateCleanupRecord cleanup;
  };

  struct ThreadedActionMutationCleanup {
    explicit ThreadedActionMutationCleanup(IrqHandlerRegistry* owner)
        : registry(owner), thread(nullptr), cleanup() {}

    IrqHandlerRegistry* registry;
    Thread* thread;
    AtomicStateCleanupRecord cleanup;
  };

  static size_t makePublication(size_t generation, uint8_t irq, SlotMode mode, Delivery delivery);
  static size_t generationOf(size_t publication);
  static uint8_t irqOf(size_t publication);
  static SlotMode modeOf(size_t publication);
  static Delivery deliveryOf(size_t publication);
  static bool generationReached(size_t current, size_t target);
  static size_t encodePolicy(const IrqPolicy* policy);
  static void decodePolicy(size_t policy, LineConfiguration& configuration);
  static bool mixedPoliciesCompatible(size_t first, size_t second);
  static size_t effectiveMixedPolicy(size_t hard, size_t threaded);
  static LineMode lineModeForDelivery(Delivery delivery);

  bool registerHandler(uint8_t irq, IrqHandlerBase* handler, Delivery delivery, size_t policy);

  bool acquireOccurrenceReaderLeases(uint8_t irq, size_t readerBank, size_t count);
  void releaseOccurrenceReaderLeases(uint8_t irq, size_t readerBank, size_t count);
  void beginAdmissionCutoffCleanup(AdmissionCutoffCleanup& cleanup);
  void finishAdmissionCutoffCleanup(AdmissionCutoffCleanup& cleanup);
  static void abandonAdmissionCutoff(void* context);

  void beginMutation();
  void finishMutation();
  void beginThreadedActionMutation(ThreadedActionMutationCleanup& cleanup);
  void finishThreadedActionMutation(ThreadedActionMutationCleanup& cleanup);
  void completeThreadedActionMutation();
  static void abandonThreadedActionMutation(void* context);

  bool retireSlot(HandlerSlot& slot, size_t expectedPublication, IrqHandlerBase* expectedHandler);
  bool retireSlotOrObserveClosed(HandlerSlot& slot, size_t expectedPublication,
                                 IrqHandlerBase* expectedHandler);
  bool closeSlotAdmission(HandlerSlot& slot, size_t expectedPublication, size_t& closedPublication);
  void tryReclaimTombstones(uint8_t irq);
  bool occurrencePrecedesRetirement(const HandlerSlot& slot, AdmissionCutoff admissionCutoff) const;
  ActiveDispatch* publishDispatch(HandlerSlot& slot, void* owner, void* token,
                                  size_t admittedPublication, size_t controllerGeneration,
                                  bool callback = true);
  bool unpublishDispatch(void* token, HandlerSlot& slot, size_t admittedPublication, bool required);
  static void abandonDispatch(void* context);
  static void restoreDispatchInterruptState(DispatchCleanup& dispatch);
  static bool canWaitForActionFinalization();
  bool acquireFinalizationGate(HandlerSlot& slot, bool canWait);
  static void releaseFinalizationGate(HandlerSlot& slot);
  bool pinActionMutation(HandlerSlot& slot, size_t publication, DispatchCleanup& cleanup,
                         Thread* thread);
  void unpinActionMutation(HandlerSlot& slot, size_t publication, DispatchCleanup& cleanup,
                           Thread* thread);
  bool hasActiveDispatch(HandlerSlot& slot, size_t admittedPublication) const;
  bool findCurrentDispatch(void* owner, HandlerSlot* target, size_t targetPublication,
                           bool& callbackContext) const;
  static void* currentDispatchOwner();
  bool threadedGenerationValid(uint8_t irq, size_t generation) const;
  void publishSlotQuiesced(HandlerSlot& slot, uint8_t irq, size_t dispatchGeneration,
                           QuiescedLane lane);
  void publishSlotQuiescedValue(HandlerSlot& slot, uint8_t irq, size_t dispatchGeneration,
                                QuiescedLane lane);
  static size_t* quiescedLane(HandlerSlot& slot, QuiescedLane lane);
  static const size_t* quiescedLane(const HandlerSlot& slot, QuiescedLane lane);
  static bool hasQuiescedGeneration(const HandlerSlot& slot);

  HandlerSlot m_Handlers[MaxHandlerSlots];
  ActiveDispatch m_ActiveDispatches[MaxActiveDispatches];
  /** Makes the bounded per-line hard-veto scan coherent across mutations. */
  size_t m_HardHandoffEpochs[GraceBucketCount];
  size_t m_ThreadedInvalidationGenerations[IrqCount];
  size_t m_ThreadedActionMutationGeneration;
  size_t m_ThreadedActionMutationWriters;
  size_t m_OccurrenceEpochs[GraceBucketCount];
  size_t m_OccurrenceReaders[GraceBucketCount][2];
  size_t m_OccurrenceBoundaryLocks[GraceBucketCount];
  Spinlock m_HandlerLock;
  size_t m_AdmissionEpoch;
  size_t m_MutationGeneration;
  size_t m_MutationWriters;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  void observeOccurrenceCaptureForTest(uint8_t irq, OccurrenceCaptureStage stage,
                                       size_t occurrenceEpoch);
  HandlerPinHook m_HandlerPinHook;
  HandlerPrePinHook m_HandlerPrePinHook;
  HandlerHazardHook m_HandlerHazardHook;
  DispatchAbandonHook m_DispatchAbandonHook;
  OccurrenceCaptureHook m_OccurrenceCaptureHook;
#endif
};

#endif
