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
#include "PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/processor/x86_common/ProcessorInformation.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/BootstrapInfo.h"

#if defined(X86)
#include "../x86/VirtualAddressSpace.h"
#else
#include "../x64/VirtualAddressSpace.h"
#endif

#include <machine/mach_pc/LocalApic.h>
#include <machine/mach_pc/Pc.h>

void Processor::initialisationDone()
{
    // Don't allow the bootstrap info to be used anymore - we're killing it here
    g_pBootstrapInfo = nullptr;

/// \todo there HAS to be a better way than this
#if defined(X86)
    // Unmap the identity mapping of the first MBs
    X86VirtualAddressSpace &KernelAddressSpace =
        static_cast<X86VirtualAddressSpace &>(
            VirtualAddressSpace::getKernelAddressSpace());
    *reinterpret_cast<uint32_t *>(KernelAddressSpace.m_PhysicalPageDirectory) =
        0;
    invalidate(0);
#else
    // Unmap the identity mapping of the first MBs
    X64VirtualAddressSpace &KernelAddressSpace =
        static_cast<X64VirtualAddressSpace &>(
            VirtualAddressSpace::getKernelAddressSpace());
    *reinterpret_cast<uint64_t *>(KernelAddressSpace.m_PhysicalPML4) = 0;
    invalidate(0);
#endif

    X86CommonPhysicalMemoryManager::instance().initialisationDone();
}

size_t Processor::getDebugBreakpointCount()
{
    return 4;
}

uintptr_t Processor::getDebugBreakpoint(
    size_t nBpNumber, DebugFlags::FaultType &nFaultType, size_t &nLength,
    bool &bEnabled)
{
    uintptr_t nLinearAddress = 0;
    switch (nBpNumber)
    {
        case 0:
            asm volatile("mov %%db0, %0" : "=r"(nLinearAddress));
            break;
        case 1:
            asm volatile("mov %%db1, %0" : "=r"(nLinearAddress));
            break;
        case 2:
            asm volatile("mov %%db2, %0" : "=r"(nLinearAddress));
            break;
        case 3:
            asm volatile("mov %%db3, %0" : "=r"(nLinearAddress));
            break;
    }

    uintptr_t nStatus;
    asm volatile("mov %%db7, %0" : "=r"(nStatus));

    bEnabled = static_cast<bool>(
        nStatus & (1 << (nBpNumber * 2 + 1)));  // See intel manual 3b.
    nFaultType = static_cast<DebugFlags::FaultType>(
        (nStatus >> (nBpNumber * 4 + 16)) & 0x3);
    switch ((nStatus >> (nBpNumber * 4 + 18)) & 0x3)
    {
        case 0:
            nLength = 1;
            break;
        case 1:
            nLength = 2;
            break;
        case 2:
            nLength = 8;
            break;
        case 3:
            nLength = 4;
            break;
    }

    return nLinearAddress;
}

void Processor::enableDebugBreakpoint(
    size_t nBpNumber, uintptr_t nLinearAddress,
    DebugFlags::FaultType nFaultType, size_t nLength)
{
    switch (nBpNumber)
    {
        case 0:
            asm volatile("mov %0, %%db0" ::"r"(nLinearAddress));
            break;
        case 1:
            asm volatile("mov %0, %%db1" ::"r"(nLinearAddress));
            break;
        case 2:
            asm volatile("mov %0, %%db2" ::"r"(nLinearAddress));
            break;
        case 3:
            asm volatile("mov %0, %%db3" ::"r"(nLinearAddress));
            break;
    }

    uintptr_t nStatus;
    asm volatile("mov %%db7, %0" : "=r"(nStatus));

    size_t lengthField = 0;
    switch (nLength)
    {
        case 1:
            lengthField = 0;
            break;
        case 2:
            lengthField = 1;
            break;
        case 8:
            lengthField = 2;
            break;
        case 4:
            lengthField = 3;
            break;
    }

    nStatus |= 1 << (nBpNumber * 2 + 1);
    nStatus |= (nFaultType & 0x3) << (nBpNumber * 4 + 16);
    nStatus |= (lengthField & 0x3) << (nBpNumber * 4 + 18);
    asm volatile("mov %0, %%db7" ::"r"(nStatus));
}

void Processor::disableDebugBreakpoint(size_t nBpNumber)
{
    uintptr_t nStatus;
    asm volatile("mov %%db7, %0" : "=r"(nStatus));

    nStatus &= ~(1 << (nBpNumber * 2 + 1));
    asm volatile("mov %0, %%db7" ::"r"(nStatus));
}

void Processor::setInterrupts(bool bEnable)
{
    if (bEnable)
        asm volatile("sti");
    else
        asm volatile("cli");
}

bool Processor::getInterrupts()
{
    size_t result;
    asm volatile("pushf\n"
                 "pop %0\n"
                 "and $0x200, %0\n"
                 : "=r"(result));
    return (result != 0);
}

void Processor::setSingleStep(bool bEnable, InterruptState &state)
{
    uintptr_t eflags = state.getFlags();
    if (bEnable)
        eflags |= 0x100;
    else
        eflags &= ~0x100;
    state.setFlags(eflags);
}

uint64_t Processor::readMachineSpecificRegister(uint32_t index)
{
    uint32_t eax, edx;
    asm volatile("rdmsr" : "=a"(eax), "=d"(edx) : "c"(index));
    return static_cast<uint64_t>(eax) | (static_cast<uint64_t>(edx) << 32);
}

void Processor::writeMachineSpecificRegister(uint32_t index, uint64_t value)
{
    uint32_t eax = value, edx = value >> 32;
    asm volatile("wrmsr" ::"a"(eax), "d"(edx), "c"(index));
}

void Processor::invalidate(void *pAddress)
{
    asm volatile("invlpg (%0)" ::"a"(pAddress));
}

void Processor::cpuid(
    uint32_t inEax, uint32_t inEcx, uint32_t &eax, uint32_t &ebx, uint32_t &ecx,
    uint32_t &edx)
{
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(inEax), "c"(inEcx));
}

#if defined(MULTIPROCESSOR)
ProcessorId Processor::id()
{
    if (m_Initialised < 2)
        return 0;

    Pc &pc = Pc::instance();
    uint8_t apicId = pc.getLocalApic().getId();

    for (size_t i = 0; i < m_ProcessorInformation.count(); i++)
        if (m_ProcessorInformation[i]->m_LocalApicId == apicId)
            return m_ProcessorInformation[i]->m_ProcessorId;

    return 0;
}

ProcessorInformation &Processor::information()
{
    if (m_Initialised < 2)
        return m_SafeBspProcessorInformation;

    Pc &pc = Pc::instance();
    uint8_t apicId = pc.getLocalApic().getId();

    for (size_t i = 0; i < m_ProcessorInformation.count(); i++)
        if (m_ProcessorInformation[i]->m_LocalApicId == apicId)
            return *m_ProcessorInformation[i];

    return m_SafeBspProcessorInformation;
}

size_t Processor::getCount()
{
    return m_ProcessorInformation.count();
}
#endif

void Processor::breakpoint()
{
    asm volatile("int $3");
}

void Processor::halt()
{
    asm volatile("hlt");
}

void Processor::pause()
{
    asm volatile("pause");
}

void Processor::reset()
{
    // Load null IDT for now
    size_t zero = 0x0;
    asm volatile("lidt %0; int $3" ::"m"(zero));
}

void Processor::haltUntilInterrupt()
{
    bool bWasInterrupts = getInterrupts();
    __asm__ __volatile__("sti; hlt");
    if (!bWasInterrupts)
        setInterrupts(false);
}
