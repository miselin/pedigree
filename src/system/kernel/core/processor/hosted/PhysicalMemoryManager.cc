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
#include "pedigree/kernel/machine/Trace.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/utilities/Cache.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#if TRACK_PAGE_ALLOCATIONS
#include "pedigree/kernel/debugger/commands/AllocationCommand.h"
#endif

#include "VirtualAddressSpace.h"

#include "pedigree/kernel/core/SlamAllocator.h"
#include "pedigree/kernel/process/MemoryPressureManager.h"

namespace __pedigree_hosted
{
};
using namespace __pedigree_hosted;

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#define USE_BITMAP

#ifdef USE_BITMAP
uint32_t g_PageBitmap[16384] = {0};
#endif

HostedPhysicalMemoryManager *HostedPhysicalMemoryManager::m_Instance = nullptr;

static char instanceStorage[sizeof(HostedPhysicalMemoryManager)] = {0};

PhysicalMemoryManager &PhysicalMemoryManager::instance()
{
    return HostedPhysicalMemoryManager::instance();
}

HostedPhysicalMemoryManager &HostedPhysicalMemoryManager::instance()
{
    if (!m_Instance)
    {
        m_Instance = new (reinterpret_cast<void *>(instanceStorage))
            HostedPhysicalMemoryManager();
    }
    return *m_Instance;
}

physical_uintptr_t HostedPhysicalMemoryManager::allocatePage(size_t pageConstraints)
{
    static bool bDidHitWatermark = false;
    static bool bHandlingPressure = false;

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

    ptr = m_PageStack.allocate(0);
    if (!ptr)
    {
        panic("Out of memory.");
    }

#ifdef USE_BITMAP
    physical_uintptr_t ptr_bitmap = ptr / 0x1000;
    size_t idx = ptr_bitmap / 32;
    size_t bit = ptr_bitmap % 32;
    if (g_PageBitmap[idx] & (1 << bit))
    {
        m_Lock.release();
        FATAL_NOLOCK("PhysicalMemoryManager allocate()d a page twice");
    }
    g_PageBitmap[idx] |= (1 << bit);
#endif

    m_Lock.release();

#if TRACK_PAGE_ALLOCATIONS
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
void HostedPhysicalMemoryManager::freePage(physical_uintptr_t page)
{
    RecursingLockGuard<Spinlock> guard(m_Lock);

    freePageUnlocked(page);
}
void HostedPhysicalMemoryManager::freePageUnlocked(physical_uintptr_t page)
{
    if (!m_Lock.acquired())
        FATAL("HostedPhysicalMemoryManager::freePageUnlocked called without an "
              "acquired lock");

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

    m_PageStack.free(page, 0x1000);
}

void HostedPhysicalMemoryManager::pin(physical_uintptr_t page)
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

bool HostedPhysicalMemoryManager::allocateRegion(
    MemoryRegion &Region, size_t cPages, size_t pageConstraints, size_t Flags,
    physical_uintptr_t start)
{
    LockGuard<Spinlock> guard(m_RegionLock);

    // Allocate a specific physical memory region (always physically continuous)
    if (start != static_cast<physical_uintptr_t>(-1))
    {
        // Page-align the start address.
        start &= ~(getPageSize() - 1);

        if ((pageConstraints & continuous) != continuous)
            panic("PhysicalMemoryManager::allocateRegion(): function misused");

        // Remove the memory from the range-lists (if desired/possible)
        if ((pageConstraints & nonRamMemory) == nonRamMemory)
        {
            Region.setNonRamMemory(true);
            if (m_PhysicalRanges.allocateSpecific(
                    start, cPages * getPageSize()) == false)
            {
                if ((pageConstraints & force) != force)
                    return false;
                else
                    Region.setForced(true);
            }
        }
        else
        {
            // Ensure that free() does not attempt to free the given memory...
            Region.setNonRamMemory(true);
            Region.setForced(true);
        }

        // Allocate the virtual address space
        uintptr_t vAddress;

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
        PhysicalMemoryManager::m_MemoryRegions.pushBack(&Region);
        return true;
    }
    else
    {
        // Allocate the virtual address space
        uintptr_t vAddress;
        if (m_MemoryRegions.allocate(
                cPages * PhysicalMemoryManager::getPageSize(), vAddress) ==
            false)
        {
            WARNING("AllocateRegion: MemoryRegion allocation failed.");
            return false;
        }

        uint32_t start = 0;
        VirtualAddressSpace &virtualAddressSpace =
            Processor::information().getVirtualAddressSpace();

        // Map the physical memory into the allocated space
        for (size_t i = 0; i < cPages; i++)
        {
            physical_uintptr_t page = m_PageStack.allocate(pageConstraints);
            if (virtualAddressSpace.map(
                    page,
                    reinterpret_cast<void *>(
                        vAddress + i * PhysicalMemoryManager::getPageSize()),
                    Flags) == false)
            {
                WARNING("AllocateRegion: VirtualAddressSpace::map failed.");
                return false;
            }
        }

        // Set the memory-region's members
        Region.m_VirtualAddress = reinterpret_cast<void *>(vAddress);
        Region.m_PhysicalAddress = start;
        Region.m_Size = cPages * PhysicalMemoryManager::getPageSize();

        // Add to the list of memory-regions
        PhysicalMemoryManager::m_MemoryRegions.pushBack(&Region);
        return true;
    }
    return false;
}

void HostedPhysicalMemoryManager::initialise(const BootstrapStruct_t &Info)
{
    TRACE("Hosted PMM: init");

    NOTICE("memory-map:");

    size_t pageSize = getPageSize();

    // Free pages into the page stack first.
    m_PageStack.increaseCapacity((HOSTED_PHYSICAL_MEMORY_SIZE / pageSize) + 1);
    m_PageStack.free(0, HOSTED_PHYSICAL_MEMORY_SIZE);
    m_PageStack.markBelow4GReady();
    TRACE("Hosted PMM: page stack done");

    m_PageMetadata.reserve(HOSTED_PHYSICAL_MEMORY_SIZE >> 12);

    // Initialise the free physical ranges
    m_PhysicalRanges.free(0, 0x100000000ULL);
    m_PhysicalRanges.allocateSpecific(0, HOSTED_PHYSICAL_MEMORY_SIZE);

// Print the ranges
#if VERBOSE_MEMORY_MANAGER
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

void HostedPhysicalMemoryManager::initialisationDone()
{
    NOTICE("PhysicalMemoryManager: kernel initialisation complete");
}

HostedPhysicalMemoryManager::HostedPhysicalMemoryManager()
    : m_PhysicalRanges(), m_MemoryRegions(), m_Lock(false, true),
      m_RegionLock(false, true), m_PageMetadata(), m_BackingFile(-1)
{
    // Create our backing memory file.
    // This lseek/write creates a sparse file on disk.
    m_BackingFile = open("physical.bin", O_RDWR | O_CREAT, 0644);
    lseek(m_BackingFile, HOSTED_PHYSICAL_MEMORY_SIZE - 1, SEEK_SET);
    write(m_BackingFile, "\0", 1);
    lseek(m_BackingFile, 0, SEEK_SET);
}

HostedPhysicalMemoryManager::~HostedPhysicalMemoryManager()
{
    return;

    PhysicalMemoryManager::m_MemoryRegions.clear();

    close(m_BackingFile);
    m_BackingFile = -1;
}

void HostedPhysicalMemoryManager::unmapRegion(MemoryRegion *pRegion)
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

            for (size_t i = 0; i < cPages; i++)
            {
                void *vAddr = reinterpret_cast<void *>(
                    start + i * PhysicalMemoryManager::getPageSize());
                if (!virtualAddressSpace.isMapped(vAddr))
                {
                    FATAL("Algorithmic error in "
                          "PhysicalMemoryManager::unmapRegion");
                }
                physical_uintptr_t pAddr;
                size_t flags;
                virtualAddressSpace.getMapping(vAddr, pAddr, flags);

                if (!pRegion->getNonRamMemory() && pAddr > 0x1000000)
                    m_PageStack.free(pAddr, 0x1000);

                virtualAddressSpace.unmap(vAddr);
            }
            m_MemoryRegions.free(start, pRegion->size());
            PhysicalMemoryManager::m_MemoryRegions.erase(it);
            break;
        }
    }
}
