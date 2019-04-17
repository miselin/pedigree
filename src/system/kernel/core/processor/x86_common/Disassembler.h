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

#ifndef X86_COMMON_DISASSEMBLER_H
#define X86_COMMON_DISASSEMBLER_H

#include "pedigree/kernel/debugger/libudis86/udis86.h"
#include "pedigree/kernel/processor/Disassembler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/StaticString.h"

/**
 * A disassembler for x86/x64 processors.
 */
class X86Disassembler : public DisassemblerBase
{
  public:
    X86Disassembler();
    ~X86Disassembler();

    /**
     * Sets the location of the next instruction to be disassembled.
     */
    void setLocation(uintptr_t location);

    /**
     * Gets the location of the next instruction to be disassembled.
     */
    uintptr_t getLocation();

    /**
     * Sets the mode of disassembly - 16-bit, 32-bit or 64-bit
     * If a disassembler doesn't support a requested mode, it should
     * return without changing anything.
     * \param mode Mode - 16, 32 or 64.
     */
    void setMode(size_t mode);

    /**
     * Disassembles one instruction and populates the given StaticString
     * with a textual representation.
     */
    void disassemble(LargeStaticString &text);

  private:
    /**
     * Current disassembling location in memory.
     */
    uintptr_t m_Location;

    /**
     * Current mode.
     */
    int m_Mode;

    /**
     * Disassembler object.
     */
    ud_t m_Obj;
};

#endif
