/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "pedigree/kernel/processor/PageFaultHandler.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/utilities/assert.h"

MemoryTrapHandler::~MemoryTrapHandler() = default;

namespace
{
void *currentDispatchOwner()
{
    ProcessorInformation &information = Processor::information();
    Thread *thread = information.getCurrentThread();
    return thread ? static_cast<void *>(thread)
                  : static_cast<void *>(&information);
}
}  // namespace

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
PageFaultHandler::HandlerPinHook PageFaultHandler::m_HandlerPinHook = nullptr;
#endif

PageFaultHandler::PageFaultHandler() : m_Handlers(), m_HandlerLock()
{
}

bool PageFaultHandler::registerHandler(MemoryTrapHandler *pHandler)
{
    if (!pHandler)
    {
        return false;
    }

    LockGuard<Spinlock> guard(m_HandlerLock);
    for (size_t i = 0; i < MaxMemoryTrapHandlers; ++i)
    {
        if (m_Handlers[i].handler == pHandler)
        {
            return false;
        }
    }

    for (size_t i = 0; i < MaxMemoryTrapHandlers; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        if (!slot.handler)
        {
            assert(!slot.inFlight);
            assert(!slot.draining);
            assert(!slot.dispatches);
            slot.handler = pHandler;
            slot.enabled = true;
            slot.deferredRemoval = false;
            return true;
        }
    }

    return false;
}

bool PageFaultHandler::unregisterHandler(MemoryTrapHandler *pHandler)
{
    if (!pHandler)
    {
        return false;
    }

    // Destruction must return through this ownership barrier even if a
    // terminal request arrives while another CPU owns the final callback pin.
    TerminationDeferral terminationDeferral;
    void *owner = currentDispatchOwner();
    Thread *current = Processor::information().getCurrentThread();
    const bool canYield = current && Processor::getInterrupts();

    m_HandlerLock.acquire();

    HandlerSlot *slot = nullptr;
    for (size_t i = 0; i < MaxMemoryTrapHandlers; ++i)
    {
        if (m_Handlers[i].handler == pHandler)
        {
            slot = &m_Handlers[i];
            break;
        }
    }

    if (!slot)
    {
        m_HandlerLock.release();
        return false;
    }

    bool callbackContext = false;
    bool selfUnregister = false;
    for (size_t i = 0; i < MaxMemoryTrapHandlers; ++i)
    {
        auto pinGuard = m_Handlers[i].drainWaiters.acquire();
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
        // Waiting for this callback's own pin would deadlock. Disable future
        // pins now and release the slot when the callback unwinds.
        slot->enabled = false;
        if (!slot->draining)
        {
            slot->deferredRemoval = true;
        }
        m_HandlerLock.release();
        return false;
    }

    bool mustDrain = false;
    bool rejected = false;
    {
        auto pinGuard = slot->drainWaiters.acquire();
        if (callbackContext && slot->inFlight)
        {
            // A nested fault callback cannot wait for another callback on its
            // own fault stack. Leave that handler registered.
            rejected = true;
        }
        else if (slot->draining || slot->deferredRemoval)
        {
            rejected = true;
        }
        else
        {
            // Closing admission and inspecting the completion predicate are
            // atomic with future pins.
            slot->enabled = false;
            slot->draining = true;
            mustDrain = slot->inFlight != 0;
        }
    }
    // Preserve strict LIFO lock release. Once enabled is false, the guarded
    // predicate loop below cannot miss a completion even though we drop both
    // locks before enrolling.
    m_HandlerLock.release();
    if (rejected)
    {
        return false;
    }

    if (canYield)
    {
        while (true)
        {
            auto pinGuard = slot->drainWaiters.acquire();
            if (!slot->inFlight)
            {
                break;
            }
            const WaitQueue::WakeReason reason =
                pinGuard.waitForCompletion(
                    WaitQueue::Channel(slot), Thread::CallbackDrain,
                    reinterpret_cast<uintptr_t>(pHandler));
            (void) reason;
        }
    }
    else if (mustDrain)
    {
        while (true)
        {
            {
                auto pinGuard = slot->drainWaiters.acquire();
                if (!slot->inFlight)
                {
                    break;
                }
            }

            // Page-fault/early-boot atomic contexts cannot enter the
            // scheduler, but still inspect the guarded completion predicate.
            Processor::pause();
        }
    }

    m_HandlerLock.acquire();
    assert(slot->handler == pHandler);
    assert(slot->draining);
    assert(!slot->inFlight);
    slot->handler = nullptr;
    slot->draining = false;
    m_HandlerLock.release();
    return true;
}

void PageFaultHandler::abandonedHandlerCleanup(void *context)
{
    HandlerDispatch *dispatch =
        reinterpret_cast<HandlerDispatch *>(context);
    if (dispatch && dispatch->manager && dispatch->slot)
    {
        dispatch->manager->releaseDispatch(*dispatch, false);
    }
}

void PageFaultHandler::releaseDispatch(
    HandlerDispatch &dispatch, bool normalReturn)
{
    HandlerSlot *slot = dispatch.slot;
    assert(slot);

    m_HandlerLock.acquire();
    if (normalReturn && dispatch.thread)
    {
        dispatch.thread->disarmStateCleanup(dispatch.cleanup);
    }

    auto pinGuard = slot->drainWaiters.acquire();
    HandlerDispatch **link = &slot->dispatches;
    while (*link && *link != &dispatch)
    {
        link = &(*link)->next;
    }
    assert(*link == &dispatch);
    *link = dispatch.next;
    assert(slot->inFlight);
    --slot->inFlight;

    if (slot->deferredRemoval && !slot->inFlight)
    {
        slot->handler = nullptr;
        slot->deferredRemoval = false;
    }
    if (!slot->inFlight)
    {
        pinGuard.wakeAll(
            WaitQueue::WakeReason::Signalled,
            WaitQueue::Channel(slot));
    }

    dispatch.manager = nullptr;
    dispatch.slot = nullptr;
    dispatch.thread = nullptr;
    dispatch.next = nullptr;
    m_HandlerLock.release();
}

bool PageFaultHandler::dispatchHandlers(
    InterruptState &state, uintptr_t address, bool bIsWrite,
    MemoryTrapHandler *pOnlyHandler)
{
    for (size_t i = 0; i < MaxMemoryTrapHandlers; ++i)
    {
        Thread *thread = Processor::information().getCurrentThread();
        HandlerDispatch dispatch = {};
        dispatch.owner = currentDispatchOwner();
        dispatch.manager = this;
        dispatch.thread = thread;
        MemoryTrapHandler *handler = nullptr;

        if (thread)
        {
            thread->armStateCleanup(
                dispatch.cleanup, abandonedHandlerCleanup, &dispatch);
        }

        m_HandlerLock.acquire();
        HandlerSlot &slot = m_Handlers[i];
        if (
            slot.handler && slot.enabled &&
            (!pOnlyHandler || slot.handler == pOnlyHandler))
        {
            auto pinGuard = slot.drainWaiters.acquire();
            handler = slot.handler;
            ++slot.inFlight;
            dispatch.slot = &slot;
            dispatch.next = slot.dispatches;
            slot.dispatches = &dispatch;
        }
        m_HandlerLock.release();

        if (!handler)
        {
            if (thread)
            {
                thread->disarmStateCleanup(dispatch.cleanup);
            }
            continue;
        }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        HandlerPinHook hook =
            __atomic_load_n(&m_HandlerPinHook, __ATOMIC_ACQUIRE);
        if (hook)
        {
            hook(handler);
        }
#endif

        bool handled = false;
        {
            TerminationDeferral callbackDeferral;
            handled = handler->trap(state, address, bIsWrite);
        }
        releaseDispatch(dispatch, true);

        if (handled)
        {
            return true;
        }
    }

    return false;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void PageFaultHandler::setHandlerPinHook(HandlerPinHook hook)
{
    __atomic_store_n(&m_HandlerPinHook, hook, __ATOMIC_RELEASE);
}

bool PageFaultHandler::dispatchHandlerForTest(MemoryTrapHandler *pHandler)
{
    InterruptState state;
    return dispatchHandlers(state, 0, false, pHandler);
}
#endif
