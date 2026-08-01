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
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/InterruptManager.h"
#include "pedigree/kernel/processor/state.h"

namespace __pedigree_hosted
{
};  // namespace __pedigree_hosted
using namespace __pedigree_hosted;

#include <signal.h>

namespace
{
constexpr size_t NumHostedIrqs = 2;
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
    uint8_t irq, IrqHandler *handler, bool bEdge)
{
    if (
        UNLIKELY(
            irq >= NumHostedIrqs || !handler || !irqToSignal[irq]))
        return 0;

    if (!m_Handlers.registerHandler(irq, handler))
        return 0;

    return irqToSignal[irq];
}

irq_id_t
HostedIrqManager::registerPciIrqHandler(IrqHandler *handler, Device *pDevice)
{
    if (UNLIKELY(!pDevice))
        return 0;
    irq_id_t irq = pDevice->getInterruptNumber();
    if (
        UNLIKELY(
            irq >= NumHostedIrqs || !handler || !irqToSignal[irq]))
        return 0;

    if (!m_Handlers.registerHandler(irq, handler))
        return 0;

    return irqToSignal[irq];
}

void HostedIrqManager::acknowledgeIrq(irq_id_t Id)
{
}

bool HostedIrqManager::unregisterHandler(irq_id_t Id, IrqHandler *handler)
{
    uint8_t irq = 0;
    if (!irqForSignal(Id, irq))
    {
        return false;
    }

    const IrqHandlerRegistry::UnregisterResult result =
        m_Handlers.unregisterHandler(irq, handler);
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

    irqToSignal[0] = SIGUSR1;
    irqToSignal[1] = SIGUSR2;

    return true;
}

HostedIrqManager::HostedIrqManager() : m_Handlers()
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

    bool handled = false;
    if (!m_Handlers.dispatch(irq, state, handled))
    {
        NOTICE("HostedIrqManager: unhandled irq #" << irq << " occurred");
    }
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

bool HostedIrqManager::dispatchHandlerForTest(
    uint8_t irq, IrqHandler *handler, InterruptState &state, bool &handled)
{
    return m_Instance.m_Handlers.dispatch(irq, state, handled, handler);
}

void HostedIrqManager::withRegistryMutationLockForTest(
    MutationLockHook hook)
{
    m_Instance.m_Handlers.withMutationLockForTest(hook);
}

size_t HostedIrqManager::activeDispatchCountForTest(IrqHandler *handler)
{
    return m_Instance.m_Handlers.activeDispatchCountForTest(handler);
}

void HostedIrqManager::abandonCurrentThreadForTest()
{
    Processor::information().getScheduler().killCurrentThread();
}
#endif
