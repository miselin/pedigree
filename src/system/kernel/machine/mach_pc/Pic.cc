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
Pic::registerIsaIrqHandler(uint8_t irq, IrqHandler *handler, bool bEdge)
{
    // Threaded line dispatch is added by the manager-owned worker checkpoint.
    return 0;
}

irq_id_t
Pic::registerHardIsaIrqHandler(uint8_t irq, HardIrqHandler *handler, bool bEdge)
{
    if (UNLIKELY(irq >= PicIrqState::LineCount || !handler))
        return 0;

    LockGuard<Spinlock> guard(m_Lock);
    if (!m_IrqState.canRegister(irq, bEdge))
    {
        ERROR(
            "PIC: IRQ " << Dec << irq
                        << " was registered with incompatible trigger modes");
        return 0;
    }
    if (!m_Handlers.registerHardHandler(irq, handler))
        return 0;

    m_IrqState.handlerRegistered(irq, bEdge);
    applyMaskLocked();

    return irq + BASE_INTERRUPT_VECTOR;
}
irq_id_t Pic::registerPciIrqHandler(IrqHandler *handler, Device *pDevice)
{
    // Threaded line dispatch is added by the manager-owned worker checkpoint.
    return 0;
}

irq_id_t
Pic::registerHardPciIrqHandler(HardIrqHandler *handler, Device *pDevice)
{
    if (UNLIKELY(!pDevice))
        return 0;
    irq_id_t irq = pDevice->getInterruptNumber();
    if (UNLIKELY(irq >= PicIrqState::LineCount || !handler))
        return 0;

    LockGuard<Spinlock> guard(m_Lock);
    if (!m_IrqState.canRegister(irq, false))
    {
        ERROR(
            "PIC: PCI IRQ " << Dec << irq
                            << " conflicts with an edge-triggered handler");
        return 0;
    }
    if (!m_Handlers.registerHardHandler(irq, handler))
        return 0;

    m_IrqState.handlerRegistered(irq, false);
    applyMaskLocked();

    return irq + BASE_INTERRUPT_VECTOR;
}
bool Pic::unregisterHandler(irq_id_t Id, IrqHandlerBase *handler)
{
    if (Id < BASE_INTERRUPT_VECTOR ||
        Id >= BASE_INTERRUPT_VECTOR + PicIrqState::LineCount || !handler)
        return false;

    uint8_t irq = Id - BASE_INTERRUPT_VECTOR;

    const IrqHandlerRegistry::UnregisterResult result =
        m_Handlers.unregisterHandler(irq, handler);
    if (result == IrqHandlerRegistry::UnregisterResult::Completed ||
        result == IrqHandlerRegistry::UnregisterResult::Deferred)
    {
        LockGuard<Spinlock> guard(m_Lock);
        m_IrqState.handlerUnregistered(irq);
        if (!m_IrqState.handlerCount(irq))
        {
            applyMaskLocked();
        }
    }

    if (result == IrqHandlerRegistry::UnregisterResult::Rejected)
    {
        ERROR(
            "PIC: rejected an IRQ removal which could not synchronously "
            "drain");
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
        m_IrqCount[i] = 0;
        m_MitigatedIrqs[i] = false;
        m_MitigationThreshold[i] = DEFAULT_IRQ_MITIGATE_THRESHOLD;
    }

    // Disable all IRQ's (exept IRQ2)
    enableAll(false);

    return true;
}

Pic::Pic()
    : m_SlavePort("PIC #2"), m_MasterPort("PIC #1"), m_Handlers(), m_IrqState(),
      m_Lock(false)
{
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

    bool edgeTriggered = false;
    size_t dispatchGeneration = 0;
    {
        LockGuard<Spinlock> guard(m_Lock);
        ++m_IrqCount[irq];
        edgeTriggered = m_IrqState.edgeTriggered(irq);

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
            ERROR("PIC: spurious IRQ" << Dec << irq << Hex);
            return;
        }

        dispatchGeneration = m_IrqState.beginDispatch(irq);
        if (edgeTriggered)
        {
            eoiLocked(irq);
        }
    }

    bool bHandled = false;
    const bool admitted = m_Handlers.dispatchHard(irq, state, bHandled);

    {
        LockGuard<Spinlock> guard(m_Lock);
        const bool wasEnabled = m_IrqState.enabled(irq);
        m_IrqState.completeDispatch(
            irq, dispatchGeneration, admitted && !bHandled);
        if (wasEnabled != m_IrqState.enabled(irq))
        {
            applyMaskLocked();
        }

        if (!edgeTriggered)
        {
            eoiLocked(irq);
        }
    }

    if (!admitted)
        NOTICE("PIC: unhandled irq #" << irq << " occurred");
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
    setEnabledLocked(irq, enable);
}
void Pic::enableAll(bool enable)
{
    LockGuard<Spinlock> guard(m_Lock);
    m_IrqState.setAllEnabled(enable);
    m_MasterPort.write8(m_IrqState.masterMask(), 1);
    m_SlavePort.write8(m_IrqState.slaveMask(), 1);
}
