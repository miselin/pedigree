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
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/InputManager.h"
#include "pedigree/kernel/machine/HidInputManager.h"
#include "pedigree/kernel/machine/KeymapManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Trace.h"

#ifdef DEBUGGER
#include "pedigree/kernel/debugger/Debugger.h"

#ifdef TRACK_PAGE_ALLOCATIONS
#include "pedigree/kernel/debugger/commands/AllocationCommand.h"
#endif
#endif

#ifdef DEBUGGER
#include "pedigree/kernel/debugger/commands/SlamCommand.h"
#endif

#include "pedigree/kernel/core/SlamAllocator.h"

#ifdef MEMORY_TRACING
extern void toggleTracingAllocations();
#endif

X86Keyboard::X86Keyboard(uint32_t portBase)
    : m_bDebugState(false), m_Escape(KeymapManager::EscapeNone), m_pBase(0),
      m_IrqId(0), m_LedState(0)
{
}

X86Keyboard::~X86Keyboard()
{
}

void X86Keyboard::initialise()
{
    auto f = [this](Device *p) {
        if (m_pBase)
        {
            return p;
        }

        if (p->addresses().count() > 0)
        {
            if (p->addresses()[0]->m_Name == "ps2-base")
            {
                m_pBase = p->addresses()[0]->m_Io;
            }
        }

        return p;
    };
    auto c = pedigree_std::make_callable(f);
    Device::foreach (c, 0);

    if (!m_pBase)
    {
        // Handle the impossible case properly
        FATAL("X86Keyboard: Could not find the PS/2 base ports in the device "
              "tree");
    }

    // Now that we have the base, initialize the PS/2 controller and keyboard

    TRACE("PS/2: disable all devices #1");
    m_pBase->write8(0xAD, 4);  // disable all devices
    TRACE("PS/2: disable all devices #2");
    m_pBase->write8(0xA7, 4);
    TRACE("PS/2: clear output buffer");
    m_pBase->read8(0);  // clear output buffer
    TRACE("PS/2: read config byte");
    m_pBase->write8(0x20, 4);
    waitForReadable();
    uint8_t configByte = m_pBase->read8(0);
    configByte |= ~0x43;  // disable IRQs & translation
    TRACE("PS/2: write new config byte");
    m_pBase->write8(0x60, 4);
    TRACE("PS/2: waiting for writeable");
    waitForWriteable();
    TRACE("PS/2: now writing the new config byte");
    m_pBase->write8(configByte, 0);

    /// \todo check bit 5 in config byte to make sure we have a dual-channel ps2 controller

    TRACE("PS/2: perform self-test");
    m_pBase->write8(0xAA, 4);
    waitForReadable();
    uint8_t selfTestResponse = m_pBase->read8(0);
    NOTICE("PS/2 self-test response: " << Hex << selfTestResponse);

    // finally enable the ports
    TRACE("PS/2: enable first port");
    m_pBase->write8(0xAE, 4);
    TRACE("PS/2: enable second port");
    m_pBase->write8(0xA8, 4);

    TRACE("PS/2: read config byte #2");
    m_pBase->write8(0x20, 4);
    waitForReadable();
    configByte = m_pBase->read8(0);
    configByte |= 1;  // enable IRQ for first port
    TRACE("PS/2: write config byte to enable IRQs for first port");
    m_pBase->write8(0x60, 4);
    waitForWriteable();
    m_pBase->write8(configByte, 0);

    // reset all devices
    // response should arrive via IRQ
    TRACE("PS/2: performing device reset");
    m_pBase->write8(0xFF, 4);

    // Register the irq
    IrqManager &irqManager = *Machine::instance().getIrqManager();
    m_IrqId = irqManager.registerIsaIrqHandler(1, this, true);
    if (m_IrqId == 0)
    {
        ERROR("X86Keyboard: failed to register IRQ handler!");
    }

    irqManager.control(1, IrqManager::MitigationThreshold, 100);
}

bool X86Keyboard::irq(irq_id_t number, InterruptState &state)
{
    if (m_bDebugState)
        return true;

    // Get the keyboard's status byte
    uint8_t status = m_pBase->read8(4);
    NOTICE("keyboard: status = " << Hex << status);
    if (!(status & 0x01))
        return true;

    // Get the scancode for the pending keystroke
    uint8_t scancode = m_pBase->read8(0);
    NOTICE("keyboard: scancode = " << Hex << scancode);
    if (scancode == 0xFA)
    {
        // just an ack
        return true;
    }
    else if (scancode == 0xFE)
    {
        // potentially need to resend but we have no way to do that here
        WARNING("PS/2 keyboard requests a resend, but this is not implemented.");
        return true;
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

        return true;
    }
    if (scancode == 0x58)  // F12
    {
        LargeStaticString sError;
        sError += "User-induced breakpoint";
        Debugger::instance().start(state, sError);
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

    InputManager::instance().machineKeyUpdate(scancode & 0x7F, scancode & 0x80);

    // Get the HID keycode corresponding to the scancode
    uint8_t keyCode =
        KeymapManager::instance().convertPc102ScancodeToHidKeycode(
            scancode, m_Escape);
    if (!keyCode)
        return true;

    // Send the keycode to the HID input manager
    if (scancode & 0x80)
        HidInputManager::instance().keyUp(keyCode);
    else
        HidInputManager::instance().keyDown(keyCode);
    return true;
}

char X86Keyboard::getChar()
{
    if (m_bDebugState)
    {
        uint8_t scancode, status;
        do
        {
            // Get the keyboard's status byte
            status = m_pBase->read8(4);
        } while (!(status & 0x01));  // Spin until there's a key ready

        // Get the scancode for the pending keystroke
        scancode = m_pBase->read8(0);

        // Convert the scancode into ASCII and return it
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
    if (m_bDebugState)
    {
        uint8_t scancode, status;
        // Get the keyboard's status byte
        status = m_pBase->read8(4);
        if (!(status & 0x01))
            return 0;

        // Get the scancode for the pending keystroke
        scancode = m_pBase->read8(0);

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
    m_bDebugState = enableDebugState;
    IrqManager &irqManager = *Machine::instance().getIrqManager();
    if (m_bDebugState)
    {
        irqManager.enable(1, false);

        // Disable the PS/2 mouse
        m_pBase->write8(0xD4, 4);
        m_pBase->write8(0xF5, 4);
    }
    else
    {
        irqManager.enable(1, true);

        // Enable the PS/2 mouse
        m_pBase->write8(0xD4, 4);
        m_pBase->write8(0xF4, 4);
    }
}

bool X86Keyboard::getDebugState()
{
    return m_bDebugState;
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

    while (m_pBase->read8(4) & 2)
        ;
    m_pBase->write8(0xED, 0);
    while (m_pBase->read8(4) & 2)
        ;
    m_pBase->write8(state, 0);
}

void X86Keyboard::waitForReadable()
{
    while ((m_pBase->read8(4) & 1) == 0)
        ;
}

void X86Keyboard::waitForWriteable()
{
    while (m_pBase->read8(4) & 2)
        ;
}
