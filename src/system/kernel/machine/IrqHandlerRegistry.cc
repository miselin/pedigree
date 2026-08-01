/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/machine/IrqHandlerRegistry.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/utilities/assert.h"

IrqHandlerRegistry::IrqHandlerRegistry()
    : m_Handlers(), m_ActiveDispatches(), m_HandlerLock(false)
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      ,
      m_HandlerPinHook(nullptr), m_HandlerPrePinHook(nullptr)
#endif
{
}

size_t IrqHandlerRegistry::makePublication(
    size_t generation, uint8_t irq, SlotMode mode)
{
    return (generation << GenerationShift) |
           (static_cast<size_t>(irq) << IrqShift) |
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

IrqHandlerRegistry::SlotMode
IrqHandlerRegistry::modeOf(size_t publication)
{
    return static_cast<SlotMode>(publication & ModeMask);
}

bool IrqHandlerRegistry::retireSlot(
    HandlerSlot &slot, size_t expectedPublication,
    IrqHandler *expectedHandler)
{
    const size_t retiringPublication = makePublication(
        generationOf(expectedPublication), irqOf(expectedPublication),
        SlotMode::Retiring);
    if (!__atomic_compare_exchange_n(
            &slot.publication, &expectedPublication, retiringPublication,
            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        return false;
    }

    assert(
        __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) ==
        expectedHandler);
    __atomic_store_n(&slot.handler, nullptr, __ATOMIC_RELEASE);
    __atomic_store_n(
        &slot.publication,
        makePublication(
            generationOf(retiringPublication), InvalidIrq, SlotMode::Empty),
        __ATOMIC_RELEASE);
    return true;
}

IrqHandlerRegistry::ActiveDispatch *
IrqHandlerRegistry::publishDispatch(HandlerSlot &slot, void *owner)
{
    assert(owner);
    for (size_t i = 0; i < MaxActiveDispatches; ++i)
    {
        ActiveDispatch &dispatch = m_ActiveDispatches[i];
        void *expectedOwner = nullptr;
        if (__atomic_compare_exchange_n(
                &dispatch.owner, &expectedOwner, owner, false,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        {
            // Only this owner can observe itself in this record. Publish the
            // target before entering handler code which can unregister it.
            __atomic_store_n(&dispatch.slot, &slot, __ATOMIC_RELEASE);
            return &dispatch;
        }
    }

    return nullptr;
}

void IrqHandlerRegistry::unpublishDispatch(ActiveDispatch *dispatch)
{
    assert(dispatch);
    __atomic_store_n(&dispatch->slot, nullptr, __ATOMIC_RELEASE);
    __atomic_store_n(&dispatch->owner, nullptr, __ATOMIC_RELEASE);
}

bool IrqHandlerRegistry::findCurrentDispatch(
    void *owner, HandlerSlot *target, bool &callbackContext) const
{
    callbackContext = false;
    bool foundTarget = false;
    for (size_t i = 0; i < MaxActiveDispatches; ++i)
    {
        const ActiveDispatch &dispatch = m_ActiveDispatches[i];
        if (__atomic_load_n(&dispatch.owner, __ATOMIC_ACQUIRE) == owner)
        {
            callbackContext = true;
            foundTarget |=
                __atomic_load_n(&dispatch.slot, __ATOMIC_ACQUIRE) == target;
        }
    }
    return foundTarget;
}

void IrqHandlerRegistry::releasePin(HandlerSlot &slot)
{
    const size_t remaining =
        __atomic_sub_fetch(&slot.inFlight, 1, __ATOMIC_ACQ_REL);
    if (remaining)
    {
        return;
    }

    const size_t publication =
        __atomic_load_n(&slot.publication, __ATOMIC_ACQUIRE);
    const SlotMode mode = modeOf(publication);
    if (mode == SlotMode::Deferred)
    {
        IrqHandler *handler =
            __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE);
        retireSlot(slot, publication, handler);
    }
    else if (mode == SlotMode::Draining)
    {
        slot.drainWaiters.wakeAll(
            WaitQueue::WakeReason::Signalled,
            WaitQueue::Channel(&slot));
    }
}

void *IrqHandlerRegistry::currentDispatchOwner()
{
    ProcessorInformation &information = Processor::information();
    Thread *thread = information.getCurrentThread();
    return thread ? static_cast<void *>(thread)
                  : static_cast<void *>(&information);
}

bool IrqHandlerRegistry::registerHandler(uint8_t irq, IrqHandler *handler)
{
    if (!handler)
    {
        return false;
    }

    LockGuard<Spinlock> guard(m_HandlerLock);
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        const size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_ACQUIRE);
        if (
            __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) == handler &&
            irqOf(publication) == irq && modeOf(publication) != SlotMode::Empty)
        {
            if (modeOf(publication) == SlotMode::Deferred)
            {
                size_t expectedPublication = publication;
                const size_t enabledPublication = makePublication(
                    generationOf(publication), irq, SlotMode::Enabled);
                return __atomic_compare_exchange_n(
                    &slot.publication, &expectedPublication,
                    enabledPublication, false, __ATOMIC_ACQ_REL,
                    __ATOMIC_ACQUIRE);
            }
            return false;
        }
    }

    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        const size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_ACQUIRE);
        if (
            modeOf(publication) == SlotMode::Empty &&
            !__atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE))
        {
            const size_t generation = generationOf(publication) + 1;
            __atomic_store_n(&slot.handler, handler, __ATOMIC_RELEASE);
            __atomic_store_n(
                &slot.publication,
                makePublication(generation, irq, SlotMode::Enabled),
                __ATOMIC_RELEASE);
            return true;
        }
    }

    return false;
}

IrqHandlerRegistry::UnregisterResult
IrqHandlerRegistry::unregisterHandler(uint8_t irq, IrqHandler *handler)
{
    if (!handler)
    {
        return UnregisterResult::NotFound;
    }

    // Teardown must finish retiring registry-owned state before a terminal
    // request is allowed to consume this thread's stack.
    TerminationDeferral terminationDeferral;
    void *owner = currentDispatchOwner();
    Thread *current = Processor::information().getCurrentThread();
    const bool canYield = current && Processor::getInterrupts();

    if (!canYield)
    {
        // Callback and early atomic contexts cannot wait on a writer. A
        // callback can close its own admission and let its final pin retire
        // the slot; a non-callback removal completes only when no callback is
        // already committed.
        for (size_t i = 0; i < MaxHandlerSlots; ++i)
        {
            HandlerSlot &candidate = m_Handlers[i];
            size_t publication =
                __atomic_load_n(&candidate.publication, __ATOMIC_ACQUIRE);
            if (
                modeOf(publication) != SlotMode::Enabled ||
                irqOf(publication) != irq ||
                __atomic_load_n(&candidate.handler, __ATOMIC_ACQUIRE) !=
                    handler)
            {
                continue;
            }

            bool callbackContext = false;
            if (findCurrentDispatch(
                    owner, &candidate, callbackContext))
            {
                const size_t deferredPublication = makePublication(
                    generationOf(publication), irq, SlotMode::Deferred);
                if (__atomic_compare_exchange_n(
                        &candidate.publication, &publication,
                        deferredPublication, false, __ATOMIC_ACQ_REL,
                        __ATOMIC_ACQUIRE))
                {
                    return UnregisterResult::Deferred;
                }
                return UnregisterResult::Rejected;
            }

            const size_t drainingPublication = makePublication(
                generationOf(publication), irq, SlotMode::Draining);
            if (!__atomic_compare_exchange_n(
                    &candidate.publication, &publication,
                    drainingPublication, false, __ATOMIC_ACQ_REL,
                    __ATOMIC_ACQUIRE))
            {
                return UnregisterResult::Rejected;
            }

            if (__atomic_load_n(&candidate.inFlight, __ATOMIC_ACQUIRE))
            {
                size_t expectedPublication = drainingPublication;
                __atomic_compare_exchange_n(
                    &candidate.publication, &expectedPublication,
                    makePublication(
                        generationOf(drainingPublication), irq,
                        SlotMode::Enabled),
                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
                return UnregisterResult::Rejected;
            }

            return retireSlot(candidate, drainingPublication, handler)
                       ? UnregisterResult::Completed
                       : UnregisterResult::Rejected;
        }

        return UnregisterResult::NotFound;
    }

    m_HandlerLock.acquire();

    HandlerSlot *slot = nullptr;
    size_t publication = 0;
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        size_t candidatePublication =
            __atomic_load_n(&m_Handlers[i].publication, __ATOMIC_ACQUIRE);
        if (
            modeOf(candidatePublication) != SlotMode::Empty &&
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

    bool callbackContext = false;
    const bool selfUnregister =
        findCurrentDispatch(owner, slot, callbackContext);
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
                generationOf(publication), irq, SlotMode::Deferred),
            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        m_HandlerLock.release();
        return deferred ? UnregisterResult::Deferred
                        : UnregisterResult::Rejected;
    }

    if (modeOf(publication) != SlotMode::Enabled)
    {
        m_HandlerLock.release();
        return UnregisterResult::Rejected;
    }

    size_t expectedPublication = publication;
    const size_t drainingPublication = makePublication(
        generationOf(publication), irq, SlotMode::Draining);
    if (!__atomic_compare_exchange_n(
            &slot->publication, &expectedPublication, drainingPublication,
            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        m_HandlerLock.release();
        return UnregisterResult::Rejected;
    }

    if (
        callbackContext &&
        __atomic_load_n(&slot->inFlight, __ATOMIC_ACQUIRE))
    {
        expectedPublication = drainingPublication;
        __atomic_compare_exchange_n(
            &slot->publication, &expectedPublication, publication, false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        m_HandlerLock.release();
        return UnregisterResult::Rejected;
    }

    if (!__atomic_load_n(&slot->inFlight, __ATOMIC_ACQUIRE))
    {
        const bool retired =
            retireSlot(*slot, drainingPublication, handler);
        m_HandlerLock.release();
        return retired ? UnregisterResult::Completed
                       : UnregisterResult::Rejected;
    }
    m_HandlerLock.release();

    while (true)
    {
        auto waitGuard = slot->drainWaiters.acquire();
        if (!__atomic_load_n(&slot->inFlight, __ATOMIC_ACQUIRE))
        {
            break;
        }

        const WaitQueue::WakeReason reason =
            waitGuard.waitForCompletion(
                WaitQueue::Channel(slot), Thread::CallbackDrain,
                reinterpret_cast<uintptr_t>(handler));
        (void) reason;
    }

    m_HandlerLock.acquire();
    const bool retired =
        retireSlot(*slot, drainingPublication, handler);
    m_HandlerLock.release();
    return retired ? UnregisterResult::Completed
                   : UnregisterResult::Rejected;
}

bool IrqHandlerRegistry::dispatch(
    uint8_t irq, InterruptState &state, bool &handled,
    IrqHandler *onlyHandler)
{
    bool admitted = false;
    handled = false;

    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        const size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_ACQUIRE);
        if (
            modeOf(publication) != SlotMode::Enabled ||
            irqOf(publication) != irq)
        {
            continue;
        }

        IrqHandler *handler =
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

        __atomic_add_fetch(&slot.inFlight, 1, __ATOMIC_ACQ_REL);
        if (
            __atomic_load_n(&slot.publication, __ATOMIC_ACQUIRE) !=
                publication ||
            __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) != handler)
        {
            releasePin(slot);
            continue;
        }

        ActiveDispatch *activeDispatch =
            publishDispatch(slot, currentDispatchOwner());
        if (!activeDispatch)
        {
            releasePin(slot);
            continue;
        }

        admitted = true;
        TerminationDeferral callbackDeferral;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        HandlerPinHook hook =
            __atomic_load_n(&m_HandlerPinHook, __ATOMIC_ACQUIRE);
        if (hook)
        {
            hook(handler);
        }
#endif

        handled |= handler->irq(irq, state);
        unpublishDispatch(activeDispatch);
        releasePin(slot);
    }

    return admitted;
}

size_t IrqHandlerRegistry::handlerCount(uint8_t irq)
{
    size_t count = 0;
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        const size_t publication =
            __atomic_load_n(&m_Handlers[i].publication, __ATOMIC_ACQUIRE);
        if (
            modeOf(publication) == SlotMode::Enabled &&
            irqOf(publication) == irq)
        {
            ++count;
        }
    }
    return count;
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

void IrqHandlerRegistry::withMutationLockForTest(MutationLockHook hook)
{
    m_HandlerLock.acquire();
    if (hook)
    {
        hook();
    }
    m_HandlerLock.release();
}
#endif
