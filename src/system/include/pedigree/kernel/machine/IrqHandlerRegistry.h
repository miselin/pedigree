/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_MACHINE_IRQHANDLERREGISTRY_H
#define PEDIGREE_KERNEL_MACHINE_IRQHANDLERREGISTRY_H

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/process/AtomicStateCleanup.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"

class IrqHandler;

/**
 * Allocation-free IRQ callback registry with synchronous removal.
 *
 * Dispatch never takes the registry's writer lock. It pins an atomically
 * published slot and revalidates that publication before entering the
 * callback. Removal first closes admission and then drains callbacks which
 * committed their pins before that transition.
 */
class IrqHandlerRegistry
{
  public:
    enum class UnregisterResult
    {
        Completed,
        Deferred,
        Rejected,
        NotFound,
    };

    IrqHandlerRegistry();

    bool registerHandler(uint8_t irq, IrqHandler *handler);

    /**
     * Stops future callbacks and drains callbacks already in progress.
     *
     * Self-removal cannot synchronously drain its own callback and is deferred
     * until that callback returns. Atomic contexts reject a removal which
     * would otherwise have to wait.
     */
    UnregisterResult unregisterHandler(uint8_t irq, IrqHandler *handler);

    /**
     * Dispatches all enabled handlers for an IRQ.
     *
     * Returns true if at least one callback was admitted. `handled` aggregates
     * the callback return values.
     */
    bool dispatch(
        uint8_t irq, InterruptState &state, bool &handled,
        IrqHandler *onlyHandler = nullptr);

    size_t handlerCount(uint8_t irq);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    using HandlerPinHook = void (*)(IrqHandler *);
    using HandlerPrePinHook = void (*)(IrqHandler *);
    using MutationLockHook = void (*)();

    void setHandlerPinHook(HandlerPinHook hook);
    void setHandlerPrePinHook(HandlerPrePinHook hook);
    void withMutationLockForTest(MutationLockHook hook);
    size_t activeDispatchCountForTest(IrqHandler *handler);
#endif

  private:
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
    static constexpr size_t IrqShift = ModeBits;
    static constexpr size_t IrqMask = 0xFF << IrqShift;
    static constexpr size_t GenerationShift = IrqShift + 8;

    struct HandlerSlot;

    struct ActiveDispatch
    {
        ActiveDispatch()
            : token(nullptr), generation(0), owner(nullptr), slot(nullptr)
        {
        }

        void *token;
        size_t generation;
        void *owner;
        HandlerSlot *slot;
    };

    struct HandlerSlot
    {
        HandlerSlot() : handler(nullptr), publication(0)
        {
        }

        IrqHandler *handler;
        size_t publication;
    };

    struct DispatchCleanup
    {
        DispatchCleanup(
            IrqHandlerRegistry *registry, HandlerSlot *handlerSlot,
            void *dispatchOwner)
            : registry(registry), slot(handlerSlot), owner(dispatchOwner),
              cleanup()
        {
        }

        IrqHandlerRegistry *registry;
        HandlerSlot *slot;
        void *owner;
        AtomicStateCleanupRecord cleanup;
    };

    static size_t
    makePublication(size_t generation, uint8_t irq, SlotMode mode);
    static size_t generationOf(size_t publication);
    static uint8_t irqOf(size_t publication);
    static SlotMode modeOf(size_t publication);

    bool retireSlot(
        HandlerSlot &slot, size_t expectedPublication,
        IrqHandler *expectedHandler);
    ActiveDispatch *
    publishDispatch(HandlerSlot &slot, void *owner, void *token);
    void unpublishDispatch(void *token, HandlerSlot &slot);
    static void abandonDispatch(void *context);
    bool hasActiveDispatch(HandlerSlot &slot) const;
    bool findCurrentDispatch(
        void *owner, HandlerSlot *target, bool &callbackContext) const;
    static void *currentDispatchOwner();

    HandlerSlot m_Handlers[MaxHandlerSlots];
    ActiveDispatch m_ActiveDispatches[MaxActiveDispatches];
    Spinlock m_HandlerLock;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    HandlerPinHook m_HandlerPinHook;
    HandlerPrePinHook m_HandlerPrePinHook;
#endif
};

#endif
