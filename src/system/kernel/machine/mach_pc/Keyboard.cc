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

#include "Keyboard.h"
#include "Ps2Controller.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/HidInputManager.h"
#include "pedigree/kernel/machine/InputManager.h"
#include "pedigree/kernel/machine/KeymapManager.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/new"

#ifdef DEBUGGER
#ifdef TRACK_PAGE_ALLOCATIONS
#include "pedigree/kernel/debugger/commands/AllocationCommand.h"
#endif

#include "pedigree/kernel/core/SlamAllocator.h"
#include "pedigree/kernel/debugger/commands/SlamCommand.h"
#endif

class Process;

#ifdef MEMORY_TRACING
extern void toggleTracingAllocations();
#endif

X86Keyboard::X86Keyboard(Ps2Controller *controller)
    : m_pPs2Controller(controller), m_Escape(KeymapManager::EscapeNone),
      m_IrqId(0), m_LedState(0)
{
}

X86Keyboard::~X86Keyboard()
{
}

void X86Keyboard::initialise()
{
    /// \todo do we need to switch into a specific scancode set?

    // enable data stream
    uint8_t result = 0;
    m_pPs2Controller->writeFirstPort(0xF4);
    m_pPs2Controller->readFirstPort(result);
    NOTICE("X86Keyboard: 'enable stream' response: " << Hex << result);
}

char X86Keyboard::getChar()
{
    if (m_pPs2Controller->getDebugState())
    {
        // Convert the scancode into ASCII and return it
        uint8_t scancode = m_pPs2Controller->readByte();
        return scancodeToAscii(scancode);
    }
    else
    {
        ERROR("Keyboard::getChar() should not be called outside debug mode");
        return 0;
    }
}

char X86Keyboard::getCharNonBlock()
{
    if (m_pPs2Controller->getDebugState())
    {
        uint8_t scancode = m_pPs2Controller->readByteNonBlock();
        if (!scancode)
        {
            return 0;
        }

        // Convert the scancode into ASCII and return it
        return scancodeToAscii(scancode);
    }
    else
    {
        ERROR("Keyboard::getCharNonBlock should not be called outside debug "
              "mode");
        return 0;
    }
}

void X86Keyboard::setDebugState(bool enableDebugState)
{
    m_pPs2Controller->setDebugState(enableDebugState);
}

bool X86Keyboard::getDebugState()
{
    return m_pPs2Controller->getDebugState();
}

char X86Keyboard::scancodeToAscii(uint8_t scancode)
{
    // Get the HID keycode corresponding to the scancode
    uint8_t keyCode =
        KeymapManager::instance().convertPc102ScancodeToHidKeycode(
            scancode, m_Escape);
    if (!keyCode)
        return 0;

    uint64_t key = 0;
    // Let KeymapManager handle the modifiers
    if (!KeymapManager::instance().handleHidModifier(
            keyCode, !(scancode & 0x80)) &&
        !(scancode & 0x80))
        key = KeymapManager::instance().resolveHidKeycode(
            keyCode);  // Get the actual key

    // Check for special keys
    if (key & Keyboard::Special)
    {
        char a = key & 0xFF, b = (key >> 8) & 0xFF, c = (key >> 16) & 0xFF,
             d = (key >> 24) & 0xFF;
        // Up
        if (a == 'u' && b == 'p')
            return 'j';
        // Down
        if (a == 'd' && b == 'o' && c == 'w' && d == 'n')
            return 'k';
        // PageUp
        if (a == 'p' && b == 'g' && c == 'u' && d == 'p')
            return '\b';
        // PageDown
        if (a == 'p' && b == 'g' && c == 'd' && d == 'n')
            return ' ';
    }
    // Discard non-ASCII keys
    if ((key & Keyboard::Special) || ((key & 0xFFFFFFFF) > 0x7f))
        return 0;
    return static_cast<char>(key & 0x7f);
}

char X86Keyboard::getLedState()
{
    return m_LedState;
}

void X86Keyboard::setLedState(char state)
{
    m_LedState = state;

    m_pPs2Controller->writeFirstPort(0xED);
    m_pPs2Controller->writeFirstPort(state);

    uint8_t response = 0;
    if (m_pPs2Controller->readFirstPort(response))
    {
        NOTICE("X86Keyboard: setLedState response: " << Hex << response);
    }
    else
    {
        ERROR("X86Keyboard: failed to read response in setLedState");
    }
}

void X86Keyboard::startReaderThread()
{
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    Thread *pThread = new Thread(pProcess, readerThreadTrampoline, this);
    pThread->detach();

    // Now that we're listening - enable IRQs from the keyboard
    m_pPs2Controller->setIrqEnable(true, false);
}

int X86Keyboard::readerThreadTrampoline(void *param)
{
    X86Keyboard *instance = reinterpret_cast<X86Keyboard *>(param);
    instance->readerThread();
}

void X86Keyboard::readerThread()
{
    while (true)
    {
        uint8_t scancode;
        if (!m_pPs2Controller->readFirstPort(scancode))
        {
            continue;
        }
        if (scancode == 0xFA || scancode == 0xFE)
        {
            // ignore for now
            continue;
        }

        // Check for keys with special functions
#ifdef DEBUGGER
#if CRIPPLINGLY_VIGILANT
        if (scancode == 0x43)  // F9
            SlamAllocator::instance().setVigilance(true);
        if (scancode == 0x44)  // F10
            SlamAllocator::instance().setVigilance(false);
#endif
        if (scancode == 0x57)  // F11
        {
#ifdef MEMORY_TRACING
            WARNING("Toggling allocation tracing.");
            toggleTracingAllocations();
#else
#ifdef TRACK_PAGE_ALLOCATIONS
            g_AllocationCommand.checkpoint();
#endif
            g_SlamCommand.clean();
#endif

            continue;
        }
        if (scancode == 0x58)  // F12
        {
            FATAL("User-induced breakpoint.");
        }
#endif

        // Check for LED manipulation
        if (scancode & 0x80)
        {
            uint8_t code = scancode & ~0x80;
            if (code == 0x3A)
            {
                DEBUG_LOG("X86Keyboard: Caps Lock toggled");
                m_LedState ^= Keyboard::CapsLock;
                setLedState(m_LedState);
            }
            else if (code == 0x45)
            {
                DEBUG_LOG("X86Keyboard: Num Lock toggled");
                m_LedState ^= Keyboard::NumLock;
                setLedState(m_LedState);
            }
            else if (code == 0x46)
            {
                DEBUG_LOG("X86Keyboard: Scroll Lock toggled");
                m_LedState ^= Keyboard::ScrollLock;
                setLedState(m_LedState);
            }
        }

        InputManager::instance().machineKeyUpdate(
            scancode & 0x7F, scancode & 0x80);

        // Get the HID keycode corresponding to the scancode
        uint8_t keyCode =
            KeymapManager::instance().convertPc102ScancodeToHidKeycode(
                scancode, m_Escape);
        if (!keyCode)
        {
            ERROR(
                "X86Keyboard: failed to translate scancode " << Hex
                                                             << scancode);
            continue;
        }

        // Send the keycode to the HID input manager
        if (scancode & 0x80)
            HidInputManager::instance().keyUp(keyCode);
        else
            HidInputManager::instance().keyDown(keyCode);
    }
}
