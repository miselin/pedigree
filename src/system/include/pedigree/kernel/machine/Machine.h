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

#ifndef MACHINE_MACHINE_H
#define MACHINE_MACHINE_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"

class IrqManager;
class Keyboard;
class SchedulerTimer;
class Serial;
class Timer;
class Vga;

/**
 * This is an abstraction on a machine, or board.
 * It provides functions to retrieve Timers, Serial controllers,
 * VGA controllers, Ethernet controllers etc, without having to
 * know the exact implementation required or memory map.
 * It also provides a "probe" function, which will attempt to detect
 * if a machine is present.
 */
class EXPORTED_PUBLIC Machine
{
    friend void system_reset();

  public:
    static Machine &instance();

    /**
     * Initialises the machine.
     */
    virtual void initialise() = 0;
    /// Called after debugger startup.
    virtual void initialise2()
    {
    }
    /// Called after processor startup - for thread creation etc
    virtual void initialise3()
    {
    }
    virtual void deinitialise()
    {
        m_bInitialised = false;
    }
    inline bool isInitialised()
    {
        return m_bInitialised;
    }

    /**
     * Initialises the machine's base device tree, if one exists, to prefill
     * the tree with those devices that are not otherwise able to be detected
     * via some sort of bus support.
     */
    virtual void initialiseDeviceTree()
    {
    }

    /**
     * Returns the n'th Serial device.
     */
    virtual Serial *getSerial(size_t n) = 0;

    /**
     * Returns the number of Serial device.
     */
    virtual size_t getNumSerial() = 0;

    /**
     * Returns the n'th VGA device.
     */
    virtual Vga *getVga(size_t n) = 0;

    /**
     * Returns the number of VGA devices.
     */
    virtual size_t getNumVga() = 0;

    virtual IrqManager *getIrqManager() = 0;
    /**
     * Returns the SchedulerTimer device.
     */
    virtual SchedulerTimer *getSchedulerTimer() = 0;

    /**
     * Returns the n'th Timer device.
     */
    virtual Timer *getTimer() = 0;

    /**
     * Returns the keyboard device.
     */
    virtual Keyboard *getKeyboard() = 0;

    /**
     * Sets the keyboard device.
     */
    virtual void setKeyboard(Keyboard *kb) = 0;

    /**
     * Stops all other cores. This is used during debugger initialisation.
     */
    virtual void stopAllOtherProcessors();

  protected:
    inline Machine() : m_bInitialised(false)
    {
    }
    virtual ~Machine();

    bool m_bInitialised;

  private:
    Machine(const Machine &);
    Machine &operator=(const Machine &);
};

#endif
