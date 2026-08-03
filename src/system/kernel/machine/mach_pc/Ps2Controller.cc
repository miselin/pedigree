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
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/assert.h"

static constexpr size_t Ps2IoPollLimit = 1000000;

Ps2Controller::Ps2Controller(Controller *pDev)
    : Controller(pDev),
      SplitIrqHandler(MakeConstantString("PS/2 controller bottom half")),
      m_pBase(nullptr), m_bHasSecondPort(false), m_FirstPortBuffer(16384),
      m_SecondPortBuffer(16384), m_FirstIrqEnabled(0), m_SecondIrqEnabled(0),
      m_FirstIrqId(0), m_SecondIrqId(0), m_DebugState(0), m_ConfigByte(0),
      m_bDebugStateFirstIrqEnabled(false), m_bDebugStateSecondIrqEnabled(false),
      m_IoGate(), m_CapturedBytes(), m_ReadMode(PollingReadMode),
      m_RejectedThreadIo(0), m_HardGateContentions(0), m_CaptureDeferrals(0),
      m_CaptureDrops(0), m_FirstPortDrops(0), m_SecondPortDrops(0),
      m_RouteMismatches(0), m_EmptyIrqs(0), m_SplitInitialised(false)
{
}

Ps2Controller::Ps2Controller()
    : Controller(),
      SplitIrqHandler(MakeConstantString("PS/2 controller bottom half")),
      m_pBase(nullptr), m_bHasSecondPort(false), m_FirstPortBuffer(16384),
      m_SecondPortBuffer(16384), m_FirstIrqEnabled(0), m_SecondIrqEnabled(0),
      m_FirstIrqId(0), m_SecondIrqId(0), m_DebugState(0), m_ConfigByte(0),
      m_bDebugStateFirstIrqEnabled(false), m_bDebugStateSecondIrqEnabled(false),
      m_IoGate(), m_CapturedBytes(), m_ReadMode(PollingReadMode),
      m_RejectedThreadIo(0), m_HardGateContentions(0), m_CaptureDeferrals(0),
      m_CaptureDrops(0), m_FirstPortDrops(0), m_SecondPortDrops(0),
      m_RouteMismatches(0), m_EmptyIrqs(0), m_SplitInitialised(false)
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
    readByteNonBlock();  // clear output buffer

    TRACE("PS2: disabling IRQs");
    m_ConfigByte = sendCommandWithResponse(0x20);
    m_ConfigByte = (m_ConfigByte & ~0x3) |
                   0x40;  // disable IRQs, leave translation enabled
    sendCommand(0x60, m_ConfigByte);

    m_bHasSecondPort = (m_ConfigByte & (1 << 5)) != 0;

    TRACE("PS2: performing self-test");
    uint8_t selfTestResponse = sendCommandWithResponse(0xAA);
    NOTICE("PS/2: self-test response: " << Hex << selfTestResponse);

    // Some 8042 implementations reset their config during self-test. Restore
    // our cached IRQ-disabled/translation state before either port is enabled.
    sendCommand(0x60, m_ConfigByte);

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

    TRACE("PS2: startup complete");
}

bool Ps2Controller::initialise3()
{
    if (m_SplitInitialised || !initialiseSplitIrq())
    {
        return false;
    }
    m_SplitInitialised = true;

    m_FirstPortBuffer.wipe();
    m_SecondPortBuffer.wipe();
    m_FirstPortBuffer.enableWrites();
    m_SecondPortBuffer.enableWrites();
    m_CapturedBytes.reset();
    m_RejectedThreadIo = 0;
    m_HardGateContentions = 0;
    m_CaptureDeferrals = 0;
    m_CaptureDrops = 0;
    m_FirstPortDrops = 0;
    m_SecondPortDrops = 0;
    m_RouteMismatches = 0;
    m_EmptyIrqs = 0;

    IrqManager &irqManager = *Machine::instance().getIrqManager();
    m_FirstIrqId = registerIsaSplitIrq(irqManager, 1, IrqPolicy::edgeHard());
    m_SecondIrqId = registerIsaSplitIrq(irqManager, 12, IrqPolicy::edgeHard());
    if (!m_FirstIrqId || !m_SecondIrqId)
    {
        if (!shutdownSplitIrq())
        {
            FATAL("PS/2 could not stop a partially registered split IRQ");
        }
        m_FirstIrqId = 0;
        m_SecondIrqId = 0;
        m_SplitInitialised = false;
        m_ReadMode = StoppingReadMode;
        m_FirstPortBuffer.disableWrites();
        m_SecondPortBuffer.disableWrites();
        return false;
    }

    irqManager.control(1, IrqManager::MitigationThreshold, 100);
    irqManager.control(12, IrqManager::MitigationThreshold, 100);
    m_ReadMode = BufferedReadMode;
    return true;
}

void Ps2Controller::uninitialise()
{
    if (m_SplitInitialised && !shutdownSplitIrq())
    {
        FATAL("PS/2 teardown could not drain its split IRQ worker");
    }
    m_FirstIrqId = 0;
    m_SecondIrqId = 0;
    m_SplitInitialised = false;

    // No producer remains. Publish the terminal read mode before waking blocked
    // readers so none can fall back to polling during teardown.
    m_ReadMode = StoppingReadMode;
    m_FirstPortBuffer.disableWrites();
    m_SecondPortBuffer.disableWrites();
}

bool Ps2Controller::acquireIoForThread()
{
    if (Processor::inDeviceHardIrq())
    {
        m_RejectedThreadIo += 1;
        return false;
    }

    while (!m_IoGate.tryAcquire())
    {
#if THREADS
        Thread *current = Processor::information().getCurrentThread();
        if (!current || !Processor::getInterrupts())
        {
            m_RejectedThreadIo += 1;
            return false;
        }
#if HOSTED
        if (current->getHostedSignalDepth())
        {
            m_RejectedThreadIo += 1;
            return false;
        }
#endif
        // The current owner may be a preempted thread. Yielding lets it finish
        // without turning controller configuration into a same-core spin.
        Scheduler::instance().yield();
#else
        m_RejectedThreadIo += 1;
        return false;
#endif
    }
    return true;
}

void Ps2Controller::releaseIo()
{
    m_IoGate.release();
}

bool Ps2Controller::sendCommandLocked(uint8_t command)
{
    if (!waitForWritingLocked())
        return false;
    m_pBase->write8(command, 4);
    return true;
}

bool Ps2Controller::sendCommandLocked(uint8_t command, uint8_t data)
{
    if (!sendCommandLocked(command) || !waitForWritingLocked())
        return false;
    m_pBase->write8(data, 0);
    return true;
}

bool Ps2Controller::sendCommandWithResponseLocked(
    uint8_t command, uint8_t &response)
{
    if (!sendCommandLocked(command) || !waitForReadingLocked())
        return false;
    response = m_pBase->read8(0);
    return true;
}

bool Ps2Controller::sendCommandWithResponseLocked(
    uint8_t command, uint8_t data, uint8_t &response)
{
    if (!sendCommandLocked(command, data) || !waitForReadingLocked())
        return false;
    response = m_pBase->read8(0);
    return true;
}

bool Ps2Controller::writeFirstPortLocked(uint8_t byte)
{
    if (!waitForWritingLocked())
        return false;
    m_pBase->write8(byte, 0);
    return true;
}

void Ps2Controller::sendCommand(uint8_t command)
{
    if (!acquireIoForThread())
    {
        return;
    }
    (void) sendCommandLocked(command);
    releaseIo();
}

void Ps2Controller::sendCommand(uint8_t command, uint8_t data)
{
    if (!acquireIoForThread())
    {
        return;
    }
    (void) sendCommandLocked(command, data);
    releaseIo();
}

uint8_t Ps2Controller::sendCommandWithResponse(uint8_t command)
{
    if (!acquireIoForThread())
    {
        return 0;
    }
    uint8_t response = 0;
    (void) sendCommandWithResponseLocked(command, response);
    releaseIo();
    return response;
}

uint8_t Ps2Controller::sendCommandWithResponse(uint8_t command, uint8_t data)
{
    if (!acquireIoForThread())
    {
        return 0;
    }
    uint8_t response = 0;
    (void) sendCommandWithResponseLocked(command, data, response);
    releaseIo();
    return response;
}

void Ps2Controller::writeFirstPort(uint8_t byte)
{
    if (!acquireIoForThread())
    {
        return;
    }
    (void) writeFirstPortLocked(byte);
    releaseIo();
}

void Ps2Controller::writeSecondPort(uint8_t byte)
{
    if (!acquireIoForThread())
    {
        return;
    }
    (void) sendCommandLocked(0xD4, byte);
    releaseIo();
}

bool Ps2Controller::hasSecondPort() const
{
    return m_bHasSecondPort;
}

void Ps2Controller::setIrqEnable(bool firstEnabled, bool secondEnabled)
{
    if (m_DebugState.value())
    {
        m_bDebugStateFirstIrqEnabled = firstEnabled;
        m_bDebugStateSecondIrqEnabled = secondEnabled;
        return;
    }

    configureIrqEnable(firstEnabled, secondEnabled);
}

bool Ps2Controller::configureIrqEnable(bool firstEnabled, bool secondEnabled)
{
    if (!acquireIoForThread())
    {
        return false;
    }

    IrqManager &irqManager = *Machine::instance().getIrqManager();

    // Keep the gate across both the PIC and 8042 transitions. A hard callback
    // which arrives on another CPU fails admission once and publishes recovery.
    m_FirstIrqEnabled = 0;
    m_SecondIrqEnabled = 0;
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

    NOTICE("Old config byte: " << Hex << m_ConfigByte);
    m_ConfigByte |= flagAdd;
    m_ConfigByte &= flagRemove;
    NOTICE("New config byte: " << Hex << m_ConfigByte);
    if (!sendCommandLocked(0x60, m_ConfigByte))
    {
        releaseIo();
        return false;
    }
    NOTICE("completed!");

    // re-enable now that we're done here
    m_FirstIrqEnabled = firstEnabled ? 1 : 0;
    irqManager.enable(1, firstEnabled);
    m_SecondIrqEnabled = secondEnabled ? 1 : 0;
    irqManager.enable(12, secondEnabled);
    releaseIo();
    return true;
}

uint8_t Ps2Controller::readByte()
{
    if (m_DebugState.value())
    {
        // KDB may have interrupted the current gate owner and may still carry
        // the hard-IRQ marker. A single probe is safe; waiting is not.
        return readByteNonBlock();
    }

    if (!acquireIoForThread())
    {
        return 0;
    }
    if (!waitForReadingLocked())
    {
        releaseIo();
        return 0;
    }
    const uint8_t result = m_pBase->read8();
    releaseIo();
    return result;
}

uint8_t Ps2Controller::readByteNonBlock()
{
    if (!m_IoGate.tryAcquire())
    {
        return 0;
    }
    if ((m_pBase->read8(4) & 1) == 0)
    {
        releaseIo();
        return 0;
    }
    const uint8_t result = m_pBase->read8();
    releaseIo();
    return result;
}

bool Ps2Controller::readFirstPort(uint8_t &byte, bool block)
{
    if (m_DebugState.value())
    {
        byte = readByteNonBlock();
        return byte != 0;
    }

    const size_t readMode = m_ReadMode.value();
    if (readMode == StoppingReadMode)
    {
        return false;
    }
    if (readMode == PollingReadMode)
    {
        byte = readByte();
        return true;
    }

    size_t numRead = m_FirstPortBuffer.read(&byte, 1, block);
    return numRead > 0;
}

bool Ps2Controller::readSecondPort(uint8_t &byte, bool block)
{
    if (m_DebugState.value())
    {
        byte = readByteNonBlock();
        return byte != 0;
    }

    const size_t readMode = m_ReadMode.value();
    if (readMode == StoppingReadMode)
    {
        return false;
    }
    if (readMode == PollingReadMode)
    {
        byte = readByte();
        return true;
    }

    size_t numRead = m_SecondPortBuffer.read(&byte, 1, block);
    return numRead > 0;
}

void Ps2Controller::setDebugState(bool debugState)
{
    if (debugState == (m_DebugState.value() != 0))
    {
        return;
    }

    // The debugger can interrupt a thread which owns m_IoGate. It must never
    // wait for that frozen owner or alter the owner's in-flight 8042 protocol.
    // Only the PIC masks and software polling mode change here.
    IrqManager &irqManager = *Machine::instance().getIrqManager();
    if (debugState)
    {
        m_bDebugStateFirstIrqEnabled = m_FirstIrqEnabled.value() != 0;
        m_bDebugStateSecondIrqEnabled = m_SecondIrqEnabled.value() != 0;
        m_DebugState = 1;
        m_FirstIrqEnabled = 0;
        m_SecondIrqEnabled = 0;
        irqManager.enable(1, false);
        irqManager.enable(12, false);
    }
    else
    {
        m_FirstIrqEnabled = m_bDebugStateFirstIrqEnabled ? 1 : 0;
        m_SecondIrqEnabled = m_bDebugStateSecondIrqEnabled ? 1 : 0;
        m_DebugState = 0;
        irqManager.enable(1, m_bDebugStateFirstIrqEnabled);
        irqManager.enable(12, m_bDebugStateSecondIrqEnabled);
    }
}

SplitIrqHandler::HardIrqDisposition
Ps2Controller::hardIrq(irq_id_t number, InterruptState &state, size_t &work)
{
    (void) state;
    if (m_DebugState.value())
    {
        return HardIrqDisposition::Handled;
    }

    // Never wait behind controller configuration in hard context. The worker
    // polls the shared output register after the interrupted owner releases it.
    if (!m_IoGate.tryAcquire())
    {
        m_HardGateContentions += 1;
        work = RecoveryWork;
        return HardIrqDisposition::Deferred;
    }

    const uint8_t status = m_pBase->read8(4);
    if (!(status & OutputBufferFull))
    {
        releaseIo();
        m_EmptyIrqs += 1;
        return HardIrqDisposition::Handled;
    }

    if (!m_CapturedBytes.hasCapacity())
    {
        // Leave port 0x60 untouched. The worker first drains the fixed queue,
        // then polls this still-latched byte under ordinary thread admission.
        releaseIo();
        m_CaptureDeferrals += 1;
        work = RecoveryWork;
        return HardIrqDisposition::Deferred;
    }

    const uint8_t received = m_pBase->read8(0);
    const bool secondPort = (status & SecondPortData) != 0;
    if (secondPort != (number == 12))
    {
        m_RouteMismatches += 1;
    }
    if (!m_CapturedBytes.tryPush(Ps2CapturedByte(received, secondPort)))
    {
        m_CaptureDrops += 1;
    }
    releaseIo();

    work = CapturedWork;
    return HardIrqDisposition::Deferred;
}

bool Ps2Controller::captureOneLocked()
{
    const uint8_t status = m_pBase->read8(4);
    if (!(status & OutputBufferFull))
    {
        return false;
    }

    if (!m_CapturedBytes.hasCapacity())
    {
        m_CaptureDeferrals += 1;
        return false;
    }

    const Ps2CapturedByte record(
        m_pBase->read8(0), (status & SecondPortData) != 0);
    if (!m_CapturedBytes.tryPush(record))
    {
        m_CaptureDrops += 1;
    }
    return true;
}

void Ps2Controller::drainCapturedBytes()
{
    Ps2CapturedByte record;
    while (m_CapturedBytes.pop(record))
    {
        Buffer<uint8_t> &destination =
            record.secondPort ? m_SecondPortBuffer : m_FirstPortBuffer;
        if (destination.write(&record.value, 1, false) != 1)
        {
            if (record.secondPort)
            {
                m_SecondPortDrops += 1;
            }
            else
            {
                m_FirstPortDrops += 1;
            }
        }
    }
}

void Ps2Controller::threadedIrq(size_t work)
{
    drainCapturedBytes();
    if (work & RecoveryWork)
    {
        if (acquireIoForThread())
        {
            captureOneLocked();
            releaseIo();
        }
        else
        {
            return;
        }
    }
    drainCapturedBytes();
}

bool Ps2Controller::quiesceIrqSources()
{
    return configureIrqEnable(false, false);
}

void Ps2Controller::rearmIrqSources(size_t work)
{
    (void) work;
    // The edge-triggered 8042 source was quiesced by reading port 0x60.
}

bool Ps2Controller::waitForReadingLocked()
{
    // Wait for the controller's output buffer to fill.
    for (size_t poll = 0; poll < Ps2IoPollLimit; ++poll)
    {
        if (m_pBase->read8(4) & OutputBufferFull)
            return true;
        Processor::pause();
    }
    if (m_pBase->read8(4) & OutputBufferFull)
        return true;
    ERROR("PS/2 controller output buffer did not fill before timeout");
    return false;
}

bool Ps2Controller::waitForWritingLocked()
{
    // Wait for the controller's input buffer to empty.
    for (size_t poll = 0; poll < Ps2IoPollLimit; ++poll)
    {
        if (!(m_pBase->read8(4) & InputBufferFull))
            return true;
        Processor::pause();
    }
    if (!(m_pBase->read8(4) & InputBufferFull))
        return true;
    ERROR("PS/2 controller input buffer did not empty before timeout");
    return false;
}
