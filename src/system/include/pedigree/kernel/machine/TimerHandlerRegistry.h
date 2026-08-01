/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_MACHINE_TIMERHANDLERREGISTRY_H
#define PEDIGREE_KERNEL_MACHINE_TIMERHANDLERREGISTRY_H

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"

class TimerHandler;

/**
 * Allocation-free timer callback registry with synchronous removal.
 *
 * Dispatch pins atomically published slots without taking the writer lock.
 * Removal closes admission before draining callbacks which committed a pin.
 */
class TimerHandlerRegistry
{
  public:
    TimerHandlerRegistry();

    bool registerHandler(TimerHandler *handler);

    /**
     * Stops future callbacks and drains callbacks already in progress.
     *
     * Self-removal is deferred until the callback returns and reports false,
     * preserving Timer's synchronous boolean contract. Other atomic callers
     * report false instead of waiting for an in-flight callback.
     */
    bool unregisterHandler(TimerHandler *handler);

    /** Calls each handler admitted by the current publication. */
    bool dispatch(
        uint64_t delta, InterruptState &state,
        TimerHandler *onlyHandler = nullptr);

    /** Clears quiescent registry state during timer lifecycle transitions. */
    void reset();

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    using HandlerPinHook = void (*)(TimerHandler *);
    using HandlerPrePinHook = void (*)(TimerHandler *);
    using HandlerAtomicDrainHook = void (*)(TimerHandler *);
    using MutationLockHook = void (*)();

    void setHandlerPinHook(HandlerPinHook hook);
    void setHandlerPrePinHook(HandlerPrePinHook hook);
    void setHandlerAtomicDrainHook(HandlerAtomicDrainHook hook);
    void withMutationLockForTest(MutationLockHook hook);
#endif

  private:
    static constexpr size_t MaxHandlerSlots = 32;
    static constexpr size_t MaxActiveDispatches = 64;

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
    static constexpr size_t SelfRemovalBit = 1 << ModeBits;
    static constexpr size_t SynchronousDrainBit = 1 << (ModeBits + 1);
    static constexpr size_t GenerationShift = ModeBits + 2;

    struct HandlerSlot;

    struct ActiveDispatch
    {
        ActiveDispatch() : owner(nullptr), slot(nullptr)
        {
        }

        void *owner;
        HandlerSlot *slot;
    };

    struct HandlerSlot
    {
        HandlerSlot() : handler(nullptr), publication(0), inFlight(0)
        {
        }

        TimerHandler *handler;
        size_t publication;
        size_t inFlight;
    };

    static size_t makePublication(
        size_t generation, SlotMode mode, bool selfRemoval = false,
        bool synchronousDrain = false);
    static size_t generationOf(size_t publication);
    static SlotMode modeOf(size_t publication);
    static bool selfRemovalOf(size_t publication);
    static bool synchronousDrainOf(size_t publication);

    bool retireSlot(
        HandlerSlot &slot, size_t expectedPublication,
        TimerHandler *expectedHandler);
    ActiveDispatch *publishDispatch(HandlerSlot &slot, void *owner);
    void unpublishDispatch(ActiveDispatch *dispatch);
    void releasePin(HandlerSlot &slot);
    bool findCurrentDispatch(
        void *owner, HandlerSlot *target, bool &callbackContext) const;
    static void *currentDispatchOwner();

    HandlerSlot m_Handlers[MaxHandlerSlots];
    ActiveDispatch m_ActiveDispatches[MaxActiveDispatches];
    Spinlock m_HandlerLock;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    HandlerPinHook m_HandlerPinHook;
    HandlerPrePinHook m_HandlerPrePinHook;
    HandlerAtomicDrainHook m_HandlerAtomicDrainHook;
#endif
};

#endif
