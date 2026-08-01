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
    : m_Handlers(), m_HandlerLock(false)
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      ,
      m_HandlerPinHook(nullptr)
#endif
{
}

void IrqHandlerRegistry::clearSlot(HandlerSlot &slot)
{
    assert(!slot.inFlight);
    assert(!slot.dispatches);
    slot.handler = nullptr;
    slot.irq = InvalidIrq;
    slot.enabled = false;
    slot.deferredRemoval = false;
    slot.draining = false;
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
        if (slot.handler == handler && slot.irq == irq)
        {
            if (
                !slot.enabled && slot.deferredRemoval && !slot.draining)
            {
                // A new owner arrived before a callback's deferred
                // self-removal completed.
                slot.enabled = true;
                slot.deferredRemoval = false;
                return true;
            }
            return false;
        }
    }

    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        if (!slot.handler)
        {
            assert(!slot.inFlight);
            assert(!slot.draining);
            assert(!slot.dispatches);
            slot.handler = handler;
            slot.irq = irq;
            slot.enabled = true;
            slot.deferredRemoval = false;
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

    m_HandlerLock.acquire();

    HandlerSlot *slot = nullptr;
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        if (m_Handlers[i].handler == handler && m_Handlers[i].irq == irq)
        {
            slot = &m_Handlers[i];
            break;
        }
    }

    if (!slot)
    {
        m_HandlerLock.release();
        return UnregisterResult::NotFound;
    }

    bool callbackContext = false;
    bool selfUnregister = false;
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        for (
            HandlerDispatch *dispatch = m_Handlers[i].dispatches; dispatch;
            dispatch = dispatch->next)
        {
            if (dispatch->owner == owner)
            {
                callbackContext = true;
                selfUnregister |= &m_Handlers[i] == slot;
            }
        }
    }

    if (selfUnregister)
    {
        // Waiting for this callback's own pin would deadlock. Close admission
        // now and release the slot when the callback unwinds.
        if (!slot->draining)
        {
            slot->enabled = false;
            slot->deferredRemoval = true;
        }
        m_HandlerLock.release();
        return UnregisterResult::Deferred;
    }

    if (
        slot->draining ||
        (slot->inFlight && (!canYield || callbackContext)))
    {
        // A callback stack must not wait on another callback, and an atomic
        // context cannot enter the scheduler. Leave ownership unchanged so
        // the caller cannot mistake a partial removal for safe destruction.
        m_HandlerLock.release();
        return UnregisterResult::Rejected;
    }

    slot->enabled = false;
    slot->deferredRemoval = false;
    slot->draining = true;
    if (!slot->inFlight)
    {
        clearSlot(*slot);
        m_HandlerLock.release();
        return UnregisterResult::Completed;
    }
    m_HandlerLock.release();

    while (true)
    {
        auto waitGuard = slot->drainWaiters.acquire();
        m_HandlerLock.acquire();
        if (!slot->inFlight)
        {
            assert(slot->handler == handler);
            assert(slot->draining);
            clearSlot(*slot);
            m_HandlerLock.release();
            return UnregisterResult::Completed;
        }
        m_HandlerLock.release();

        const WaitQueue::WakeReason reason =
            waitGuard.waitForCompletion(
                WaitQueue::Channel(slot), Thread::CallbackDrain,
                reinterpret_cast<uintptr_t>(handler));
        (void) reason;
    }
}

bool IrqHandlerRegistry::dispatch(
    uint8_t irq, InterruptState &state, bool &handled,
    IrqHandler *onlyHandler)
{
    bool admitted = false;
    handled = false;

    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        HandlerDispatch dispatch = {currentDispatchOwner(), nullptr};
        IrqHandler *handler = nullptr;

        m_HandlerLock.acquire();
        HandlerSlot &slot = m_Handlers[i];
        if (
            slot.handler && slot.irq == irq && slot.enabled &&
            (!onlyHandler || slot.handler == onlyHandler))
        {
            handler = slot.handler;
            ++slot.inFlight;
            dispatch.next = slot.dispatches;
            slot.dispatches = &dispatch;
        }
        m_HandlerLock.release();

        if (!handler)
        {
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

        bool wakeDrainer = false;
        m_HandlerLock.acquire();
        HandlerDispatch **link = &slot.dispatches;
        while (*link && *link != &dispatch)
        {
            link = &(*link)->next;
        }
        assert(*link == &dispatch);
        *link = dispatch.next;
        assert(slot.inFlight);
        --slot.inFlight;
        wakeDrainer = !slot.inFlight && slot.draining;
        if (!slot.inFlight && slot.deferredRemoval)
        {
            clearSlot(slot);
        }
        m_HandlerLock.release();

        if (wakeDrainer)
        {
            slot.drainWaiters.wakeAll(
                WaitQueue::WakeReason::Signalled,
                WaitQueue::Channel(&slot));
        }
    }

    return admitted;
}

size_t IrqHandlerRegistry::handlerCount(uint8_t irq)
{
    size_t count = 0;
    LockGuard<Spinlock> guard(m_HandlerLock);
    for (size_t i = 0; i < MaxHandlerSlots; ++i)
    {
        if (
            m_Handlers[i].handler && m_Handlers[i].irq == irq &&
            m_Handlers[i].enabled)
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
#endif
