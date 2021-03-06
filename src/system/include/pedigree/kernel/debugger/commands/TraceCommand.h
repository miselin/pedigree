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

#ifndef TRACECOMMAND_H
#define TRACECOMMAND_H

#include "pedigree/kernel/debugger/Backtrace.h"
#include "pedigree/kernel/debugger/DebuggerCommand.h"
#include "pedigree/kernel/debugger/DebuggerIO.h"
#include "pedigree/kernel/debugger/Scrollable.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/StaticString.h"

/** @addtogroup kerneldebuggercommands
 * @{ */

/**
 * Allows the tracing of an execution path, single stepping and continuing to
 * breakpoints, while displaying a disassembly, the target CPU state and a stack
 * backtrace.
 */
class TraceCommand : public DebuggerCommand
{
  public:
    /**
     * Default constructor - zero's stuff.
     */
    TraceCommand();

    /**
     * Default destructor - does nothing.
     */
    ~TraceCommand();

    /**
     * Return an autocomplete string, given an input string.
     */
    void autocomplete(const HugeStaticString &input, HugeStaticString &output);

    void setInterface(int nInterface);

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
        return NormalStaticString("trace");
    }

    /**
     * Returns >=0 if the debugger should immediately call us. Returns the
     * interface index to use.
     */
    int execTrace()
    {
        return m_nExec;
    }

  private:
    class Disassembly : public Scrollable
    {
      public:
        Disassembly(InterruptState &state);
        ~Disassembly()
        {
        }
        const char *getLine1(
            size_t index, DebuggerIO::Colour &colour,
            DebuggerIO::Colour &bgColour);
        const char *getLine2(
            size_t index, size_t &colOffset, DebuggerIO::Colour &colour,
            DebuggerIO::Colour &bgColour);
        size_t getLineCount();

      private:
        size_t m_nInstructions;
        uintptr_t m_nFirstInstruction;
        uintptr_t m_nIp;
        /**
         * These provide a useful speedup for the disassembly tracer. Because we
         * have to essentially trawl a singly linked list from a known position
         * (symbol start point) to get to any specific instruction, we keep this
         * counter as the last line that was executed, and what the instruction
         * location was for that line.
         */
        size_t m_LastLine;
        uintptr_t m_LastInstructionLocation;
    };

    class Registers : public Scrollable
    {
      public:
        Registers(InterruptState &state);
        ~Registers()
        {
        }
        const char *getLine1(
            size_t index, DebuggerIO::Colour &colour,
            DebuggerIO::Colour &bgColour);
        const char *getLine2(
            size_t index, size_t &colOffset, DebuggerIO::Colour &colour,
            DebuggerIO::Colour &bgColour);
        size_t getLineCount();

      private:
        InterruptState &m_State;
    };

    class Stacktrace : public Scrollable
    {
      public:
        Stacktrace(InterruptState &state);
        ~Stacktrace()
        {
        }
        const char *getLine1(
            size_t index, DebuggerIO::Colour &colour,
            DebuggerIO::Colour &bgColour);
        const char *getLine2(
            size_t index, size_t &colOffset, DebuggerIO::Colour &colour,
            DebuggerIO::Colour &bgColour);
        size_t getLineCount();

      private:
        Backtrace m_Bt;
    };

    void drawBackground(size_t nCols, size_t nLines, DebuggerIO *pScreen);

    /**
     * Should the debugger immediately call our execute function? and what
     * interface should it use?
     */
    int m_nExec;
    int m_nInterface;
};

/** @} */

#endif
