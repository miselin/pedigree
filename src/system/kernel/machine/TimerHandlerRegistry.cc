/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/machine/TimerHandlerRegistry.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/TimerHandler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/utilities/assert.h"

TimerHandlerRegistry::TimerHandlerRegistry()
    : m_Handlers(), m_ActiveDispatches(), m_DispatchWaiters(),
      m_HandlerLock(false)
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      ,
      m_HandlerPinHook(nullptr), m_HandlerPrePinHook(nullptr),
      m_HandlerHazardClaimHook(nullptr), m_HandlerAtomicDrainHook(nullptr)
#endif
{
}

size_t TimerHandlerRegistry::makePublication(
    size_t generation, SlotMode mode, bool selfRemoval, bool synchronousDrain)
{
    return (generation << GenerationShift) |
           (selfRemoval ? SelfRemovalBit : 0) |
           (synchronousDrain ? SynchronousDrainBit : 0) |
           static_cast<size_t>(mode);
}

size_t TimerHandlerRegistry::generationOf(size_t publication)
{
    return publication >> GenerationShift;
}

TimerHandlerRegistry::SlotMode TimerHandlerRegistry::modeOf(size_t publication)
{
    return static_cast<SlotMode>(publication & ModeMask);
}

bool TimerHandlerRegistry::selfRemovalOf(size_t publication)
{
    return publication & SelfRemovalBit;
}

bool TimerHandlerRegistry::synchronousDrainOf(size_t publication)
{
    return publication & SynchronousDrainBit;
}

bool TimerHandlerRegistry::retireSlot(
    HandlerSlot &slot, size_t expectedPublication,
    TimerHandler *expectedHandler)
{
    const size_t retiringPublication =
        makePublication(generationOf(expectedPublication), SlotMode::Retiring);
    if (!__atomic_compare_exchange_n(
            &slot.publication, &expectedPublication, retiringPublication, false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        return false;
    }

    assert(__atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) == expectedHandler);
    __atomic_store_n(&slot.handler, nullptr, __ATOMIC_RELEASE);
    __atomic_store_n(
        &slot.publication,
        makePublication(generationOf(retiringPublication), SlotMode::Empty),
        __ATOMIC_RELEASE);
    return true;
}

TimerHandlerRegistry::ActiveDispatch *TimerHandlerRegistry::publishDispatch(
    HandlerSlot &slot, void *owner, void *token)
{
    assert(owner);
    assert(token);
    for (size_t i = 0; i < MaxActiveDispatches; ++i)
    {
        ActiveDispatch &dispatch = m_ActiveDispatches[i];
        void *expectedToken = nullptr;
        if (__atomic_compare_exchange_n(
                &dispatch.token, &expectedToken, token, false, __ATOMIC_SEQ_CST,
                __ATOMIC_SEQ_CST))
        {
            __atomic_add_fetch(
                &dispatch.generation, static_cast<size_t>(1), __ATOMIC_ACQ_REL);
            __atomic_store_n(&dispatch.owner, owner, __ATOMIC_RELAXED);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
            HandlerHazardClaimHook hazardClaimHook =
                __atomic_load_n(&m_HandlerHazardClaimHook, __ATOMIC_ACQUIRE);
            if (hazardClaimHook)
            {
                hazardClaimHook(
                    __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE));
            }
#endif

            // This store is the callback pin. Cleanup is armed before the
            // token claim, so abandoning either side of this publication is
            // recoverable without a separate in-flight counter.
            __atomic_store_n(&dispatch.slot, &slot, __ATOMIC_SEQ_CST);
            return &dispatch;
        }
    }

    return nullptr;
}

bool TimerHandlerRegistry::unpublishDispatch(
    void *token, HandlerSlot &slot, size_t admittedPublication, bool required)
{
    assert(token);
    bool found = false;
    bool committed = false;
    for (size_t i = 0; i < MaxActiveDispatches; ++i)
    {
        ActiveDispatch &dispatch = m_ActiveDispatches[i];
        if (__atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE) != token)
        {
            continue;
        }

        const size_t generation =
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE);
        HandlerSlot *publishedSlot =
            __atomic_load_n(&dispatch.slot, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE) != token ||
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE) !=
                generation)
        {
            continue;
        }

        if (publishedSlot && publishedSlot != &slot)
        {
            FATAL_NOLOCK("Timer callback hazard changed slots during release.");
            return false;
        }

        committed = publishedSlot == &slot;
        __atomic_store_n(&dispatch.slot, nullptr, __ATOMIC_SEQ_CST);
        __atomic_store_n(&dispatch.owner, nullptr, __ATOMIC_RELAXED);
        __atomic_store_n(&dispatch.token, nullptr, __ATOMIC_RELEASE);
        found = true;
        break;
    }

    if (!found)
    {
        if (required)
        {
            FATAL_NOLOCK("Timer callback hazard was released more than once.");
        }
        return false;
    }

    if (!committed || hasActiveDispatch(slot))
    {
        return true;
    }

    const size_t publication =
        __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
    if (generationOf(publication) != generationOf(admittedPublication))
    {
        // A remover won before a partial hazard publication committed and the
        // slot has since been reused. This stale dispatch must not retire the
        // new generation.
        return true;
    }

    const SlotMode mode = modeOf(publication);
    if (
        mode != SlotMode::Draining && mode != SlotMode::Deferred &&
        !selfRemovalOf(publication))
    {
        return true;
    }

    const size_t drainGeneration = generationOf(publication);
    auto guard = m_DispatchWaiters.acquire();
    const size_t finalPublication =
        __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
    if (
        generationOf(finalPublication) != drainGeneration ||
        hasActiveDispatch(slot))
    {
        return true;
    }

    const bool synchronousDrain = synchronousDrainOf(finalPublication);
    if (
        modeOf(finalPublication) == SlotMode::Deferred ||
        (selfRemovalOf(finalPublication) && !synchronousDrain))
    {
        TimerHandler *handler =
            __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE);
        if (handler)
        {
            retireSlot(slot, finalPublication, handler);
        }
    }

    if (synchronousDrain)
    {
        guard.wakeAll(
            WaitQueue::WakeReason::Signalled,
            WaitQueue::Channel(&slot, drainGeneration));
    }
    return true;
}

void TimerHandlerRegistry::abandonDispatch(void *context)
{
    DispatchCleanup *dispatch = reinterpret_cast<DispatchCleanup *>(context);
    dispatch->registry->unpublishDispatch(
        dispatch, *dispatch->slot, dispatch->publication, false);
}

bool TimerHandlerRegistry::hasActiveDispatch(HandlerSlot &target) const
{
    for (size_t i = 0; i < MaxActiveDispatches; ++i)
    {
        const ActiveDispatch &dispatch = m_ActiveDispatches[i];
        void *token = __atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE);
        if (!token)
        {
            continue;
        }

        const size_t generation =
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE);
        HandlerSlot *slot = __atomic_load_n(&dispatch.slot, __ATOMIC_SEQ_CST);
        if (slot == &target &&
            __atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE) == token &&
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE) ==
                generation)
        {
            return true;
        }
    }
    return false;
}

bool TimerHandlerRegistry::findCurrentDispatch(
    void *owner, HandlerSlot *target, bool &callbackContext) const
{
    callbackContext = false;
    bool foundTarget = false;
    for (size_t i = 0; i < MaxActiveDispatches; ++i)
    {
        const ActiveDispatch &dispatch = m_ActiveDispatches[i];
        void *token = __atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE);
        if (!token)
        {
            continue;
        }

        const size_t generation =
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE);
        void *dispatchOwner =
            __atomic_load_n(&dispatch.owner, __ATOMIC_RELAXED);
        HandlerSlot *slot = __atomic_load_n(&dispatch.slot, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE) != token ||
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE) !=
                generation)
        {
            continue;
        }

        if (dispatchOwner == owner && slot)
        {
            callbackContext = true;
            foundTarget |= slot == target;
        }
    }
    return foundTarget;
}

void *TimerHandlerRegistry::currentDispatchOwner()
{
    ProcessorInformation &information = Processor::information();
    Thread *thread = information.getCurrentThread();
    return thread ? static_cast<void *>(thread) :
                    static_cast<void *>(&information);
}

bool TimerHandlerRegistry::registerHandler(TimerHandler *handler)
{
    if (!handler)
    {
        return false;
    }

    LockGuard<Spinlock> guard(m_HandlerLock);
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        while (true)
        {
            const size_t publication =
                __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) != handler)
            {
                break;
            }

            const SlotMode mode = modeOf(publication);
            if (mode == SlotMode::Retiring)
            {
                Processor::pause();
                continue;
            }
            if (mode == SlotMode::Empty)
            {
                break;
            }
            const bool abandonedAtomicDrain = mode == SlotMode::Draining &&
                                              selfRemovalOf(publication) &&
                                              !synchronousDrainOf(publication);
            if (mode != SlotMode::Deferred &&
                !(mode == SlotMode::Enabled && selfRemovalOf(publication)) &&
                !abandonedAtomicDrain)
            {
                return false;
            }

            size_t expectedPublication = publication;
            if (__atomic_compare_exchange_n(
                    &slot.publication, &expectedPublication,
                    makePublication(
                        generationOf(publication), SlotMode::Enabled),
                    false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            {
                return true;
            }
        }
    }

    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        const size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
        if (modeOf(publication) == SlotMode::Empty &&
            !__atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE))
        {
            const size_t generation = generationOf(publication) + 1;
            __atomic_store_n(&slot.handler, handler, __ATOMIC_RELEASE);
            __atomic_store_n(
                &slot.publication,
                makePublication(generation, SlotMode::Enabled),
                __ATOMIC_SEQ_CST);
            return true;
        }
    }

    return false;
}

bool TimerHandlerRegistry::unregisterHandler(TimerHandler *handler)
{
    if (!handler)
    {
        return false;
    }

    void *owner = currentDispatchOwner();
    Thread *current = Processor::information().getCurrentThread();
    const bool canYield = current && Processor::getInterrupts();
    bool callbackContext = false;
    findCurrentDispatch(owner, nullptr, callbackContext);

    // A callback must not arm TerminationDeferral even when a hosted test
    // invokes dispatch with interrupts enabled.
    if (!canYield || callbackContext)
    {
        for (size_t i = 0; i < MaxHandlerSlots; ++i)
        {
            HandlerSlot &candidate = m_Handlers[i];
            while (true)
            {
                size_t publication =
                    __atomic_load_n(&candidate.publication, __ATOMIC_SEQ_CST);
                if (__atomic_load_n(&candidate.handler, __ATOMIC_ACQUIRE) !=
                    handler)
                {
                    break;
                }

                const SlotMode mode = modeOf(publication);
                bool currentTargetDispatch = false;
                const bool selfUnregister = findCurrentDispatch(
                    owner, &candidate, currentTargetDispatch);
                if (selfUnregister)
                {
                    if (mode == SlotMode::Empty || mode == SlotMode::Retiring ||
                        selfRemovalOf(publication))
                    {
                        return false;
                    }

                    const SlotMode deferredMode =
                        mode == SlotMode::Enabled ? SlotMode::Deferred : mode;
                    size_t expectedPublication = publication;
                    if (__atomic_compare_exchange_n(
                            &candidate.publication, &expectedPublication,
                            makePublication(
                                generationOf(publication), deferredMode, true,
                                synchronousDrainOf(publication)),
                            false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                    {
                        return false;
                    }
                    continue;
                }

                if (mode != SlotMode::Enabled || selfRemovalOf(publication))
                {
                    return false;
                }

                const size_t drainingPublication = makePublication(
                    generationOf(publication), SlotMode::Draining);
                if (!__atomic_compare_exchange_n(
                        &candidate.publication, &publication,
                        drainingPublication, false, __ATOMIC_SEQ_CST,
                        __ATOMIC_SEQ_CST))
                {
                    continue;
                }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
                HandlerAtomicDrainHook atomicDrainHook = __atomic_load_n(
                    &m_HandlerAtomicDrainHook, __ATOMIC_ACQUIRE);
                if (atomicDrainHook)
                {
                    atomicDrainHook(handler);
                }
#endif

                if (hasActiveDispatch(candidate))
                {
                    size_t expectedPublication = drainingPublication;
                    const bool reopened = __atomic_compare_exchange_n(
                        &candidate.publication, &expectedPublication,
                        makePublication(
                            generationOf(drainingPublication),
                            SlotMode::Enabled,
                            selfRemovalOf(drainingPublication)),
                        false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                    if (!reopened &&
                        modeOf(expectedPublication) == SlotMode::Draining &&
                        selfRemovalOf(expectedPublication) &&
                        !synchronousDrainOf(expectedPublication))
                    {
                        // This atomic drainer has abandoned removal. Leave the
                        // callback revivable without discarding the concurrent
                        // self-removal request.
                        size_t markedDraining = expectedPublication;
                        __atomic_compare_exchange_n(
                            &candidate.publication, &markedDraining,
                            makePublication(
                                generationOf(expectedPublication),
                                SlotMode::Deferred, true),
                            false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                    }
                    return false;
                }

                return retireSlot(candidate, drainingPublication, handler);
            }
        }

        return false;
    }

    TerminationDeferral terminationDeferral;

    m_HandlerLock.acquire();

    HandlerSlot *slot = nullptr;
    size_t publication = 0;
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        const size_t candidatePublication =
            __atomic_load_n(&m_Handlers[i].publication, __ATOMIC_SEQ_CST);
        if (modeOf(candidatePublication) != SlotMode::Empty &&
            __atomic_load_n(&m_Handlers[i].handler, __ATOMIC_ACQUIRE) ==
                handler)
        {
            slot = &m_Handlers[i];
            publication = candidatePublication;
            break;
        }
    }

    if (!slot)
    {
        m_HandlerLock.release();
        return false;
    }

    size_t drainingPublication = 0;
    while (true)
    {
        const SlotMode mode = modeOf(publication);
        if (mode == SlotMode::Retiring)
        {
            Processor::pause();
            publication = __atomic_load_n(&slot->publication, __ATOMIC_SEQ_CST);
            continue;
        }
        if (mode == SlotMode::Empty)
        {
            const bool retired =
                __atomic_load_n(&slot->handler, __ATOMIC_ACQUIRE) != handler;
            m_HandlerLock.release();
            return retired;
        }
        if (mode != SlotMode::Enabled && mode != SlotMode::Deferred)
        {
            m_HandlerLock.release();
            return false;
        }

        drainingPublication = makePublication(
            generationOf(publication), SlotMode::Draining,
            selfRemovalOf(publication), true);
        size_t expectedPublication = publication;
        if (__atomic_compare_exchange_n(
                &slot->publication, &expectedPublication, drainingPublication,
                false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        {
            break;
        }
        publication = expectedPublication;
    }

    m_HandlerLock.release();

    const size_t drainGeneration = generationOf(drainingPublication);
    while (true)
    {
        auto guard = m_DispatchWaiters.acquire();
        const size_t finalPublication =
            __atomic_load_n(&slot->publication, __ATOMIC_SEQ_CST);
        TimerHandler *finalHandler =
            __atomic_load_n(&slot->handler, __ATOMIC_ACQUIRE);
        if (generationOf(finalPublication) != drainGeneration)
        {
            return true;
        }

        const SlotMode finalMode = modeOf(finalPublication);
        if (finalMode == SlotMode::Empty)
        {
            return finalHandler != handler;
        }
        if (
            finalHandler != handler || !synchronousDrainOf(finalPublication) ||
            (finalMode != SlotMode::Draining &&
             finalMode != SlotMode::Deferred))
        {
            return false;
        }
        if (!hasActiveDispatch(*slot))
        {
            if (retireSlot(*slot, finalPublication, handler))
            {
                return true;
            }
            continue;
        }

        const WaitQueue::WakeReason reason = guard.waitForCompletion(
            WaitQueue::Channel(slot, drainGeneration), Thread::CallbackDrain,
            reinterpret_cast<uintptr_t>(handler));
        (void) reason;
    }
}

bool TimerHandlerRegistry::dispatch(
    uint64_t delta, InterruptState &state, TimerHandler *onlyHandler)
{
    bool admitted = false;

    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        const size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_ACQUIRE);
        if (modeOf(publication) != SlotMode::Enabled)
        {
            continue;
        }

        TimerHandler *handler =
            __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE);
        if (!handler || (onlyHandler && handler != onlyHandler))
        {
            continue;
        }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        HandlerPrePinHook prePinHook =
            __atomic_load_n(&m_HandlerPrePinHook, __ATOMIC_ACQUIRE);
        if (prePinHook)
        {
            prePinHook(handler);
        }
#endif

        void *owner = currentDispatchOwner();
        Thread *thread = Processor::information().getCurrentThread();
        DispatchCleanup dispatchCleanup(this, &slot, owner, publication);
        if (thread)
        {
            // Cleanup is visible before even the hazard-table token is
            // claimed. A no-return callback, nested exception, or timeout
            // restore can therefore abandon every later publication point.
            thread->armAtomicStateCleanup(
                dispatchCleanup.cleanup, abandonDispatch, &dispatchCleanup);
        }

        ActiveDispatch *activeDispatch =
            publishDispatch(slot, owner, &dispatchCleanup);
        if (!activeDispatch)
        {
            if (thread)
            {
                thread->disarmAtomicStateCleanup(dispatchCleanup.cleanup);
            }
            FATAL_NOLOCK("Timer callback hazard table exhausted.");
            return admitted;
        }

        if (__atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST) !=
                publication ||
            __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) != handler)
        {
            unpublishDispatch(&dispatchCleanup, slot, publication, true);
            if (thread)
            {
                thread->disarmAtomicStateCleanup(dispatchCleanup.cleanup);
            }
            continue;
        }

        admitted = true;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        HandlerPinHook hook =
            __atomic_load_n(&m_HandlerPinHook, __ATOMIC_ACQUIRE);
        if (hook)
        {
            hook(handler);
        }
#endif

        // Keep callback entry and return independent of ordinary deferred-scope
        // state: a timer interrupt can arrive while that state is being
        // mutated.
        handler->timer(delta, state);
        unpublishDispatch(&dispatchCleanup, slot, publication, true);
        if (thread)
        {
            thread->disarmAtomicStateCleanup(dispatchCleanup.cleanup);
        }
    }

    return admitted;
}

void TimerHandlerRegistry::reset()
{
    LockGuard<Spinlock> guard(m_HandlerLock);
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        assert(!hasActiveDispatch(slot));
        const size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_ACQUIRE);
        __atomic_store_n(&slot.handler, nullptr, __ATOMIC_RELEASE);
        __atomic_store_n(
            &slot.publication,
            makePublication(generationOf(publication) + 1, SlotMode::Empty),
            __ATOMIC_RELEASE);
    }

    for (size_t i = 0; i < MaxActiveDispatches; ++i)
    {
        assert(
            !__atomic_load_n(&m_ActiveDispatches[i].token, __ATOMIC_ACQUIRE));
        assert(
            !__atomic_load_n(&m_ActiveDispatches[i].owner, __ATOMIC_ACQUIRE));
        assert(!__atomic_load_n(&m_ActiveDispatches[i].slot, __ATOMIC_ACQUIRE));
    }
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void TimerHandlerRegistry::setHandlerPinHook(HandlerPinHook hook)
{
    __atomic_store_n(&m_HandlerPinHook, hook, __ATOMIC_RELEASE);
}

void TimerHandlerRegistry::setHandlerPrePinHook(HandlerPrePinHook hook)
{
    __atomic_store_n(&m_HandlerPrePinHook, hook, __ATOMIC_RELEASE);
}

void TimerHandlerRegistry::setHandlerHazardClaimHook(
    HandlerHazardClaimHook hook)
{
    __atomic_store_n(&m_HandlerHazardClaimHook, hook, __ATOMIC_RELEASE);
}

void TimerHandlerRegistry::setHandlerAtomicDrainHook(
    HandlerAtomicDrainHook hook)
{
    __atomic_store_n(&m_HandlerAtomicDrainHook, hook, __ATOMIC_RELEASE);
}

void TimerHandlerRegistry::withMutationLockForTest(MutationLockHook hook)
{
    m_HandlerLock.acquire();
    if (hook)
    {
        hook();
    }
    m_HandlerLock.release();
}

size_t TimerHandlerRegistry::activeDispatchCountForTest(TimerHandler *handler)
{
    size_t count = 0;
    for (size_t i = 0; i < MaxActiveDispatches; ++i)
    {
        ActiveDispatch &dispatch = m_ActiveDispatches[i];
        void *token = __atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE);
        if (!token)
        {
            continue;
        }
        const size_t generation =
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE);
        HandlerSlot *slot = __atomic_load_n(&dispatch.slot, __ATOMIC_SEQ_CST);
        TimerHandler *activeHandler =
            slot ? __atomic_load_n(&slot->handler, __ATOMIC_ACQUIRE) : nullptr;
        if (activeHandler == handler &&
            __atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE) == token &&
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE) ==
                generation)
        {
            ++count;
        }
    }
    return count;
}

size_t TimerHandlerRegistry::claimedDispatchCountForTest()
{
    size_t count = 0;
    for (size_t i = 0; i < MaxActiveDispatches; ++i)
    {
        if (__atomic_load_n(&m_ActiveDispatches[i].token, __ATOMIC_ACQUIRE))
        {
            ++count;
        }
    }
    return count;
}
#endif
