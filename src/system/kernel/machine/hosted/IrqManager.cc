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
#include "pedigree/kernel/machine/SchedulerIrqHandler.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Scheduler.h"
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
HostedIrqManager::SchedulerRoutePublicationHook
    schedulerRoutePublicationHook = nullptr;
#endif

class HostedLineLifecycleGuard
{
  public:
    HostedLineLifecycleGuard(
        size_t &busy, uint8_t irq, bool reportRejection = true)
        : m_Busy(busy), m_Owned(false)
    {
        size_t expected = 0;
        m_Owned = __atomic_compare_exchange_n(
            &m_Busy, &expected, static_cast<size_t>(1), false, __ATOMIC_ACQUIRE,
            __ATOMIC_RELAXED);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        if (!m_Owned && reportRejection)
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
        release();
    }

    bool owned() const
    {
        return m_Owned;
    }

    void release()
    {
        if (m_Owned)
        {
            __atomic_store_n(&m_Busy, static_cast<size_t>(0), __ATOMIC_RELEASE);
            m_Owned = false;
        }
    }

  private:
    size_t &m_Busy;
    bool m_Owned;
};

IrqDelivery deliveryForLineMode(IrqHandlerRegistry::LineMode mode)
{
    switch (mode)
    {
        case IrqHandlerRegistry::LineMode::Threaded:
            return IrqDelivery::Threaded;
        case IrqHandlerRegistry::LineMode::HardOnly:
            return IrqDelivery::Hard;
        case IrqHandlerRegistry::LineMode::Mixed:
            return IrqDelivery::Mixed;
        case IrqHandlerRegistry::LineMode::Empty:
            return IrqDelivery::None;
    }

    return IrqDelivery::None;
}

bool hasThreadedDelivery(IrqDelivery delivery)
{
    return delivery == IrqDelivery::Threaded ||
           delivery == IrqDelivery::Mixed;
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
            __atomic_load_n(&m_ShuttingDown, __ATOMIC_ACQUIRE) ||
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
        if (__atomic_load_n(&m_ShuttingDown, __ATOMIC_ACQUIRE))
        {
            return 0;
        }

        const size_t previousDelivery = __atomic_load_n(
            &m_LineDeliveries[irq], __ATOMIC_ACQUIRE);
        if (previousDelivery == static_cast<size_t>(IrqDelivery::None))
        {
            __atomic_store_n(
                &m_LineDeliveries[irq],
                static_cast<size_t>(IrqDelivery::Threaded), __ATOMIC_RELEASE);
        }
        if (!m_Handlers.registerThreadedHandler(irq, handler, policy))
        {
            __atomic_store_n(
                &m_LineDeliveries[irq], previousDelivery, __ATOMIC_RELEASE);
            return 0;
        }
        __atomic_store_n(
            &m_LineDeliveries[irq],
            static_cast<size_t>(deliveryForLineMode(m_Handlers.lineMode(irq))),
            __ATOMIC_RELEASE);
    }

    publishDiagnosticLine(irq);

    return irqToSignal[irq];
}

irq_id_t HostedIrqManager::registerHardIsaIrqHandler(
    uint8_t irq, HardIrqHandler *handler, const IrqPolicy &policy)
{
    if (UNLIKELY(
            __atomic_load_n(&m_ShuttingDown, __ATOMIC_ACQUIRE) ||
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
        if (__atomic_load_n(&m_ShuttingDown, __ATOMIC_ACQUIRE))
        {
            return 0;
        }

        const size_t previousDelivery = __atomic_load_n(
            &m_LineDeliveries[irq], __ATOMIC_ACQUIRE);
        if (previousDelivery == static_cast<size_t>(IrqDelivery::None))
        {
            __atomic_store_n(
                &m_LineDeliveries[irq],
                static_cast<size_t>(IrqDelivery::Hard), __ATOMIC_RELEASE);
        }
        if (!m_Handlers.registerHardHandler(irq, handler, policy))
        {
            __atomic_store_n(
                &m_LineDeliveries[irq], previousDelivery, __ATOMIC_RELEASE);
            return 0;
        }
        __atomic_store_n(
            &m_LineDeliveries[irq],
            static_cast<size_t>(deliveryForLineMode(m_Handlers.lineMode(irq))),
            __ATOMIC_RELEASE);
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
            __atomic_load_n(&m_ShuttingDown, __ATOMIC_ACQUIRE) ||
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
        if (__atomic_load_n(&m_ShuttingDown, __ATOMIC_ACQUIRE))
        {
            return 0;
        }

        const size_t previousDelivery = __atomic_load_n(
            &m_LineDeliveries[irq], __ATOMIC_ACQUIRE);
        if (previousDelivery == static_cast<size_t>(IrqDelivery::None))
        {
            __atomic_store_n(
                &m_LineDeliveries[irq],
                static_cast<size_t>(IrqDelivery::Threaded), __ATOMIC_RELEASE);
        }
        if (!m_Handlers.registerThreadedHandler(irq, handler, policy))
        {
            __atomic_store_n(
                &m_LineDeliveries[irq], previousDelivery, __ATOMIC_RELEASE);
            return 0;
        }
        __atomic_store_n(
            &m_LineDeliveries[irq],
            static_cast<size_t>(deliveryForLineMode(m_Handlers.lineMode(irq))),
            __ATOMIC_RELEASE);
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
            __atomic_load_n(&m_ShuttingDown, __ATOMIC_ACQUIRE) ||
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
        if (__atomic_load_n(&m_ShuttingDown, __ATOMIC_ACQUIRE))
        {
            return 0;
        }

        const size_t previousDelivery = __atomic_load_n(
            &m_LineDeliveries[irq], __ATOMIC_ACQUIRE);
        if (previousDelivery == static_cast<size_t>(IrqDelivery::None))
        {
            __atomic_store_n(
                &m_LineDeliveries[irq],
                static_cast<size_t>(IrqDelivery::Hard), __ATOMIC_RELEASE);
        }
        if (!m_Handlers.registerHardHandler(irq, handler, policy))
        {
            __atomic_store_n(
                &m_LineDeliveries[irq], previousDelivery, __ATOMIC_RELEASE);
            return 0;
        }
        __atomic_store_n(
            &m_LineDeliveries[irq],
            static_cast<size_t>(deliveryForLineMode(m_Handlers.lineMode(irq))),
            __ATOMIC_RELEASE);
    }

    publishDiagnosticLine(irq);

    return irqToSignal[irq];
}

irq_id_t HostedIrqManager::registerSchedulerIrqHandler(
    uint8_t irq, SchedulerIrqHandler *handler, const IrqPolicy &policy)
{
    if (irq != 1 || !handler || policy != IrqPolicy::syntheticHard() ||
        !irqToSignal[irq] ||
        __atomic_load_n(&m_ShuttingDown, __ATOMIC_ACQUIRE))
        return 0;

    SchedulerIrqHandler *expected = nullptr;
    if (!__atomic_compare_exchange_n(
            &m_SchedulerIrqHandlers[irq], &expected, handler, false,
            __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
        return 0;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    SchedulerRoutePublicationHook hook = __atomic_load_n(
        &schedulerRoutePublicationHook, __ATOMIC_ACQUIRE);
    if (hook)
    {
        hook(&m_ShuttingDown);
    }
#endif

    if (__atomic_load_n(&m_ShuttingDown, __ATOMIC_ACQUIRE))
    {
        SchedulerIrqHandler *published = handler;
        __atomic_compare_exchange_n(
            &m_SchedulerIrqHandlers[irq], &published,
            static_cast<SchedulerIrqHandler *>(nullptr), false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        publishDiagnosticLine(irq);
        return 0;
    }

    publishDiagnosticLine(irq);
    return irqToSignal[irq];
}

bool HostedIrqManager::unregisterSchedulerIrqHandler(
    irq_id_t Id, SchedulerIrqHandler *handler)
{
    uint8_t irq = 0;
    if (!handler || !irqForSignal(Id, irq) || irq != 1)
        return false;

    SchedulerIrqHandler *expected = handler;
    if (!__atomic_compare_exchange_n(
            &m_SchedulerIrqHandlers[irq], &expected,
            static_cast<SchedulerIrqHandler *>(nullptr), false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return false;

    publishDiagnosticLine(irq);
    return true;
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
    IrqHandlerRegistry::LineMode removedDelivery =
        IrqHandlerRegistry::LineMode::Empty;
    {
        HostedLineLifecycleGuard lifecycle(m_LineLifecycleBusy[irq], irq);
        if (!lifecycle.owned())
        {
            __atomic_add_fetch(
                &m_RemovalRejections[irq], static_cast<size_t>(1),
                __ATOMIC_RELAXED);
            return false;
        }

        result = m_Handlers.unregisterHandler(irq, handler, removedDelivery);
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
        if (removed)
        {
            const IrqHandlerRegistry::LineMode remainingMode =
                m_Handlers.lineMode(irq);
            const bool threadedLifetimeEnded =
                removedDelivery == IrqHandlerRegistry::LineMode::Threaded &&
                remainingMode != IrqHandlerRegistry::LineMode::Threaded &&
                remainingMode != IrqHandlerRegistry::LineMode::Mixed;
            const bool lineEmpty =
                remainingMode == IrqHandlerRegistry::LineMode::Empty;
            if (threadedLifetimeEnded || lineEmpty)
            {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
                if (hook)
                {
                    hook(
                        irq, LineOwnershipStage::BeforeFinalCookieAdvance, 0);
                }
#endif
                const size_t boundary = advanceThreadedCookie(irq);
                m_Handlers.invalidateThreadedLine(irq, boundary);
                __atomic_store_n(
                    &m_MixedHardOutcomeCookies[irq], static_cast<size_t>(0),
                    __ATOMIC_RELEASE);
            }
            __atomic_store_n(
                &m_LineDeliveries[irq],
                static_cast<size_t>(deliveryForLineMode(remainingMode)),
                __ATOMIC_RELEASE);
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
        __atomic_add_fetch(
            &m_RemovalRejections[irq], static_cast<size_t>(1),
            __ATOMIC_RELAXED);
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
    return !__atomic_load_n(&m_ShuttingDown, __ATOMIC_ACQUIRE) &&
           m_ThreadedDispatcher.initialise();
}

bool HostedIrqManager::shutdownThreaded()
{
    if (!m_ThreadedDispatcher.canShutdown() ||
        !lifecycleTerminationCanBeDeferred())
    {
        return false;
    }

    size_t expected = 0;
    if (!__atomic_compare_exchange_n(
            &m_ShuttingDown, &expected, static_cast<size_t>(1), false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        return !m_ThreadedDispatcher.isInitialised();
    }

    TerminationDeferral lifecycleTermination(true);
    for (size_t irq = 0; irq < NumHostedIrqs; ++irq)
    {
        const uint8_t line = static_cast<uint8_t>(irq);
        while (true)
        {
            HostedLineLifecycleGuard lifecycle(
                m_LineLifecycleBusy[irq], line, false);
            if (!lifecycle.owned())
            {
                Scheduler::instance().yield();
                continue;
            }

            const size_t boundary = advanceThreadedCookie(line);
            m_Handlers.invalidateThreadedLine(line, boundary);
            __atomic_store_n(
                &m_MixedHardOutcomeCookies[line], static_cast<size_t>(0),
                __ATOMIC_RELEASE);
            __atomic_store_n(
                &m_LineDeliveries[irq],
                static_cast<size_t>(IrqDelivery::None), __ATOMIC_RELEASE);
            break;
        }
    }
    return m_ThreadedDispatcher.shutdown();
}

HostedIrqManager::HostedIrqManager()
    : m_Handlers(), m_SchedulerIrqHandlers(), m_ThreadedDispatcher(
                        MakeConstantString("hosted IRQ bottom half"),
                        NumHostedIrqs, dispatchThreadedLine, this),
      m_ThreadedCookies(), m_MixedHardOutcomeCookies(),
      m_MixedHardHandled(), m_LineDeliveries(),
      m_ThreadedPublicationFailures(), m_RemovalRejections(),
      m_DispatchGenerations(), m_UnhandledInterrupts(), m_Diagnostics(),
      m_LineLifecycleBusy(), m_ShuttingDown(0)
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

size_t HostedIrqManager::advanceThreadedCookie(uint8_t irq)
{
    size_t cookie = __atomic_add_fetch(
        &m_ThreadedCookies[irq], static_cast<size_t>(1), __ATOMIC_ACQ_REL);
    if (!cookie)
    {
        cookie = __atomic_add_fetch(
            &m_ThreadedCookies[irq], static_cast<size_t>(1),
            __ATOMIC_ACQ_REL);
    }
    __atomic_store_n(
        &m_MixedHardOutcomeCookies[irq], static_cast<size_t>(0),
        __ATOMIC_RELEASE);
    return cookie;
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
        const bool schedulerLine =
            __atomic_load_n(
                &m_SchedulerIrqHandlers[irq], __ATOMIC_ACQUIRE) != nullptr;
        line.handlerCount = configuration.handlerCount +
                            (schedulerLine ? static_cast<size_t>(1) : 0);
        line.configured = line.handlerCount != 0;
        line.effectiveMasked = !line.configured;
        line.requestedEnabled = line.configured;
        if (!line.configured)
        {
            line.maskReasons = IrqMaskNoHandler;
        }
        else if (configuration.handlerCount)
        {
            line.delivery = deliveryForLineMode(configuration.mode);
            if (schedulerLine && line.delivery == IrqDelivery::Threaded)
                line.delivery = IrqDelivery::Mixed;
            if (configuration.policyConfigured)
            {
                line.trigger = configuration.trigger;
                line.controllerAck = configuration.controllerAck;
                line.lineRelease = configuration.lineRelease;
            }
        }
        else
        {
            line.delivery = IrqDelivery::Hard;
            line.trigger = IrqTrigger::Synthetic;
            line.controllerAck = IrqControllerAck::None;
            line.lineRelease = IrqLineRelease::AfterHardStage;
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
        out[irq].activeThreadedDispatchCount =
            m_Handlers.threadedDispatchState(
                static_cast<uint8_t>(irq),
                out[irq].activeThreadedHandlerIdentity);
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
        out[irq].interruptCount = __atomic_load_n(
            &m_DispatchGenerations[irq], __ATOMIC_RELAXED);
        out[irq].unhandledCount = __atomic_load_n(
            &m_UnhandledInterrupts[irq], __ATOMIC_RELAXED);
        out[irq].publicationFailures = __atomic_load_n(
            &m_ThreadedPublicationFailures[irq], __ATOMIC_RELAXED);
        out[irq].removalRejections = __atomic_load_n(
            &m_RemovalRejections[irq], __ATOMIC_RELAXED);
        out[irq].diagnosticPublicationFailures =
            m_Diagnostics.missedPublications(irq);
        out[irq].workerIdentity =
            m_ThreadedDispatcher.workerIdentity(static_cast<uint8_t>(irq));
        out[irq].dispatcherInitialised = dispatcherInitialised;
        out[irq].dispatcherActive =
            m_ThreadedDispatcher.callbackActive(static_cast<uint8_t>(irq));
        out[irq].dispatcherClosed =
            m_ThreadedDispatcher.publicationClosed(static_cast<uint8_t>(irq));
        m_ThreadedDispatcher.snapshotDiagnostics(
            static_cast<uint8_t>(irq), out[irq]);
    }
    return count;
}

void HostedIrqManager::interrupt(size_t interruptNumber, InterruptState &state)
{
    uint8_t irq = 0;
    if (!irqForSignal(interruptNumber, irq))
    {
        return;
    }

    if (__atomic_load_n(&m_ShuttingDown, __ATOMIC_ACQUIRE))
    {
        __atomic_add_fetch(
            &m_UnhandledInterrupts[irq], static_cast<size_t>(1),
            __ATOMIC_RELAXED);
        return;
    }

    SchedulerIrqHandler *schedulerHandler = __atomic_load_n(
        &m_SchedulerIrqHandlers[irq], __ATOMIC_ACQUIRE);
    const bool deviceOccurrenceAccounted =
        dispatchDeviceLine(irq, state, schedulerHandler != nullptr);

    if (schedulerHandler)
    {
        if (__atomic_load_n(&m_ShuttingDown, __ATOMIC_ACQUIRE))
        {
            __atomic_add_fetch(
                &m_UnhandledInterrupts[irq], static_cast<size_t>(1),
                __ATOMIC_RELAXED);
            return;
        }

        if (!deviceOccurrenceAccounted)
        {
            // The scheduler route used to be a generic hard handler, so retain
            // its occurrence accounting without entering the device registry.
            size_t dispatchGeneration = __atomic_add_fetch(
                &m_DispatchGenerations[irq], static_cast<size_t>(1),
                __ATOMIC_ACQ_REL);
            if (!dispatchGeneration)
            {
                __atomic_add_fetch(
                    &m_DispatchGenerations[irq], static_cast<size_t>(1),
                    __ATOMIC_ACQ_REL);
            }
        }

        // A scheduler tick may abandon this signal frame. It is deliberately
        // the terminal controller action for this occurrence.
        schedulerHandler->schedulerIrq(irq, state);
    }
}

bool HostedIrqManager::dispatchDeviceLine(
    uint8_t irq, InterruptState &state, bool schedulerRoutePresent)
{
    if (__atomic_load_n(&m_ShuttingDown, __ATOMIC_ACQUIRE))
    {
        __atomic_add_fetch(
            &m_UnhandledInterrupts[irq], static_cast<size_t>(1),
            __ATOMIC_RELAXED);
        return false;
    }

    HostedLineLifecycleGuard lifecycle(
        m_LineLifecycleBusy[irq], irq, false);
    if (!lifecycle.owned() ||
        __atomic_load_n(&m_ShuttingDown, __ATOMIC_ACQUIRE))
    {
        __atomic_add_fetch(
            &m_UnhandledInterrupts[irq], static_cast<size_t>(1),
            __ATOMIC_RELAXED);
        return false;
    }

    const IrqDelivery delivery = static_cast<IrqDelivery>(__atomic_load_n(
        &m_LineDeliveries[irq], __ATOMIC_ACQUIRE));
    if (delivery == IrqDelivery::None && schedulerRoutePresent)
    {
        // Lifecycle ownership makes this the admission boundary: a generic
        // route published after this point begins with the next signal.
        return false;
    }

    IrqHandlerRegistry::AdmissionCutoff admissionCutoff = {};
    IrqHandlerRegistry::MixedAdmissionCutoffs mixedAdmissionCutoffs = {};
    bool cutoffCaptured = false;
    if (delivery == IrqDelivery::Mixed)
    {
        cutoffCaptured = m_Handlers.captureMixedAdmissionCutoffs(
            irq, mixedAdmissionCutoffs);
    }
    else
    {
        cutoffCaptured =
            m_Handlers.captureAdmissionCutoff(irq, admissionCutoff);
    }
    if (!cutoffCaptured)
    {
        __atomic_add_fetch(
            delivery == IrqDelivery::Threaded ||
                    delivery == IrqDelivery::Mixed ?
                &m_ThreadedPublicationFailures[irq] :
                &m_UnhandledInterrupts[irq],
            static_cast<size_t>(1), __ATOMIC_RELAXED);
        return false;
    }

    size_t dispatchGeneration = __atomic_add_fetch(
        &m_DispatchGenerations[irq], static_cast<size_t>(1), __ATOMIC_ACQ_REL);
    if (!dispatchGeneration)
    {
        dispatchGeneration = __atomic_add_fetch(
            &m_DispatchGenerations[irq], static_cast<size_t>(1),
            __ATOMIC_ACQ_REL);
    }

    if (delivery == IrqDelivery::Threaded)
    {
        const size_t cookie = advanceThreadedCookie(irq);
        m_Handlers.publishThreadedDispatch(irq, cookie, admissionCutoff);
        if (!m_ThreadedDispatcher.publishFromInterrupt(irq, cookie))
        {
            m_Handlers.cancelThreadedDispatch(irq, cookie);
            __atomic_add_fetch(
                &m_ThreadedPublicationFailures[irq], static_cast<size_t>(1),
                __ATOMIC_RELAXED);
        }
        return true;
    }

    if (delivery == IrqDelivery::Mixed)
    {
        const size_t cookie = advanceThreadedCookie(irq);
        m_Handlers.publishThreadedDispatch(
            irq, cookie, mixedAdmissionCutoffs.threaded);

        // The occurrence leases now pin exact registry membership. A hard
        // callback must be free to remove itself through this manager.
        lifecycle.release();

        bool handled = false;
        const bool admitted = m_Handlers.dispatchHard(
            irq, state, handled, nullptr, dispatchGeneration,
            mixedAdmissionCutoffs.hard);

        HostedLineLifecycleGuard tail(m_LineLifecycleBusy[irq], irq, false);
        const IrqDelivery currentDelivery =
            static_cast<IrqDelivery>(__atomic_load_n(
                &m_LineDeliveries[irq], __ATOMIC_ACQUIRE));
        if (!tail.owned() ||
            __atomic_load_n(&m_ShuttingDown, __ATOMIC_ACQUIRE))
        {
            m_Handlers.cancelThreadedDispatch(irq, cookie);
            __atomic_add_fetch(
                &m_ThreadedPublicationFailures[irq], static_cast<size_t>(1),
                __ATOMIC_RELAXED);
            return true;
        }

        const bool cookieCurrent =
            cookie == __atomic_load_n(
                          &m_ThreadedCookies[irq], __ATOMIC_ACQUIRE);
        if (!hasThreadedDelivery(currentDelivery) || !cookieCurrent)
        {
            // A newer occurrence owns the current cookie, while ending the
            // old threaded lifetime requires its source to be quiesced. A
            // replacement handler belongs to the next occurrence, so neither
            // transition is a publication failure for this old action.
            m_Handlers.cancelThreadedDispatch(irq, cookie);
            return true;
        }

        const bool hardStageQuiesced =
            currentDelivery == IrqDelivery::Threaded;
        __atomic_store_n(
            &m_MixedHardHandled[irq],
            static_cast<size_t>(
                (admitted && handled) || hardStageQuiesced),
            __ATOMIC_RELAXED);
        __atomic_store_n(
            &m_MixedHardOutcomeCookies[irq], cookie, __ATOMIC_RELEASE);
        if (!m_ThreadedDispatcher.publishFromInterrupt(irq, cookie))
        {
            m_Handlers.cancelThreadedDispatch(irq, cookie);
            __atomic_store_n(
                &m_MixedHardOutcomeCookies[irq], static_cast<size_t>(0),
                __ATOMIC_RELEASE);
            __atomic_add_fetch(
                &m_ThreadedPublicationFailures[irq], static_cast<size_t>(1),
                __ATOMIC_RELAXED);
        }
        return true;
    }

    // The registry cutoff and grace record now own hard-handler membership.
    // Releasing controller lifetime ownership here preserves hard callbacks'
    // ability to unregister themselves without admitting a replacement.
    lifecycle.release();

    if (delivery != IrqDelivery::Hard)
    {
        m_Handlers.releaseAdmissionCutoff(admissionCutoff);
        __atomic_add_fetch(
            &m_UnhandledInterrupts[irq], static_cast<size_t>(1),
            __ATOMIC_RELAXED);
        return true;
    }

    bool handled = false;
    const bool admitted = m_Handlers.dispatchHard(
        irq, state, handled, nullptr, dispatchGeneration, admissionCutoff);
    if (!admitted || !handled)
    {
        __atomic_add_fetch(
            &m_UnhandledInterrupts[irq], static_cast<size_t>(1),
            __ATOMIC_RELAXED);
    }
    return true;
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
    const IrqDelivery delivery = static_cast<IrqDelivery>(__atomic_load_n(
        &manager->m_LineDeliveries[irq], __ATOMIC_ACQUIRE));
    if (cookie !=
            __atomic_load_n(
                &manager->m_ThreadedCookies[irq], __ATOMIC_ACQUIRE) ||
        !hasThreadedDelivery(delivery))
    {
        return;
    }
    const bool mixedOccurrence =
        cookie == __atomic_load_n(
                      &manager->m_MixedHardOutcomeCookies[irq],
                      __ATOMIC_ACQUIRE);
    IrqHandlerRegistry::ThreadedDispatchResult result = {};
    const bool admitted = manager->m_Handlers.dispatchThreaded(
        irq, cookie, result);
    const IrqDelivery completedDelivery =
        static_cast<IrqDelivery>(__atomic_load_n(
            &manager->m_LineDeliveries[irq], __ATOMIC_ACQUIRE));
    if (cookie !=
            __atomic_load_n(
                &manager->m_ThreadedCookies[irq], __ATOMIC_ACQUIRE) ||
        !hasThreadedDelivery(completedDelivery))
    {
        return;
    }

    bool accepted = admitted && result.allowRearm;
    if (mixedOccurrence)
    {
        if (cookie != __atomic_load_n(
                          &manager->m_MixedHardOutcomeCookies[irq],
                          __ATOMIC_ACQUIRE))
        {
            return;
        }
        accepted |= __atomic_load_n(
                        &manager->m_MixedHardHandled[irq],
                        __ATOMIC_RELAXED) != 0;
        const IrqDelivery currentDelivery =
            static_cast<IrqDelivery>(__atomic_load_n(
                &manager->m_LineDeliveries[irq], __ATOMIC_ACQUIRE));
        if (cookie !=
                __atomic_load_n(
                    &manager->m_ThreadedCookies[irq], __ATOMIC_ACQUIRE) ||
            !hasThreadedDelivery(currentDelivery) ||
            cookie != __atomic_load_n(
                          &manager->m_MixedHardOutcomeCookies[irq],
                          __ATOMIC_ACQUIRE))
        {
            return;
        }

        size_t exactCookie = cookie;
        if (!__atomic_compare_exchange_n(
                &manager->m_MixedHardOutcomeCookies[irq], &exactCookie,
                static_cast<size_t>(0), false, __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE))
        {
            return;
        }
    }
    if (!accepted)
    {
        __atomic_add_fetch(
            &manager->m_UnhandledInterrupts[irq], static_cast<size_t>(1),
            __ATOMIC_RELAXED);
    }
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void HostedIrqManager::setHandlerPinHook(HandlerPinHook hook)
{
    m_Instance.m_Handlers.setHandlerPinHook(hook);
}

void HostedIrqManager::setDispatchAbandonHook(DispatchAbandonHook hook)
{
    m_Instance.m_Handlers.setDispatchAbandonHook(hook);
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

void HostedIrqManager::setSchedulerRoutePublicationHookForTest(
    SchedulerRoutePublicationHook hook)
{
    __atomic_store_n(
        &schedulerRoutePublicationHook, hook, __ATOMIC_RELEASE);
}

SchedulerIrqHandler *HostedIrqManager::schedulerIrqHandlerForTest(uint8_t irq)
{
    if (irq >= NumHostedIrqs)
        return nullptr;
    return __atomic_load_n(
        &m_Instance.m_SchedulerIrqHandlers[irq], __ATOMIC_ACQUIRE);
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

size_t HostedIrqManager::claimedDispatchCountForOwnerForTest(void *owner)
{
    return m_Instance.m_Handlers.claimedDispatchCountForOwnerForTest(owner);
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
