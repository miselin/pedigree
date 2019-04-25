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

#ifndef KERNEL_PROCESSOR_ARMV7_PROCESSOR_H
#define KERNEL_PROCESSOR_ARMV7_PROCESSOR_H

#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/arm_common/Processor.h"

class ARMv7Processor : public ARMCommonProcessor
{
public:
    /** Read TTBR0 */
    static physical_uintptr_t readTTBR0();
    /** Read TTBR1 */
    static physical_uintptr_t readTTBR1();
    /** Read TTBCR */
    static physical_uintptr_t readTTBCR();
    /** Write TTBR0 */
    static void writeTTBR0(physical_uintptr_t value);
    /** Write TTBR1 */
    static void writeTTBR1(physical_uintptr_t value);
    /** Write TTBCR */
    static void writeTTBCR(physical_uintptr_t value);
};

#endif
