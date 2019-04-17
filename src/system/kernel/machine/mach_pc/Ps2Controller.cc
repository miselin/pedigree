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

#include "Ps2Controller.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Controller.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Trace.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/assert.h"

Ps2Controller::Ps2Controller(Controller *pDev)
    : Controller(pDev), m_pBase(0), m_bHasSecondPort(false),
      m_FirstPortBuffer(16384), m_SecondPortBuffer(16384),
      m_bFirstIrqEnabled(false), m_bSecondIrqEnabled(false), m_ConfigByte(0),
      m_bDebugStateFirstIrqEnabled(false), m_bDebugStateSecondIrqEnabled(false)
{
}

Ps2Controller::Ps2Controller()
    : m_FirstPortBuffer(16384), m_SecondPortBuffer(16384)
{
}

void Ps2Controller::initialise()
{
    TRACE("PS2 Controller startup");
    m_pBase = addresses()[0]->m_Io;
    assert(m_pBase);

    TRACE("PS2: disabling devices");
    sendCommand(0xAD);  // disable all devices
    sendCommand(0xA7);
    m_pBase->read8(0);  // clear output buffer

    TRACE("PS2: disabling IRQs");
    m_ConfigByte = sendCommandWithResponse(0x20);
    m_ConfigByte = (m_ConfigByte & ~0x3) |
                   0x40;  // disable IRQs, leave translation enabled
    sendCommand(0x60, m_ConfigByte);

    m_bHasSecondPort = (m_ConfigByte & (1 << 5)) != 0;

    TRACE("PS2: performing self-test");
    uint8_t selfTestResponse = sendCommandWithResponse(0xAA);
    NOTICE("PS/2: self-test response: " << Hex << selfTestResponse);

    // Enable both ports.
    TRACE("PS2: enabling ports");
    sendCommand(0xAE);
    sendCommand(0xA8);

    // Reset all devices.
    TRACE("PS2: resetting first device");
    writeFirstPort(0xFF);
    uint8_t ack, status;
    readFirstPort(ack);
    readFirstPort(status);
    NOTICE("PS/2: first port reset result: " << Hex << ack << ", " << status);

    TRACE("PS2: resetting second device");
    writeSecondPort(0xFF);
    uint8_t extra = 0;
    readSecondPort(ack);
    readSecondPort(status);
    readSecondPort(extra);
    NOTICE(
        "PS/2: second port reset result: " << Hex << ack << ", " << status
                                           << ", " << extra);

    IrqManager &irqManager = *Machine::instance().getIrqManager();
    m_FirstIrqId = irqManager.registerIsaIrqHandler(1, this, true);
    if (m_FirstIrqId == 0)
    {
        ERROR("PS/2: failed to register first IRQ handler!");
    }

    m_SecondIrqId = irqManager.registerIsaIrqHandler(12, this, true);
    if (m_SecondIrqId == 0)
    {
        ERROR("PS/2: failed to register second IRQ handler!");
    }

    irqManager.control(1, IrqManager::MitigationThreshold, 100);
    irqManager.control(12, IrqManager::MitigationThreshold, 100);

    TRACE("PS2: startup complete");
}

void Ps2Controller::sendCommand(uint8_t command)
{
    waitForWriting();
    m_pBase->write8(command, 4);
}

void Ps2Controller::sendCommand(uint8_t command, uint8_t data)
{
    sendCommand(command);

    waitForWriting();
    m_pBase->write8(data, 0);
}

uint8_t Ps2Controller::sendCommandWithResponse(uint8_t command)
{
    sendCommand(command);

    /// \todo handle this when we have irqs enabled
    waitForReading();
    return m_pBase->read8(0);
}

uint8_t Ps2Controller::sendCommandWithResponse(uint8_t command, uint8_t data)
{
    sendCommand(command, data);

    /// \todo handle this when we have irqs enabled
    waitForReading();
    return m_pBase->read8(0);
}

void Ps2Controller::writeFirstPort(uint8_t byte)
{
    waitForWriting();
    m_pBase->write8(byte, 0);
}

void Ps2Controller::writeSecondPort(uint8_t byte)
{
    sendCommand(0xD4, byte);
}

bool Ps2Controller::hasSecondPort() const
{
    return m_bHasSecondPort;
}

void Ps2Controller::setIrqEnable(bool firstEnabled, bool secondEnabled)
{
    IrqManager &irqManager = *Machine::instance().getIrqManager();

    // disable IRQs while we do this - polling
    m_bFirstIrqEnabled = false;
    m_bSecondIrqEnabled = false;
    irqManager.enable(1, false);
    irqManager.enable(12, false);

    // Never accidentally remove translation
    uint8_t flagAdd = 0x40, flagRemove = ~0;
    if (firstEnabled)
    {
        flagAdd |= 1;
    }
    else
    {
        flagRemove &= ~1;
    }
    if (secondEnabled)
    {
        flagAdd |= 2;
    }
    else
    {
        flagRemove &= ~2;
    }

    m_ConfigByte = sendCommandWithResponse(0x20);
    NOTICE("Old config byte: " << Hex << m_ConfigByte);
    m_ConfigByte |= flagAdd;
    m_ConfigByte &= flagRemove;
    NOTICE("New config byte: " << Hex << m_ConfigByte);
    sendCommand(0x60, m_ConfigByte);
    NOTICE("completed!");

    // re-enable now that we're done here
    m_bFirstIrqEnabled = firstEnabled;
    irqManager.enable(1, firstEnabled);
    m_bSecondIrqEnabled = secondEnabled;
    irqManager.enable(12, secondEnabled);
}

uint8_t Ps2Controller::readByte()
{
    waitForReading();
    return m_pBase->read8();
}

uint8_t Ps2Controller::readByteNonBlock()
{
    if ((m_pBase->read8(4) & 1) == 0)
    {
        return 0;
    }
    return m_pBase->read8();
}

bool Ps2Controller::readFirstPort(uint8_t &byte, bool block)
{
    if (!m_bFirstIrqEnabled)
    {
        // fall back to polling
        byte = readByte();
        return true;
    }

    size_t numRead = m_FirstPortBuffer.read(&byte, 1, block);
    return numRead > 0;
}

bool Ps2Controller::readSecondPort(uint8_t &byte, bool block)
{
    if (!m_bSecondIrqEnabled)
    {
        // fall back to polling
        byte = readByte();
        return true;
    }

    size_t numRead = m_SecondPortBuffer.read(&byte, 1, block);
    return numRead > 0;
}

void Ps2Controller::setDebugState(bool debugState)
{
    m_bDebugState = debugState;

    // block IRQs if going into debug state
    IrqManager &irqManager = *Machine::instance().getIrqManager();
    if (m_bDebugState)
    {
        irqManager.enable(1, false);
        irqManager.enable(12, false);

        m_bDebugStateFirstIrqEnabled = m_bFirstIrqEnabled;
        m_bDebugStateSecondIrqEnabled = m_bSecondIrqEnabled;

        // force using polling for setIrqEnable
        m_bFirstIrqEnabled = false;
        m_bSecondIrqEnabled = false;

        setIrqEnable(false, false);

        // disable mouse reports
        if (m_bDebugStateSecondIrqEnabled)
        {
            uint8_t x;
            writeSecondPort(0xF5);
            readSecondPort(x);
        }
    }
    else
    {
        setIrqEnable(
            m_bDebugStateFirstIrqEnabled, m_bDebugStateSecondIrqEnabled);

        // re-enable mouse reports
        if (m_bDebugStateSecondIrqEnabled)
        {
            uint8_t x;
            writeSecondPort(0xF4);
            readSecondPort(x);
        }
    }
}

bool Ps2Controller::irq(irq_id_t number, InterruptState &state)
{
    if (m_bDebugState)
    {
        return true;
    }

    if ((m_pBase->read8(4) & 1) == 0)
    {
        ERROR("PS/2: IRQ #" << number << " with no pending data");
        return true;
    }

    uint8_t received = readByte();
    bool ok = false;
    size_t numWritten = 0;
    if (number == 1)
    {
        if (m_bFirstIrqEnabled)
        {
            m_FirstPortBuffer.write(&received, 1, false);
            ok = true;
        }
    }
    else
    {
        if (m_bSecondIrqEnabled)
        {
            m_SecondPortBuffer.write(&received, 1, false);
            ok = true;
        }
    }

    if (!ok)
    {
        ERROR("PS/2: unexpected IRQ #" << number);
    }

#if VERBOSE_KERNEL
    if (ok && !numWritten)
    {
        ERROR(
            "PS/2: dropping byte " << Hex << received
                                   << " from device, not enough buffer space");
    }
#endif

    return true;
}

void Ps2Controller::waitForReading()
{
    // wait for controller's output buffer to empty
    while ((m_pBase->read8(4) & 1) == 0)
        ;
}

void Ps2Controller::waitForWriting()
{
    // wait for controller's input buffer to fill
    while (m_pBase->read8(4) & 2)
        ;
}
