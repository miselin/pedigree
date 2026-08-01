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
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/state.h"

MemoryTrapHandler::~MemoryTrapHandler() = default;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
PageFaultHandler::HandlerPinHook PageFaultHandler::m_HandlerPinHook = nullptr;
PageFaultHandler::HandlerPrePinHook PageFaultHandler::m_HandlerPrePinHook =
    nullptr;
PageFaultHandler::AtomicDrainHook PageFaultHandler::m_AtomicDrainHook = nullptr;
#endif

PageFaultHandler::PageFaultHandler()
    : m_Handlers(), m_ActiveDispatches(), m_DispatchWaiters(),
      m_HandlerLock(false)
{
}

size_t PageFaultHandler::makePublication(size_t generation, SlotMode mode)
{
    return (generation << GenerationShift) | static_cast<size_t>(mode);
}

size_t PageFaultHandler::generationOf(size_t publication)
{
    return publication >> GenerationShift;
}

PageFaultHandler::SlotMode PageFaultHandler::modeOf(size_t publication)
{
    return static_cast<SlotMode>(publication & ModeMask);
}

bool PageFaultHandler::retireSlot(
    HandlerSlot &slot, size_t expectedPublication,
    MemoryTrapHandler *expectedHandler)
{
    const size_t retiringPublication = makePublication(
        generationOf(expectedPublication), SlotMode::Retiring);
    if (!__atomic_compare_exchange_n(
            &slot.publication, &expectedPublication, retiringPublication,
            false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    {
        return false;
    }

    MemoryTrapHandler *handler =
        __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE);
    if (handler != expectedHandler)
    {
        size_t expectedRetiringPublication = retiringPublication;
        __atomic_compare_exchange_n(
            &slot.publication, &expectedRetiringPublication,
            expectedPublication, false, __ATOMIC_SEQ_CST,
            __ATOMIC_SEQ_CST);
        return false;
    }

    __atomic_store_n(&slot.handler, nullptr, __ATOMIC_RELEASE);
    __atomic_store_n(
        &slot.publication,
        makePublication(
            generationOf(retiringPublication), SlotMode::Empty),
        __ATOMIC_SEQ_CST);
    return true;
}

bool PageFaultHandler::publishDispatch(
    HandlerSlot &slot, void *owner, void *token)
{
    for (size_t i = 0; i < MaxActiveDispatches; ++i)
    {
        ActiveDispatch &dispatch = m_ActiveDispatches[i];
        void *expectedToken = nullptr;
        if (__atomic_compare_exchange_n(
                &dispatch.token, &expectedToken, token, false,
                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        {
            __atomic_add_fetch(
                &dispatch.generation, static_cast<size_t>(1),
                __ATOMIC_ACQ_REL);
            __atomic_store_n(&dispatch.owner, owner, __ATOMIC_RELAXED);
            // This final store commits the callback hazard. Admission close
            // either observes it or wins the total order and is then observed
            // by dispatch revalidation.
            __atomic_store_n(&dispatch.slot, &slot, __ATOMIC_SEQ_CST);
            return true;
        }
    }

    return false;
}

void PageFaultHandler::unpublishDispatch(void *token)
{
    HandlerSlot *releasedSlot = nullptr;
    for (size_t i = 0; i < MaxActiveDispatches; ++i)
    {
        ActiveDispatch &dispatch = m_ActiveDispatches[i];
        if (__atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE) != token)
        {
            continue;
        }

        releasedSlot =
            __atomic_load_n(&dispatch.slot, __ATOMIC_SEQ_CST);
        __atomic_store_n(&dispatch.slot, nullptr, __ATOMIC_SEQ_CST);
        __atomic_store_n(&dispatch.owner, nullptr, __ATOMIC_RELAXED);
        __atomic_store_n(&dispatch.token, nullptr, __ATOMIC_RELEASE);
        break;
    }

    // Cleanup may run after it was armed but before a hazard entry was
    // claimed, so a missing token is a valid abandoned-stack outcome.
    if (!releasedSlot || hasActiveDispatch(*releasedSlot))
    {
        return;
    }

    const size_t publication =
        __atomic_load_n(&releasedSlot->publication, __ATOMIC_SEQ_CST);
    const SlotMode mode = modeOf(publication);
    if (mode != SlotMode::Draining && mode != SlotMode::Deferred)
    {
        return;
    }

    const size_t drainGeneration = generationOf(publication);
    auto guard = m_DispatchWaiters.acquire();
    const size_t finalPublication =
        __atomic_load_n(&releasedSlot->publication, __ATOMIC_SEQ_CST);
    if (
        generationOf(finalPublication) == drainGeneration &&
        modeOf(finalPublication) == SlotMode::Deferred &&
        !hasActiveDispatch(*releasedSlot))
    {
        MemoryTrapHandler *handler =
            __atomic_load_n(&releasedSlot->handler, __ATOMIC_ACQUIRE);
        if (handler)
        {
            retireSlot(*releasedSlot, finalPublication, handler);
        }
    }

    guard.wakeAll(
        WaitQueue::WakeReason::Signalled,
        WaitQueue::Channel(releasedSlot, drainGeneration));
}

void PageFaultHandler::abandonedHandlerCleanup(void *context)
{
    DispatchCleanup *dispatch =
        reinterpret_cast<DispatchCleanup *>(context);
    if (dispatch && dispatch->registry)
    {
        dispatch->registry->unpublishDispatch(dispatch);
    }
}

bool PageFaultHandler::hasActiveDispatch(HandlerSlot &target) const
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
        HandlerSlot *slot =
            __atomic_load_n(&dispatch.slot, __ATOMIC_SEQ_CST);
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

bool PageFaultHandler::findCurrentDispatch(
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
        HandlerSlot *slot =
            __atomic_load_n(&dispatch.slot, __ATOMIC_SEQ_CST);
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

void *PageFaultHandler::currentDispatchOwner()
{
    ProcessorInformation &information = Processor::information();
    Thread *thread = information.getCurrentThread();
    return thread ? static_cast<void *>(thread) :
                    static_cast<void *>(&information);
}

bool PageFaultHandler::registerHandler(MemoryTrapHandler *pHandler)
{
    if (!pHandler)
    {
        return false;
    }

    bool callbackContext = false;
    findCurrentDispatch(
        currentDispatchOwner(), nullptr, callbackContext);
    if (callbackContext)
    {
        return false;
    }

    LockGuard<Spinlock> guard(m_HandlerLock);
    for (size_t i = 0; i < MaxMemoryTrapHandlers; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        const size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) == pHandler &&
            modeOf(publication) != SlotMode::Empty)
        {
            return false;
        }
    }

    for (size_t i = 0; i < MaxMemoryTrapHandlers; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        const size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
        if (modeOf(publication) == SlotMode::Empty &&
            !__atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE))
        {
            const size_t generation = generationOf(publication) + 1;
            if (!generation)
            {
                return false;
            }
            __atomic_store_n(&slot.handler, pHandler, __ATOMIC_RELEASE);
            __atomic_store_n(
                &slot.publication,
                makePublication(generation, SlotMode::Enabled),
                __ATOMIC_SEQ_CST);
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

    void *owner = currentDispatchOwner();
    Thread *current = Processor::information().getCurrentThread();
    const bool canYield = current && Processor::getInterrupts();
    bool callbackContext = false;
    findCurrentDispatch(owner, nullptr, callbackContext);

    // Exception and callback contexts cannot wait on a writer or on another
    // callback. Self-removal closes admission and lets the final callback
    // hazard retire the slot.
    if (!canYield || callbackContext)
    {
        for (size_t i = 0; i < MaxMemoryTrapHandlers; ++i)
        {
            HandlerSlot &slot = m_Handlers[i];
            size_t publication =
                __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
            if (modeOf(publication) == SlotMode::Empty ||
                __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) != pHandler)
            {
                continue;
            }

            bool currentTargetDispatch = false;
            const bool selfUnregister = findCurrentDispatch(
                owner, &slot, currentTargetDispatch);
            if (selfUnregister)
            {
                while (true)
                {
                    const SlotMode mode = modeOf(publication);
                    if (mode == SlotMode::Deferred ||
                        mode == SlotMode::Empty ||
                        mode == SlotMode::Retiring)
                    {
                        return false;
                    }
                    if (mode != SlotMode::Enabled &&
                        mode != SlotMode::Draining)
                    {
                        return false;
                    }

                    const size_t deferredPublication = makePublication(
                        generationOf(publication), SlotMode::Deferred);
                    if (__atomic_compare_exchange_n(
                            &slot.publication, &publication,
                            deferredPublication, false, __ATOMIC_SEQ_CST,
                            __ATOMIC_SEQ_CST))
                    {
                        return false;
                    }
                    if (__atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) !=
                        pHandler)
                    {
                        return false;
                    }
                }
            }

            if (callbackContext || modeOf(publication) != SlotMode::Enabled)
            {
                return false;
            }

            const size_t drainingPublication = makePublication(
                generationOf(publication), SlotMode::Draining);
            if (!__atomic_compare_exchange_n(
                    &slot.publication, &publication, drainingPublication,
                    false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            {
                return false;
            }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
            AtomicDrainHook drainHook =
                __atomic_load_n(&m_AtomicDrainHook, __ATOMIC_ACQUIRE);
            if (drainHook)
            {
                drainHook(pHandler);
            }
#endif

            if (hasActiveDispatch(slot))
            {
                size_t expectedPublication = drainingPublication;
                __atomic_compare_exchange_n(
                    &slot.publication, &expectedPublication,
                    makePublication(
                        generationOf(drainingPublication), SlotMode::Enabled),
                    false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                return false;
            }

            return retireSlot(slot, drainingPublication, pHandler);
        }
        return false;
    }

    // Keep this ordinary unregister stack alive while admitted callbacks
    // drain. Dispatch itself never touches ordinary deferral state.
    TerminationDeferral terminationDeferral;
    m_HandlerLock.acquire();

    HandlerSlot *slot = nullptr;
    size_t publication = 0;
    for (size_t i = 0; i < MaxMemoryTrapHandlers; ++i)
    {
        const size_t candidatePublication =
            __atomic_load_n(&m_Handlers[i].publication, __ATOMIC_SEQ_CST);
        if (modeOf(candidatePublication) != SlotMode::Empty &&
            __atomic_load_n(&m_Handlers[i].handler, __ATOMIC_ACQUIRE) ==
                pHandler)
        {
            slot = &m_Handlers[i];
            publication = candidatePublication;
            break;
        }
    }

    if (!slot || modeOf(publication) != SlotMode::Enabled)
    {
        m_HandlerLock.release();
        return false;
    }

    size_t expectedPublication = publication;
    const size_t drainingPublication = makePublication(
        generationOf(publication), SlotMode::Draining);
    if (!__atomic_compare_exchange_n(
            &slot->publication, &expectedPublication, drainingPublication,
            false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    {
        m_HandlerLock.release();
        return false;
    }

    m_HandlerLock.release();

    const size_t drainGeneration = generationOf(drainingPublication);
    while (true)
    {
        auto guard = m_DispatchWaiters.acquire();
        const size_t finalPublication =
            __atomic_load_n(&slot->publication, __ATOMIC_SEQ_CST);
        MemoryTrapHandler *finalHandler =
            __atomic_load_n(&slot->handler, __ATOMIC_ACQUIRE);
        if (generationOf(finalPublication) != drainGeneration)
        {
            return true;
        }

        const SlotMode finalMode = modeOf(finalPublication);
        if (finalMode == SlotMode::Empty)
        {
            return finalHandler != pHandler;
        }
        if (
            finalHandler != pHandler ||
            (finalMode != SlotMode::Draining &&
             finalMode != SlotMode::Deferred))
        {
            return false;
        }
        if (!hasActiveDispatch(*slot))
        {
            if (retireSlot(*slot, finalPublication, pHandler))
            {
                return true;
            }
            continue;
        }

        const WaitQueue::WakeReason reason = guard.waitForCompletion(
            WaitQueue::Channel(slot, drainGeneration), Thread::CallbackDrain,
            reinterpret_cast<uintptr_t>(pHandler));
        (void) reason;
    }
}

bool PageFaultHandler::dispatchHandlers(
    InterruptState &state, uintptr_t address, bool bIsWrite,
    MemoryTrapHandler *pOnlyHandler)
{
    for (size_t i = 0; i < MaxMemoryTrapHandlers; ++i)
    {
        HandlerSlot &slot = m_Handlers[i];
        const size_t publication =
            __atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST);
        if (modeOf(publication) != SlotMode::Enabled)
        {
            continue;
        }

        MemoryTrapHandler *handler =
            __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE);
        if (!handler || (pOnlyHandler && handler != pOnlyHandler))
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

        Thread *thread = Processor::information().getCurrentThread();
        DispatchCleanup dispatchCleanup(this);
        if (thread)
        {
            // Cleanup is visible before callback admission commits. Every
            // later stack-abandonment point can therefore release the hazard.
            thread->armAtomicStateCleanup(
                dispatchCleanup.cleanup, abandonedHandlerCleanup,
                &dispatchCleanup);
        }

        if (!publishDispatch(
                slot, currentDispatchOwner(), &dispatchCleanup))
        {
            if (thread)
            {
                thread->disarmAtomicStateCleanup(dispatchCleanup.cleanup);
            }
            FATAL_NOLOCK("Page-fault callback hazard table exhausted.");
            return false;
        }

        if (__atomic_load_n(&slot.publication, __ATOMIC_SEQ_CST) !=
                publication ||
            __atomic_load_n(&slot.handler, __ATOMIC_ACQUIRE) != handler)
        {
            unpublishDispatch(&dispatchCleanup);
            if (thread)
            {
                thread->disarmAtomicStateCleanup(dispatchCleanup.cleanup);
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

        const bool handled = handler->trap(state, address, bIsWrite);
        unpublishDispatch(&dispatchCleanup);
        if (thread)
        {
            thread->disarmAtomicStateCleanup(dispatchCleanup.cleanup);
        }

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

void PageFaultHandler::setHandlerPrePinHook(HandlerPrePinHook hook)
{
    __atomic_store_n(&m_HandlerPrePinHook, hook, __ATOMIC_RELEASE);
}

void PageFaultHandler::setAtomicDrainHook(AtomicDrainHook hook)
{
    __atomic_store_n(&m_AtomicDrainHook, hook, __ATOMIC_RELEASE);
}

void PageFaultHandler::withMutationLockForTest(MutationLockHook hook)
{
    m_HandlerLock.acquire();
    if (hook)
    {
        hook();
    }
    m_HandlerLock.release();
}

bool PageFaultHandler::dispatchHandlerForTest(MemoryTrapHandler *pHandler)
{
    InterruptState state;
    return dispatchHandlers(state, 0, false, pHandler);
}

size_t PageFaultHandler::activeDispatchCountForTest(
    MemoryTrapHandler *pHandler)
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
        HandlerSlot *slot =
            __atomic_load_n(&dispatch.slot, __ATOMIC_SEQ_CST);
        MemoryTrapHandler *handler =
            slot ? __atomic_load_n(&slot->handler, __ATOMIC_ACQUIRE) : nullptr;
        if (handler == pHandler &&
            __atomic_load_n(&dispatch.token, __ATOMIC_ACQUIRE) == token &&
            __atomic_load_n(&dispatch.generation, __ATOMIC_ACQUIRE) ==
                generation)
        {
            ++count;
        }
    }
    return count;
}
#endif
