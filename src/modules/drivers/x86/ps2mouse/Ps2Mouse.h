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

#ifndef _PS2_MOUSE_H
#define _PS2_MOUSE_H

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/String.h"

class Ps2Controller;

extern class Ps2Mouse *g_Ps2Mouse WEAK;

class Ps2Mouse : public Device
{
  public:
    typedef void (*MouseHandlerFunction)(void *, const void *, size_t);

    Ps2Mouse(Device *pDev);
    virtual ~Ps2Mouse();

    virtual bool initialise(Ps2Controller *pController);

    virtual void getName(String &str)
    {
        str.assign("mouse", 6);
    }

    EXPORTED_PUBLIC void write(const char *bytes, size_t len);

    // subscribe to the raw bus protocol
    EXPORTED_PUBLIC void subscribe(MouseHandlerFunction handler, void *param);

  private:
    Ps2Controller *m_pController;

    enum WaitType
    {
        Data,
        Signal
    };

    enum Ps2Ports
    {
        KbdStat = 0x64,
        KbdCommand = 0x60
    };

    enum Ps2Commands
    {
        EnablePS2 = 0xA8,
        DisableKbd = 0xAD,
        EnableKbd = 0xAE,
        Mouse = 0xD4,
        MouseStream = 0xF4,
        MouseDisable = 0xF5,
        SetDefaults = 0xF6,
        MouseAck = 0xFA
    };

    static int readerThreadTrampoline(void *) NORETURN;
    void readerThread() NORETURN;

    void updateSubscribers(const void *buffer, size_t len);

    /// Mouse data buffer
    uint8_t m_Buffer[3];

    /// Index into the data buffer
    size_t m_BufferIndex;

    /// Lock for the mouse data buffer
    Spinlock m_BufferLock;

    /// IRQ wait semaphore
    Semaphore m_IrqWait;

    Ps2Mouse(const Ps2Mouse &);
    void operator=(const Ps2Mouse &);

    static const size_t m_nHandlers = 32;
    MouseHandlerFunction m_Handlers[m_nHandlers];
    void *m_HandlerParams[m_nHandlers];
};

#endif
