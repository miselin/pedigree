/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/machine/IrqHandlerRegistry.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/utilities/assert.h"

static_assert(
    __atomic_always_lock_free(sizeof(size_t), nullptr),
    "IRQ callback hazard words must be lock-free");
static_assert(
    __atomic_always_lock_free(sizeof(void *), nullptr),
    "IRQ callback hazard pointers must be lock-free");

IrqHandlerRegistry::IrqHandlerRegistry()
    : m_Handlers(), m_ActiveDispatches(), m_HandlerLock(false)
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      ,
      m_HandlerPinHook(nullptr), m_HandlerPrePinHook(nullptr),
      m_HandlerHazardHook(nullptr)
#endif
{
}

size_t IrqHandlerRegistry::makePublication(
    size_t generation, uint8_t irq, SlotMode mode, Delivery delivery)
{
    return (generation << GenerationShift) |
           (static_cast<size_t>(irq) << IrqShift) |
           (static_cast<size_t>(delivery) << DeliveryShift) |
           static_cast<size_t>(mode);
}

size_t IrqHandlerRegistry::generationOf(size_t publication)
{
    return publication >> GenerationShift;
}

uint8_t IrqHandlerRegistry::irqOf(size_t publication)
{
    return static_cast<uint8_t>((publication & IrqMask) >> IrqShift);
}

IrqHandlerRegistry::SlotMode IrqHandlerRegistry::modeOf(size_t publication)
{
    return static_cast<SlotMode>(publication & ModeMask);
}

IrqHandlerRegistry::Delivery IrqHandlerRegistry::deliveryOf(size_t publication)
{
    return static_cast<Delivery>((publication & DeliveryMask) >> DeliveryShift);
}

bool IrqHandlerRegistry::retireSlot(
    HandlerSlot &slot, size_t expectedPublication,
    IrqHandlerBase *expectedHandler)
{
    const size_t retiringPublication = makePublication(
        generationOf(expectedPublication), irqOf(expectedPublication),
        SlotMode::Retiring, deliveryOf(expectedPublication));
    if (!__atomic_compare_exchange_n(
            &slot.publication, &expectedPublication, retiringPublication, false,
            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    {
        return false;
    }

    assert(__atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) == expectedHandler);
    __atomic_store_n(&slot.handler, nullptr, __ATOMIC_RELEASE);
    __atomic_store_n(
        &slot.publication,
        makePublication(
            generationOf(retiringPublication), InvalidIrq, SlotMode::Empty,
            Delivery::Threaded),
        __ATOMIC_SEQ_CST);
    return true;
}

IrqHandlerRegistry::ActiveDispatch *IrqHandlerRegistry::publishDispatch(
    HandlerSlot &slot, void *owner, void *token, size_t admittedPublication)
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
            __atomic_store_n(
                &dispatch.admittedPublication, admittedPublication,
                __ATOMIC_RELAXED);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
            HandlerHazardHook hazardHook =
                __atomic_load_n(&m_HandlerHazardHook, __ATOMIC_ACQUIRE);
            if (hazardHook)
            {
                hazardHook(
                    __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE),
                    HandlerHazardStage::Claimed);
            }
#endif

            // Publishing the slot commits the callback pin. A removal which
            // closes admission before this store can retire the handler; the
            // dispatch revalidation below will then reject this callback.
            __atomic_store_n(&dispatch.slot, &slot, __ATOMIC_SEQ_CST);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
            hazardHook =
                __atomic_load_n(&m_HandlerHazardHook, __ATOMIC_ACQUIRE);
            if (hazardHook)
            {
                hazardHook(
                    __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE),
                    HandlerHazardStage::Committed);
            }
#endif
            return &dispatch;
        }
    }

    return nullptr;
}

bool IrqHandlerRegistry::unpublishDispatch(
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
            FATAL_NOLOCK("IRQ callback hazard changed slots during release.");
            return false;
        }

        committed = publishedSlot == &slot;
        __atomic_store_n(&dispatch.slot, nullptr, __ATOMIC_SEQ_CST);
        __atomic_store_n(
            &dispatch.admittedPublication, static_cast<size_t>(0),
            __ATOMIC_RELAXED);
        __atomic_store_n(&dispatch.owner, nullptr, __ATOMIC_RELAXED);
        __atomic_store_n(&dispatch.token, nullptr, __ATOMIC_RELEASE);
        found = true;
        break;
    }

    if (!found)
    {
        if (required)
        {
            FATAL_NOLOCK("IRQ callback hazard was released more than once.");
        }
        return false;
    }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    IrqHandlerBase *releasedHandler = nullptr;
    HandlerHazardHook releaseHook = nullptr;
    if (committed)
    {
        releasedHandler =
            __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE);
        releaseHook =
            __atomic_load_n(&m_HandlerHazardHook, __ATOMIC_ACQUIRE);
    }
#endif

    if (committed && !hasActiveDispatch(slot, admittedPublication))
    {
        const size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
        if (generationOf(publication) == generationOf(admittedPublication) &&
            modeOf(publication) == SlotMode::Deferred)
        {
            IrqHandlerBase *handler =
                __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE);
            if (handler)
            {
                retireSlot(slot, publication, handler);
            }
        }
    }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    if (releaseHook)
    {
        releaseHook(releasedHandler, HandlerHazardStage::Released);
    }
#endif
    return true;
}

void IrqHandlerRegistry::abandonDispatch(void *context)
{
    DispatchCleanup *dispatch = reinterpret_cast<DispatchCleanup *>(context);
    dispatch->registry->unpublishDispatch(
        dispatch, *dispatch->slot, dispatch->publication, false);
}

bool IrqHandlerRegistry::hasActiveDispatch(
    HandlerSlot &target, size_t admittedPublication) const
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
        const size_t dispatchPublication =
            __atomic_load_n(&dispatch.admittedPublication, __ATOMIC_RELAXED);
        if (slot == &target &&
            generationOf(dispatchPublication) ==
                generationOf(admittedPublication) &&
            __atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE) == token &&
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE) ==
                generation)
        {
            return true;
        }
    }
    return false;
}

bool IrqHandlerRegistry::findCurrentDispatch(
    void *owner, HandlerSlot *target, size_t targetPublication,
    bool &callbackContext) const
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
        const size_t dispatchPublication =
            __atomic_load_n(&dispatch.admittedPublication, __ATOMIC_RELAXED);
        if (__atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE) != token ||
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE) !=
                generation)
        {
            continue;
        }

        if (dispatchOwner == owner && slot)
        {
            callbackContext = true;
            foundTarget |= slot == target && target &&
                           generationOf(dispatchPublication) ==
                               generationOf(targetPublication);
        }
    }
    return foundTarget;
}

void *IrqHandlerRegistry::currentDispatchOwner()
{
    ProcessorInformation &information = Processor::information();
    Thread *thread = information.getCurrentThread();
    return thread ? static_cast<void *>(thread) :
                    static_cast<void *>(&information);
}

bool IrqHandlerRegistry::registerHandler(
    uint8_t irq, IrqHandlerBase *handler, Delivery delivery)
{
    if (!handler ||
        (delivery != Delivery::Threaded && delivery != Delivery::HardOnly))
    {
        return false;
    }

    LockGuard<Spinlock> guard(m_HandlerLock);
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        const size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
        if (modeOf(publication) == SlotMode::Empty || irqOf(publication) != irq)
        {
            continue;
        }

        // A physical line has one dispatch context. Keeping its delivery mode
        // in the publication makes this check part of dispatch revalidation.
        if (deliveryOf(publication) != delivery)
        {
            return false;
        }

        if (__atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) == handler)
        {
            if (modeOf(publication) == SlotMode::Deferred)
            {
                size_t expectedPublication = publication;
                const size_t enabledPublication = makePublication(
                    generationOf(publication), irq, SlotMode::Enabled,
                    delivery);
                return __atomic_compare_exchange_n(
                    &slot.publication, &expectedPublication, enabledPublication,
                    false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            }
            return false;
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
                makePublication(generation, irq, SlotMode::Enabled, delivery),
                __ATOMIC_SEQ_CST);
            return true;
        }
    }

    return false;
}

bool IrqHandlerRegistry::registerThreadedHandler(
    uint8_t irq, IrqHandler *handler)
{
    return registerHandler(irq, handler, Delivery::Threaded);
}

bool IrqHandlerRegistry::registerHardHandler(
    uint8_t irq, HardIrqHandler *handler)
{
    return registerHandler(irq, handler, Delivery::HardOnly);
}

IrqHandlerRegistry::UnregisterResult
IrqHandlerRegistry::unregisterHandler(uint8_t irq, IrqHandlerBase *handler)
{
    if (!handler)
    {
        return UnregisterResult::NotFound;
    }

    void *owner = currentDispatchOwner();
    Thread *current = Processor::information().getCurrentThread();
    bool canYield = current && Processor::getInterrupts();
#if HOSTED
    canYield = canYield && !current->getHostedSignalDepth();
#endif
    bool callbackContext = false;
    findCurrentDispatch(owner, nullptr, 0, callbackContext);

    if (!canYield || callbackContext)
    {
        // Callback and early atomic contexts cannot wait on a writer. A
        // callback can close its own admission and let its final pin retire
        // the slot; a non-callback removal completes only when no callback is
        // already committed.
        for (size_t i = 0; i < MaxHandlerSlots; ++i)
        {
            HandlerSlot &candidate = m_Handlers[i];
            size_t publication =
                __atomic_load_n(&candidate.publication, __ATOMIC_SEQ_CST);
            if (modeOf(publication) != SlotMode::Enabled ||
                irqOf(publication) != irq ||
                __atomic_load_n(&candidate.handler, __ATOMIC_ACQUIRE) !=
                    handler)
            {
                continue;
            }

            bool currentTargetDispatch = false;
            if (findCurrentDispatch(
                    owner, &candidate, publication, currentTargetDispatch))
            {
                const size_t deferredPublication = makePublication(
                    generationOf(publication), irq, SlotMode::Deferred,
                    deliveryOf(publication));
                if (__atomic_compare_exchange_n(
                        &candidate.publication, &publication,
                        deferredPublication, false, __ATOMIC_SEQ_CST,
                        __ATOMIC_SEQ_CST))
                {
                    return UnregisterResult::Deferred;
                }
                return UnregisterResult::Rejected;
            }

            const size_t drainingPublication = makePublication(
                generationOf(publication), irq, SlotMode::Draining,
                deliveryOf(publication));
            if (!__atomic_compare_exchange_n(
                    &candidate.publication, &publication, drainingPublication,
                    false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            {
                return UnregisterResult::Rejected;
            }

            if (hasActiveDispatch(candidate, drainingPublication))
            {
                size_t expectedPublication = drainingPublication;
                __atomic_compare_exchange_n(
                    &candidate.publication, &expectedPublication,
                    makePublication(
                        generationOf(drainingPublication), irq,
                        SlotMode::Enabled, deliveryOf(drainingPublication)),
                    false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                return UnregisterResult::Rejected;
            }

            return retireSlot(candidate, drainingPublication, handler) ?
                       UnregisterResult::Completed :
                       UnregisterResult::Rejected;
        }

        return UnregisterResult::NotFound;
    }

    // Only an ordinary thread can synchronously drain. Keep its stack alive
    // after the atomic callback path has returned without touching Thread
    // deferred-scope state.
    TerminationDeferral terminationDeferral;
    m_HandlerLock.acquire();

    HandlerSlot *slot = nullptr;
    size_t publication = 0;
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        size_t candidatePublication =
            __atomic_load_n(&m_Handlers[i].publication, __ATOMIC_SEQ_CST);
        if (modeOf(candidatePublication) != SlotMode::Empty &&
            irqOf(candidatePublication) == irq &&
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
        return UnregisterResult::NotFound;
    }

    const bool selfUnregister =
        findCurrentDispatch(owner, slot, publication, callbackContext);
    if (selfUnregister)
    {
        if (modeOf(publication) != SlotMode::Enabled)
        {
            m_HandlerLock.release();
            return UnregisterResult::Rejected;
        }

        size_t expectedPublication = publication;
        const bool deferred = __atomic_compare_exchange_n(
            &slot->publication, &expectedPublication,
            makePublication(
                generationOf(publication), irq, SlotMode::Deferred,
                deliveryOf(publication)),
            false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        m_HandlerLock.release();
        return deferred ? UnregisterResult::Deferred :
                          UnregisterResult::Rejected;
    }

    if (modeOf(publication) != SlotMode::Enabled)
    {
        m_HandlerLock.release();
        return UnregisterResult::Rejected;
    }

    size_t expectedPublication = publication;
    const size_t drainingPublication = makePublication(
        generationOf(publication), irq, SlotMode::Draining,
        deliveryOf(publication));
    if (!__atomic_compare_exchange_n(
            &slot->publication, &expectedPublication, drainingPublication,
            false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    {
        m_HandlerLock.release();
        return UnregisterResult::Rejected;
    }

    if (callbackContext && hasActiveDispatch(*slot, drainingPublication))
    {
        expectedPublication = drainingPublication;
        __atomic_compare_exchange_n(
            &slot->publication, &expectedPublication, publication, false,
            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        m_HandlerLock.release();
        return UnregisterResult::Rejected;
    }

    if (!hasActiveDispatch(*slot, drainingPublication))
    {
        const bool retired = retireSlot(*slot, drainingPublication, handler);
        m_HandlerLock.release();
        return retired ? UnregisterResult::Completed :
                         UnregisterResult::Rejected;
    }
    m_HandlerLock.release();

    uintptr_t previousDebugAddress = 0;
    const Thread::DebugState previousDebugState =
        current->getDebugState(previousDebugAddress);
    current->setDebugState(
        Thread::CallbackDrain, reinterpret_cast<uintptr_t>(handler));
    while (hasActiveDispatch(*slot, drainingPublication))
    {
        // Callback release can run in hard IRQ context. It only clears its
        // atomic hazard; this ordinary teardown thread owns all scheduling.
        Scheduler::instance().yield();
    }
    current->setDebugState(previousDebugState, previousDebugAddress);

    m_HandlerLock.acquire();
    const bool retired = retireSlot(*slot, drainingPublication, handler);
    m_HandlerLock.release();
    return retired ? UnregisterResult::Completed : UnregisterResult::Rejected;
}

bool IrqHandlerRegistry::dispatchHard(
    uint8_t irq, InterruptState &state, bool &handled,
    HardIrqHandler *onlyHandler)
{
    bool admitted = false;
    handled = false;

    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        const size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
        if (modeOf(publication) != SlotMode::Enabled ||
            irqOf(publication) != irq ||
            deliveryOf(publication) != Delivery::HardOnly)
        {
            continue;
        }

        IrqHandlerBase *handler =
            __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE);
        if (!handler || (onlyHandler &&
                         handler != static_cast<IrqHandlerBase *>(onlyHandler)))
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
            // Publish cleanup before committing the active-dispatch hazard.
            // A nested exception can therefore abandon this stack at every
            // later instruction without leaking callback admission.
            thread->armAtomicStateCleanup(
                dispatchCleanup.cleanup, abandonDispatch, &dispatchCleanup);
        }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        HandlerHazardHook hazardHook =
            __atomic_load_n(&m_HandlerHazardHook, __ATOMIC_ACQUIRE);
        if (hazardHook)
        {
            hazardHook(handler, HandlerHazardStage::BeforeClaim);
        }
#endif

        if (!publishDispatch(slot, owner, &dispatchCleanup, publication))
        {
            if (thread)
            {
                thread->disarmAtomicStateCleanup(dispatchCleanup.cleanup);
            }
            FATAL_NOLOCK("IRQ callback hazard table exhausted.");
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

        handled |= static_cast<HardIrqHandler *>(handler)->irq(irq, state);
        unpublishDispatch(&dispatchCleanup, slot, publication, true);
        if (thread)
        {
            thread->disarmAtomicStateCleanup(dispatchCleanup.cleanup);
        }
    }

    return admitted;
}

bool IrqHandlerRegistry::dispatchThreaded(
    uint8_t irq, bool &handled, IrqHandler *onlyHandler)
{
    bool admitted = false;
    handled = false;
    Thread *dispatchThread = Processor::information().getCurrentThread();
    if (!dispatchThread || !Processor::getInterrupts())
    {
        return false;
    }
#if HOSTED
    if (dispatchThread->getHostedSignalDepth())
    {
        return false;
    }
#endif

    struct Candidate
    {
        HandlerSlot *slot;
        size_t publication;
        IrqHandlerBase *handler;
    };
    Candidate candidates[MaxHandlerSlots];
    size_t candidateCount = 0;

    // A physical occurrence has a fixed callback set. Without this snapshot,
    // a handler registered while an earlier callback is running could receive
    // an interrupt which predates its registration.
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        const size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
        if (modeOf(publication) != SlotMode::Enabled ||
            irqOf(publication) != irq ||
            deliveryOf(publication) != Delivery::Threaded)
        {
            continue;
        }

        IrqHandlerBase *handler =
            __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE);
        if (!handler || (onlyHandler &&
                         handler != static_cast<IrqHandlerBase *>(onlyHandler)))
        {
            continue;
        }

        candidates[candidateCount++] = {&slot, publication, handler};
    }

    for (size_t i = 0; i < candidateCount; ++i)
    {
        HandlerSlot &slot = *candidates[i].slot;
        const size_t publication = candidates[i].publication;
        IrqHandlerBase *handler = candidates[i].handler;

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
            // Publish cleanup before committing the active-dispatch hazard.
            // A nested exception can therefore abandon this stack at every
            // later instruction without leaking callback admission.
            thread->armAtomicStateCleanup(
                dispatchCleanup.cleanup, abandonDispatch, &dispatchCleanup);
        }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        HandlerHazardHook hazardHook =
            __atomic_load_n(&m_HandlerHazardHook, __ATOMIC_ACQUIRE);
        if (hazardHook)
        {
            hazardHook(handler, HandlerHazardStage::BeforeClaim);
        }
#endif

        if (!publishDispatch(slot, owner, &dispatchCleanup, publication))
        {
            if (thread)
            {
                thread->disarmAtomicStateCleanup(dispatchCleanup.cleanup);
            }
            FATAL_NOLOCK("IRQ callback hazard table exhausted.");
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

        handled |= static_cast<IrqHandler *>(handler)->irq(irq) ==
                   IrqDisposition::Handled;
        unpublishDispatch(&dispatchCleanup, slot, publication, true);
        if (thread)
        {
            thread->disarmAtomicStateCleanup(dispatchCleanup.cleanup);
        }
    }

    return admitted;
}

size_t IrqHandlerRegistry::handlerCount(uint8_t irq)
{
    size_t count = 0;
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        const size_t publication =
            __atomic_load_n(&m_Handlers[i].publication, __ATOMIC_SEQ_CST);
        if (modeOf(publication) == SlotMode::Enabled &&
            irqOf(publication) == irq)
        {
            ++count;
        }
    }
    return count;
}

IrqHandlerRegistry::LineMode IrqHandlerRegistry::lineMode(uint8_t irq)
{
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        const size_t publication =
            __atomic_load_n(&m_Handlers[i].publication, __ATOMIC_SEQ_CST);
        if (modeOf(publication) != SlotMode::Enabled ||
            irqOf(publication) != irq)
        {
            continue;
        }

        return deliveryOf(publication) == Delivery::Threaded ?
                   LineMode::Threaded :
                   LineMode::HardOnly;
    }
    return LineMode::Empty;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void IrqHandlerRegistry::setHandlerPinHook(HandlerPinHook hook)
{
    __atomic_store_n(&m_HandlerPinHook, hook, __ATOMIC_RELEASE);
}

void IrqHandlerRegistry::setHandlerPrePinHook(HandlerPrePinHook hook)
{
    __atomic_store_n(&m_HandlerPrePinHook, hook, __ATOMIC_RELEASE);
}

void IrqHandlerRegistry::setHandlerHazardHook(HandlerHazardHook hook)
{
    __atomic_store_n(&m_HandlerHazardHook, hook, __ATOMIC_RELEASE);
}

void IrqHandlerRegistry::withMutationLockForTest(MutationLockHook hook)
{
    m_HandlerLock.acquire();
    if (hook)
    {
        hook();
    }
    m_HandlerLock.release();
}

size_t IrqHandlerRegistry::activeDispatchCountForTest(IrqHandlerBase *handler)
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
        IrqHandlerBase *activeHandler =
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

size_t IrqHandlerRegistry::claimedDispatchCountForTest()
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

bool IrqHandlerRegistry::containsHandlerForTest(
    uint8_t irq, IrqHandlerBase *handler)
{
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        const size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
        if (modeOf(publication) != SlotMode::Empty &&
            irqOf(publication) == irq &&
            __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) == handler &&
            __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST) == publication)
        {
            return true;
        }
    }
    return false;
}
#endif
