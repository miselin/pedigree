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

#ifndef PROCESSOR_DISASSEMBLER_H
#define PROCESSOR_DISASSEMBLER_H

#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/StaticString.h"

/** @addtogroup kernelprocessor
 * @{ */

/**
 * Abstraction of a code disassembler.
 */
class DisassemblerBase
{
  public:
    /**
     * Destructor does nothing.
     */
    virtual ~DisassemblerBase();

    /**
     * Sets the location of the next instruction to be disassembled.
     */
    virtual void setLocation(uintptr_t nLocation) = 0;

    /**
     * Gets the location of the next instruction to be disassembled.
     */
    virtual uintptr_t getLocation() = 0;

    /**
     * Sets the mode of disassembly - 16-bit, 32-bit or 64-bit
     * If a disassembler doesn't support a requested mode, it should
     * return without changing anything.
     * \param nMode Mode - 16, 32 or 64.
     */
    virtual void setMode(size_t nMode) = 0;

    /**
     * Disassembles one instruction and populates the given StaticString
     * with a textual representation.
     */
    virtual void disassemble(LargeStaticString &text) = 0;

  protected:
    DisassemblerBase();
};

/** @} */

// forward-declare the architecture-specific types to avoid circular includes
// breaking the typedefs below
class X86Disassembler;
class MipsDisassembler;
class Arm926EDisassembler;
class PPCDisassembler;

#include "system/kernel/core/processor/x86_common/Disassembler.h"  // IWYU pragma: export
#include "system/kernel/core/processor/mips_common/Disassembler.h"  // IWYU pragma: export
#include "system/kernel/core/processor/arm_common/Disassembler.h"  // IWYU pragma: export
#include "system/kernel/core/processor/ppc_common/Disassembler.h"  // IWYU pragma: export

#if X86_COMMON
typedef X86Disassembler Disassembler;
#elif MIPS_COMMON
typedef MipsDisassembler Disassembler;
#elif ARM926E
typedef Arm926EDisassembler Disassembler;
#elif PPC_COMMON
typedef PPCDisassembler Disassembler;
#elif ARMV7
typedef Arm926EDisassembler Disassembler;
#elif HOSTED
// \todo x86-specific
typedef X86Disassembler Disassembler;
#endif

#endif
