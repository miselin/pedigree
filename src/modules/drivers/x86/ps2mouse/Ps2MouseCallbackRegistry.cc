/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "Ps2MouseCallbackRegistry.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/assert.h"

Ps2MouseCallbackRegistry::CallbackSlot::CallbackSlot()
    : handler(nullptr), parameter(nullptr), registration(nullptr), inFlight(0),
      enabled(false), draining(false), deferredRemoval(false),
      dispatches(nullptr), drainWaiters()
{
}

Ps2MouseCallbackRegistry::Ps2MouseCallbackRegistry()
    : m_Callbacks(), m_CallbackLock()
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      ,
      m_CallbackPinHook(nullptr)
#endif
{
}

Ps2MouseCallbackRegistry::~Ps2MouseCallbackRegistry()
{
    for (size_t i = 0; i < MaxCallbacks; ++i)
    {
        CallbackSlot &slot = m_Callbacks[i];
        Registration *registration = nullptr;
        {
            m_CallbackLock.acquire();
            registration = slot.registration;
            m_CallbackLock.release();
        }

        if (registration)
        {
            registration->reset();
        }
    }
}

void *Ps2MouseCallbackRegistry::currentDispatchOwner()
{
    ProcessorInformation &information = Processor::information();
    Thread *thread = information.getCurrentThread();
    return thread ? static_cast<void *>(thread)
                  : static_cast<void *>(&information);
}

void Ps2MouseCallbackRegistry::unregisterThunk(
    void *owner, void *slot, Registration *registration)
{
    reinterpret_cast<Ps2MouseCallbackRegistry *>(owner)->unregister(
        reinterpret_cast<CallbackSlot *>(slot), registration);
}

void Ps2MouseCallbackRegistry::clearSlot(CallbackSlot &slot)
{
    assert(!slot.inFlight);
    assert(!slot.dispatches);
    if (slot.registration)
    {
        slot.registration->releaseFromOwner(this, &slot);
    }
    slot.handler = nullptr;
    slot.parameter = nullptr;
    slot.registration = nullptr;
    slot.enabled = false;
    slot.draining = false;
    slot.deferredRemoval = false;
}

bool Ps2MouseCallbackRegistry::subscribe(
    Handler handler, void *parameter, Registration &registration)
{
    if (!handler || registration)
    {
        return false;
    }

    m_CallbackLock.acquire();
    for (size_t i = 0; i < MaxCallbacks; ++i)
    {
        CallbackSlot &slot = m_Callbacks[i];
        if (slot.handler)
        {
            continue;
        }

        assert(!slot.inFlight);
        assert(!slot.dispatches);
        slot.handler = handler;
        slot.parameter = parameter;
        slot.registration = &registration;
        slot.enabled = true;
        slot.draining = false;
        slot.deferredRemoval = false;
        registration.adopt(this, &slot, unregisterThunk);
        m_CallbackLock.release();
        return true;
    }
    m_CallbackLock.release();
    return false;
}

void Ps2MouseCallbackRegistry::unregister(
    CallbackSlot *slot, Registration *registration)
{
    Thread *current = Processor::information().getCurrentThread();
    const bool canYield = current && Processor::getInterrupts();
    TerminationDeferral terminationDeferral;
    m_CallbackLock.acquire();
    if (
        !slot || !slot->handler ||
        (slot->registration && slot->registration != registration))
    {
        m_CallbackLock.release();
        return;
    }

    if (slot->registration == registration)
    {
        slot->registration = nullptr;
    }

    void *owner = currentDispatchOwner();
    for (
        CallbackDispatch *dispatch = slot->dispatches; dispatch;
        dispatch = dispatch->next)
    {
        if (dispatch->owner == owner)
        {
            slot->enabled = false;
            slot->deferredRemoval = true;
            m_CallbackLock.release();
            return;
        }
    }

    if (slot->inFlight && !canYield)
    {
        m_CallbackLock.release();
        FATAL(
            "PS/2 mouse callback removal cannot drain in this context.");
    }

    slot->enabled = false;
    slot->draining = true;
    if (!slot->inFlight)
    {
        clearSlot(*slot);
        m_CallbackLock.release();
        return;
    }
    m_CallbackLock.release();

    while (true)
    {
        auto waitGuard = slot->drainWaiters.acquire();
        m_CallbackLock.acquire();
        if (!slot->inFlight)
        {
            clearSlot(*slot);
            m_CallbackLock.release();
            return;
        }
        m_CallbackLock.release();

        const WaitQueue::WakeReason reason = waitGuard.waitForCompletion(
            WaitQueue::Channel(slot), Thread::CallbackDrain,
            reinterpret_cast<uintptr_t>(slot->parameter));
        (void) reason;
    }
}

void Ps2MouseCallbackRegistry::releaseCallback(
    CallbackSlot &slot, CallbackDispatch &dispatch)
{
    bool wakeDrainer = false;
    m_CallbackLock.acquire();
    CallbackDispatch **link = &slot.dispatches;
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
    m_CallbackLock.release();

    if (wakeDrainer)
    {
        slot.drainWaiters.wakeAll(
            WaitQueue::WakeReason::Signalled, WaitQueue::Channel(&slot));
    }
}

void Ps2MouseCallbackRegistry::dispatch(
    const void *buffer, size_t length)
{
    TerminationDeferral callbackDeferral;
    for (size_t i = 0; i < MaxCallbacks; ++i)
    {
        CallbackDispatch dispatch = {currentDispatchOwner(), nullptr};
        Handler handler = nullptr;
        void *parameter = nullptr;

        m_CallbackLock.acquire();
        CallbackSlot &slot = m_Callbacks[i];
        if (slot.handler && slot.enabled)
        {
            handler = slot.handler;
            parameter = slot.parameter;
            ++slot.inFlight;
            dispatch.next = slot.dispatches;
            slot.dispatches = &dispatch;
        }
        m_CallbackLock.release();

        if (!handler)
        {
            continue;
        }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        CallbackPinHook hook =
            __atomic_load_n(&m_CallbackPinHook, __ATOMIC_ACQUIRE);
        if (hook)
        {
            hook(handler, parameter);
        }
#endif

        handler(parameter, buffer, length);
        releaseCallback(slot, dispatch);
    }
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void Ps2MouseCallbackRegistry::setCallbackPinHook(CallbackPinHook hook)
{
    __atomic_store_n(&m_CallbackPinHook, hook, __ATOMIC_RELEASE);
}
#endif
