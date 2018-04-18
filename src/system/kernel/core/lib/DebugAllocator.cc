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

#include "pedigree/kernel/core/SlamAllocator.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/utilities/MemoryTracing.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/assert.h"

#ifdef THREADS
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#endif

SlamAllocator SlamAllocator::m_Instance;

inline uintptr_t getHeapBase()
{
    return VirtualAddressSpace::getKernelAddressSpace().getKernelHeapStart();
}

inline uintptr_t getHeapEnd()
{
    return VirtualAddressSpace::getKernelAddressSpace().getKernelHeapEnd();
}

inline size_t getPageSize()
{
    return PhysicalMemoryManager::getPageSize();
}

inline void allocateAndMapAt(void *addr)
{
    size_t standardFlags = VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write;

    static physical_uintptr_t physZero = 0;

    physical_uintptr_t phys = PhysicalMemoryManager::instance().allocatePage();

    VirtualAddressSpace &va = VirtualAddressSpace::getKernelAddressSpace();
    if (!va.map(phys, addr, standardFlags))
    {
        FATAL("SlamAllocator: failed to allocate and map at " << addr);
    }
}

inline void unmap(void *addr)
{
    VirtualAddressSpace &va = VirtualAddressSpace::getKernelAddressSpace();
    if (!va.isMapped(addr))
        return;

    physical_uintptr_t phys;
    size_t flags;
    va.getMapping(addr, phys, flags);
    va.unmap(addr);

    PhysicalMemoryManager::instance().freePage(phys);
}

inline bool isMapped(void *addr)
{
    VirtualAddressSpace &va = VirtualAddressSpace::getKernelAddressSpace();
    return va.isMapped(addr);
}

inline void markReadOnly(void *addr)
{
    VirtualAddressSpace &va = VirtualAddressSpace::getKernelAddressSpace();
    va.setFlags(addr, VirtualAddressSpace::KernelMode);
}

SlamCache::SlamCache()
    : m_PartialLists(), m_ObjectSize(0), m_SlabSize(0),
      m_FirstSlab()
#ifdef THREADS
      ,
      m_RecoveryLock(false)
#endif
      ,
      m_EmptyNode()
{
}

SlamCache::~SlamCache()
{
}

void SlamCache::initialise(SlamAllocator *parent, size_t objectSize)
{
    // no-op for debug allocator
}

SlamCache::Node *SlamCache::pop(SlamCache::alignedNode *head)
{
    // no-op for debug allocator
    return nullptr;
}

void SlamCache::push(
    SlamCache::alignedNode *head, SlamCache::Node *newTail,
    SlamCache::Node *newHead)
{
    // no-op for debug allocator
}

uintptr_t SlamCache::allocate()
{
    // no-op for debug allocator
    return 0;
}

void SlamCache::free(uintptr_t object)
{
    // no-op for debug allocator
}

bool SlamCache::isPointerValid(uintptr_t object) const
{
    // no-op for debug allocator
    return false;
}

uintptr_t SlamCache::getSlab()
{
    // no-op for debug allocator
    return 0;
}

void SlamCache::freeSlab(uintptr_t slab)
{
    // no-op for debug allocator
}

size_t SlamCache::recovery(size_t maxSlabs)
{
    // no-op for debug allocator
    return 0;
}

SlamCache::Node *SlamCache::initialiseSlab(uintptr_t slab)
{
    // no-op for debug allocator
    return nullptr;
}

#if CRIPPLINGLY_VIGILANT
void SlamCache::check()
{
    // no-op for debug allocator
}

void SlamCache::trackSlab(uintptr_t slab)
{
    // no-op for debug allocator
}
#endif

SlamAllocator::SlamAllocator()
    : m_bInitialised(false), m_bVigilant(false),
#ifdef THREADS
      m_SlabRegionLock(false),
#endif
      m_HeapPageCount(0), m_SlabRegionBitmap(), m_SlabRegionBitmapEntries(0),
      m_Base(0)
{
}

SlamAllocator::~SlamAllocator()
{
    if (m_bInitialised)
    {
        wipe();
    }
}

void SlamAllocator::initialise()
{
    RecursingLockGuard<Spinlock> guard(m_Lock);

    if (m_bInitialised)
    {
        return;
    }

    m_Base = getHeapBase();
    m_bInitialised = true;
}

void SlamAllocator::wipe()
{
    // no-op for debug allocator
}

uintptr_t SlamAllocator::getSlab(size_t fullSize)
{
    // no-op for debug allocator
    return 0;
}

void SlamAllocator::freeSlab(uintptr_t address, size_t length)
{
    // no-op for debug allocator
}

size_t SlamAllocator::recovery(size_t maxSlabs)
{
    // no-op for debug allocator
    return 0;
}

uintptr_t SlamAllocator::allocate(size_t nBytes)
{
    if (!m_bInitialised)
    {
        initialise();
    }

    RecursingLockGuard<Spinlock> guard(m_Lock);

    if (!nBytes)
    {
        return 0;
    }

    size_t numPages = nBytes / getPageSize();
    if (nBytes % getPageSize())
    {
        ++numPages;
    }
    if (!numPages)
    {
        ++numPages;
    }
    size_t nTotalBytes = numPages * getPageSize();

    m_Base += getPageSize();  // gap between allocations
    uintptr_t mapStart = m_Base;
    m_Base += getPageSize(); // page for the allocation header (readonly once it's written to)
    uintptr_t result = m_Base;
    m_Base += numPages * getPageSize();
    uintptr_t mapEnd = m_Base;

    for (uintptr_t addr = mapStart; addr < mapEnd; addr += getPageSize())
    {
        allocateAndMapAt(reinterpret_cast<void *>(addr));
    }

    *((size_t *) (result - sizeof(size_t))) = numPages;

    // now that the size is written we can mark the header section readonly
    markReadOnly(reinterpret_cast<void *>(mapStart));

#ifdef THREADS
    if (Processor::m_Initialised == 2)
    {
        Thread *pThread = Processor::information().getCurrentThread();
        if (pThread)
        {
            pThread->getParent()->trackHeap(nTotalBytes);
        }
    }
#endif

#ifdef MEMORY_TRACING
    traceAllocation(
        reinterpret_cast<void *>(result), MemoryTracing::Allocation, nTotalBytes);
#endif

    return result;
}

size_t SlamAllocator::allocSize(uintptr_t mem)
{
    if (!m_bInitialised)
    {
        return 0;
    }

    if (!mem)
    {
        return 0;
    }

    return *((size_t *) (mem - sizeof(size_t))) * getPageSize();
}

void SlamAllocator::free(uintptr_t mem)
{
    RecursingLockGuard<Spinlock> guard(m_Lock);

    assert(m_bInitialised);

    if (!mem)
    {
        return;
    }

#ifdef MEMORY_TRACING
    // do this first so we can detect double frees before the asserts/memory
    // accesses below - this just helps a lot with tracing these issues after
    // the fact
    traceAllocation(reinterpret_cast<void *>(mem), MemoryTracing::Free, 0);
#endif

    assert(isMapped(reinterpret_cast<void *>(mem)));

    if (!isPointerValid(mem))
    {
        return;
    }

    size_t numPages = *((size_t *) (mem - sizeof(size_t)));
    size_t nBytes = numPages * getPageSize();

    uintptr_t unmapStart = mem - getPageSize();
    uintptr_t unmapEnd = mem + nBytes;

    for (uintptr_t addr = unmapStart; addr < unmapEnd; addr += getPageSize())
    {
        unmap(reinterpret_cast<void *>(addr));
    }

#ifdef THREADS
    if (Processor::m_Initialised == 2)
    {
        Thread *pThread = Processor::information().getCurrentThread();
        if (pThread)
        {
            pThread->getParent()->trackHeap(-nBytes);
        }
    }
#endif
}

bool SlamAllocator::isPointerValid(uintptr_t mem)
#if !SLAM_LOCKED
    const
#endif
{
    RecursingLockGuard<Spinlock> guard(m_Lock);

    if (!m_bInitialised)
    {
        return false;
    }

    // On the heap?
    if (!Processor::information().getVirtualAddressSpace().memIsInKernelHeap(
            reinterpret_cast<void *>(mem)))
    {
#if VERBOSE_ISPOINTERVALID
        WARNING("SlamAllocator::isPointerValid: memory " << Hex << mem << " is not in the heap region.");
#endif
        return false;
    }

    if (!isMapped(reinterpret_cast<void *>(mem)))
    {
#if VERBOSE_ISPOINTERVALID
        WARNING("SlamAllocator::isPointerValid: memory " << Hex << mem << " is not mapped [current base = " << Hex << m_Base << "].");
#endif
        if (mem >= m_Base)
        {
#if VERBOSE_ISPOINTERVALID
        WARNING(" (pointer being deleted is beyond the end of the heap somehow)");
#endif
        }
        return false;
    }

    return true;
}

bool SlamAllocator::isWithinHeap(uintptr_t mem) const
{
    if (!Processor::information().getVirtualAddressSpace().memIsInKernelHeap(
            reinterpret_cast<void *>(mem)))
    {
#if VERBOSE_ISPOINTERVALID
        WARNING("SlamAllocator::isWithinHeap: memory " << Hex << mem << " is not in the heap region.");
#endif
        return false;
    }

    return true;
}

bool _assert_ptr_valid(uintptr_t ptr)
{
    return SlamAllocator::instance().isPointerValid(ptr);
}
