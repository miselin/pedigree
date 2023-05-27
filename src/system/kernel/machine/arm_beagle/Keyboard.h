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

#ifndef MACHINE_ARMBEAGLE_KEYBOARD_H
#define MACHINE_ARMBEAGLE_KEYBOARD_H

#include "pedigree/kernel/machine/Keyboard.h"
#include "pedigree/kernel/processor/types.h"

/// \note No actual attached keyboard apart from one on USB perhaps - so this
///       is completely stubbed.
class ArmBeagleKeyboard : public Keyboard
{
  public:
    ArmBeagleKeyboard();
    virtual ~ArmBeagleKeyboard();

    /**
     * Initialises the device.
     */
    virtual void initialise()
    {
    }

    /**
     * Retrieves a character from the keyboard. Blocking I/O.
     * \return The character recieved or zero if it is a character
     *         without an ascii representation.
     */
    virtual char getChar()
    {
        return 0;
    }

    /**
     * Retrieves a character from the keyboard. Non blocking I/O.
     * \return The character recieved or zero if it is a character
     *         without an ascii representation, or zero also if no
     *         character was present.
     */
    virtual char getCharNonBlock()
    {
        return 0;
    }

    /**
     * \return True if shift is currently held.
     */
    virtual bool shift()
    {
        return false;
    }

    /**
     * \return True if ctrl is currently held.
     */
    virtual bool ctrl()
    {
        return false;
    }

    /**
     * \return True if alt is currently held.
     */
    virtual bool alt()
    {
        return false;
    }

    /**
     * \return True if caps lock is currently on.
     */
    virtual bool capsLock()
    {
        return false;
    }

    virtual void setDebugState(bool enableDebugState){};
    virtual bool getDebugState()
    {
        return false;
    };

    typedef void (*KeyPressedCallback)(uint64_t);
    virtual void registerCallback(KeyPressedCallback callback){};

    virtual uint64_t getCharacter()
    {
        return 0;
    };
    virtual uint64_t getCharacterNonBlock()
    {
        return 0;
    };
};

#endif
