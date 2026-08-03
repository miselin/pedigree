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

#ifndef MACHINE_KEYBOARD_H
#define MACHINE_KEYBOARD_H

/**
 * Keyboard device abstraction.
 */
class Keyboard
{
  public:
    enum KeyFlags
    {
        Special = 1ULL << 63,
        Ctrl = 1ULL << 62,
        Shift = 1ULL << 61,
        Alt = 1ULL << 60,
        AltGr = 1ULL << 59
    };

    /// Bit numbers follow the same format as the PS/2 keyboard LED byte
    enum KeyboardLeds
    {
        /// Scroll Lock LED
        ScrollLock = 1 << 0,

        /// Number Lock LED
        NumLock = 1 << 1,

        /// Caps Lock LED
        CapsLock = 1 << 2,

        /// LEDs 1-5 cover non-standard LEDs that might be present on some
        /// keyboards - very vendor-specific.
        Led1 = 1 << 3,
        Led2 = 1 << 4,
        Led3 = 1 << 5,
        Led4 = 1 << 6,
        Led5 = 1 << 7,
    };

    Keyboard();
    virtual ~Keyboard();

    /**
     * Initialises the device.
     */
    virtual void initialise() = 0;

    /**
     * Selects polling input while the kernel debugger owns the processor.
     *
     * A debugger trap can interrupt code which owns arbitrary device or IRQ
     * controller locks. Both this setter and getDebugState() must therefore
     * access this mode without taking locks, waiting, allocating, logging,
     * changing interrupt masks, or starting a device protocol. The debugger
     * trap itself prevents ordinary interrupt delivery while this mode is
     * active.
     */
    virtual void setDebugState(bool enableDebugState) = 0;
    virtual bool getDebugState() = 0;

    /**
     * Retrieves a character from the keyboard. Blocking I/O.
     * If DebugState is false this returns zero.
     * If DebugState is true this returns the next character received, or zero
     * if the character is non-ASCII.
     */
    virtual char getChar() = 0;

    /**
     * Retrieves a character from the keyboard. Non blocking I/O.
     * If DebugState is false this returns zero.
     * If DebugState is true this returns the next character received, or zero
     * if the character is non-ASCII.
     */
    virtual char getCharNonBlock() = 0;

    /**
     * Gets the current state of the LEDs on the keyboard.
     * A single byte bitmap is returned with flags from KeyboardLeds
     * identifying which LEDs are on or off.
     */
    virtual char getLedState();

    /**
     * Sets the current state of LEDs on the keyboard.
     * If a keyboard does not have any LEDs this is essentially a no-op.
     */
    virtual void setLedState(char state);
};

#endif
