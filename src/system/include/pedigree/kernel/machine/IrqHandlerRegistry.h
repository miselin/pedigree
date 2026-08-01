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
#include "pedigree/kernel/process/AtomicStateCleanup.h"
#include "pedigree/kernel/process/WaitQueue.h"
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
 * committed their pins before that transition.
 */
class EXPORTED_PUBLIC IrqHandlerRegistry
{
  public:
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

    IrqHandlerRegistry();

    /** Publishes an ordinary thread-context handler. */
    bool registerThreadedHandler(uint8_t irq, IrqHandler *handler);

    /** Publishes an explicit hard-IRQ handler. */
    bool registerHardHandler(uint8_t irq, HardIrqHandler *handler);

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
        HardIrqHandler *onlyHandler = nullptr);

    /** Dispatches threaded handlers without exposing interrupted state. */
    bool dispatchThreaded(
        uint8_t irq, bool &handled, IrqHandler *onlyHandler = nullptr);

    size_t handlerCount(uint8_t irq);

    /** Returns the delivery type of an enabled physical line. */
    LineMode lineMode(uint8_t irq);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    enum class HandlerHazardStage
    {
        BeforeClaim,
        Claimed,
        Committed,
    };

    using HandlerPinHook = void (*)(IrqHandlerBase *);
    using HandlerPrePinHook = void (*)(IrqHandlerBase *);
    using HandlerHazardHook = void (*)(IrqHandlerBase *, HandlerHazardStage);
    using MutationLockHook = void (*)();

    void setHandlerPinHook(HandlerPinHook hook);
    void setHandlerPrePinHook(HandlerPrePinHook hook);
    void setHandlerHazardHook(HandlerHazardHook hook);
    void withMutationLockForTest(MutationLockHook hook);
    size_t activeDispatchCountForTest(IrqHandlerBase *handler);
    size_t claimedDispatchCountForTest();
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

    struct HandlerSlot;

    struct ActiveDispatch
    {
        ActiveDispatch()
            : token(nullptr), generation(0), owner(nullptr),
              admittedPublication(0), slot(nullptr)
        {
        }

        void *token;
        size_t generation;
        void *owner;
        size_t admittedPublication;
        HandlerSlot *slot;
    };

    struct HandlerSlot
    {
        HandlerSlot() : handler(nullptr), publication(0)
        {
        }

        IrqHandlerBase *handler;
        size_t publication;
    };

    struct DispatchCleanup
    {
        DispatchCleanup(
            IrqHandlerRegistry *registry, HandlerSlot *handlerSlot,
            void *dispatchOwner, size_t admittedPublication)
            : registry(registry), slot(handlerSlot), owner(dispatchOwner),
              publication(admittedPublication), cleanup()
        {
        }

        IrqHandlerRegistry *registry;
        HandlerSlot *slot;
        void *owner;
        size_t publication;
        AtomicStateCleanupRecord cleanup;
    };

    static size_t makePublication(
        size_t generation, uint8_t irq, SlotMode mode, Delivery delivery);
    static size_t generationOf(size_t publication);
    static uint8_t irqOf(size_t publication);
    static SlotMode modeOf(size_t publication);
    static Delivery deliveryOf(size_t publication);

    /** All handlers on one physical line must use one delivery mode. */
    bool
    registerHandler(uint8_t irq, IrqHandlerBase *handler, Delivery delivery);

    bool retireSlot(
        HandlerSlot &slot, size_t expectedPublication,
        IrqHandlerBase *expectedHandler);
    ActiveDispatch *publishDispatch(
        HandlerSlot &slot, void *owner, void *token,
        size_t admittedPublication);
    bool unpublishDispatch(
        void *token, HandlerSlot &slot, size_t admittedPublication,
        bool required);
    static void abandonDispatch(void *context);
    bool hasActiveDispatch(HandlerSlot &slot, size_t admittedPublication) const;
    bool findCurrentDispatch(
        void *owner, HandlerSlot *target, size_t targetPublication,
        bool &callbackContext) const;
    static void *currentDispatchOwner();

    HandlerSlot m_Handlers[MaxHandlerSlots];
    ActiveDispatch m_ActiveDispatches[MaxActiveDispatches];
    WaitQueue m_DispatchWaiters;
    Spinlock m_HandlerLock;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    HandlerPinHook m_HandlerPinHook;
    HandlerPrePinHook m_HandlerPrePinHook;
    HandlerHazardHook m_HandlerHazardHook;
#endif
};

#endif
