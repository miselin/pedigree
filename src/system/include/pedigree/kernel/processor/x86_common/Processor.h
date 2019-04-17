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

#ifndef KERNEL_PROCESSOR_X86_COMMON_PROCESSOR_H
#define KERNEL_PROCESSOR_X86_COMMON_PROCESSOR_H

#include "pedigree/kernel/processor/Processor.h"

/** Common x86 processor functionality. Cast needed to get these. */
class X86CommonProcessor : public ProcessorBase
{
public:
    /** Read a Machine/Model-specific register
     *\param[in] index the register index
     *\return the value of the register */
    static uint64_t readMachineSpecificRegister(uint32_t index);
    /** Write a Machine/Model-specific register
     *\param[in] index the register index
     *\param[in] value the new value of the register */
    static void writeMachineSpecificRegister(uint32_t index, uint64_t value);
    /** Executes the CPUID machine instruction
     *\param[in] inEax eax before the CPUID instruction
     *\param[in] inEcx ecx before the CPUID instruction
     *\param[out] eax eax afterwards
     *\param[out] ebx ebx afterwards
     *\param[out] ecx ecx afterwards
     *\param[out] edx edx afterwards */
    static void cpuid(
        uint32_t inEax, uint32_t inEcx, uint32_t &eax, uint32_t &ebx,
        uint32_t &ecx, uint32_t &edx);

    static physical_uintptr_t readCr3();
};

#endif
