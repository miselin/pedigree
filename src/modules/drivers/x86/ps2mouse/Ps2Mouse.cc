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

#include "Ps2Mouse.h"

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/InputManager.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/processor/Processor.h"
#include "system/kernel/machine/mach_pc/Ps2Controller.h"

Ps2Mouse::Ps2Mouse(Device *pDev)
    : m_pController(0), m_Buffer(), m_BufferIndex(0), m_BufferLock(),
      m_IrqWait(0)
{
    setSpecificType(String("ps2-mouse"));

    for (size_t i = 0; i < m_nHandlers; ++i)
    {
        m_Handlers[i] = nullptr;
        m_HandlerParams[i] = nullptr;
    }
}

Ps2Mouse::~Ps2Mouse()
{
}

bool Ps2Mouse::initialise(Ps2Controller *pController)
{
    m_pController = pController;

    /// \todo handle errors, resend requests, etc

    // Set up the mouse.
    uint8_t result = 0;
    m_pController->writeSecondPort(SetDefaults);
    m_pController->readSecondPort(result);
    m_pController->writeSecondPort(MouseStream);
    m_pController->readSecondPort(result);

    // Finally, enable IRQs for the mouse
    m_pController->setIrqEnable(true, true);

    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    Thread *pThread = new Thread(pProcess, readerThreadTrampoline, this);
    pThread->detach();

    return true;
}

void Ps2Mouse::write(const char *bytes, size_t len)
{
    for (size_t i = 0; i < len; ++i)
    {
        m_pController->writeSecondPort(bytes[i]);
    }
}

void Ps2Mouse::subscribe(MouseHandlerFunction handler, void *param)
{
    for (size_t i = 0; i < m_nHandlers; ++i)
    {
        if (m_Handlers[i])
        {
            continue;
        }

        m_Handlers[i] = handler;
        m_HandlerParams[i] = param;
        break;
    }
}

void Ps2Mouse::updateSubscribers(const void *buffer, size_t len)
{
    for (size_t i = 0; i < m_nHandlers; ++i)
    {
        if (!m_Handlers[i])
        {
            continue;
        }

        m_Handlers[i](m_HandlerParams[i], buffer, len);
    }
}

int Ps2Mouse::readerThreadTrampoline(void *param)
{
    Ps2Mouse *instance = reinterpret_cast<Ps2Mouse *>(param);
    instance->readerThread();
}

void Ps2Mouse::readerThread()
{
    while (true)
    {
        uint8_t byte;
        if (!m_pController->readSecondPort(byte))
        {
            continue;
        }

        updateSubscribers(&byte, 1);

        if (byte == 0xFA || byte == 0xFE)
        {
            // ignore for now
            continue;
        }

        ssize_t xrel = 0;
        ssize_t yrel = 0;
        uint32_t buttons = 0;
        bool needUpdate = false;
        {
            m_BufferLock.acquire();
            m_Buffer[m_BufferIndex++] = byte;
            needUpdate = m_BufferIndex == 3;
            if (needUpdate)
            {
                xrel = static_cast<ssize_t>(static_cast<int8_t>(m_Buffer[1]));
                yrel = static_cast<ssize_t>(static_cast<int8_t>(m_Buffer[2]));
                buttons = static_cast<uint32_t>(m_Buffer[0]) & 0x3;
                m_BufferIndex = 0;
            }
            m_BufferLock.release();
        }

        // lock no longer taken, safe to send the update
        if (needUpdate)
        {
            InputManager::instance().mouseUpdate(xrel, yrel, 0, buttons);
        }
    }
}
