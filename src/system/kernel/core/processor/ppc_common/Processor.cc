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

#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/Log.h"

void Processor::breakpoint()
{
    asm volatile("trap");
}

void Processor::halt()
{
    // TODO: gcc will most certainly optimize this away in -O1/2/3 so please
    //       replace it with some unoptimizable mighty magic
    for (;;)
        ;
}

void Processor::invalidate(void *pAddress)
{
    asm volatile("tlbie %0" : : "r"(pAddress));
}

void Processor::setSegmentRegisters(
    uint32_t segmentBase, bool supervisorKey, bool userKey)
{
    uint32_t segs[16];
    for (int i = 0; i < 16; i++)
    {
        segs[i] = 0;
        if (supervisorKey)
            segs[i] |= 0x40000000;
        if (userKey)
            segs[i] |= 0x20000000;
        segs[i] |= (segmentBase + i) & 0x00FFFFFF;
    }
    asm volatile("mtsr 0, %0" : : "r"(segs[0]));
    asm volatile("mtsr 1, %0" : : "r"(segs[1]));
    asm volatile("mtsr 2, %0" : : "r"(segs[2]));
    asm volatile("mtsr 3, %0" : : "r"(segs[3]));
    asm volatile("mtsr 4, %0" : : "r"(segs[4]));
    asm volatile("mtsr 5, %0" : : "r"(segs[5]));
    asm volatile("mtsr 6, %0" : : "r"(segs[6]));
    asm volatile("mtsr 7, %0" : : "r"(segs[7]));
    // Don't set kernel regs!
    // asm volatile("mtsr 8, %0" : :"r"(segs[8]));
    // asm volatile("mtsr 9, %0" : :"r"(segs[9]));
    // asm volatile("mtsr 10, %0" : :"r"(segs[10]));
    // asm volatile("mtsr 11, %0" : :"r"(segs[11]));
    // asm volatile("mtsr 12, %0" : :"r"(segs[12]));
    // asm volatile("mtsr 13, %0" : :"r"(segs[13]));
    // asm volatile("mtsr 14, %0" : :"r"(segs[14]));
    // asm volatile("mtsr 15, %0" : :"r"(segs[15]));
    asm volatile("sync");
}

size_t Processor::getDebugBreakpointCount()
{
    return 1;
}

uintptr_t Processor::getDebugBreakpoint(
    size_t nBpNumber, DebugFlags::FaultType &nFaultType, size_t &nLength,
    bool &bEnabled)
{
    return 0;
}

void Processor::enableDebugBreakpoint(
    size_t nBpNumber, uintptr_t nLinearAddress,
    DebugFlags::FaultType nFaultType, size_t nLength)
{
}

void Processor::disableDebugBreakpoint(size_t nBpNumber)
{
}

void Processor::setInterrupts(bool bEnable)
{
    asm volatile("sync; isync;");
    uint32_t msr;
    asm volatile("mfmsr %0" : "=r"(msr));
    if (bEnable)
        msr |= MSR_EE;
    else
        msr &= ~MSR_EE;
    asm volatile("mtmsr %0" : : "r"(msr));
    asm volatile("sync; isync;");
}

void Processor::setSingleStep(bool bEnable, InterruptState &state)
{
    if (bEnable)
        state.m_Srr1 |= MSR_SE;
    else
        state.m_Srr1 &= ~MSR_SE;
}

void Processor::invalidateICache(uintptr_t nAddr)
{
    asm volatile("icbi 0, %0" : : "r"(nAddr));
}

void Processor::invalidateDCache(uintptr_t nAddr)
{
    asm volatile("dcbi 0, %0" : : "r"(nAddr));
}

void Processor::flushDCache(uintptr_t nAddr)
{
    asm volatile("dcbst 0, %0" : : "r"(nAddr));
}

void Processor::flushDCacheAndInvalidateICache(
    uintptr_t startAddr, uintptr_t endAddr)
{
    for (uintptr_t i = startAddr; i < endAddr; i += 4)
        flushDCache(i);

    asm volatile("sync");

    for (uintptr_t i = startAddr; i < endAddr; i += 4)
        invalidateICache(i);

    asm volatile("sync");
    asm volatile("isync");
}
