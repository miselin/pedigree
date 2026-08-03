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

#include "Pic.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/SchedulerIrqHandler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/InterruptManager.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/utility.h"

#define BASE_INTERRUPT_VECTOR 0x20

// Number of IRQs in a single millisecond before an IRQ source is blocked.
// A value of 10, for example, would mean if an IRQ matches the threshold
// and sustained its output for a second, 10,000 IRQs would be triggered.
#define DEFAULT_IRQ_MITIGATE_THRESHOLD 10

Pic Pic::m_Instance;

void Pic::tick()
{
}

bool Pic::control(uint8_t irq, ControlCode code, size_t argument)
{
    if (UNLIKELY(irq >= 16))
        return false;

    LockGuard<Spinlock> guard(m_Lock);

    switch (code)
    {
        case MitigationThreshold:
            if (LIKELY(argument))
            {
                if (UNLIKELY(m_IrqState.handlerCount(irq) > 1))
                    m_MitigationThreshold[irq] += argument;
                else
                    m_MitigationThreshold[irq] = argument;
            }
            else
                m_MitigationThreshold[irq] = DEFAULT_IRQ_MITIGATE_THRESHOLD;
            return true;
    }

    return false;
}

irq_id_t
Pic::registerIsaIrqHandler(
    uint8_t irq, IrqHandler *handler, const IrqPolicy &policy)
{
    if (UNLIKELY(
            irq >= PicIrqState::LineCount || !handler ||
            !m_ThreadedDispatcher.isInitialised() ||
            !policy.validForThreaded() ||
            policy.trigger() == IrqTrigger::Synthetic))
        return 0;

    LockGuard<Spinlock> guard(m_Lock);
    if (m_ShuttingDown || m_UnregisterReservations[irq] ||
        !m_ThreadedDispatcher.isInitialised())
        return 0;
    if (!m_IrqState.canRegister(irq, policy, IrqDelivery::Threaded))
    {
        ERROR(
            "PIC: IRQ " << Dec << irq
                        << " was registered with incompatible trigger modes");
        return 0;
    }
    const bool firstHandler = !m_IrqState.handlerCount(irq);
    if (!m_Handlers.registerThreadedHandler(irq, handler, policy))
        return 0;

    if (firstHandler)
    {
        advanceThreadedCookieLocked(irq);
    }
    m_IrqState.handlerRegistered(irq, policy, IrqDelivery::Threaded);
    applyMaskLocked();
    publishDiagnosticLineLocked(irq);

    return irq + BASE_INTERRUPT_VECTOR;
}

irq_id_t
Pic::registerHardIsaIrqHandler(
    uint8_t irq, HardIrqHandler *handler, const IrqPolicy &policy)
{
    if (UNLIKELY(
            irq >= PicIrqState::LineCount || !handler ||
            !policy.validForHard() ||
            policy.trigger() == IrqTrigger::Synthetic))
        return 0;

    LockGuard<Spinlock> guard(m_Lock);
    if (m_ShuttingDown || m_UnregisterReservations[irq])
        return 0;
    if (!m_IrqState.canRegister(irq, policy, IrqDelivery::Hard))
    {
        ERROR(
            "PIC: IRQ " << Dec << irq
                        << " was registered with incompatible trigger modes");
        return 0;
    }
    const bool firstHandler = !m_IrqState.handlerCount(irq);
    if (!m_Handlers.registerHardHandler(irq, handler, policy))
        return 0;

    if (firstHandler)
    {
        advanceThreadedCookieLocked(irq);
    }
    m_IrqState.handlerRegistered(irq, policy, IrqDelivery::Hard);
    applyMaskLocked();
    publishDiagnosticLineLocked(irq);

    return irq + BASE_INTERRUPT_VECTOR;
}
irq_id_t Pic::registerPciIrqHandler(
    IrqHandler *handler, Device *pDevice, const IrqPolicy &policy)
{
    if (UNLIKELY(!pDevice))
        return 0;
    irq_id_t irq = pDevice->getInterruptNumber();
    if (UNLIKELY(
            irq >= PicIrqState::LineCount || !handler ||
            !m_ThreadedDispatcher.isInitialised() ||
            !policy.validForThreaded() ||
            policy.trigger() != IrqTrigger::Level))
        return 0;

    LockGuard<Spinlock> guard(m_Lock);
    if (m_ShuttingDown || m_UnregisterReservations[irq] ||
        !m_ThreadedDispatcher.isInitialised())
        return 0;
    if (!m_IrqState.canRegister(irq, policy, IrqDelivery::Threaded))
    {
        ERROR(
            "PIC: PCI IRQ " << Dec << irq
                            << " conflicts with an edge-triggered handler");
        return 0;
    }
    const bool firstHandler = !m_IrqState.handlerCount(irq);
    if (!m_Handlers.registerThreadedHandler(irq, handler, policy))
        return 0;

    if (firstHandler)
    {
        advanceThreadedCookieLocked(irq);
    }
    m_IrqState.handlerRegistered(irq, policy, IrqDelivery::Threaded);
    applyMaskLocked();
    publishDiagnosticLineLocked(irq);

    return irq + BASE_INTERRUPT_VECTOR;
}

irq_id_t
Pic::registerHardPciIrqHandler(
    HardIrqHandler *handler, Device *pDevice, const IrqPolicy &policy)
{
    if (UNLIKELY(!pDevice))
        return 0;
    irq_id_t irq = pDevice->getInterruptNumber();
    if (UNLIKELY(
            irq >= PicIrqState::LineCount || !handler ||
            !policy.validForHard() ||
            policy.trigger() != IrqTrigger::Level))
        return 0;

    LockGuard<Spinlock> guard(m_Lock);
    if (m_ShuttingDown || m_UnregisterReservations[irq])
        return 0;
    if (!m_IrqState.canRegister(irq, policy, IrqDelivery::Hard))
    {
        ERROR(
            "PIC: PCI IRQ " << Dec << irq
                            << " conflicts with an edge-triggered handler");
        return 0;
    }
    const bool firstHandler = !m_IrqState.handlerCount(irq);
    if (!m_Handlers.registerHardHandler(irq, handler, policy))
        return 0;

    if (firstHandler)
    {
        advanceThreadedCookieLocked(irq);
    }
    m_IrqState.handlerRegistered(irq, policy, IrqDelivery::Hard);
    applyMaskLocked();
    publishDiagnosticLineLocked(irq);

    return irq + BASE_INTERRUPT_VECTOR;
}

irq_id_t Pic::registerSchedulerIrqHandler(
    uint8_t irq, SchedulerIrqHandler *handler, const IrqPolicy &policy)
{
    if (irq >= PicIrqState::LineCount || !handler)
        return 0;

    LockGuard<Spinlock> guard(m_Lock);
    if (m_ShuttingDown || m_UnregisterReservations[irq] ||
        !m_IrqState.canRegisterScheduler(irq, policy))
        return 0;

    m_SchedulerIrqHandler = handler;
    m_IrqState.schedulerRegistered(irq, policy);
    applyMaskLocked();
    publishDiagnosticLineLocked(irq);
    return irq + BASE_INTERRUPT_VECTOR;
}

bool Pic::unregisterSchedulerIrqHandler(
    irq_id_t Id, SchedulerIrqHandler *handler)
{
    if (Id != BASE_INTERRUPT_VECTOR || !handler)
        return false;

    LockGuard<Spinlock> guard(m_Lock);
    if (m_SchedulerIrqHandler != handler ||
        !m_IrqState.schedulerRegistered(0))
        return false;

    m_SchedulerIrqHandler = nullptr;
    m_IrqState.schedulerUnregistered(0);
    applyMaskLocked();
    publishDiagnosticLineLocked(0);
    return true;
}

bool Pic::unregisterHandler(irq_id_t Id, IrqHandlerBase *handler)
{
    if (Id < BASE_INTERRUPT_VECTOR ||
        Id >= BASE_INTERRUPT_VECTOR + PicIrqState::LineCount || !handler)
        return false;

    uint8_t irq = Id - BASE_INTERRUPT_VECTOR;

    {
        LockGuard<Spinlock> guard(m_Lock);
        if (!m_IrqState.handlerCount(irq))
        {
            return false;
        }
        ++m_UnregisterReservations[irq];
    }

    IrqHandlerRegistry::LineMode removedDelivery =
        IrqHandlerRegistry::LineMode::Empty;
    const IrqHandlerRegistry::UnregisterResult result =
        m_Handlers.unregisterHandler(irq, handler, removedDelivery);
    {
        LockGuard<Spinlock> guard(m_Lock);
        assert(m_UnregisterReservations[irq]);
        --m_UnregisterReservations[irq];
        if (result == IrqHandlerRegistry::UnregisterResult::Completed ||
            result == IrqHandlerRegistry::UnregisterResult::Deferred)
        {
            assert(
                removedDelivery == IrqHandlerRegistry::LineMode::Threaded ||
                removedDelivery == IrqHandlerRegistry::LineMode::HardOnly);
            const IrqDelivery previousDelivery = m_IrqState.delivery(irq);
            const IrqDelivery delivery =
                removedDelivery == IrqHandlerRegistry::LineMode::Threaded ?
                    IrqDelivery::Threaded :
                    IrqDelivery::Hard;
            m_IrqState.handlerUnregistered(irq, delivery);
            const IrqDelivery currentDelivery = m_IrqState.delivery(irq);
            if (currentDelivery == IrqDelivery::None ||
                (previousDelivery == IrqDelivery::Mixed &&
                 currentDelivery == IrqDelivery::Hard))
            {
                const size_t boundary = advanceThreadedCookieLocked(irq);
                m_Handlers.invalidateThreadedLine(irq, boundary);
                m_ThreadedDispatchGenerations[irq] = 0;
                m_ThreadedHadHardStage[irq] = false;
                m_ThreadedHardAdmitted[irq] = false;
                m_ThreadedHardHandled[irq] = false;
            }
            applyMaskLocked();
            publishDiagnosticLineLocked(irq);
        }
    }

    if (result == IrqHandlerRegistry::UnregisterResult::Rejected)
    {
        __atomic_add_fetch(
            &m_RemovalRejections[irq], static_cast<size_t>(1),
            __ATOMIC_RELAXED);
    }
    return result == IrqHandlerRegistry::UnregisterResult::Completed;
}

bool Pic::initialise()
{
    // Allocate the I/O ports
    if (m_SlavePort.allocate(0xA0, 4) == false)
        return false;
    if (m_MasterPort.allocate(0x20, 4) == false)
        return false;

    // Initialise the slave and master PIC
    m_MasterPort.write8(0x11, 0);
    m_SlavePort.write8(0x11, 0);
    m_MasterPort.write8(BASE_INTERRUPT_VECTOR, 1);
    m_SlavePort.write8(BASE_INTERRUPT_VECTOR + 0x08, 1);
    m_MasterPort.write8(0x04, 1);
    m_SlavePort.write8(0x02, 1);
    m_MasterPort.write8(0x01, 1);
    m_SlavePort.write8(0x01, 1);
    m_MasterPort.write8(0x00, 1);
    m_SlavePort.write8(0x00, 1);

    // Register the interrupts
    InterruptManager &IntManager = InterruptManager::instance();
    for (size_t i = 0; i < 16; i++)
        if (IntManager.registerInterruptHandler(
                i + BASE_INTERRUPT_VECTOR, this) == false)
            return false;

    for (size_t i = 0; i < 16; i++)
    {
        __atomic_store_n(
            &m_IrqCount[i], static_cast<size_t>(0), __ATOMIC_RELAXED);
        __atomic_store_n(
            &m_SpuriousIrqCount[i], static_cast<size_t>(0), __ATOMIC_RELAXED);
        __atomic_store_n(
            &m_UnhandledIrqCount[i], static_cast<size_t>(0), __ATOMIC_RELAXED);
        m_MitigatedIrqs[i] = false;
        m_MitigationThreshold[i] = DEFAULT_IRQ_MITIGATE_THRESHOLD;
    }

    // Disable all IRQ's (exept IRQ2)
    enableAll(false);

    return true;
}

bool Pic::initialiseThreaded()
{
    return m_ThreadedDispatcher.initialise();
}

bool Pic::shutdownThreaded()
{
    if (!m_ThreadedDispatcher.canShutdown())
    {
        return false;
    }

    TerminationDeferral shutdownTermination;
    {
        LockGuard<Spinlock> guard(m_Lock);
        m_ShuttingDown = true;
        m_IrqState.setAllEnabled(false);
        m_IrqState.setEnabled(2, false);
        for (size_t irq = 0; irq < PicIrqState::LineCount; ++irq)
        {
            const uint8_t line = static_cast<uint8_t>(irq);
            const size_t boundary = advanceThreadedCookieLocked(line);
            m_Handlers.invalidateThreadedLine(line, boundary);
            m_ThreadedDispatchGenerations[irq] = 0;
            m_ThreadedHadHardStage[irq] = false;
            m_ThreadedHardAdmitted[irq] = false;
            m_ThreadedHardHandled[irq] = false;
        }
        applyMaskLocked();
        publishAllDiagnosticLinesLocked();
    }
    return m_ThreadedDispatcher.shutdown();
}

Pic::Pic()
    : m_SlavePort("PIC #2"), m_MasterPort("PIC #1"), m_Handlers(),
      m_SchedulerIrqHandler(nullptr), m_IrqState(),
      m_ThreadedDispatcher(
          MakeConstantString("PIC IRQ bottom half"), PicIrqState::LineCount,
          dispatchThreadedLine, this),
      m_ThreadedCookies(), m_ThreadedDispatchGenerations(),
      m_ThreadedHadHardStage(), m_ThreadedHardAdmitted(),
      m_ThreadedHardHandled(),
      m_ThreadedPublicationFailures(), m_RemovalRejections(),
      m_Diagnostics(), m_UnregisterReservations(), m_ShuttingDown(false),
      m_IrqCount(), m_SpuriousIrqCount(),
      m_UnhandledIrqCount(), m_MitigatedIrqs(), m_MitigationThreshold(),
      m_Lock(false)
{
    publishAllDiagnosticLinesLocked();
}

void Pic::publishDiagnosticLineLocked(uint8_t irq)
{
    if (irq >= PicIrqState::LineCount)
    {
        return;
    }

    size_t targetBank = 0;
    IrqLineDiagnosticSnapshot *target =
        m_Diagnostics.beginPublication(irq, targetBank);
    if (!target)
    {
        return;
    }

    IrqLineDiagnosticSnapshot line = {};
    line.line = irq;
    line.handlerCount = m_IrqState.handlerCount(irq);
    line.configured = line.handlerCount != 0;
    line.delivery = m_IrqState.delivery(irq);
    line.effectiveMasked = !m_IrqState.enabled(irq);
    line.requestedEnabled = m_IrqState.requestedEnabled(irq);
    line.acknowledgementPending = m_IrqState.acknowledgementPending(irq);
    line.threadedPending = m_IrqState.threadedPending(irq);
    line.dispatchGeneration = m_IrqState.dispatchGeneration(irq);
    line.acknowledgedGeneration = m_IrqState.acknowledgedGeneration(irq);
    line.publicationCookie = m_ThreadedCookies[irq];
    line.interruptCount =
        __atomic_load_n(&m_IrqCount[irq], __ATOMIC_RELAXED);
    line.spuriousCount =
        __atomic_load_n(&m_SpuriousIrqCount[irq], __ATOMIC_RELAXED);
    line.unhandledCount =
        __atomic_load_n(&m_UnhandledIrqCount[irq], __ATOMIC_RELAXED);
    line.publicationFailures =
        __atomic_load_n(&m_ThreadedPublicationFailures[irq], __ATOMIC_RELAXED);
    line.removalRejections =
        __atomic_load_n(&m_RemovalRejections[irq], __ATOMIC_RELAXED);

    if (!line.configured)
    {
        line.maskReasons |= IrqMaskNoHandler;
    }
    else
    {
        line.trigger = m_IrqState.trigger(irq);
        line.controllerAck = m_IrqState.controllerAck(irq);
        line.lineRelease = m_IrqState.lineRelease(irq);
    }
    if (!line.requestedEnabled)
    {
        line.maskReasons |= IrqMaskAdministrativelyDisabled;
    }
    if (line.acknowledgementPending)
    {
        line.maskReasons |= IrqMaskAwaitingAcknowledgement;
    }
    if (line.threadedPending)
    {
        line.maskReasons |= IrqMaskAwaitingThreadedCompletion;
    }
    if (m_MitigatedIrqs[irq])
    {
        line.maskReasons |= IrqMaskMitigated;
    }
    if (m_ShuttingDown)
    {
        line.maskReasons |= IrqMaskShuttingDown;
    }

    *target = line;
    m_Diagnostics.finishPublication(irq, targetBank);
}

void Pic::publishAllDiagnosticLinesLocked()
{
    for (size_t irq = 0; irq < PicIrqState::LineCount; ++irq)
    {
        publishDiagnosticLineLocked(static_cast<uint8_t>(irq));
    }
}

size_t
Pic::snapshotIrqLines(IrqLineDiagnosticSnapshot *out, size_t capacity) const
{
    if (!out || !capacity)
    {
        return 0;
    }

    const size_t count =
        capacity < PicIrqState::LineCount ? capacity : PicIrqState::LineCount;
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
        out[irq].pendingCookie =
            m_ThreadedDispatcher.pendingCookie(static_cast<uint8_t>(irq));
        out[irq].activeHardDispatchCount = m_Handlers.hardDispatchState(
            static_cast<uint8_t>(irq),
            out[irq].activeHardDispatchGeneration);
        out[irq].hardStageActive = out[irq].activeHardDispatchCount != 0;
        out[irq].activeThreadedDispatchCount =
            m_Handlers.threadedDispatchState(
                static_cast<uint8_t>(irq),
                out[irq].activeThreadedHandlerIdentity);
        out[irq].activeCookie =
            m_ThreadedDispatcher.activeCookie(static_cast<uint8_t>(irq));
        out[irq].completedCookie =
            m_ThreadedDispatcher.completedCookie(static_cast<uint8_t>(irq));
        out[irq].completedBatches =
            m_ThreadedDispatcher.completedBatches(static_cast<uint8_t>(irq));
        out[irq].interruptCount =
            __atomic_load_n(&m_IrqCount[irq], __ATOMIC_RELAXED);
        out[irq].spuriousCount =
            __atomic_load_n(&m_SpuriousIrqCount[irq], __ATOMIC_RELAXED);
        out[irq].unhandledCount =
            __atomic_load_n(&m_UnhandledIrqCount[irq], __ATOMIC_RELAXED);
        out[irq].publicationFailures = __atomic_load_n(
            &m_ThreadedPublicationFailures[irq], __ATOMIC_RELAXED);
        out[irq].removalRejections =
            __atomic_load_n(&m_RemovalRejections[irq], __ATOMIC_RELAXED);
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

size_t Pic::advanceThreadedCookieLocked(uint8_t irq)
{
    assert(irq < PicIrqState::LineCount);
    size_t cookie = ++m_ThreadedCookies[irq];
    if (!cookie)
    {
        cookie = ++m_ThreadedCookies[irq];
    }
    return cookie;
}

bool Pic::spuriousLocked(size_t irq)
{
    if (irq > 7)
    {
        // Get ISR for slave.
        uint8_t mask = 1 << (irq - 8);
        m_SlavePort.write8(0x0B, 0);
        uint8_t isr = m_SlavePort.read8(0);
        m_SlavePort.write8(0x0A, 0);
        return (isr & mask) == 0;
    }
    else
    {
        // Get ISR for master.
        uint8_t mask = 1 << irq;
        m_MasterPort.write8(0x0B, 0);
        uint8_t isr = m_MasterPort.read8(0);
        m_MasterPort.write8(0x0A, 0);
        return (isr & mask) == 0;
    }
}

void Pic::interrupt(size_t interruptNumber, InterruptState &state)
{
    size_t irq = (interruptNumber - BASE_INTERRUPT_VECTOR);
    if (irq >= PicIrqState::LineCount)
    {
        return;
    }

    if (irq == 0)
    {
        SchedulerIrqHandler *schedulerHandler = nullptr;
        {
            LockGuard<Spinlock> guard(m_Lock);
            schedulerHandler = m_SchedulerIrqHandler;
            if (schedulerHandler)
            {
                __atomic_add_fetch(
                    &m_IrqCount[irq], static_cast<size_t>(1),
                    __ATOMIC_RELAXED);
                if (!m_IrqState.enabled(irq) && spuriousLocked(irq))
                {
                    __atomic_add_fetch(
                        &m_SpuriousIrqCount[irq], static_cast<size_t>(1),
                        __ATOMIC_RELAXED);
                    publishDiagnosticLineLocked(static_cast<uint8_t>(irq));
                    return;
                }

                const size_t generation = m_IrqState.beginDispatch(irq);

                // timer() can switch away permanently, so complete controller
                // and software acknowledgement before entering the scheduler.
                eoiLocked(static_cast<uint8_t>(irq));
                m_IrqState.acknowledge(irq);
                m_IrqState.completeDispatch(irq, generation, false);
                publishDiagnosticLineLocked(static_cast<uint8_t>(irq));
            }
        }

        if (schedulerHandler)
        {
            schedulerHandler->schedulerIrq(static_cast<irq_id_t>(irq), state);
            return;
        }
    }

    IrqControllerAck controllerAck = IrqControllerAck::None;
    IrqLineRelease lineRelease = IrqLineRelease::AfterHardStage;
    IrqDelivery delivery = IrqDelivery::None;
    bool hasHardStage = false;
    bool hasThreadedStage = false;
    size_t dispatchGeneration = 0;
    size_t threadedCookie = 0;
    IrqHandlerRegistry::AdmissionCutoff hardAdmissionCutoff = {};
    {
        LockGuard<Spinlock> guard(m_Lock);
        __atomic_add_fetch(
            &m_IrqCount[irq], static_cast<size_t>(1), __ATOMIC_RELAXED);
        controllerAck = m_IrqState.controllerAck(irq);
        lineRelease = m_IrqState.lineRelease(irq);

        // IRQ7 and IRQ15 are the architectural spurious-vector cases. A
        // disabled line can also have a vector already in flight, so retain
        // the broader check before touching its in-service state.
        if ((!m_IrqState.enabled(irq) || irq == 7 || irq == 15) &&
            spuriousLocked(irq))
        {
            if (irq > 7)
            {
                // A spurious slave vector never entered the slave ISR, but
                // the master still accepted the cascade interrupt.
                m_MasterPort.write8(0x62, 0);
            }
            __atomic_add_fetch(
                &m_SpuriousIrqCount[irq], static_cast<size_t>(1),
                __ATOMIC_RELAXED);
            publishDiagnosticLineLocked(static_cast<uint8_t>(irq));
            return;
        }

        dispatchGeneration = m_IrqState.beginDispatch(irq);
        delivery = m_IrqState.delivery(irq);
        hasThreadedStage = delivery == IrqDelivery::Threaded ||
                           delivery == IrqDelivery::Mixed;
        hasHardStage = delivery != IrqDelivery::Threaded;

        IrqHandlerRegistry::AdmissionCutoff threadedAdmissionCutoff = {};
        bool cutoffCaptured = false;
        if (delivery == IrqDelivery::Mixed)
        {
            IrqHandlerRegistry::MixedAdmissionCutoffs cutoffs = {};
            cutoffCaptured = m_Handlers.captureMixedAdmissionCutoffs(
                static_cast<uint8_t>(irq), cutoffs);
            hardAdmissionCutoff = cutoffs.hard;
            threadedAdmissionCutoff = cutoffs.threaded;
        }
        else if (hasThreadedStage)
        {
            cutoffCaptured = m_Handlers.captureAdmissionCutoff(
                static_cast<uint8_t>(irq), threadedAdmissionCutoff);
        }
        else
        {
            // Preserve the ordinary unhandled-vector path for a disabled line
            // which was already accepted by the controller.
            cutoffCaptured = m_Handlers.captureAdmissionCutoff(
                static_cast<uint8_t>(irq), hardAdmissionCutoff);
        }

        if (!cutoffCaptured)
        {
            if (controllerAck != IrqControllerAck::None)
            {
                eoiLocked(irq);
            }
            __atomic_add_fetch(
                hasThreadedStage ? &m_ThreadedPublicationFailures[irq] :
                                   &m_UnhandledIrqCount[irq],
                static_cast<size_t>(1), __ATOMIC_RELAXED);
            publishDiagnosticLineLocked(static_cast<uint8_t>(irq));
            return;
        }

        if (hasThreadedStage)
        {
            // A one-shot threaded policy masks before EOI. Immediate-release
            // policies remain open while the worker runs.
            m_IrqState.beginThreadedDispatch(irq);
            if (lineRelease == IrqLineRelease::AfterThreadedCompletion)
            {
                applyMaskLocked();
            }
            if (controllerAck == IrqControllerAck::BeforeHardStage)
            {
                eoiLocked(irq);
            }
            threadedCookie = advanceThreadedCookieLocked(irq);
            m_ThreadedDispatchGenerations[irq] = dispatchGeneration;
            m_ThreadedHadHardStage[irq] = hasHardStage;
            m_ThreadedHardAdmitted[irq] = false;
            m_ThreadedHardHandled[irq] = false;
            m_Handlers.publishThreadedDispatch(
                irq, threadedCookie, threadedAdmissionCutoff);

            // A mixed line's worker must not overlap the hard callbacks which
            // share its physical occurrence. Its doorbell is rung only after
            // the hard stage below has completed.
            if (!hasHardStage &&
                !m_ThreadedDispatcher.publishFromInterrupt(
                    irq, threadedCookie))
            {
                m_Handlers.cancelThreadedDispatch(irq, threadedCookie);
                m_ThreadedHadHardStage[irq] = false;
                m_ThreadedHardAdmitted[irq] = false;
                m_ThreadedHardHandled[irq] = false;
                __atomic_add_fetch(
                    &m_ThreadedPublicationFailures[irq],
                    static_cast<size_t>(1), __ATOMIC_RELAXED);
            }
            if (!hasHardStage &&
                controllerAck == IrqControllerAck::AfterHardStage)
            {
                eoiLocked(irq);
            }
        }
        else if (controllerAck == IrqControllerAck::BeforeHardStage)
        {
            eoiLocked(irq);
        }
        publishDiagnosticLineLocked(static_cast<uint8_t>(irq));
    }

    if (!hasHardStage)
    {
        return;
    }

    bool bHandled = false;
    const bool admitted = m_Handlers.dispatchHard(
        irq, state, bHandled, nullptr, dispatchGeneration,
        hardAdmissionCutoff);

    {
        LockGuard<Spinlock> guard(m_Lock);
        if (controllerAck == IrqControllerAck::AfterHardStage)
        {
            eoiLocked(irq);
        }

        if (hasThreadedStage)
        {
            const IrqDelivery currentDelivery = m_IrqState.delivery(irq);
            const bool threadedLifetimeCurrent =
                threadedCookie == m_ThreadedCookies[irq] &&
                dispatchGeneration == m_ThreadedDispatchGenerations[irq] &&
                (currentDelivery == IrqDelivery::Threaded ||
                 currentDelivery == IrqDelivery::Mixed);
            if (threadedLifetimeCurrent)
            {
                const bool hardStageQuiesced =
                    currentDelivery == IrqDelivery::Threaded;
                m_ThreadedHardAdmitted[irq] =
                    admitted || hardStageQuiesced;
                m_ThreadedHardHandled[irq] =
                    bHandled || hardStageQuiesced;
                if (!m_ThreadedDispatcher.publishFromInterrupt(
                        irq, threadedCookie))
                {
                    m_Handlers.cancelThreadedDispatch(irq, threadedCookie);
                    m_ThreadedHadHardStage[irq] = false;
                    m_ThreadedHardAdmitted[irq] = false;
                    m_ThreadedHardHandled[irq] = false;
                    __atomic_add_fetch(
                        &m_ThreadedPublicationFailures[irq],
                        static_cast<size_t>(1), __ATOMIC_RELAXED);
                }
            }
            else if (
                dispatchGeneration == m_IrqState.dispatchGeneration(irq) &&
                m_IrqState.hardHandlerCount(irq) &&
                (currentDelivery == IrqDelivery::Hard ||
                 currentDelivery == IrqDelivery::Mixed))
            {
                // The old threaded action was synchronously quiesced. A
                // replacement threaded handler belongs to the next
                // occurrence, but this hard result still needs one terminal
                // decision for the occurrence already in flight.
                const bool threadedStageQuiesced = true;
                const bool aggregateAdmitted =
                    admitted || threadedStageQuiesced;
                const bool aggregateAllowRearm =
                    bHandled || threadedStageQuiesced;
                const bool wasEnabled = m_IrqState.enabled(irq);
                m_IrqState.completeDispatch(
                    irq, dispatchGeneration,
                    aggregateAdmitted && !aggregateAllowRearm);
                if (!aggregateAdmitted || !aggregateAllowRearm)
                {
                    __atomic_add_fetch(
                        &m_UnhandledIrqCount[irq], static_cast<size_t>(1),
                        __ATOMIC_RELAXED);
                }
                if (wasEnabled != m_IrqState.enabled(irq))
                {
                    applyMaskLocked();
                }
            }
            publishDiagnosticLineLocked(static_cast<uint8_t>(irq));
            return;
        }

        const bool wasEnabled = m_IrqState.enabled(irq);
        m_IrqState.completeDispatch(
            irq, dispatchGeneration, admitted && !bHandled);
        if (!admitted || !bHandled)
        {
            __atomic_add_fetch(
                &m_UnhandledIrqCount[irq], static_cast<size_t>(1),
                __ATOMIC_RELAXED);
        }
        if (wasEnabled != m_IrqState.enabled(irq))
        {
            applyMaskLocked();
        }
        publishDiagnosticLineLocked(static_cast<uint8_t>(irq));
    }
}

void Pic::dispatchThreadedLine(void *context, uint8_t irq, size_t cookie)
{
    Pic *pic = reinterpret_cast<Pic *>(context);
    size_t dispatchGeneration = 0;
    {
        LockGuard<Spinlock> guard(pic->m_Lock);
        if (irq >= PicIrqState::LineCount)
        {
            return;
        }
        const IrqDelivery delivery = pic->m_IrqState.delivery(irq);
        if (cookie != pic->m_ThreadedCookies[irq] ||
            (delivery != IrqDelivery::Threaded &&
             delivery != IrqDelivery::Mixed))
        {
            return;
        }
        dispatchGeneration = pic->m_ThreadedDispatchGenerations[irq];
    }

    IrqHandlerRegistry::ThreadedDispatchResult result = {};
    const bool admitted =
        pic->m_Handlers.dispatchThreaded(irq, cookie, result);

    {
        LockGuard<Spinlock> guard(pic->m_Lock);
        const IrqDelivery delivery = pic->m_IrqState.delivery(irq);
        if (cookie != pic->m_ThreadedCookies[irq] ||
            dispatchGeneration != pic->m_ThreadedDispatchGenerations[irq] ||
            (delivery != IrqDelivery::Threaded &&
             delivery != IrqDelivery::Mixed))
        {
            return;
        }

        const bool hadHardStage = pic->m_ThreadedHadHardStage[irq];
        const bool aggregateAdmitted =
            pic->m_ThreadedHardAdmitted[irq] || admitted;
        const bool aggregateAllowRearm =
            pic->m_ThreadedHardHandled[irq] || result.allowRearm;
        const bool wasEnabled = pic->m_IrqState.enabled(irq);
        if (!aggregateAdmitted || !aggregateAllowRearm)
        {
            __atomic_add_fetch(
                &pic->m_UnhandledIrqCount[irq], static_cast<size_t>(1),
                __ATOMIC_RELAXED);
        }
        if (hadHardStage &&
            pic->m_IrqState.lineRelease(irq) ==
                IrqLineRelease::AfterHardStage)
        {
            pic->m_IrqState.completeDispatch(
                irq, dispatchGeneration,
                aggregateAdmitted && !aggregateAllowRearm);
        }
        pic->m_IrqState.completeThreadedDispatch(
            irq, dispatchGeneration,
            aggregateAdmitted && aggregateAllowRearm);
        pic->m_ThreadedHadHardStage[irq] = false;
        pic->m_ThreadedHardAdmitted[irq] = false;
        pic->m_ThreadedHardHandled[irq] = false;
        if (wasEnabled != pic->m_IrqState.enabled(irq))
        {
            pic->applyMaskLocked();
        }
        pic->publishDiagnosticLineLocked(irq);
    }
}

void Pic::eoiLocked(uint8_t irq)
{
    if (irq > 7)
    {
        m_SlavePort.write8(0x60 + (irq - 8), 0);

        // ACK the cascade IRQ (IRQ2).
        m_MasterPort.write8(0x62, 0);
    }
    else
    {
        m_MasterPort.write8(0x60 + irq, 0);
    }
}

void Pic::applyMaskLocked()
{
    m_MasterPort.write8(m_IrqState.masterMask(), 1);
    m_SlavePort.write8(m_IrqState.slaveMask(), 1);
}

void Pic::setEnabledLocked(uint8_t irq, bool enable)
{
    m_IrqState.setEnabled(irq, enable);
    applyMaskLocked();
}

void Pic::enable(uint8_t irq, bool enable)
{
    if (irq >= PicIrqState::LineCount)
    {
        return;
    }

    LockGuard<Spinlock> guard(m_Lock);
    if (m_ShuttingDown && enable)
    {
        return;
    }
    setEnabledLocked(irq, enable);
    publishDiagnosticLineLocked(irq);
}
void Pic::enableAll(bool enable)
{
    LockGuard<Spinlock> guard(m_Lock);
    m_IrqState.setAllEnabled(enable);
    m_MasterPort.write8(m_IrqState.masterMask(), 1);
    m_SlavePort.write8(m_IrqState.slaveMask(), 1);
    publishAllDiagnosticLinesLocked();
}
