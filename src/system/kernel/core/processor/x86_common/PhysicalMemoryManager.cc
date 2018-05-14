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

#include "PhysicalMemoryManager.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/utilities/Cache.h"
#include "pedigree/kernel/utilities/MemoryTracing.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#if defined(X86)
#include "../x86/VirtualAddressSpace.h"
#elif defined(X64)
#include "../x64/VirtualAddressSpace.h"
#include "../x64/utils.h"
#endif

#if defined(TRACK_PAGE_ALLOCATIONS)
#include "pedigree/kernel/debugger/commands/AllocationCommand.h"
#endif

#include "pedigree/kernel/core/SlamAllocator.h"
#include "pedigree/kernel/process/MemoryPressureManager.h"

#if defined(X86) && defined(DEBUGGER)
#define USE_BITMAP
#endif

#ifdef USE_BITMAP
uint32_t g_PageBitmap[16384] = {0};
#endif

EXPORTED_PUBLIC size_t g_FreePages = 0;
EXPORTED_PUBLIC size_t g_AllocedPages = 0;

X86CommonPhysicalMemoryManager X86CommonPhysicalMemoryManager::m_Instance;

static void trackPages(ssize_t v, ssize_t p, ssize_t s)
{
    // Track, if we can.
    Thread *pThread = Processor::information().getCurrentThread();
    if (pThread)
    {
        Process *pProcess = pThread->getParent();
        if (pProcess)
        {
            pProcess->trackPages(v, p, s);
        }
    }
}

PhysicalMemoryManager &PhysicalMemoryManager::instance()
{
    return X86CommonPhysicalMemoryManager::instance();
}

size_t X86CommonPhysicalMemoryManager::freePageCount() const
{
    return m_PageStack.freePages();
}

physical_uintptr_t
X86CommonPhysicalMemoryManager::allocatePage(size_t pageConstraints)
{
    static bool bDidHitWatermark = false;
    static bool bHandlingPressure = false;

    // Recursion allowed, to permit e.g. calls from the manager to the heap to
    // succeed without needing to release/re-acquire the lock.
    m_Lock.acquire(true);

    physical_uintptr_t ptr;

    // Some methods of handling memory pressure require allocating pages, so
    // we need to not end up recursively trying to release the pressure.
    if (!bHandlingPressure)
    {
        if (m_PageStack.freePages() < MemoryPressureManager::getHighWatermark())
        {
            bHandlingPressure = true;

            // Make sure the compact can trigger frees.
            m_Lock.release();

            WARNING_NOLOCK(
                "Memory pressure encountered, performing a compact...");
            if (!MemoryPressureManager::instance().compact())
                ERROR_NOLOCK("Compact did not alleviate any memory pressure.");
            else
                NOTICE_NOLOCK("Compact was successful.");

            m_Lock.acquire(true);

            bDidHitWatermark = true;
            bHandlingPressure = false;
        }
        else if (bDidHitWatermark)
        {
            ERROR_NOLOCK("<pressure was hit, but is no longer being hit>");
            bDidHitWatermark = false;
        }
    }

    ptr = m_PageStack.allocate(pageConstraints);
    if (!ptr)
    {
        panic("Out of memory.");
    }

#ifdef MEMORY_TRACING
    traceAllocation(
        reinterpret_cast<void *>(ptr), MemoryTracing::PageAlloc, 4096);
#endif

    trackPages(0, 1, 0);

#ifdef USE_BITMAP
    physical_uintptr_t ptr_bitmap = ptr / 0x1000;
    size_t idx = ptr_bitmap / 32;
    size_t bit = ptr_bitmap % 32;
    g_PageBitmap[idx] |= (1 << bit);
#endif

    m_Lock.release();

#if defined(TRACK_PAGE_ALLOCATIONS)
    if (Processor::m_Initialised == 2)
    {
        if (!g_AllocationCommand.isMallocing())
        {
            g_AllocationCommand.allocatePage(ptr);
        }
    }
#endif

    return ptr;
}
void X86CommonPhysicalMemoryManager::freePage(physical_uintptr_t page)
{
    RecursingLockGuard<Spinlock> guard(m_Lock);

    freePageUnlocked(page);
}
void X86CommonPhysicalMemoryManager::freePageUnlocked(physical_uintptr_t page)
{
    if (!m_Lock.acquired())
        FATAL("X86CommonPhysicalMemoryManager::freePageUnlocked called without "
              "an acquired lock");

    // Check for pinned page.
    PageHashable index(page);
    MetadataTable::LookupResult result = m_PageMetadata.lookup(index);
    if (result.hasValue())
    {
        struct page p = result.value();
        if (p.active)
        {
            if (--p.refcount)
            {
                // Still references.
                m_PageMetadata.update(index, p);
                return;
            }
            else
            {
                // No more references, stop tracking page.
                p.active = false;
                m_PageMetadata.update(index, p);
            }
        }
    }

#ifdef USE_BITMAP
    physical_uintptr_t ptr_bitmap = page / 0x1000;
    size_t idx = ptr_bitmap / 32;
    size_t bit = ptr_bitmap % 32;
    if (!(g_PageBitmap[idx] & (1 << bit)))
    {
        m_Lock.release();
        FATAL_NOLOCK("PhysicalMemoryManager DOUBLE FREE");
    }

    g_PageBitmap[idx] &= ~(1 << bit);
#endif

    m_PageStack.free(page, getPageSize());

#ifdef MEMORY_TRACING
    traceAllocation(
        reinterpret_cast<void *>(page), MemoryTracing::PageFree, 4096);
#endif

    trackPages(0, -1, 0);
}
void X86CommonPhysicalMemoryManager::pin(physical_uintptr_t page)
{
    RecursingLockGuard<Spinlock> guard(m_Lock);

    PageHashable index(page);
    MetadataTable::LookupResult result = m_PageMetadata.lookup(index);
    if (result.hasValue())
    {
        struct page p = result.value();
        ++p.refcount;
        p.active = true;
        m_PageMetadata.update(index, p);
    }
    else
    {
        struct page p;
        p.refcount = 1;
        p.active = true;
        m_PageMetadata.insert(index, p);
    }
}
bool X86CommonPhysicalMemoryManager::allocateRegion(
    MemoryRegion &Region, size_t cPages, size_t pageConstraints, size_t Flags,
    physical_uintptr_t start)
{
    LockGuard<Spinlock> guard(m_RegionLock);

    // Allocate a specific physical memory region (always physically continuous)
    if (start != static_cast<physical_uintptr_t>(-1))
    {
        // Page-align the start address.
        start &= ~(getPageSize() - 1);

        if (((pageConstraints & continuous) != continuous) ||
            (pageConstraints & virtualOnly))
            panic("PhysicalMemoryManager::allocateRegion(): function misused");

        // Remove the memory from the range-lists (if desired/possible)
        if ((pageConstraints & nonRamMemory) == nonRamMemory)
        {
            Region.setNonRamMemory(true);
            if (m_PhysicalRanges.allocateSpecific(
                    start, cPages * getPageSize()) == false)
            {
                if ((pageConstraints & force) != force)
                {
                    ERROR("PhysicalMemoryManager::allocateRegion() [specific] "
                          "- failed to get space from general range list and "
                          "force is not set");
                    return false;
                }
                else
                    Region.setForced(true);
            }
        }
        else
        {
            if (start < 0x100000 && (start + cPages * getPageSize()) < 0x100000)
            {
                if (m_RangeBelow1MB.allocateSpecific(
                        start, cPages * getPageSize()) == false)
                {
                    ERROR("PhysicalMemoryManager::allocateRegion() [specific] "
                          "- failed to get space from <1MB range list");
                    return false;
                }
            }
            else if (
                start < 0x1000000 &&
                (start + cPages * getPageSize()) < 0x1000000)
            {
                if (m_RangeBelow16MB.allocateSpecific(
                        start, cPages * getPageSize()) == false)
                {
                    ERROR(
                        "PhysicalMemoryManager::allocateRegion() [specific] - "
                        "failed to get "
                        << cPages
                        << " pages of memory from <16MB range list at " << Hex
                        << start);
                    return false;
                }
            }
            else if (start < 0x1000000)
            {
                ERROR("PhysicalMemoryManager: Memory region neither completely "
                      "below nor above 1MB");
                return false;
            }
            else
            {
                // Ensure that free() does not attempt to free the given
                // memory...
                Region.setNonRamMemory(true);
                Region.setForced(true);
            }
        }

        // Allocate the virtual address space
        uintptr_t vAddress = 0;

        if (m_MemoryRegions.allocate(
                cPages * PhysicalMemoryManager::getPageSize(), vAddress) ==
            false)
        {
            WARNING("AllocateRegion: MemoryRegion allocation failed.");
            return false;
        }

        // Map the physical memory into the allocated space
        VirtualAddressSpace &virtualAddressSpace =
            Processor::information().getVirtualAddressSpace();
        for (size_t i = 0; i < cPages; i++)
            if (virtualAddressSpace.map(
                    start + i * PhysicalMemoryManager::getPageSize(),
                    reinterpret_cast<void *>(
                        vAddress + i * PhysicalMemoryManager::getPageSize()),
                    Flags) == false)
            {
                m_MemoryRegions.free(
                    vAddress, cPages * PhysicalMemoryManager::getPageSize());
                WARNING("AllocateRegion: VirtualAddressSpace::map failed.");
                return false;
            }

        // Set the memory-region's members
        Region.m_VirtualAddress = reinterpret_cast<void *>(vAddress);
        Region.m_PhysicalAddress = start;
        Region.m_Size = cPages * PhysicalMemoryManager::getPageSize();
        //       NOTICE("MR: Allocated " << Hex << vAddress << " (phys " <<
        //       static_cast<uintptr_t>(start) << "), size " << (cPages*4096));

        // Add to the list of memory-regions
        if (!(pageConstraints & PhysicalMemoryManager::anonymous))
        {
            PhysicalMemoryManager::m_MemoryRegions.pushBack(&Region);
        }
        return true;
    }
    else
    {
        // If we need continuous memory, switch to below16 if not already
        if ((pageConstraints & continuous) == continuous)
            if ((pageConstraints & addressConstraints) != below1MB &&
                (pageConstraints & addressConstraints) != below16MB)
                pageConstraints =
                    (pageConstraints & ~addressConstraints) | below16MB;

        // Allocate the virtual address space
        uintptr_t vAddress;
        if (m_MemoryRegions.allocate(
                cPages * PhysicalMemoryManager::getPageSize(), vAddress) ==
            false)
        {
            WARNING("AllocateRegion: MemoryRegion allocation failed.");
            return false;
        }

        uint32_t allocatedStart = 0;
        if (!(pageConstraints & virtualOnly))
        {
            VirtualAddressSpace &virtualAddressSpace =
                Processor::information().getVirtualAddressSpace();

            if ((pageConstraints & addressConstraints) == below1MB ||
                (pageConstraints & addressConstraints) == below16MB)
            {
                // Allocate a range
                if ((pageConstraints & addressConstraints) == below1MB)
                {
                    if (m_RangeBelow1MB.allocate(
                            cPages * getPageSize(), allocatedStart) == false)
                    {
                        ERROR("PhysicalMemoryManager::allocateRegion() - "
                              "failed to get space from <1MB range list");
                        return false;
                    }
                }
                else if ((pageConstraints & addressConstraints) == below16MB)
                {
                    if (m_RangeBelow16MB.allocate(
                            cPages * getPageSize(), allocatedStart) == false)
                    {
                        ERROR("PhysicalMemoryManager::allocateRegion() - "
                              "failed to get space from <16MB range list");
                        return false;
                    }
                }

                // Map the physical memory into the allocated space
                for (size_t i = 0; i < cPages; i++)
                    if (virtualAddressSpace.map(
                            allocatedStart +
                                i * PhysicalMemoryManager::getPageSize(),
                            reinterpret_cast<void *>(
                                vAddress +
                                i * PhysicalMemoryManager::getPageSize()),
                            Flags) == false)
                    {
                        WARNING(
                            "AllocateRegion: VirtualAddressSpace::map failed.");
                        return false;
                    }
            }
            else
            {
                // Map the physical memory into the allocated space
                for (size_t i = 0; i < cPages; i++)
                {
                    physical_uintptr_t page = m_PageStack.allocate(
                        pageConstraints & addressConstraints);
                    if (virtualAddressSpace.map(
                            page,
                            reinterpret_cast<void *>(
                                vAddress +
                                i * PhysicalMemoryManager::getPageSize()),
                            Flags) == false)
                    {
                        WARNING(
                            "AllocateRegion: VirtualAddressSpace::map failed.");
                        return false;
                    }
                }
            }
        }

        // Set the memory-region's members
        Region.m_VirtualAddress = reinterpret_cast<void *>(vAddress);
        Region.m_PhysicalAddress = allocatedStart;
        Region.m_Size = cPages * PhysicalMemoryManager::getPageSize();

        // Add to the list of memory-regions
        if (!(pageConstraints & PhysicalMemoryManager::anonymous))
        {
            PhysicalMemoryManager::m_MemoryRegions.pushBack(&Region);
        }
        return true;
    }
}

void X86CommonPhysicalMemoryManager::shutdown()
{
    NOTICE("Shutting down X86CommonPhysicalMemoryManager");
    PhysicalMemoryManager::m_MemoryRegions.clear();
    m_PageMetadata.clear();
}

void X86CommonPhysicalMemoryManager::initialise(const BootstrapStruct_t &Info)
{
    NOTICE("memory-map:");

    physical_uintptr_t top = 0;
    size_t pageSize = getPageSize();

    // Fill the page-stack (usable memory above 16MB)
    // NOTE: We must do the page-stack first, because the range-lists already
    // need the
    //       memory-management
    void *MemoryMap = Info.getMemoryMap();
    if (!MemoryMap)
        panic("no memory map provided by the bootloader");

    // Fill our stack with pages below the 4GB threshold.
    while (MemoryMap)
    {
        uint64_t addr = Info.getMemoryMapEntryAddress(MemoryMap);
        uint64_t length = Info.getMemoryMapEntryLength(MemoryMap);
        uint32_t type = Info.getMemoryMapEntryType(MemoryMap);

        NOTICE(
            " " << Hex << addr << " - " << (addr + length)
                << ", type: " << type);

        MemoryMap = Info.nextMemoryMapEntry(MemoryMap);

        if (type != 1)
        {
            continue;
        }

        // We don't want pages below 1MB, and don't want any over 4GB.
        uint64_t rangeTop = addr + length;
        if (rangeTop < 0x1000000)
        {
            // Entire region is below 1MB.
            continue;
        }
        else if (rangeTop >= 0x100000000ULL)
        {
            // Region is too high.
            continue;
        }

        if (addr < 0x1000000)
        {
            // Region crosses 1MB mark. Fix to base at 1MB instead.
            length = rangeTop - 0x1000000;
            addr = 0x1000000;
        }

        if (rangeTop >= top)
        {
            // Update the "top of memory" value.
            top = rangeTop;
        }

        // Prepare the page stack for the additional pages we're giving it.
        m_PageStack.increaseCapacity((length / pageSize) + 1);

        m_PageStack.free(addr, length);
    }

    // Stack with <4GB is done.
    m_PageStack.markBelow4GReady();

    /// \todo do this in initialise64 too, copying any existing entries.
    m_PageMetadata.reserve(top >> 12);  // number of 4k pages in this zone

    // Fill the range-lists (usable memory below 1/16MB & ACPI)
    MemoryMap = Info.getMemoryMap();
    while (MemoryMap)
    {
        uint64_t addr = Info.getMemoryMapEntryAddress(MemoryMap);
        uint64_t length = Info.getMemoryMapEntryLength(MemoryMap);
        uint32_t type = Info.getMemoryMapEntryType(MemoryMap);

        if (type == 1)
        {
            if (addr < 0x100000)
            {
                // NOTE: Assumes that the entry/entries starting below 1MB don't
                // cross the
                //       1MB barrier
                if ((addr + length) >= 0x100000)
                    panic("PhysicalMemoryManager: strange memory-map");

                m_RangeBelow1MB.free(addr, length);
            }
            else if (addr < 0x1000000)
            {
                uint64_t upperBound = addr + length;
                if (upperBound >= 0x1000000)
                    upperBound = 0x1000000;

                m_RangeBelow16MB.free(addr, upperBound - addr);
            }
        }
#if defined(ACPI)
        else if (type == 3 || type == 4)
        {
            m_AcpiRanges.free(addr, length);
        }
#endif

        MemoryMap = Info.nextMemoryMapEntry(MemoryMap);
    }

    // Remove the pages used by the kernel from the range-list (below 16MB)
    extern void *kernel_start;
    extern void *kernel_end;
    if (m_RangeBelow16MB.allocateSpecific(
            reinterpret_cast<uintptr_t>(&kernel_start) -
                reinterpret_cast<uintptr_t>(KERNEL_VIRTUAL_ADDRESS),
            reinterpret_cast<uintptr_t>(&kernel_end) -
                reinterpret_cast<uintptr_t>(&kernel_start)) == false)
    {
        panic("PhysicalMemoryManager: could not remove the kernel image from "
              "the range-list");
    }

// Print the ranges
#if defined(VERBOSE_MEMORY_MANAGER)
    NOTICE("free memory ranges (below 1MB):");
    for (size_t i = 0; i < m_RangeBelow1MB.size(); i++)
        NOTICE(
            " " << Hex << m_RangeBelow1MB.getRange(i).address << " - "
                << (m_RangeBelow1MB.getRange(i).address +
                    m_RangeBelow1MB.getRange(i).length));
    NOTICE("free memory ranges (below 16MB):");
    for (size_t i = 0; i < m_RangeBelow16MB.size(); i++)
        NOTICE(
            " " << Hex << m_RangeBelow16MB.getRange(i).address << " - "
                << (m_RangeBelow16MB.getRange(i).address +
                    m_RangeBelow16MB.getRange(i).length));
#if defined(ACPI)
    NOTICE("ACPI ranges:");
    for (size_t i = 0; i < m_AcpiRanges.size(); i++)
        NOTICE(
            " " << Hex << m_AcpiRanges.getRange(i).address << " - "
                << (m_AcpiRanges.getRange(i).address +
                    m_AcpiRanges.getRange(i).length));
#endif
#endif

    // Initialise the free physical ranges
    m_PhysicalRanges.free(0, 0x100000000ULL);
    MemoryMap = Info.getMemoryMap();
    while (MemoryMap)
    {
        uint64_t addr = Info.getMemoryMapEntryAddress(MemoryMap);
        uint64_t length = Info.getMemoryMapEntryLength(MemoryMap);

        // Only map if the variable fits into a uintptr_t - no overflow!
        if (addr > ~0ULL)
        {
            WARNING("Memory region " << addr << " not used.");
        }
        else if (addr >= 0x100000000ULL)
        {
            // Skip >= 4 GB for now, done in initialise64
            break;
        }
        else if (m_PhysicalRanges.allocateSpecific(addr, length) == false)
            panic("PhysicalMemoryManager: Failed to create the list of ranges "
                  "of free physical space");

        MemoryMap = Info.nextMemoryMapEntry(MemoryMap);
    }

// Print the ranges
#if defined(VERBOSE_MEMORY_MANAGER)
    NOTICE("physical memory ranges:");
    for (size_t i = 0; i < m_PhysicalRanges.size(); i++)
    {
        NOTICE(
            " " << Hex << m_PhysicalRanges.getRange(i).address << " - "
                << (m_PhysicalRanges.getRange(i).address +
                    m_PhysicalRanges.getRange(i).length));
    }
#endif

    // Initialise the range of virtual space for MemoryRegions
    m_MemoryRegions.free(
        reinterpret_cast<uintptr_t>(KERNEL_VIRTUAL_MEMORYREGION_ADDRESS),
        KERNEL_VIRTUAL_MEMORYREGION_SIZE);
}
#ifdef X64
void X86CommonPhysicalMemoryManager::initialise64(const BootstrapStruct_t &Info)
{
    NOTICE("64-bit memory-map:");

    // Fill the page-stack (usable memory above 16MB)
    // NOTE: We must do the page-stack first, because the range-lists already
    // need the
    //       memory-management
    size_t numPagesOver4G = 0;
    uint64_t base = 0;
    void *MemoryMap = Info.getMemoryMap();
    while (MemoryMap)
    {
        uint64_t addr = Info.getMemoryMapEntryAddress(MemoryMap);
        uint64_t length = Info.getMemoryMapEntryLength(MemoryMap);
        uint32_t type = Info.getMemoryMapEntryType(MemoryMap);

        if (addr >= 0x100000000ULL)
        {
            if (base == 0 || addr < base)
            {
                base = addr;
            }

            NOTICE(
                " " << Hex << addr << " - " << (addr + length)
                    << ", type: " << type);

            if (type == 1)
            {
                size_t numPages = length / getPageSize();
                m_PageStack.increaseCapacity(numPages);
                m_PageStack.free(addr, length);

                m_PhysicalRanges.free(addr, length);

                numPagesOver4G += numPages;
            }
        }

        MemoryMap = Info.nextMemoryMapEntry(MemoryMap);
    }

    // Map physical memory above 4G into the kernel address space.
    // Everything below 4G is already mapped using 2MB pages.
    /// \todo this will break if there's over 64 TiB of RAM on the machine.
    VirtualAddressSpace &kernelSpace =
        VirtualAddressSpace::getKernelAddressSpace();
    bool ok = kernelSpace.mapHuge(
        base, reinterpret_cast<void *>(0xFFFF800000000000 + base),
        numPagesOver4G,
        VirtualAddressSpace::Write | VirtualAddressSpace::KernelMode);
    if (!ok)
    {
        FATAL("failed to map physical memory");
    }

    NOTICE(" --> " << numPagesOver4G << " pages exist above 4G!");

    // Stacks >=4GB are done.
    m_PageStack.markAbove4GReady();

// Fill the range-lists (usable memory below 1/16MB & ACPI)
#if defined(ACPI)
    MemoryMap = Info.getMemoryMap();
    while (MemoryMap)
    {
        if ((Info.getMemoryMapEntryType(MemoryMap) == 3 ||
             Info.getMemoryMapEntryType(MemoryMap) == 4) &&
            Info.getMemoryMapEntryAddress(MemoryMap) >= 0x100000000ULL)
        {
            m_AcpiRanges.free(
                Info.getMemoryMapEntryAddress(MemoryMap),
                Info.getMemoryMapEntryLength(MemoryMap));
        }

        MemoryMap = Info.nextMemoryMapEntry(MemoryMap);
    }

#if defined(VERBOSE_MEMORY_MANAGER)
    // Print the ranges
    NOTICE("ACPI ranges (x64 added):");
    for (size_t i = 0; i < m_AcpiRanges.size(); i++)
        NOTICE(
            " " << Hex << m_AcpiRanges.getRange(i).address << " - "
                << (m_AcpiRanges.getRange(i).address +
                    m_AcpiRanges.getRange(i).length));
#endif
#endif

    // Initialise the free physical ranges
    MemoryMap = Info.getMemoryMap();
    while (MemoryMap)
    {
        // Only map if the variable fits into a uintptr_t - no overflow!
        if ((Info.getMemoryMapEntryAddress(MemoryMap)) > ~0ULL)
        {
            WARNING(
                "Memory region " << Info.getMemoryMapEntryAddress(MemoryMap)
                                 << " not used.");
        }
        else if (
            (Info.getMemoryMapEntryAddress(MemoryMap) >= 0x100000000ULL) &&
            (m_PhysicalRanges.allocateSpecific(
                 Info.getMemoryMapEntryAddress(MemoryMap),
                 Info.getMemoryMapEntryLength(MemoryMap)) == false))
            panic("PhysicalMemoryManager: Failed to create the list of ranges "
                  "of free physical space");

        MemoryMap = Info.nextMemoryMapEntry(MemoryMap);
    }

// Print the ranges
#if defined(VERBOSE_MEMORY_MANAGER)
    NOTICE("physical memory ranges, 64-bit added:");
    for (size_t i = 0; i < m_PhysicalRanges.size(); i++)
    {
        NOTICE(
            " " << Hex << m_PhysicalRanges.getRange(i).address << " - "
                << (m_PhysicalRanges.getRange(i).address +
                    m_PhysicalRanges.getRange(i).length));
    }
#endif
}
#endif

void X86CommonPhysicalMemoryManager::initialisationDone()
{
    extern void *kernel_init;
    extern void *kernel_init_end;

    NOTICE("PhysicalMemoryManager: kernel initialisation complete, cleaning "
           "up...");

    // Unmap & free the .init section
    VirtualAddressSpace &kernelSpace =
        VirtualAddressSpace::getKernelAddressSpace();
    size_t count = (reinterpret_cast<uintptr_t>(&kernel_init_end) -
                    reinterpret_cast<uintptr_t>(&kernel_init)) /
                   getPageSize();
    for (size_t i = 0; i < count; i++)
    {
        void *vAddress = adjust_pointer(
            reinterpret_cast<void *>(&kernel_init), i * getPageSize());

        // Get the physical address
        size_t flags;
        physical_uintptr_t pAddress;
        kernelSpace.getMapping(vAddress, pAddress, flags);

        // Unmap the page
        kernelSpace.unmap(vAddress);
    }

    // Free the physical pages
    m_RangeBelow16MB.free(
        reinterpret_cast<uintptr_t>(&kernel_init) -
            reinterpret_cast<uintptr_t>(KERNEL_VIRTUAL_ADDRESS),
        count * getPageSize());

    NOTICE(
        "PhysicalMemoryManager: cleaned up " << Dec << (count * 4) << Hex
                                             << "KB of init-only code.");
}

X86CommonPhysicalMemoryManager::X86CommonPhysicalMemoryManager()
    : m_PageStack(), m_RangeBelow1MB(), m_RangeBelow16MB(), m_PhysicalRanges(),
#if defined(ACPI)
      m_AcpiRanges(),
#endif
      m_MemoryRegions(), m_Lock(false, true), m_RegionLock(false, true),
      m_PageMetadata()
{
}
X86CommonPhysicalMemoryManager::~X86CommonPhysicalMemoryManager()
{
}

void X86CommonPhysicalMemoryManager::unmapRegion(MemoryRegion *pRegion)
{
    LockGuard<Spinlock> guard(m_RegionLock);

    for (Vector<MemoryRegion *>::Iterator it =
             PhysicalMemoryManager::m_MemoryRegions.begin();
         it != PhysicalMemoryManager::m_MemoryRegions.end(); it++)
    {
        if (*it == pRegion)
        {
            size_t cPages =
                pRegion->size() / PhysicalMemoryManager::getPageSize();
            uintptr_t start =
                reinterpret_cast<uintptr_t>(pRegion->virtualAddress());
            physical_uintptr_t phys = pRegion->physicalAddress();
            VirtualAddressSpace &virtualAddressSpace =
                VirtualAddressSpace::getKernelAddressSpace();

            if (pRegion->getNonRamMemory())
            {
                if (!pRegion->getForced())
                    m_PhysicalRanges.free(phys, pRegion->size());
            }
            else
            {
                if (phys < 0x100000 &&
                    (phys + cPages * getPageSize()) < 0x100000)
                {
                    m_RangeBelow1MB.free(phys, cPages * getPageSize());
                }
                else if (
                    phys < 0x1000000 &&
                    (phys + cPages * getPageSize()) < 0x1000000)
                {
                    m_RangeBelow16MB.free(phys, cPages * getPageSize());
                }
                else if (phys < 0x1000000)
                {
                    ERROR("PhysicalMemoryManager: Memory region neither "
                          "completely below nor above 1MB");
                    return;
                }
            }

            for (size_t i = 0; i < cPages; i++)
            {
                void *vAddr = reinterpret_cast<void *>(
                    start + i * PhysicalMemoryManager::getPageSize());
                if (!virtualAddressSpace.isMapped(vAddr))
                {
                    // Can happen with virtualOnly mappings.
                    /// \todo copy the pageConstraints to the Region object
                    continue;
                }
                physical_uintptr_t pAddr;
                size_t flags;
                virtualAddressSpace.getMapping(vAddr, pAddr, flags);

                if (!pRegion->getNonRamMemory() && pAddr > 0x1000000)
                    m_PageStack.free(pAddr, getPageSize());

                virtualAddressSpace.unmap(vAddr);
            }
            //            NOTICE("MR: Freed " << Hex << start << ", size " <<
            //            (cPages*4096));
            m_MemoryRegions.free(start, pRegion->size());
            PhysicalMemoryManager::m_MemoryRegions.erase(it);
            break;
        }
    }
}

physical_uintptr_t
X86CommonPhysicalMemoryManager::PageStack::allocate(size_t constraints)
{
    size_t index = 0;
#if defined(X64)
    if (constraints == X86CommonPhysicalMemoryManager::below4GB)
        index = 0;
    else if (constraints == X86CommonPhysicalMemoryManager::below64GB)
        index = 1;
    else
    {
        index = 2;

        // Degrade quietly if this stack is not ready.
        if (!m_StackReady[index])
        {
            index = 1;

            if (!m_StackReady[index])
            {
                index = 0;
            }
        }
    }

    // Wait for the stack to be ready. With constraints, this will block until
    // a specific page stack is ready. With no constraints, this will just
    // block until the first page stack is ready (which should almost always
    // be the case).
    while (!m_StackReady[index])
    {
        Processor::pause();
    }

    if (index == 2 && (m_StackMax[2] == m_StackSize[2] || !m_StackReady[2]))
        index = 1;
    if (index == 1 && (m_StackMax[1] == m_StackSize[1] || !m_StackReady[1]))
        index = 0;
#endif

    physical_uintptr_t result = 0;
    if ((m_StackMax[index] != m_StackSize[index]) && m_StackSize[index])
    {
        if (index == 0)
        {
            m_StackSize[0] -= 4;
            result = *(
                reinterpret_cast<uint32_t *>(m_Stack[0]) + m_StackSize[0] / 4);
        }
        else
        {
            m_StackSize[index] -= 8;
            result =
                *(reinterpret_cast<uint64_t *>(m_Stack[index]) +
                  m_StackSize[index] / 8);
        }
    }

    if (result)
    {
        /// \note Testing.
        if (g_FreePages)
            g_FreePages--;
        g_AllocedPages++;

        if (m_FreePages)
            --m_FreePages;
    }

    return result;
}

template <class T>
static void
performPush(T *stack, size_t &stackSize, uint64_t physicalAddress, size_t count)
{
    size_t nextEntry = stackSize / sizeof(T);
    T addend = 0;
    for (size_t i = 0; i < count; ++i)
    {
        stack[nextEntry + i] = static_cast<T>(physicalAddress + addend);
        addend += PhysicalMemoryManager::getPageSize();
    }

    stackSize += sizeof(T) * count;
}

void X86CommonPhysicalMemoryManager::PageStack::free(
    uint64_t physicalAddress, size_t length)
{
    // Select the right stack
    /// \todo make sure callers split any regions that cross over before calling
    size_t index = 0;
    if (physicalAddress >= 0x100000000ULL)
    {
#if defined(X86)
        return;
#elif defined(X64)
        if (physicalAddress >= 0x1000000000ULL)
        {
            index = 2;
        }
        else
        {
            index = 1;
        }
#endif
    }

    // Don't attempt to map address zero.
    if (UNLIKELY(!m_Stack[index]))
    {
        return;
    }

    uint64_t topPhysical = physicalAddress + length;

    for (; physicalAddress < topPhysical; physicalAddress += getPageSize())
    {
        // Expand the stack if necessary.
        if (!maybeMap(index, physicalAddress))
        {
            break;
        }
    }

    size_t numPages = (topPhysical - physicalAddress) / getPageSize();

    if (index == 0)
    {
        performPush(
            reinterpret_cast<uint32_t *>(m_Stack[index]), m_StackSize[index],
            physicalAddress, numPages);
    }
    else
    {
        performPush(
            reinterpret_cast<uint64_t *>(m_Stack[index]), m_StackSize[index],
            physicalAddress, numPages);
    }

    /// \note Testing.
    g_FreePages += numPages;
    if (g_AllocedPages > 0)
    {
        if (g_AllocedPages >= numPages)
        {
            g_AllocedPages -= numPages;
        }
        else
        {
            g_AllocedPages = 0;
        }
    }

    m_FreePages += numPages;
}

X86CommonPhysicalMemoryManager::PageStack::PageStack()
{
    m_Capacity = 0;
    m_DesiredCapacity = 0;

    for (size_t i = 0; i < StackCount; i++)
    {
        m_StackMax[i] = 0;
        m_StackSize[i] = 0;
        m_StackReady[i] = false;
    }

    // Set the locations for the page stacks in the virtual address space
    m_Stack[0] = KERNEL_VIRTUAL_PAGESTACK_4GB;
#if defined(X64)
    m_Stack[1] = KERNEL_VIRTUAL_PAGESTACK_ABV4GB1;
    m_Stack[2] = KERNEL_VIRTUAL_PAGESTACK_ABV4GB2;
#endif

    m_FreePages = 0;
}

void X86CommonPhysicalMemoryManager::PageStack::markAbove4GReady()
{
    for (size_t i = 1; i < StackCount; ++i)
    {
        m_StackReady[i] = true;
    }
}

void X86CommonPhysicalMemoryManager::PageStack::markBelow4GReady()
{
    m_StackReady[0] = true;
}

bool X86CommonPhysicalMemoryManager::PageStack::maybeMap(
    size_t index, uint64_t physicalAddress)
{
    bool mapped = false;

    void *virtualAddress = adjust_pointer(m_Stack[index], m_StackMax[index]);

    // Do we even need to do this mapping?
    if (m_Capacity >= m_DesiredCapacity)
    {
        return false;
    }

#if defined(X86)
    // Get the kernel virtual address-space
    X86VirtualAddressSpace &AddressSpace =
        static_cast<X86VirtualAddressSpace &>(
            VirtualAddressSpace::getKernelAddressSpace());
#elif defined(X64)
    X64VirtualAddressSpace &AddressSpace =
        static_cast<X64VirtualAddressSpace &>(
            VirtualAddressSpace::getKernelAddressSpace());
#endif

    if (!index)
    {
        if (AddressSpace.mapPageStructures(
                physicalAddress, virtualAddress,
                VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write) ==
            true)
        {
            mapped = true;
        }
    }
    else
    {
#if defined(X64)
        if (AddressSpace.mapPageStructuresAbove4GB(
                physicalAddress, virtualAddress,
                VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write) ==
            true)
        {
            mapped = true;
        }
#else
        FATAL("PageStack::free - index > 0 when not built as x86_64");
#endif
    }

    // Another page worth of entries is mapped - update capacity accordingly.
    if (AddressSpace.isMapped(virtualAddress))
    {
        // This address is now valid for stack usage, so it adds capacity for
        // significantly more pages to the stack.
        size_t entrySize = sizeof(uint32_t);
        if (index != 0)
        {
            entrySize = sizeof(uint64_t);
        }
        m_Capacity += getPageSize() / entrySize;

        // This page is mapped, so we need to go ahead and start allocating the
        // next page in the stack. This way we always have the entire stack
        // mapped before we start pushing pages into it.
        m_StackMax[index] += getPageSize();
        virtualAddress = adjust_pointer(virtualAddress, getPageSize());

        // Top of stack mapped, do we need to expand further?
        if (m_Capacity >= m_DesiredCapacity)
        {
            // No need to map here.
            return false;
        }
    }

    return mapped;
}
