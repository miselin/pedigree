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
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/InterruptManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/state.h"

namespace __pedigree_hosted
{
};  // namespace __pedigree_hosted
using namespace __pedigree_hosted;

#include <signal.h>

static_assert(
    __atomic_always_lock_free(sizeof(size_t), nullptr),
    "hosted IRQ line-lifetime ownership must remain lock-free");

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

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
HostedIrqManager::DiagnosticPublicationHook diagnosticPublicationHook = nullptr;
HostedIrqManager::LineOwnershipHook lineOwnershipHook = nullptr;
#endif

class HostedLineLifecycleGuard
{
  public:
    HostedLineLifecycleGuard(size_t &busy, uint8_t irq)
        : m_Busy(busy), m_Owned(false)
    {
        size_t expected = 0;
        m_Owned = __atomic_compare_exchange_n(
            &m_Busy, &expected, static_cast<size_t>(1), false, __ATOMIC_ACQUIRE,
            __ATOMIC_RELAXED);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        if (!m_Owned)
        {
            HostedIrqManager::LineOwnershipHook hook =
                __atomic_load_n(&lineOwnershipHook, __ATOMIC_ACQUIRE);
            if (hook)
            {
                hook(
                    irq,
                    HostedIrqManager::LineOwnershipStage::AdmissionRejected, 0);
            }
        }
#endif
    }

    ~HostedLineLifecycleGuard()
    {
        if (m_Owned)
        {
            __atomic_store_n(&m_Busy, static_cast<size_t>(0), __ATOMIC_RELEASE);
        }
    }

    bool owned() const
    {
        return m_Owned;
    }

  private:
    size_t &m_Busy;
    bool m_Owned;
};
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

    TerminationDeferral lifecycleTermination(
        lifecycleTerminationCanBeDeferred());
    {
        HostedLineLifecycleGuard lifecycle(m_LineLifecycleBusy[irq], irq);
        if (!lifecycle.owned())
        {
            return 0;
        }

        if (!m_Handlers.registerThreadedHandler(irq, handler, policy))
        {
            return 0;
        }
    }

    publishDiagnosticLine(irq);

    return irqToSignal[irq];
}

irq_id_t HostedIrqManager::registerHardIsaIrqHandler(
    uint8_t irq, HardIrqHandler *handler, const IrqPolicy &policy)
{
    if (UNLIKELY(
            irq >= NumHostedIrqs || !handler || !irqToSignal[irq] ||
            !policy.validForHard()))
        return 0;

    TerminationDeferral lifecycleTermination(
        lifecycleTerminationCanBeDeferred());
    {
        HostedLineLifecycleGuard lifecycle(m_LineLifecycleBusy[irq], irq);
        if (!lifecycle.owned())
        {
            return 0;
        }

        if (!m_Handlers.registerHardHandler(irq, handler, policy))
        {
            return 0;
        }
    }

    publishDiagnosticLine(irq);

    return irqToSignal[irq];
}

irq_id_t HostedIrqManager::registerPciIrqHandler(
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

    TerminationDeferral lifecycleTermination(
        lifecycleTerminationCanBeDeferred());
    {
        HostedLineLifecycleGuard lifecycle(m_LineLifecycleBusy[irq], irq);
        if (!lifecycle.owned())
        {
            return 0;
        }

        if (!m_Handlers.registerThreadedHandler(irq, handler, policy))
        {
            return 0;
        }
    }

    publishDiagnosticLine(irq);

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
            !policy.validForHard() || policy.trigger() != IrqTrigger::Level))
        return 0;

    TerminationDeferral lifecycleTermination(
        lifecycleTerminationCanBeDeferred());
    {
        HostedLineLifecycleGuard lifecycle(m_LineLifecycleBusy[irq], irq);
        if (!lifecycle.owned())
        {
            return 0;
        }

        if (!m_Handlers.registerHardHandler(irq, handler, policy))
        {
            return 0;
        }
    }

    publishDiagnosticLine(irq);

    return irqToSignal[irq];
}

bool HostedIrqManager::unregisterHandler(irq_id_t Id, IrqHandlerBase *handler)
{
    uint8_t irq = 0;
    if (!irqForSignal(Id, irq))
    {
        return false;
    }

    TerminationDeferral lifecycleTermination(
        lifecycleTerminationCanBeDeferred());
    IrqHandlerRegistry::UnregisterResult result;
    {
        HostedLineLifecycleGuard lifecycle(m_LineLifecycleBusy[irq], irq);
        if (!lifecycle.owned())
        {
            return false;
        }

        result = m_Handlers.unregisterHandler(irq, handler);
        const bool removed =
            result == IrqHandlerRegistry::UnregisterResult::Completed ||
            result == IrqHandlerRegistry::UnregisterResult::Deferred;
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        LineOwnershipHook hook =
            __atomic_load_n(&lineOwnershipHook, __ATOMIC_ACQUIRE);
        if (removed && hook)
        {
            hook(irq, LineOwnershipStage::BeforeFinalStateCheck, 0);
        }
#endif
        if (removed && !m_Handlers.handlerCount(irq))
        {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
            if (hook)
            {
                hook(irq, LineOwnershipStage::BeforeFinalCookieAdvance, 0);
            }
#endif
            __atomic_add_fetch(
                &m_ThreadedCookies[irq], static_cast<size_t>(1),
                __ATOMIC_ACQ_REL);
        }
    }
    // Atomic removal can briefly publish Draining before restoring Enabled
    // when a callback is active. Refresh rejected mutations as well so an
    // observer cannot retain that transient closed state.
    if (result != IrqHandlerRegistry::UnregisterResult::NotFound)
    {
        publishDiagnosticLine(irq);
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
    : m_Handlers(), m_ThreadedDispatcher(
                        MakeConstantString("hosted IRQ bottom half"),
                        NumHostedIrqs, dispatchThreadedLine, this),
      m_ThreadedCookies(), m_ThreadedPublicationFailures(),
      m_DispatchGenerations(), m_Diagnostics(), m_LineLifecycleBusy()
{
    for (size_t irq = 0; irq < NumHostedIrqs; ++irq)
    {
        publishDiagnosticLine(static_cast<uint8_t>(irq));
    }
}

bool HostedIrqManager::lifecycleTerminationCanBeDeferred() const
{
    Thread *current = Processor::information().getCurrentThread();
    return current && Processor::getInterrupts() &&
           !current->getHostedSignalDepth() && !Processor::inDeviceHardIrq();
}

void HostedIrqManager::publishDiagnosticLine(uint8_t irq)
{
    if (irq >= NumHostedIrqs)
    {
        return;
    }

    // One bounded retry repairs either a nested diagnostic refresh or a line
    // mutation which overtook a publisher after its registry snapshot.
    for (size_t attempt = 0; attempt < 2; ++attempt)
    {
        IrqHandlerRegistry::LineConfiguration configuration;
        if (!m_Handlers.snapshotLineConfiguration(irq, configuration))
        {
            m_Diagnostics.recordMissedPublication(irq);
            continue;
        }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        DiagnosticPublicationHook hook =
            __atomic_load_n(&diagnosticPublicationHook, __ATOMIC_ACQUIRE);
        if (hook)
        {
            hook(irq, configuration.mutationGeneration);
        }
#endif

        size_t targetBank = 0;
        IrqLineDiagnosticSnapshot *target =
            m_Diagnostics.beginPublication(irq, targetBank);
        if (!target)
        {
            return;
        }

        IrqLineDiagnosticSnapshot line = {};
        line.line = irq;
        line.handlerCount = configuration.handlerCount;
        line.configured = line.handlerCount != 0;
        line.effectiveMasked = !line.configured;
        line.requestedEnabled = line.configured;
        if (!line.configured)
        {
            line.maskReasons = IrqMaskNoHandler;
        }
        else
        {
            line.delivery =
                configuration.mode == IrqHandlerRegistry::LineMode::Threaded ?
                    IrqDelivery::Threaded :
                    IrqDelivery::Hard;
            if (configuration.policyConfigured)
            {
                line.trigger = configuration.trigger;
                line.controllerAck = configuration.controllerAck;
                line.lineRelease = configuration.lineRelease;
            }
        }

        *target = line;
        m_Diagnostics.finishPublication(irq, targetBank);

        IrqHandlerRegistry::LineConfiguration current;
        const bool registryStable =
            m_Handlers.snapshotLineConfiguration(irq, current) &&
            current.mutationGeneration == configuration.mutationGeneration;
        if (!registryStable)
        {
            m_Diagnostics.recordMissedPublication(irq);
            continue;
        }
        if (!m_Diagnostics.consumeDirty(irq))
        {
            break;
        }
    }
}

size_t HostedIrqManager::snapshotIrqLines(
    IrqLineDiagnosticSnapshot *out, size_t capacity) const
{
    if (!out || !capacity)
    {
        return 0;
    }

    const size_t count = capacity < NumHostedIrqs ? capacity : NumHostedIrqs;
    for (size_t irq = 0; irq < count; ++irq)
    {
        if (!m_Diagnostics.snapshot(irq, out[irq]))
        {
            out[irq] = {};
            out[irq].line = static_cast<uint8_t>(irq);
        }
    }

    const bool dispatcherInitialised = m_ThreadedDispatcher.isInitialised();
    for (size_t irq = 0; irq < count; ++irq)
    {
        out[irq].dispatchGeneration =
            __atomic_load_n(&m_DispatchGenerations[irq], __ATOMIC_ACQUIRE);
        out[irq].acknowledgedGeneration = out[irq].dispatchGeneration;
        out[irq].activeHardDispatchCount = m_Handlers.hardDispatchState(
            static_cast<uint8_t>(irq), out[irq].activeHardDispatchGeneration);
        out[irq].hardStageActive = out[irq].activeHardDispatchCount != 0;
        out[irq].publicationCookie =
            __atomic_load_n(&m_ThreadedCookies[irq], __ATOMIC_ACQUIRE);
        out[irq].pendingCookie =
            m_ThreadedDispatcher.pendingCookie(static_cast<uint8_t>(irq));
        out[irq].activeCookie =
            m_ThreadedDispatcher.activeCookie(static_cast<uint8_t>(irq));
        out[irq].completedCookie =
            m_ThreadedDispatcher.completedCookie(static_cast<uint8_t>(irq));
        out[irq].completedBatches =
            m_ThreadedDispatcher.completedBatches(static_cast<uint8_t>(irq));
        out[irq].publicationFailures = __atomic_load_n(
            &m_ThreadedPublicationFailures[irq], __ATOMIC_RELAXED);
        out[irq].diagnosticPublicationFailures =
            m_Diagnostics.missedPublications(irq);
        out[irq].workerIdentity =
            m_ThreadedDispatcher.workerIdentity(static_cast<uint8_t>(irq));
        out[irq].dispatcherInitialised = dispatcherInitialised;
        out[irq].dispatcherActive =
            m_ThreadedDispatcher.callbackActive(static_cast<uint8_t>(irq));
        out[irq].dispatcherClosed =
            m_ThreadedDispatcher.publicationClosed(static_cast<uint8_t>(irq));
    }
    return count;
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

    size_t dispatchGeneration = __atomic_add_fetch(
        &m_DispatchGenerations[irq], static_cast<size_t>(1), __ATOMIC_ACQ_REL);
    if (!dispatchGeneration)
    {
        dispatchGeneration = __atomic_add_fetch(
            &m_DispatchGenerations[irq], static_cast<size_t>(1),
            __ATOMIC_ACQ_REL);
    }

    if (m_Handlers.lineMode(irq) == IrqHandlerRegistry::LineMode::Threaded)
    {
        size_t cookie = __atomic_add_fetch(
            &m_ThreadedCookies[irq], static_cast<size_t>(1), __ATOMIC_ACQ_REL);
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
    const bool admitted = m_Handlers.dispatchHard(
        irq, state, handled, nullptr, dispatchGeneration);
    if (!admitted)
    {
        NOTICE("HostedIrqManager: unhandled irq #" << irq << " occurred");
    }
}

void HostedIrqManager::dispatchThreadedLine(
    void *context, uint8_t irq, size_t cookie)
{
    HostedIrqManager *manager = reinterpret_cast<HostedIrqManager *>(context);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    LineOwnershipHook hook =
        __atomic_load_n(&lineOwnershipHook, __ATOMIC_ACQUIRE);
    if (hook)
    {
        hook(irq, LineOwnershipStage::BeforeThreadedCookieValidation, cookie);
    }
#endif
    if (cookie !=
        __atomic_load_n(&manager->m_ThreadedCookies[irq], __ATOMIC_ACQUIRE))
    {
        return;
    }
    (void) manager->m_Handlers.dispatchThreaded(irq);
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

void HostedIrqManager::setDiagnosticPublicationHook(
    DiagnosticPublicationHook hook)
{
    __atomic_store_n(&diagnosticPublicationHook, hook, __ATOMIC_RELEASE);
}

void HostedIrqManager::setLineOwnershipHook(LineOwnershipHook hook)
{
    __atomic_store_n(&lineOwnershipHook, hook, __ATOMIC_RELEASE);
}

bool HostedIrqManager::dispatchHandlerForTest(
    uint8_t irq, HardIrqHandler *handler, InterruptState &state, bool &handled,
    size_t dispatchGeneration)
{
    return m_Instance.m_Handlers.dispatchHard(
        irq, state, handled, handler, dispatchGeneration);
}

bool HostedIrqManager::dispatchHandlerForTest(
    uint8_t irq, HardIrqHandler *handler, bool &handled,
    size_t dispatchGeneration)
{
    InterruptState state;
    return dispatchHandlerForTest(
        irq, handler, state, handled, dispatchGeneration);
}

void HostedIrqManager::withRegistryMutationLockForTest(MutationLockHook hook)
{
    m_Instance.m_Handlers.withMutationLockForTest(hook);
}

void HostedIrqManager::withRegistryMutationEpochForTest(MutationLockHook hook)
{
    m_Instance.m_Handlers.withMutationEpochForTest(hook);
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
