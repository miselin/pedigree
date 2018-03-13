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

#ifndef MACHINE_X86_KEYBOARD_H
#define MACHINE_X86_KEYBOARD_H

#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Keyboard.h"
#include "pedigree/kernel/machine/KeymapManager.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/processor/IoPort.h"
#include "pedigree/kernel/processor/types.h"

#define BUFLEN 256

class Ps2Controller;

/**
 * Keyboard device implementation
 */
class X86Keyboard : public Keyboard
{
  public:
    X86Keyboard(Ps2Controller *controller);
    virtual ~X86Keyboard();

    /// Initialises the device
    virtual void initialise();

    virtual void setDebugState(bool enableDebugState);
    virtual bool getDebugState();

    virtual char getChar();
    virtual char getCharNonBlock();

    virtual char getLedState();
    virtual void setLedState(char state);

    void startReaderThread();

  private:
    static int readerThreadTrampoline(void *) NORETURN;
    void readerThread() NORETURN;

    /// Converts a scancode into an ASCII character (for use in debug state)
    char scancodeToAscii(uint8_t scancode);

    /// Parent PS/2 controller
    Ps2Controller *m_pPs2Controller;

    /// The current escape state
    KeymapManager::EscapeState m_Escape;

    /// IRQ id
    irq_id_t m_IrqId;

    /// Current LED state
    char m_LedState;
};

#endif
