/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_MACHINE_IRQHANDLERREGISTRY_H
#define PEDIGREE_KERNEL_MACHINE_IRQHANDLERREGISTRY_H

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/process/AtomicStateCleanup.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"

class HardIrqHandler;
class IrqHandler;
class IrqHandlerBase;

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
class EXPORTED_PUBLIC IrqHandlerRegistry
{
  public:
    struct ThreadedDispatchResult
    {
        bool handled;
        bool allowRearm;
    };

    enum class UnregisterResult
    {
        Completed,
        Deferred,
        Rejected,
        NotFound,
    };

    enum class LineMode
    {
        Empty,
        Threaded,
        HardOnly,
    };

    /** One coherent, detached view of a physical line's admitted handlers. */
    struct LineConfiguration
    {
        LineConfiguration()
            : mutationGeneration(0), handlerCount(0), mode(LineMode::Empty),
              policyConfigured(false), trigger(IrqTrigger::Edge),
              controllerAck(IrqControllerAck::None),
              lineRelease(IrqLineRelease::AfterHardStage)
        {
        }

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
    bool registerThreadedHandler(uint8_t irq, IrqHandler *handler);

    /** Publishes a typed thread-context handler and line policy. */
    bool registerThreadedHandler(
        uint8_t irq, IrqHandler *handler, const IrqPolicy &policy);

    /** Publishes an explicit hard-IRQ handler. */
    bool registerHardHandler(uint8_t irq, HardIrqHandler *handler);

    /** Publishes a typed hard-IRQ handler and line policy. */
    bool registerHardHandler(
        uint8_t irq, HardIrqHandler *handler, const IrqPolicy &policy);

    /**
     * Stops future callbacks and drains callbacks already in progress.
     *
     * Self-removal cannot synchronously drain its own callback and is deferred
     * until that callback returns. Atomic contexts reject a removal which
     * would otherwise have to wait. A synchronous caller must not retain a
     * resource which the active handler needs to finish.
     */
    UnregisterResult unregisterHandler(uint8_t irq, IrqHandlerBase *handler);

    /**
     * Dispatches all enabled handlers for an IRQ.
     *
     * Returns true if at least one callback was admitted. `handled` aggregates
     * the callback return values.
     */
    bool dispatchHard(
        uint8_t irq, InterruptState &state, bool &handled,
        HardIrqHandler *onlyHandler = nullptr, size_t dispatchGeneration = 0);

    /**
     * Dispatches threaded handlers without exposing interrupted state.
     *
     * Returns true when at least one callback was admitted and writes the
     * detached callback outcome to `result`.
     *
     * A Quiesced callback permits line rearm but remains distinct from a
     * callback which actually handled the occurrence.
     */
    bool dispatchThreaded(
        uint8_t irq, ThreadedDispatchResult &result,
        IrqHandler *onlyHandler = nullptr);

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
    bool snapshotLineConfiguration(
        uint8_t irq, LineConfiguration &configuration) const;

    /**
     * Lock-free debugger query for committed hard callbacks on one line.
     *
     * Returns the active count and writes the exact controller generation only
     * when one callback is active. Multiple callbacks make the generation
     * ambiguous and write zero.
     */
    size_t hardDispatchState(uint8_t irq, size_t &exactGeneration) const;

    /**
     * Lock-free debugger query for committed threaded callbacks on one line.
     *
     * The committed pin includes the bounded setup and cleanup immediately
     * around the virtual call. Returns the active count and writes the exact
     * handler identity only when one pin is active. Multiple callbacks make the
     * identity ambiguous and write zero.
     */
    size_t
    threadedDispatchState(uint8_t irq, uintptr_t &exactHandlerIdentity) const;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    enum class HandlerHazardStage
    {
        BeforeClaim,
        Claimed,
        Committed,
        Released,
    };

    using HandlerPinHook = void (*)(IrqHandlerBase *);
    using HandlerPrePinHook = void (*)(IrqHandlerBase *);
    using HandlerHazardHook = void (*)(IrqHandlerBase *, HandlerHazardStage);
    using DispatchAbandonHook = void (*)(void *, bool);
    using MutationLockHook = void (*)();

    void setHandlerPinHook(HandlerPinHook hook);
    void setHandlerPrePinHook(HandlerPrePinHook hook);
    void setHandlerHazardHook(HandlerHazardHook hook);
    void setDispatchAbandonHook(DispatchAbandonHook hook);
    void withMutationLockForTest(MutationLockHook hook);
    void withMutationEpochForTest(MutationLockHook hook);
    size_t activeDispatchCountForTest(IrqHandlerBase *handler);
    size_t claimedDispatchCountForOwnerForTest(void *owner);
    bool containsHandlerForTest(uint8_t irq, IrqHandlerBase *handler);
#endif

  private:
    enum class Delivery : size_t
    {
        Threaded,
        HardOnly,
    };

    static constexpr size_t MaxHandlerSlots = 64;
    static constexpr size_t MaxActiveDispatches = 64;
    static constexpr uint8_t InvalidIrq = 0xFF;

    enum class SlotMode : size_t
    {
        Empty = 0,
        Enabled,
        Draining,
        Deferred,
        Retiring,
    };

    static constexpr size_t ModeBits = 3;
    static constexpr size_t ModeMask = (1 << ModeBits) - 1;
    static constexpr size_t DeliveryBits = 1;
    static constexpr size_t DeliveryShift = ModeBits;
    static constexpr size_t DeliveryMask = ((1 << DeliveryBits) - 1)
                                           << DeliveryShift;
    static constexpr size_t IrqShift = DeliveryShift + DeliveryBits;
    static constexpr size_t IrqMask = 0xFF << IrqShift;
    static constexpr size_t GenerationShift = IrqShift + 8;

    static constexpr size_t PolicyValid = 1U << 0;
    static constexpr size_t PolicyTriggerShift = 1;
    static constexpr size_t PolicyControllerAckShift = 3;
    static constexpr size_t PolicyLineReleaseShift = 5;
    static constexpr size_t LineSnapshotAttempts = 2;

    struct HandlerSlot;

    struct ActiveDispatch
    {
        ActiveDispatch()
            : token(nullptr), generation(0), owner(nullptr),
              admittedPublication(0), controllerGeneration(0), slot(nullptr)
        {
        }

        void *token;
        size_t generation;
        void *owner;
        size_t admittedPublication;
        size_t controllerGeneration;
        HandlerSlot *slot;
    };

    struct HandlerSlot
    {
        HandlerSlot() : handler(nullptr), publication(0), policy(0)
        {
        }

        IrqHandlerBase *handler;
        size_t publication;
        size_t policy;
    };

    struct DispatchCleanup
    {
        DispatchCleanup(
            IrqHandlerRegistry *registry, HandlerSlot *handlerSlot,
            void *dispatchOwner, size_t admittedPublication)
            : registry(registry), slot(handlerSlot), owner(dispatchOwner),
              publication(admittedPublication), previousDeviceHardIrqDepth(0),
              restoreDeviceHardIrqDepth(false), previousInterruptState(false),
              restoreInterruptState(false), cleanup()
        {
        }

        IrqHandlerRegistry *registry;
        HandlerSlot *slot;
        void *owner;
        size_t publication;
        size_t previousDeviceHardIrqDepth;
        bool restoreDeviceHardIrqDepth;
        bool previousInterruptState;
        bool restoreInterruptState;
        AtomicStateCleanupRecord cleanup;
    };

    static size_t makePublication(
        size_t generation, uint8_t irq, SlotMode mode, Delivery delivery);
    static size_t generationOf(size_t publication);
    static uint8_t irqOf(size_t publication);
    static SlotMode modeOf(size_t publication);
    static Delivery deliveryOf(size_t publication);
    static size_t encodePolicy(const IrqPolicy *policy);
    static void decodePolicy(size_t policy, LineConfiguration &configuration);

    /** All handlers on one physical line must use one delivery mode. */
    bool registerHandler(
        uint8_t irq, IrqHandlerBase *handler, Delivery delivery, size_t policy);

    void beginMutation();
    void finishMutation();

    bool retireSlot(
        HandlerSlot &slot, size_t expectedPublication,
        IrqHandlerBase *expectedHandler);
    ActiveDispatch *publishDispatch(
        HandlerSlot &slot, void *owner, void *token, size_t admittedPublication,
        size_t controllerGeneration);
    bool unpublishDispatch(
        void *token, HandlerSlot &slot, size_t admittedPublication,
        bool required);
    static void abandonDispatch(void *context);
    static void restoreDispatchInterruptState(DispatchCleanup &dispatch);
    bool hasActiveDispatch(HandlerSlot &slot, size_t admittedPublication) const;
    bool findCurrentDispatch(
        void *owner, HandlerSlot *target, size_t targetPublication,
        bool &callbackContext) const;
    static void *currentDispatchOwner();

    HandlerSlot m_Handlers[MaxHandlerSlots];
    ActiveDispatch m_ActiveDispatches[MaxActiveDispatches];
    Spinlock m_HandlerLock;
    size_t m_MutationGeneration;
    size_t m_MutationWriters;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    HandlerPinHook m_HandlerPinHook;
    HandlerPrePinHook m_HandlerPrePinHook;
    HandlerHazardHook m_HandlerHazardHook;
    DispatchAbandonHook m_DispatchAbandonHook;
#endif
};

#endif
