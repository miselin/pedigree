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

#include "IrqManager.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/processor/InterruptManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/state.h"

namespace __pedigree_hosted
{
};  // namespace __pedigree_hosted
using namespace __pedigree_hosted;

#include <signal.h>

namespace
{
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
constexpr size_t NumHostedIrqs = 3;
#else
constexpr size_t NumHostedIrqs = 2;
#endif
int irqToSignal[NumHostedIrqs] = {0};

bool irqForSignal(size_t signal, uint8_t &irq)
{
    for (size_t i = 0; i < NumHostedIrqs; ++i)
    {
        if (irqToSignal[i] && static_cast<size_t>(irqToSignal[i]) == signal)
        {
            irq = static_cast<uint8_t>(i);
            return true;
        }
    }
    return false;
}
}  // namespace

HostedIrqManager HostedIrqManager::m_Instance;

void HostedIrqManager::tick()
{
}

bool HostedIrqManager::control(uint8_t irq, ControlCode code, size_t argument)
{
    return true;
}

irq_id_t HostedIrqManager::registerIsaIrqHandler(
    uint8_t irq, IrqHandler *handler, const IrqPolicy &policy)
{
    if (UNLIKELY(
            irq >= NumHostedIrqs || !handler || !irqToSignal[irq] ||
            !m_ThreadedDispatcher.isInitialised() ||
            !policy.validForThreaded()))
        return 0;

    if (!m_Handlers.registerThreadedHandler(irq, handler))
        return 0;

    return irqToSignal[irq];
}

irq_id_t HostedIrqManager::registerHardIsaIrqHandler(
    uint8_t irq, HardIrqHandler *handler, const IrqPolicy &policy)
{
    if (UNLIKELY(
            irq >= NumHostedIrqs || !handler || !irqToSignal[irq] ||
            !policy.validForHard()))
        return 0;

    if (!m_Handlers.registerHardHandler(irq, handler))
        return 0;

    return irqToSignal[irq];
}

irq_id_t
HostedIrqManager::registerPciIrqHandler(
    IrqHandler *handler, Device *pDevice, const IrqPolicy &policy)
{
    if (UNLIKELY(!pDevice))
        return 0;
    irq_id_t irq = pDevice->getInterruptNumber();
    if (UNLIKELY(
            irq >= NumHostedIrqs || !handler || !irqToSignal[irq] ||
            !m_ThreadedDispatcher.isInitialised() ||
            !policy.validForThreaded() ||
            policy.trigger() != IrqTrigger::Level))
        return 0;

    if (!m_Handlers.registerThreadedHandler(irq, handler))
        return 0;

    return irqToSignal[irq];
}

irq_id_t HostedIrqManager::registerHardPciIrqHandler(
    HardIrqHandler *handler, Device *pDevice, const IrqPolicy &policy)
{
    if (UNLIKELY(!pDevice))
        return 0;
    irq_id_t irq = pDevice->getInterruptNumber();
    if (UNLIKELY(
            irq >= NumHostedIrqs || !handler || !irqToSignal[irq] ||
            !policy.validForHard() ||
            policy.trigger() != IrqTrigger::Level))
        return 0;

    if (!m_Handlers.registerHardHandler(irq, handler))
        return 0;

    return irqToSignal[irq];
}

bool HostedIrqManager::unregisterHandler(irq_id_t Id, IrqHandlerBase *handler)
{
    uint8_t irq = 0;
    if (!irqForSignal(Id, irq))
    {
        return false;
    }

    const IrqHandlerRegistry::UnregisterResult result =
        m_Handlers.unregisterHandler(irq, handler);
    if ((result == IrqHandlerRegistry::UnregisterResult::Completed ||
         result == IrqHandlerRegistry::UnregisterResult::Deferred) &&
        !m_Handlers.handlerCount(irq))
    {
        __atomic_add_fetch(
            &m_ThreadedCookies[irq], static_cast<size_t>(1),
            __ATOMIC_ACQ_REL);
    }
    if (result == IrqHandlerRegistry::UnregisterResult::Rejected)
    {
        ERROR(
            "HostedIrqManager: rejected an IRQ removal which could not "
            "synchronously drain");
    }
    return result == IrqHandlerRegistry::UnregisterResult::Completed;
}

bool HostedIrqManager::initialise()
{
    // Register the interrupts
    InterruptManager &IntManager = InterruptManager::instance();
    if (IntManager.registerInterruptHandler(SIGUSR1, this) == false)
        return false;
    if (IntManager.registerInterruptHandler(SIGUSR2, this) == false)
        return false;
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    if (IntManager.registerInterruptHandler(SIGURG, this) == false)
        return false;
#endif

    irqToSignal[0] = SIGUSR1;
    irqToSignal[1] = SIGUSR2;
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    // Both production hosted lines are permanently occupied by timer hard
    // handlers. Keep one signal-backed line solely for end-to-end threaded
    // IRQ regression coverage.
    irqToSignal[2] = SIGURG;
#endif

    return true;
}

bool HostedIrqManager::initialiseThreaded()
{
    return m_ThreadedDispatcher.initialise();
}

bool HostedIrqManager::shutdownThreaded()
{
    return m_ThreadedDispatcher.shutdown();
}

HostedIrqManager::HostedIrqManager()
    : m_Handlers(),
      m_ThreadedDispatcher(
          MakeConstantString("hosted IRQ bottom half"), NumHostedIrqs,
          dispatchThreadedLine, this),
      m_ThreadedCookies(), m_ThreadedPublicationFailures()
{
}

void HostedIrqManager::interrupt(size_t interruptNumber, InterruptState &state)
{
    uint8_t irq = 0;
    if (!irqForSignal(interruptNumber, irq))
    {
        NOTICE(
            "HostedIrqManager: unmapped signal #" << interruptNumber
                                                  << " occurred");
        return;
    }

    if (m_Handlers.lineMode(irq) == IrqHandlerRegistry::LineMode::Threaded)
    {
        size_t cookie = __atomic_add_fetch(
            &m_ThreadedCookies[irq], static_cast<size_t>(1),
            __ATOMIC_ACQ_REL);
        if (!cookie)
        {
            cookie = __atomic_add_fetch(
                &m_ThreadedCookies[irq], static_cast<size_t>(1),
                __ATOMIC_ACQ_REL);
        }
        if (!m_ThreadedDispatcher.publishFromInterrupt(irq, cookie))
        {
            __atomic_add_fetch(
                &m_ThreadedPublicationFailures[irq], static_cast<size_t>(1),
                __ATOMIC_RELAXED);
        }
        return;
    }

    bool handled = false;
    if (!m_Handlers.dispatchHard(irq, state, handled))
    {
        NOTICE("HostedIrqManager: unhandled irq #" << irq << " occurred");
    }
}

void HostedIrqManager::dispatchThreadedLine(
    void *context, uint8_t irq, size_t cookie)
{
    HostedIrqManager *manager =
        reinterpret_cast<HostedIrqManager *>(context);
    if (cookie != __atomic_load_n(
                      &manager->m_ThreadedCookies[irq], __ATOMIC_ACQUIRE))
    {
        return;
    }
    bool handled = false;
    manager->m_Handlers.dispatchThreaded(irq, handled);
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void HostedIrqManager::setHandlerPinHook(HandlerPinHook hook)
{
    m_Instance.m_Handlers.setHandlerPinHook(hook);
}

void HostedIrqManager::setHandlerPrePinHook(HandlerPrePinHook hook)
{
    m_Instance.m_Handlers.setHandlerPrePinHook(hook);
}

void HostedIrqManager::setHandlerHazardHook(HandlerHazardHook hook)
{
    m_Instance.m_Handlers.setHandlerHazardHook(hook);
}

bool HostedIrqManager::dispatchHandlerForTest(
    uint8_t irq, HardIrqHandler *handler, InterruptState &state, bool &handled)
{
    return m_Instance.m_Handlers.dispatchHard(irq, state, handled, handler);
}

bool HostedIrqManager::dispatchHandlerForTest(
    uint8_t irq, HardIrqHandler *handler, bool &handled)
{
    InterruptState state;
    return dispatchHandlerForTest(irq, handler, state, handled);
}

void HostedIrqManager::withRegistryMutationLockForTest(MutationLockHook hook)
{
    m_Instance.m_Handlers.withMutationLockForTest(hook);
}

size_t HostedIrqManager::activeDispatchCountForTest(IrqHandlerBase *handler)
{
    return m_Instance.m_Handlers.activeDispatchCountForTest(handler);
}

size_t HostedIrqManager::claimedDispatchCountForTest()
{
    return m_Instance.m_Handlers.claimedDispatchCountForTest();
}

bool HostedIrqManager::containsHandlerForTest(
    uint8_t irq, IrqHandlerBase *handler)
{
    return m_Instance.m_Handlers.containsHandlerForTest(irq, handler);
}

void HostedIrqManager::abandonCurrentThreadForTest()
{
    Processor::information().getScheduler().killCurrentThread();
}
#endif
