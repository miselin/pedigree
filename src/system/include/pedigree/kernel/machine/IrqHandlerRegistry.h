/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_MACHINE_IRQHANDLERREGISTRY_H
#define PEDIGREE_KERNEL_MACHINE_IRQHANDLERREGISTRY_H

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"

class IrqHandler;

/**
 * Allocation-free IRQ callback registry with synchronous removal.
 *
 * Dispatch pins a handler before dropping the registry lock. Removal first
 * closes admission and then drains any pins which were already committed.
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

    void setHandlerPinHook(HandlerPinHook hook);
#endif

  private:
    static constexpr size_t MaxHandlerSlots = 64;
    static constexpr uint8_t InvalidIrq = 0xFF;

    struct HandlerDispatch
    {
        void *owner;
        HandlerDispatch *next;
    };

    struct HandlerSlot
    {
        HandlerSlot()
            : handler(nullptr), irq(InvalidIrq), inFlight(0), enabled(false),
              deferredRemoval(false), draining(false), dispatches(nullptr),
              drainWaiters()
        {
        }

        IrqHandler *handler;
        uint8_t irq;
        size_t inFlight;
        bool enabled;
        bool deferredRemoval;
        bool draining;
        HandlerDispatch *dispatches;
        WaitQueue drainWaiters;
    };

    static void clearSlot(HandlerSlot &slot);
    static void *currentDispatchOwner();

    HandlerSlot m_Handlers[MaxHandlerSlots];
    Spinlock m_HandlerLock;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    HandlerPinHook m_HandlerPinHook;
#endif
};

#endif
