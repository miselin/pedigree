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

#ifndef MACHINE_X86_PS2CONTROLLER_H
#define MACHINE_X86_PS2CONTROLLER_H

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Controller.h"
#include "pedigree/kernel/machine/Ps2CaptureState.h"
#include "pedigree/kernel/machine/SplitIrqHandler.h"
#include "pedigree/kernel/machine/types.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Buffer.h"
#include "pedigree/kernel/utilities/String.h"

class IoBase;

class Ps2Controller : public Controller, private SplitIrqHandler
{
  public:
    Ps2Controller(Controller *pDev);
    Ps2Controller();

    virtual ~Ps2Controller()
    {
    }

    virtual void getName(String &str)
    {
        str.assign("PS/2 Controller", 16);
    }

    virtual void dump(String &str)
    {
        str.assign("PS/2 Controller", 16);
    }

    void initialise();
    /** Starts the split IRQ worker after scheduler initialisation. */
    bool initialise3();
    /** Quiesces both ports and drains the split IRQ worker. */
    void uninitialise();

    /// Send a command with no response or data. A timed-out command is dropped.
    void sendCommand(uint8_t command);
    void sendCommand(uint8_t command, uint8_t data);
    /// Send a command and report its response, or zero if the operation fails.
    uint8_t sendCommandWithResponse(uint8_t command);
    uint8_t sendCommandWithResponse(uint8_t command, uint8_t data);

    /// Send a byte to the first port, dropping it if the controller times out.
    void writeFirstPort(uint8_t byte);
    /// Send a byte to the second port, dropping it on timeout.
    EXPORTED_PUBLIC void writeSecondPort(uint8_t byte);

    /// Reports whether this PS/2 controller has two ports.
    bool hasSecondPort() const;

    /// Enables/disables IRQs for the first or second ports.
    EXPORTED_PUBLIC void setIrqEnable(bool firstEnabled, bool secondEnabled);

    /// Reads a single byte from the PS/2 controller by polling.
    uint8_t readByte();
    uint8_t readByteNonBlock();

    /// Reads a single byte from the first port.
    bool readFirstPort(uint8_t &byte, bool block = true);
    /// Reads a single byte from the second port.
    EXPORTED_PUBLIC bool readSecondPort(uint8_t &byte, bool block = true);

    /// Reports that buffered readers must leave their long-lived loops.
    EXPORTED_PUBLIC bool readsStopping() const
    {
        return m_ReadMode.value() == StoppingReadMode;
    }

    /// Selects trap-safe polling without changing controller IRQ state.
    void setDebugState(bool debugState);

    /// Gets the debug state.
    bool getDebugState() const
    {
        return m_DebugState.active();
    }

  private:
    static constexpr size_t CapturedWork = 1;
    static constexpr size_t RecoveryWork = 2;
    static constexpr size_t PollingReadMode = 0;
    static constexpr size_t BufferedReadMode = 1;
    static constexpr size_t StoppingReadMode = 2;
    static constexpr uint8_t OutputBufferFull = 1;
    static constexpr uint8_t InputBufferFull = 1 << 1;
    static constexpr uint8_t SecondPortData = 1 << 5;

    HardStageDisposition
    hardIrq(irq_id_t number, InterruptState &state, size_t &work) override;
    void threadedIrq(size_t work) override;
    bool quiesceIrqSources() override;
    void rearmIrqSources(size_t work) override;

    bool acquireIoForThread();
    void releaseIo();

    bool sendCommandLocked(uint8_t command);
    bool sendCommandLocked(uint8_t command, uint8_t data);
    bool sendCommandWithResponseLocked(uint8_t command, uint8_t &response);
    bool sendCommandWithResponseLocked(
        uint8_t command, uint8_t data, uint8_t &response);
    bool writeFirstPortLocked(uint8_t byte);
    bool configureIrqEnable(bool firstEnabled, bool secondEnabled);
    bool captureOneLocked();
    void drainCapturedBytes();

    bool waitForReadingLocked();
    bool waitForWritingLocked();

    IoBase *m_pBase;
    bool m_bHasSecondPort;

    Buffer<uint8_t> m_FirstPortBuffer;
    Buffer<uint8_t> m_SecondPortBuffer;

    Atomic<size_t> m_FirstIrqEnabled;
    Atomic<size_t> m_SecondIrqEnabled;

    irq_id_t m_FirstIrqId;
    irq_id_t m_SecondIrqId;

    Ps2DebuggerPollingState m_DebugState;

    uint8_t m_ConfigByte;

    Ps2IoAdmissionGate m_IoGate;
    Ps2CaptureQueue m_CapturedBytes;
    Atomic<size_t> m_ReadMode;
    Atomic<size_t> m_RejectedThreadIo;
    Atomic<size_t> m_HardGateContentions;
    Atomic<size_t> m_CaptureDeferrals;
    Atomic<size_t> m_CaptureDrops;
    Atomic<size_t> m_FirstPortDrops;
    Atomic<size_t> m_SecondPortDrops;
    Atomic<size_t> m_RouteMismatches;
    Atomic<size_t> m_EmptyIrqs;
    bool m_SplitInitialised;
};

#endif
