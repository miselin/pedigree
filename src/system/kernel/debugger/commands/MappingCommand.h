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

#ifndef MAPPINGCOMMAND_H
#define MAPPINGCOMMAND_H

#include <DebuggerCommand.h>

/** @addtogroup kerneldebuggercommands
 * @{ */

/**
 * Calculates effective addresses and physical addresses.
 */
class MappingCommand : public DebuggerCommand
{
public:
    /**
     * Default constructor - zeroes stuff.
     */
    MappingCommand();

    /**
     * Default destructor - does nothing.
     */
    ~MappingCommand();

    /**
     * Return an autocomplete string, given an input string.
     */
    void autocomplete(const HugeStaticString &input, HugeStaticString &output);

    /**
     * Execute the command with the given screen.
     */
    bool execute(const HugeStaticString &input, HugeStaticString &output, InterruptState &state, DebuggerIO *screen);
  
    /**
     * Returns the string representation of this command.
     */
    const NormalStaticString getString()
    {
        return NormalStaticString("mapping");
    }

};

/** @} */
#endif
