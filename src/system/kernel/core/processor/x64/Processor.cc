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
#include "../x86_common/PhysicalMemoryManager.h"
#include "../x86_common/Multiprocessor.h"
#include "InterruptManager.h"
#include "SyscallManager.h"
#include "VirtualAddressSpace.h"
#include "gdt.h"
#include "pedigree/kernel/process/initialiseMultitasking.h"
#include "pedigree/kernel/processor/IoPortManager.h"
#include "pedigree/kernel/processor/NMFaultHandler.h"
#include "pedigree/kernel/processor/PageFaultHandler.h"
#include "pedigree/kernel/utilities/utility.h"

#define PAT_UC 0x00
#define PAT_WC 0x01
#define PAT_WT 0x04
#define PAT_WP 0x05
#define PAT_WB 0x06
#define PAT_UCMINUS 0x07

union pat
{
    struct
    {
        uint32_t pa0 : 3;
        uint32_t rsvd0 : 5;
        uint32_t pa1 : 3;
        uint32_t rsvd1 : 5;
        uint32_t pa2 : 3;
        uint32_t rsvd2 : 5;
        uint32_t pa3 : 3;
        uint32_t rsvd3 : 5;
        uint32_t pa4 : 3;
        uint32_t rsvd4 : 5;
        uint32_t pa5 : 3;
        uint32_t rsvd5 : 5;
        uint32_t pa6 : 3;
        uint32_t rsvd6 : 5;
        uint32_t pa7 : 3;
        uint32_t rsvd7 : 5;
    } s;
    uint64_t x;
};

static int doInitialise64(const BootstrapStruct_t &info)
{
    // Initialise the 64-bit physical memory management
    // This could  be done in parallel with system startup, as other parts of
    // the system that *need* pages above 4GB will be able to block until they
    // are available, and otherwise page allocations will be adequately
    // completed by the presence of pages under 4GB.
    X86CommonPhysicalMemoryManager &physicalMemoryManager =
        X86CommonPhysicalMemoryManager::instance();
    physicalMemoryManager.initialise64(info);

    return 0;
}

void ProcessorBase::switchAddressSpace(VirtualAddressSpace &AddressSpace)
{
    const X64VirtualAddressSpace &x64AddressSpace =
        static_cast<const X64VirtualAddressSpace &>(AddressSpace);

    // Get the current page directory
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));

    // Do we need to set a new page directory?
    if (cr3 != x64AddressSpace.m_PhysicalPML4)
    {
        // Set the new page directory
        asm volatile("mov %0, %%cr3" ::"r"(x64AddressSpace.m_PhysicalPML4));

        // Update the information in the ProcessorInformation structure
        ProcessorInformation &processorInformation = Processor::information();
        processorInformation.setVirtualAddressSpace(AddressSpace);
    }
}

void ProcessorBase::deinitialise()
{
    shutdownMultitasking();

    // Shut down remaining singleton objects.
    IoPortManager::instance().~IoPortManager();
    X86CommonPhysicalMemoryManager::instance().shutdown();
    PageFaultHandler::instance().~PageFaultHandler();
    X64SyscallManager::instance().~X64SyscallManager();
    X64InterruptManager::instance().~X64InterruptManager();
}

void ProcessorBase::initialise1(const BootstrapStruct_t &Info)
{
    // Initialise this processor's interrupt handling
    X64InterruptManager::initialiseProcessor();

    // Initialise this processor's syscall handling
    X64SyscallManager::initialiseProcessor();

    // Enable Write-Protect so the kernel can write to CoW pages and not break
    // that contract.
    asm volatile("mov %%cr0, %%rax; or $0x10000, %%rax; mov %%rax, %%cr0" ::
                     : "rax");

    PageFaultHandler::instance().initialise();

    // Initialise the physical memory-management
    X86CommonPhysicalMemoryManager &physicalMemoryManager =
        X86CommonPhysicalMemoryManager::instance();
    physicalMemoryManager.initialise(Info);

    // Initialise the I/O Manager
    IoPortManager &ioPortManager = IoPortManager::instance();
    ioPortManager.initialise(0, 0x10000);

    // Initialise floating point.
    NMFaultHandler::instance().initialise();
    NMFaultHandler::instance().initialiseProcessor();

    /// todo move to a better place
    // Write PAT MSR.
    // MSR 0x277

    /*
    PAT Entry
    Memory Type Following Power-up or Reset
    PAT0 WB
    PAT1 WT
    PAT2 UC-
    PAT3 UC
    PAT4 WB
    PAT5 WT
    PAT6 UC-
    PAT7 UC
    */
    //
    uint32_t pat_lo, pat_hi;
    asm volatile("rdmsr" : "=a"(pat_lo), "=d"(pat_hi) : "c"(0x277));

    union pat p;
    p.x = pat_lo | (static_cast<uint64_t>(pat_hi) << 32ULL);
    p.s.pa0 = PAT_WB;
    p.s.pa1 =
        PAT_WC;  // Redefine PWT in all page entries to mean WC instead of WT.
    p.s.pa2 = PAT_UCMINUS;
    p.s.pa3 = PAT_UC;
    p.s.pa4 = PAT_WB;
    p.s.pa5 = PAT_WT;  // PWT|PAT == WT.
    p.s.pa6 = PAT_UCMINUS;
    p.s.pa7 = PAT_UC;
    pat_lo = static_cast<uint32_t>(p.x);
    pat_hi = static_cast<uint32_t>(p.x >> 32ULL);

    asm volatile("wrmsr" ::"a"(pat_lo), "d"(pat_hi), "c"(0x277));

    m_Initialised = 1;
}

void ProcessorBase::initialise2(const BootstrapStruct_t &Info)
{
    size_t nProcessors = 1;

    EMIT_IF(MULTIPROCESSOR)
    {
        nProcessors = Multiprocessor::initialise1();
    }

    // Initialise the GDT
    X64GdtManager::instance().initialise(nProcessors);
    X64GdtManager::initialiseProcessor();

    initialiseMultitasking();

    doInitialise64(Info);

    m_Initialised = 2;

    EMIT_IF(MULTIPROCESSOR)
    {
        if (nProcessors != 1)
            Multiprocessor::initialise2();
    }
}

void ProcessorBase::identify(HugeStaticString &str)
{
    uint32_t eax, ebx, ecx, edx;
    char ident[13];
    X86CommonProcessor::cpuid(0, 0, eax, ebx, ecx, edx);
    MemoryCopy(ident, &ebx, 4);
    MemoryCopy(&ident[4], &edx, 4);
    MemoryCopy(&ident[8], &ecx, 4);
    ident[12] = 0;
    str = ident;
}

void ProcessorBase::setTlsBase(uintptr_t newBase)
{
    // Set FS.base MSR.
    asm volatile(
        "wrmsr" ::"a"(newBase), "d"(newBase >> 32ULL), "c"(0xC0000100));
}
