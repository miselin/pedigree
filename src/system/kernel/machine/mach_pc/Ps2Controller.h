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

#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Controller.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Buffer.h"

class Ps2Controller : public Controller, private IrqHandler
{
  public:
    Ps2Controller(Controller *pDev);
    Ps2Controller();

    virtual ~Ps2Controller()
    {
    }

    virtual void getName(String &str)
    {
        str = "PS/2 Controller";
    }

    virtual void dump(String &str)
    {
        str = "PS/2 Controller";
    }

    void initialise();

    /// Send a command to the PS/2 controller that has no response or data.
    void sendCommand(uint8_t command);
    void sendCommand(uint8_t command, uint8_t data);
    /// Send a command to the PS/2 controller and report its response.
    uint8_t sendCommandWithResponse(uint8_t command);
    uint8_t sendCommandWithResponse(uint8_t command, uint8_t data);

    /// Send a byte to the first port of the PS/2 controller.
    void writeFirstPort(uint8_t byte);
    /// Send a byte to the second port of the PS/2 controller.
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

    /// Sets the debug state (blocks IRQs to allow polling).
    void setDebugState(bool debugState);

    /// Gets the debug state.
    bool getDebugState() const
    {
        return m_bDebugState;
    }

   private:
    virtual bool irq(irq_id_t number, InterruptState &state);

    void waitForReading();
    void waitForWriting();

    IoBase *m_pBase;
    bool m_bHasSecondPort;

    Buffer<uint8_t> m_FirstPortBuffer;
    Buffer<uint8_t> m_SecondPortBuffer;

    bool m_bFirstIrqEnabled;
    bool m_bSecondIrqEnabled;

    irq_id_t m_FirstIrqId;
    irq_id_t m_SecondIrqId;

    bool m_bDebugState;

    uint8_t m_ConfigByte;

    // used to know which IRQs to enable when leaving debug state
    bool m_bDebugStateFirstIrqEnabled;
    bool m_bDebugStateSecondIrqEnabled;
};

#endif
