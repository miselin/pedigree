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

#ifndef KERNEL_PROCESSOR_PPC_COMMON_PROCESSOR_H
#define KERNEL_PROCESSOR_PPC_COMMON_PROCESSOR_H

#include "pedigree/kernel/processor/Processor.h"

#define MSR_POW 0x00040000
#define MSR_ILE 0x00010000
#define MSR_EE 0x00008000
#define MSR_PR 0x00004000
#define MSR_FP 0x00002000
#define MSR_ME 0x00001000
#define MSR_FE0 0x00000800
#define MSR_SE 0x00000400
#define MSR_BE 0x00000200
#define MSR_FE1 0x00000100
#define MSR_IP 0x00000040
#define MSR_IR 0x00000020
#define MSR_DR 0x00000010
#define MSR_RI 0x00000002
#define MSR_LE 0x00000001

class PPCCommonProcessor : public ProcessorBase
{
public:
    static void
    setSegmentRegisters(uint32_t segmentBase, bool supervisorKey, bool userKey);
};

class PPCProcessor : public PPCCommonProcessor
{
};

#endif
