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

#ifndef LOGVIEWER_H
#define LOGVIEWER_H

/** @addtogroup kerneldebuggercommands
 * @{ */

#include "pedigree/kernel/debugger/DebuggerCommand.h"
#include "pedigree/kernel/debugger/DebuggerIO.h"
#include "pedigree/kernel/debugger/Scrollable.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/StaticString.h"

/**
 * Debugger command that allows viewing of the kernel log.
 */
class LogViewer : public DebuggerCommand, public Scrollable
{
  public:
    /**
     * Default constructor - zero's stuff.
     */
    LogViewer();

    /**
     * Default destructor - does nothing.
     */
    ~LogViewer();

    /**
     * Return an autocomplete string, given an input string.
     */
    void autocomplete(const HugeStaticString &input, HugeStaticString &output);

    /**
     * Execute the command with the given screen.
     */
    bool execute(
        const HugeStaticString &input, HugeStaticString &output,
        InterruptState &state, DebuggerIO *screen);

    /**
     * Returns the string representation of this command.
     */
    const NormalStaticString getString()
    {
        return NormalStaticString("log");
    }

    //
    // Scrollable interface
    //
    virtual const char *getLine1(
        size_t index, DebuggerIO::Colour &colour, DebuggerIO::Colour &bgColour);
    virtual const char *getLine2(
        size_t index, size_t &colOffset, DebuggerIO::Colour &colour,
        DebuggerIO::Colour &bgColour);
    virtual size_t getLineCount();
};

/** @} */

#endif
