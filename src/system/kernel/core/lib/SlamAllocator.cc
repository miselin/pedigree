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

#if !SLAM_USE_DEBUG_ALLOCATOR

#include "pedigree/kernel/core/SlamAllocator.h"
#include "pedigree/kernel/debugger/Backtrace.h"
#include "pedigree/kernel/debugger/commands/SlamCommand.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/MemoryTracing.h"
#include "pedigree/kernel/utilities/pocketknife.h"
#include "pedigree/kernel/utilities/utility.h"

#if MULTIPROCESSOR
#define ATOMIC_POP_MEMORY_ORDER __ATOMIC_ACQUIRE
#define ATOMIC_POP_FAILURE_MEMORY_ORDER __ATOMIC_ACQUIRE
#define ATOMIC_PUSH_MEMORY_ORDER __ATOMIC_RELEASE
#define ATOMIC_CAS_WEAK true
#else
#define ATOMIC_POP_MEMORY_ORDER __ATOMIC_RELAXED
#define ATOMIC_POP_FAILURE_MEMORY_ORDER __ATOMIC_RELAXED
#define ATOMIC_PUSH_MEMORY_ORDER __ATOMIC_RELAXED
#define ATOMIC_CAS_WEAK true
#endif

SlamAllocator SlamAllocator::m_Instance;

static constexpr uintptr_t POINTER_MASK = 0x0000FFFFFFFFFFFFULL;
static constexpr uintptr_t POINTER_TAG_MASK = ~POINTER_MASK;
static constexpr uintptr_t POINTER_TAG_INCREMENT = 0x0001000000000000ULL;

template <typename T>
inline T *untagged(T *p) PURE;

template <typename T>
inline T *tagged(T *p) PURE;

template <typename T>
inline T *next_tag(T *p, T *currentHead) PURE;

template <typename T>
inline T *untagged(T *p)
{
    /// \todo this now requires 64-bit pointers everywhere.
    // All heap pointers begin with 32 bits of ones. So we shove a tag there.
    uintptr_t ptr = reinterpret_cast<uintptr_t>(p);
    EMIT_IF(PEDIGREE_BENCHMARK || HOSTED)
    {
        // The upper 16 bits are available for hosted userspace addresses.
        ptr &= POINTER_MASK;
    }
    else
    {
        // Restore the upper 16 bits to make this a canonical kernel address.
        ptr |= 0xFFFF000000000000ULL;
    }
    return reinterpret_cast<T *>(ptr);
}

template <typename T>
inline T *tagged(T *p)
{
    uintptr_t ptr = reinterpret_cast<uintptr_t>(p);
    ptr &= POINTER_MASK;
    return reinterpret_cast<T *>(ptr);
}

template <typename T>
inline T *next_tag(T *p, T *currentHead)
{
    uintptr_t ptr = reinterpret_cast<uintptr_t>(p) & POINTER_MASK;
    uintptr_t tag =
        (reinterpret_cast<uintptr_t>(currentHead) + POINTER_TAG_INCREMENT) &
        POINTER_TAG_MASK;
    return reinterpret_cast<T *>(ptr | tag);
}

inline void spin_pause()
{
#if PEDIGREE_BENCHMARK
#if defined(__i386__) || defined(__x86_64__)
    asm("pause");
#elif defined(__aarch64__)
    asm("yield");
#endif
#else
    Processor::pause();
#endif
}

inline uintptr_t getHeapBase()
{
#if PEDIGREE_BENCHMARK
    return SlamSupport::getHeapBase();
#else
    return VirtualAddressSpace::getKernelAddressSpace().getKernelHeapStart();
#endif
}

inline uintptr_t getHeapEnd()
{
#if PEDIGREE_BENCHMARK
    return SlamSupport::getHeapEnd();
#else
    return VirtualAddressSpace::getKernelAddressSpace().getKernelHeapEnd();
#endif
}

inline size_t getPageSize()
{
#if PEDIGREE_BENCHMARK
    return 0x1000;
#else
    return PhysicalMemoryManager::getPageSize();
#endif
}

inline void allocateAndMapAt(void *addr, bool cowOk = false)
{
#if PEDIGREE_BENCHMARK
    SlamSupport::getPageAt(addr);
#else
    size_t standardFlags =
        VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write;

    static physical_uintptr_t physZero = 0;
    bool needZeroPage = false;
    size_t extraFlags = 0;

    physical_uintptr_t phys = 0;
    if (cowOk)
    {
        if (!physZero)
        {
            // allocate the zero page, we'll zero it shortly
            physZero = PhysicalMemoryManager::instance().allocatePage();
            needZeroPage = true;

            // allow us to zero out the page
            extraFlags |= VirtualAddressSpace::Write;
        }
        else
        {
            extraFlags |= VirtualAddressSpace::CopyOnWrite;
        }

        // disable writing (for CoW to work properly)
        standardFlags &= ~VirtualAddressSpace::Write;

        phys = physZero;
    }
    else
    {
        phys = PhysicalMemoryManager::instance().allocatePage();
    }

    VirtualAddressSpace &va = VirtualAddressSpace::getKernelAddressSpace();
    if (!va.map(phys, addr, standardFlags | extraFlags))
    {
        FATAL("SlamAllocator: failed to allocate and map at " << addr);
    }

    if (needZeroPage)
    {
        ByteSet(addr, 0, PhysicalMemoryManager::getPageSize());

        // Page zeroed - mark page copy on write now so the zero page works
        va.setFlags(addr, standardFlags | VirtualAddressSpace::CopyOnWrite);
    }
#endif
}

inline void unmap(void *addr)
{
#if PEDIGREE_BENCHMARK
    SlamSupport::unmapPage(addr);
// munmap(addr, getPageSize());
#else
    VirtualAddressSpace &va = VirtualAddressSpace::getKernelAddressSpace();
    if (!va.isMapped(addr))
        return;

    physical_uintptr_t phys;
    size_t flags;
    va.getMapping(addr, phys, flags);
    va.unmap(addr);

    PhysicalMemoryManager::instance().freePage(phys);
#endif
}

SlamCache::SlamCache()
    : m_PartialLists(), m_ObjectSize(0), m_SlabSize(0), m_FirstSlab(),
#if THREADS
      m_RecoveryLock(false),
#endif
      m_EmptyNode()
{
}

SlamCache::~SlamCache()
{
}

void SlamCache::initialise(SlamAllocator *parent, size_t objectSize)
{
    if (objectSize < OBJECT_MINIMUM_SIZE)
        return;

    m_ObjectSize = objectSize;
    if (m_ObjectSize > SLAB_MINIMUM_SIZE)
        m_SlabSize = m_ObjectSize;
    else
        m_SlabSize = SLAB_MINIMUM_SIZE;

    for (size_t i = 0; i < NUM_LISTS; i++)
        m_PartialLists[i] = tagged(&m_EmptyNode);

    // Make the empty node loop always, so it can be easily linked into place.
    ByteSet(&m_EmptyNode, 0xAB, sizeof(m_EmptyNode));
    m_EmptyNode.next = tagged(&m_EmptyNode);

    m_pParentAllocator = parent;

    assert((m_SlabSize % m_ObjectSize) == 0);
}

SlamCache::Node *SlamCache::pop(SlamCache::alignedNode *head)
{
    Node *N = 0, *pNext = 0;
    alignedNode currentHead = __atomic_load_n(head, ATOMIC_POP_MEMORY_ORDER);
    while (true)
    {
        // Grab result.
        N = untagged(const_cast<Node *>(currentHead));
        pNext = N->next;

        if (__atomic_compare_exchange_n(
                head, &currentHead,
                next_tag(pNext, const_cast<Node *>(currentHead)),
                ATOMIC_CAS_WEAK, ATOMIC_POP_MEMORY_ORDER,
                ATOMIC_POP_FAILURE_MEMORY_ORDER))
        {
            // Successful CAS, we have a node to use.
            break;
        }

        // Unsuccessful CAS, pause for a bit to back off.
        spin_pause();
    }

    return N;
}

void SlamCache::push(
    SlamCache::alignedNode *head, SlamCache::Node *newTail,
    SlamCache::Node *newHead)
{
    if (!newHead)
        newHead = newTail;

    alignedNode currentHead = __atomic_load_n(head, __ATOMIC_RELAXED);
    while (true)
    {
        newTail->next = const_cast<Node *>(currentHead);
        if (__atomic_compare_exchange_n(
                head, &currentHead,
                next_tag(newHead, const_cast<Node *>(currentHead)),
                ATOMIC_CAS_WEAK, ATOMIC_PUSH_MEMORY_ORDER, __ATOMIC_RELAXED))
        {
            break;
        }

        spin_pause();
    }
}

uintptr_t SlamCache::allocate()
{
    EMIT_IF(EVERY_ALLOCATION_IS_A_SLAB)
    {
        return getSlab();
    }

    EMIT_IF(SLABS_FOR_HUGE_ALLOCS)
    {
        if (m_ObjectSize >= getPageSize())
        {
            // just return a big-enough slab - allocation is page-sized or bigger
            return getSlab();
        }
    }

    size_t thisCpu = 0;
    EMIT_IF(MULTIPROCESSOR)
    {
        thisCpu = Processor::id();
    }

    Node *N = pop(&m_PartialLists[thisCpu]);

    // Something else got there first if N == 0. Just allocate a new slab.
    if (UNLIKELY(N == &m_EmptyNode))
    {
        Node *pNode = initialiseSlab(getSlab());
        uintptr_t slab = reinterpret_cast<uintptr_t>(pNode);
        EMIT_IF(CRIPPLINGLY_VIGILANT)
        {
            if (m_pParentAllocator->getVigilance())
                trackSlab(slab);
        }
        return slab;
    }

    // Check that the block was indeed free.
    assert(N->next != reinterpret_cast<Node *>(VIGILANT_MAGIC));
    EMIT_IF(USING_MAGIC)
    {
        assert(N->magic == TEMP_MAGIC || N->magic == MAGIC_VALUE);
        N->magic = TEMP_MAGIC;
    }

    return reinterpret_cast<uintptr_t>(N);
}

void SlamCache::free(uintptr_t object)
{
    EMIT_IF(EVERY_ALLOCATION_IS_A_SLAB)
    {
        // Free the slab in the address space, but don't return it to the allocator.
        size_t numPages = m_SlabSize / getPageSize();
        if (m_SlabSize % getPageSize())
        {
            ++numPages;
        }
        object = object & ~(getPageSize() - 1);
        for (size_t i = 0; i < numPages; ++i)
        {
            unmap(reinterpret_cast<void *>(object + (i * getPageSize())));
        }

        return;
    }

    EMIT_IF(SLABS_FOR_HUGE_ALLOCS)
    {
        if (m_ObjectSize >= getPageSize())
        {
            // just free the object directly, it's an entire slab
            freeSlab(object);
            return;
        }
    }

    size_t thisCpu = 0;
    EMIT_IF(MULTIPROCESSOR)
    {
        thisCpu = Processor::id();
    }

    Node *N = reinterpret_cast<Node *>(object);
    EMIT_IF(OVERRUN_CHECK)
    {
        // Grab the footer and check it.
        SlamAllocator::AllocFooter *pFoot =
            reinterpret_cast<SlamAllocator::AllocFooter *>(
                object + m_ObjectSize - sizeof(SlamAllocator::AllocFooter));
        assert(pFoot->magic == VIGILANT_MAGIC);
    }

    EMIT_IF(USING_MAGIC)
    {
        // Possible double free?
        assert(N->magic != MAGIC_VALUE);
        N->magic = MAGIC_VALUE;
    }

    push(&m_PartialLists[thisCpu], N);
}

bool SlamCache::isPointerValid(uintptr_t object) const
{
    EMIT_IF(SLABS_FOR_HUGE_ALLOCS)
    {
        if (m_ObjectSize >= getPageSize())
        {
            /// \todo need to figure out how to do this
            return true;
        }
    }

    Node *N = reinterpret_cast<Node *>(object);
    EMIT_IF(OVERRUN_CHECK)
    {
        // Grab the footer and check it.
        SlamAllocator::AllocFooter *pFoot =
            reinterpret_cast<SlamAllocator::AllocFooter *>(
                object + m_ObjectSize - sizeof(SlamAllocator::AllocFooter));
        if (pFoot->magic != VIGILANT_MAGIC)
        {
            return false;
        }
    }

    EMIT_IF(USING_MAGIC)
    {
        // Possible double free?
        if (N->magic == MAGIC_VALUE)
        {
            EMIT_IF(VERBOSE_ISPOINTERVALID)
            {
                WARNING(
                    "SlamCache::isPointerValid: memory "
                    << Hex << object << " has invalid magic (" << N->magic
                    << " != " << MAGIC_VALUE << ").");
            }
            return false;
        }
    }

    return true;
}

uintptr_t SlamCache::getSlab()
{
    return m_pParentAllocator->getSlab(m_SlabSize);
}

void SlamCache::freeSlab(uintptr_t slab)
{
    m_pParentAllocator->freeSlab(slab, m_SlabSize);
}

size_t SlamCache::recovery(size_t maxSlabs)
{
    EMIT_IF(EVERY_ALLOCATION_IS_A_SLAB)
    {
        return 0;
    }

    EMIT_IF(SLABS_FOR_HUGE_ALLOCS)
    {
        if (m_ObjectSize >= getPageSize())
        {
            // Caches with slabs page-sized or bigger don't hold onto freed regions
            return 0;
        }
    }

    EMIT_IF(MULTIPROCESSOR)
    {
        if (m_ObjectSize < getPageSize())
        {
            // A sub-page slab's free objects can be spread across CPU-local
            // lists. Reclaiming it without stopping the other CPUs would leave
            // dangling free-list entries pointing into unmapped memory.
            return 0;
        }
    }

    size_t thisCpu = 0;
    EMIT_IF(MULTIPROCESSOR)
    {
        thisCpu = Processor::id();
    }

    ConstexprLockGuard<Spinlock, THREADS> guard(m_RecoveryLock);

    if (untagged(m_PartialLists[thisCpu]) == &m_EmptyNode)
        return 0;

    size_t freedSlabs = 0;
    if (m_ObjectSize < getPageSize())
    {
        Node *reinsertHead = tagged(&m_EmptyNode);
        Node *reinsertTail = &m_EmptyNode;
        while (maxSlabs--)
        {
            // Grab the head node of the free list.
            Node *N = pop(&m_PartialLists[thisCpu]);

            // If no head node, we're done with this free list.
            if (N == &m_EmptyNode)
            {
                break;
            }

            uintptr_t slab =
                reinterpret_cast<uintptr_t>(N) & ~(getPageSize() - 1);

            // A possible node found! Any luck?
            bool bSlabNotFree = false;
            for (size_t i = 0; i < (m_SlabSize / m_ObjectSize); ++i)
            {
                Node *pNode =
                    reinterpret_cast<Node *>(slab + (i * m_ObjectSize));
                SlamAllocator::AllocHeader *pHeader =
                    reinterpret_cast<SlamAllocator::AllocHeader *>(pNode);
                if (pHeader->cache == this)
                {
                    // Oops, an active allocation was found.
                    bSlabNotFree = true;
                    break;
                }
                EMIT_IF(USING_MAGIC)
                {
                    if (pNode->magic != MAGIC_VALUE)
                    {
                        // Not free.
                        bSlabNotFree = true;
                        break;
                    }
                }
            }

            if (bSlabNotFree)
            {
                // Link the node into our reinsert lists, as the slab contains
                // in-use nodes.
                if (untagged(reinsertHead) == &m_EmptyNode)
                {
                    reinsertHead = tagged(N);
                    reinsertTail = N;
                    N->next = tagged(&m_EmptyNode);
                }
                else
                {
                    N->next = reinsertHead;
                    reinsertHead = tagged(N);
                }

                continue;
            }

            // Unlink any of our items that exist in the free list.
            // Yes, this is slow, but we've already stopped the world.
            alignedNode head = untagged(m_PartialLists[thisCpu]);
            alignedNode prev = nullptr;
            while (head != &m_EmptyNode)
            {
                bool overlaps =
                    ((head >= reinterpret_cast<void *>(slab)) &&
                     (head < reinterpret_cast<void *>(slab + m_SlabSize)));

                if (overlaps)
                {
                    Node *next = untagged(head->next);
                    if (prev)
                    {
                        prev->next = tagged(next);
                    }
                    else
                    {
                        Node *oldHead =
                            const_cast<Node *>(m_PartialLists[thisCpu]);
                        m_PartialLists[thisCpu] = next_tag(next, oldHead);
                    }
                }
                else
                {
                    prev = head;
                }

                head = untagged(head->next);
            }

            // Kill off the slab!
            freeSlab(slab);
            ++freedSlabs;
        }

        // Relink any nodes we decided we couldn't free. This must be done here
        // as the loop may terminate before we get a chance to do this.
        if (reinsertTail != &m_EmptyNode)
        {
            // Re-link the nodes we passed over.
            push(&m_PartialLists[thisCpu], reinsertTail, reinsertHead);
        }
    }
    else
    {
        while (maxSlabs--)
        {
            if (untagged(m_PartialLists[thisCpu]) == &m_EmptyNode)
                break;

            // Pop the first free node off the free list.
            Node *N = pop(&m_PartialLists[thisCpu]);
            if (N == &m_EmptyNode)
            {
                // Emptied the partial list!
                break;
            }

            EMIT_IF(USING_MAGIC)
            {
                assert(N->magic == MAGIC_VALUE);
            }

            // Can just outright free - no need to do any further checks.
            uintptr_t slab = reinterpret_cast<uintptr_t>(N);

            freeSlab(slab);
            ++freedSlabs;
        }
    }

    return freedSlabs;
}

SlamCache::Node *SlamCache::initialiseSlab(uintptr_t slab)
{
    EMIT_IF(SLABS_FOR_HUGE_ALLOCS)
    {
        if (m_ObjectSize >= getPageSize())
        {
            return nullptr;
        }
    }

    size_t thisCpu = 0;
    EMIT_IF(MULTIPROCESSOR)
    {
        thisCpu = Processor::id();
    }

    size_t nObjects = m_SlabSize / m_ObjectSize;

    Node *N = reinterpret_cast<Node *>(slab);
    N->next = tagged(&m_EmptyNode);
    EMIT_IF(USING_MAGIC)
    {
        N->magic = TEMP_MAGIC;
    }

    // Early exit if there's no other free objects in this slab.
    if (nObjects <= 1)
        return N;

    // All objects in slab are free, generate Node*'s for each (except the
    // first) and link them together.
    Node *pFirst = 0, *pLast = 0;
    for (size_t i = 1; i < nObjects; i++)
    {
        Node *pNode = reinterpret_cast<Node *>(slab + (i * m_ObjectSize));
        pNode->next = reinterpret_cast<Node *>(slab + ((i + 1) * m_ObjectSize));
        pNode->next = tagged(pNode->next);
        EMIT_IF(USING_MAGIC)
        {
            pNode->magic = MAGIC_VALUE;
        }

        if (!pFirst)
            pFirst = tagged(pNode);

        pLast = pNode;
    }

    N->next = pFirst;

    push(&m_PartialLists[thisCpu], pLast, pFirst);

    return N;
}

static Spinlock rarp;

void SlamCache::check()
{
    if (m_ObjectSize >= getPageSize())
    {
        return;
    }

    EMIT_IF(!HOSTED)
    {
        if (!Machine::instance().isInitialised() || Processor::m_Initialised != 2)
            return;
    }
    if (m_ObjectSize == 0)
        return;
    rarp.acquire();

    size_t nObjects = m_SlabSize / m_ObjectSize;

    size_t maxPerSlab = (m_SlabSize / sizeof(uintptr_t)) - 2;

    uintptr_t curSlab = m_FirstSlab;
    while (true)
    {
        if (!curSlab)
        {
            rarp.release();
            return;
        }
        uintptr_t numAlloced = *reinterpret_cast<uintptr_t *>(curSlab);
        uintptr_t next =
            *reinterpret_cast<uintptr_t *>(curSlab + sizeof(uintptr_t));

        for (size_t i = 0; i < numAlloced; i++)
        {
            uintptr_t slab = *reinterpret_cast<uintptr_t *>(
                curSlab + sizeof(uintptr_t) * (i + 2));
            for (size_t i = 0; i < nObjects; i++)
            {
                uintptr_t addr = slab + i * m_ObjectSize;
                Node *pNode = reinterpret_cast<Node *>(addr);
                if (pNode->magic == MAGIC_VALUE || pNode->magic == TEMP_MAGIC)
                    // Free, continue.
                    continue;
                SlamAllocator::AllocHeader *pHead =
                    reinterpret_cast<SlamAllocator::AllocHeader *>(addr);
                SlamAllocator::AllocFooter *pFoot =
                    reinterpret_cast<SlamAllocator::AllocFooter *>(
                        addr + m_ObjectSize -
                        sizeof(SlamAllocator::AllocFooter));
                if (pHead->magic != VIGILANT_MAGIC)
                {
                    ERROR(
                        "Possible heap underrun: object starts at "
                        << addr << ", size: " << m_ObjectSize << ", block: "
                        << (addr + sizeof(SlamAllocator::AllocHeader)));
                }
                if (pFoot->magic != VIGILANT_MAGIC)
                {
                    ERROR("Possible heap overrun: object starts at " << addr);
                    assert(false);
                }
            }
        }
        if (numAlloced == maxPerSlab)
            curSlab = next;
        else
            break;
    }
    rarp.release();
}

void SlamCache::trackSlab(uintptr_t slab)
{
    EMIT_IF(!HOSTED)
    {
        if (!Machine::instance().isInitialised() || Processor::m_Initialised != 2)
            return;
    }
    if (m_ObjectSize == 0)
        return;

    if (!m_FirstSlab)
    {
        m_FirstSlab = getSlab();
        uintptr_t *numAlloced = reinterpret_cast<uintptr_t *>(m_FirstSlab);
        uintptr_t *next =
            reinterpret_cast<uintptr_t *>(m_FirstSlab + sizeof(uintptr_t));
        *numAlloced = 0;
        *next = 0;
    }

    size_t maxPerSlab = (m_SlabSize / sizeof(uintptr_t)) - 2;

    uintptr_t curSlab = m_FirstSlab;
    while (true)
    {
        uintptr_t *numAlloced = reinterpret_cast<uintptr_t *>(curSlab);
        uintptr_t *next =
            reinterpret_cast<uintptr_t *>(curSlab + sizeof(uintptr_t));

        if (*numAlloced < maxPerSlab)
        {
            uintptr_t *p = reinterpret_cast<uintptr_t *>(
                curSlab + (*numAlloced + 2) * sizeof(uintptr_t));
            *p = slab;
            *numAlloced = *numAlloced + 1;
            return;
        }

        if (*next)
            curSlab = *next;
        else
        {
            uintptr_t newSlab = getSlab();
            *next = newSlab;
            curSlab = newSlab;

            uintptr_t *numAlloced = reinterpret_cast<uintptr_t *>(curSlab);
            uintptr_t *next =
                reinterpret_cast<uintptr_t *>(curSlab + sizeof(uintptr_t));
            *numAlloced = 0;
            *next = 0;
        }
    }
}

SlamAllocator::SlamAllocator()
    : m_bInitialised(false), m_bVigilant(false), m_SlabRegionLock(false),
      m_HeapPageCount(0), m_SlabRegionBitmap(), m_SlabRegionBitmapEntries(0),
      m_SlabRegionPages(0), m_Base(0)
{
}

SlamAllocator::~SlamAllocator()
{
    if (m_bInitialised)
    {
        // wipe();
    }
}

void SlamAllocator::initialise()
{
    ConstexprLockGuard<Spinlock, THREADS> guard(m_SlabRegionLock);

    if (m_bInitialised)
    {
        return;
    }

    // We need to allocate our bitmap for this purpose.
    uintptr_t bitmapBase = getHeapBase();
    uintptr_t heapEnd = getHeapEnd();
    size_t heapSize = heapEnd - bitmapBase;
    size_t heapPages = heapSize / getPageSize();
    size_t bitmapBytes = (heapPages + 7) / 8;

    m_SlabRegionBitmap = reinterpret_cast<uint64_t *>(bitmapBase);

    // Ensure the bitmap size is now page-aligned before we allocate it.
    if (bitmapBytes & (getPageSize() - 1))
    {
        bitmapBytes &= ~(getPageSize() - 1);
        bitmapBytes += getPageSize();
    }

    m_Base = bitmapBase + bitmapBytes;
    m_SlabRegionPages = (heapEnd - m_Base) / getPageSize();
    m_SlabRegionBitmapEntries = (m_SlabRegionPages + 63) / 64;

    pocketknife::VirtualAddressSpaceSwitch vaswitch;

    // Allocate bitmap.
    size_t numPages = 0;
    for (uintptr_t addr = bitmapBase; addr < m_Base; addr += getPageSize())
    {
        // Don't CoW the first 32 pages so we have some slabs on hand for
        // startup before CoW is viable
        bool cowOk = numPages++ >= 32;
        allocateAndMapAt(reinterpret_cast<void *>(addr), cowOk);
        if (!cowOk)
        {
            ByteSet(reinterpret_cast<void *>(addr), 0, getPageSize());
        }
    }

    vaswitch.restore();

    EMIT_IF(!PEDIGREE_BENCHMARK)
    {
        NOTICE(
            "Kernel heap range prepared from " << Hex << m_Base << " to " << heapEnd
                                               << ", size: " << (heapEnd - m_Base));
        DEBUG_LOG(
            "  -> kernel heap bitmap is " << Dec << (bitmapBytes / 1024) << Hex
                                          << "K");
    }

    for (size_t i = 0; i < 32; i++)
    {
        m_Caches[i].initialise(this, 1ULL << i);
    }

    NOTICE("all caches init-ed");

    m_bInitialised = true;
}

void SlamAllocator::clearAll()
{
    EMIT_IF(PEDIGREE_BENCHMARK)
    {
        wipe();
        initialise();
    }
}

void SlamAllocator::wipe()
{
    if (!m_bInitialised)
    {
        return;
    }

    if (!m_SlabRegionBitmap)
    {
        return;
    }

    ConstexprLockGuard<Spinlock, THREADS> guard(m_SlabRegionLock);

    m_bInitialised = false;

    // Clean up all slabs we obtained.
    for (size_t entry = 0; entry < m_SlabRegionBitmapEntries; ++entry)
    {
        if (!m_SlabRegionBitmap[entry])
        {
            continue;
        }

        for (size_t bit = 0; bit < 64; ++bit)
        {
            uint64_t test = 1ULL << bit;
            if ((m_SlabRegionBitmap[entry] & test) == 0)
            {
                continue;
            }

            uintptr_t slab = m_Base + (((entry * 64) + bit) * getPageSize());
            freeSlabUnlocked(slab, getPageSize());
        }
    }

    // about to destroy the bitmap mappings
    m_SlabRegionBitmap = nullptr;
    m_SlabRegionBitmapEntries = 0;
    m_SlabRegionPages = 0;

    // Clean up the bitmap.
    for (uintptr_t addr = getHeapBase(); addr < m_Base; addr += getPageSize())
    {
        unmap(reinterpret_cast<void *>(addr));
    }
}

uintptr_t SlamAllocator::getSlab(size_t fullSize)
{
    if (fullSize < getPageSize() || (fullSize % getPageSize()))
    {
        panic("Attempted to get a slab smaller than the native page size.");
    }
    size_t nPages = fullSize / getPageSize();

    EMIT_IF(THREADS)
    {
        m_SlabRegionLock.acquire();
    }

    // Try to find space for this allocation.
    size_t pageIndex = ~0UL;
    size_t runStart = 0;
    size_t runLength = 0;
    for (size_t entry = 0; entry < m_SlabRegionBitmapEntries; ++entry)
    {
        const size_t entryBase = entry * 64;
        const size_t bitsInEntry = pedigree_std::min(
            static_cast<size_t>(64), m_SlabRegionPages - entryBase);
        uint64_t bitmap = m_SlabRegionBitmap[entry];

        if (!bitmap)
        {
            if (!runLength)
            {
                runStart = entryBase;
            }
            runLength += bitsInEntry;
            if (runLength >= nPages)
            {
                pageIndex = runStart;
                break;
            }
            continue;
        }

        for (size_t bit = 0; bit < bitsInEntry; ++bit)
        {
            if (bitmap & (1ULL << bit))
            {
                runLength = 0;
                continue;
            }

            if (!runLength)
            {
                runStart = entryBase + bit;
            }
            if (++runLength >= nPages)
            {
                pageIndex = runStart;
                break;
            }
        }

        if (pageIndex != ~0UL)
        {
            break;
        }
    }

    if (pageIndex == ~0UL)
    {
        panic("SlamAllocator cannot find contiguous virtual heap space.");
    }

    uintptr_t slab = m_Base + (pageIndex * getPageSize());

    // Map and mark as used.
    for (size_t i = 0; i < nPages; ++i)
    {
        size_t currentPage = pageIndex + i;
        m_SlabRegionBitmap[currentPage / 64] |=
            1ULL << (currentPage % 64);
    }

    m_HeapPageCount += nPages;

    EMIT_IF(THREADS)
    {
        // Now that we've marked the slab bits as used, we can map the pages.
        m_SlabRegionLock.release();
    }

    pocketknife::VirtualAddressSpaceSwitch vaswitch;

    // Map. This could break as we're allocating physical memory; though we are
    // free of the lock so that helps.
    for (size_t i = 0; i < nPages; ++i)
    {
        void *p = reinterpret_cast<void *>(slab + (i * getPageSize()));
        allocateAndMapAt(p);
    }

    vaswitch.restore();

    return slab;
}

void SlamAllocator::freeSlab(uintptr_t address, size_t length)
{
    ConstexprLockGuard<Spinlock, THREADS> guard(m_SlabRegionLock);

    freeSlabUnlocked(address, length);
}

void SlamAllocator::freeSlabUnlocked(uintptr_t address, size_t length)
{
    if (
        length < getPageSize() || (length % getPageSize()) ||
        (address % getPageSize()) || address < m_Base ||
        address >= getHeapEnd() || length > (getHeapEnd() - address))
    {
        panic("Attempted to free an invalid slab.");
    }
    size_t nPages = length / getPageSize();
    size_t firstPage = (address - m_Base) / getPageSize();
    if (firstPage >= m_SlabRegionPages ||
        nPages > (m_SlabRegionPages - firstPage))
    {
        panic("Attempted to free a slab outside the allocator bitmap.");
    }

    for (size_t i = 0; i < nPages; ++i)
    {
        size_t currentPage = firstPage + i;
        if (!(m_SlabRegionBitmap[currentPage / 64] &
              (1ULL << (currentPage % 64))))
        {
            panic("Attempted to free an unallocated slab.");
        }
    }

    // Perform unmapping first (so we can just modify 'address').
    pocketknife::VirtualAddressSpaceSwitch vaswitch;

    for (uintptr_t base = address; base < (address + length);
         base += getPageSize())
    {
        void *p = reinterpret_cast<void *>(base);
        unmap(p);
    }

    vaswitch.restore();

    // Adjust bitmap.
    for (size_t i = 0; i < nPages; ++i)
    {
        size_t currentPage = firstPage + i;
        m_SlabRegionBitmap[currentPage / 64] &=
            ~(1ULL << (currentPage % 64));
    }

    m_HeapPageCount -= nPages;
}

size_t SlamAllocator::recovery(size_t maxSlabs)
{
    size_t nSlabs = 0;
    size_t nPages = 0;

    for (size_t i = 0; i < 32; ++i)
    {
        // Things without slabs don't get recovered.
        if (!m_Caches[i].slabSize())
            continue;

        size_t thisSlabs = m_Caches[i].recovery(maxSlabs - nSlabs);
        nPages += (thisSlabs * m_Caches[i].slabSize()) / getPageSize();
        nSlabs += thisSlabs;
        if (nSlabs >= maxSlabs)
        {
            break;
        }
    }

    return nPages;
}

uintptr_t SlamAllocator::allocate(size_t nBytes)
{
    EMIT_IF(HOSTED_SYSTEM_MALLOC)
    {
        FATAL_NOLOCK("SlamAllocator::allocate() called when HOSTED_SYSTEM_MALLOC == 1");
    }

    EMIT_IF(DEBUGGING_SLAB_ALLOCATOR)
    {
        NOTICE_NOLOCK("SlabAllocator::allocate(" << Dec << nBytes << Hex << ")");
    }

    ConstexprLockGuard<Spinlock, SLAM_LOCKED> guard(m_Lock);

    if (UNLIKELY(!m_bInitialised))
        initialise();

    EMIT_IF(CRIPPLINGLY_VIGILANT)
    {
        if (m_bVigilant)
        {
            for (int i = 0; i < 32; i++)
            {
                m_Caches[i].check();
            }
        }
    }

    size_t origSize = nBytes;

    // Return value.
    uintptr_t ret = 0;

    // Don't allow huge allocations.
    /// \note Even 2G is a stretch on most systems. Use some other allocator
    ///       to allocate such large buffers.
    const size_t framing = sizeof(AllocHeader) + sizeof(AllocFooter);
    if (nBytes >= (1ULL << 31) || nBytes > ((1ULL << 31) - 1 - framing))
    {
        ERROR("SlamAllocator: massive allocation: " << origSize);
        panic("SlamAllocator allocation size is too large.");
    }

    nBytes += framing;

    // Default to minimum object size if we must.
    size_t lg2 = 0;
    if (UNLIKELY(nBytes < OBJECT_MINIMUM_SIZE))
    {
        nBytes = OBJECT_MINIMUM_SIZE;
    }

    // log2 of nBytes, where nBytes is rounded up to the next power-of-two.
    lg2 = 32 - __builtin_clz(static_cast<unsigned int>(nBytes - 1));
    nBytes = 1ULL << lg2;  // Round up nBytes now.
    ret = m_Caches[lg2].allocate();

    EMIT_IF(WARN_PAGE_SIZE_OR_LARGER)
    {
        // Does the allocation fit inside a slab?
        // NOTE: use something else to allocate 4K or more.
        if (nBytes >= getPageSize())
        {
#if __GNUC__ && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wframe-address"
#endif
            // return address of operator new()
            void *ret0 = __builtin_return_address(0);
            void *ret1 = __builtin_return_address(1);
            ERROR(
                "alloc of " << origSize << " rounded to " << nBytes
                            << " exceeds page size [at " << ret0 << " " << ret1
                            << "]!");
#if __GNUC__ && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        }
    }

    EMIT_IF(DEBUGGING_SLAB_ALLOCATOR)
    {
        if (UNLIKELY(!ret))
        {
            ERROR_NOLOCK(
                "SlabAllocator::allocate: Allocation failed (" << Dec << nBytes
                                                               << Hex << " bytes)");
            return ret;
        }
    }
    else
    {
        assert(ret != 0);
    }

    // Shove some data on the front that we'll use later
    AllocHeader *head = reinterpret_cast<AllocHeader *>(ret);
    AllocFooter *foot =
        reinterpret_cast<AllocFooter *>(ret + nBytes - sizeof(AllocFooter));
    ret += sizeof(AllocHeader);

    // Set up the header
    head->cache = &m_Caches[lg2];
    EMIT_IF(OVERRUN_CHECK)
    {
        head->magic = VIGILANT_MAGIC;
        foot->magic = VIGILANT_MAGIC;

        EMIT_IF(VIGILANT_OVERRUN_CHECK)
        {
            // safe cast in this case as there's an inheritance here
            auto vigilantHead = reinterpret_cast<AllocHeader_VigilantOverrunCheck *>(head);
            if (Processor::m_Initialised == 2)
            {
                Backtrace bt;
                bt.performBpBacktrace(0, 0);
                MemoryCopy(
                    &vigilantHead->backtrace, bt.m_pReturnAddresses,
                    SLAM_BT_FRAMES * sizeof(uintptr_t));
                vigilantHead->requested = nBytes;
                g_SlamCommand.addAllocation(vigilantHead->backtrace, vigilantHead->requested);
            }
        }
    }

    EMIT_IF(THREADS)
    {
        if (Processor::m_Initialised == 2)
        {
            Thread *pThread = Processor::information().getCurrentThread();
            if (pThread)
            {
                pThread->getParent()->trackHeap(nBytes);
            }
        }
    }

    EMIT_IF(MEMORY_TRACING)
    {
        traceAllocation(
            reinterpret_cast<void *>(ret), MemoryTracing::Allocation, origSize);
    }

    return ret;
}

size_t SlamAllocator::allocSize(uintptr_t mem)
{
    if (!mem)
        return 0;

    // Grab the header
    AllocHeader *head =
        reinterpret_cast<AllocHeader *>(mem - sizeof(AllocHeader));

    // If the cache is null, then the pointer is corrupted.
    assert(head->cache != 0);
    size_t result = head->cache->objectSize();

    // Remove size of header/footer.
    // This is important as we're returning the size of each object itself,
    // but we return memory framed by headers and footers. So, the "true" size
    // of memory pointed to by 'mem' is not the true object size.
    return result - (sizeof(AllocHeader) + sizeof(AllocFooter));
}

bool SlamAllocator::isAllocatedPage(uintptr_t address) const
{
    if (
        !m_bInitialised || !m_SlabRegionBitmap || address < m_Base ||
        address >= getHeapEnd())
    {
        return false;
    }

    size_t page = (address - m_Base) / getPageSize();
    if (page >= m_SlabRegionPages)
    {
        return false;
    }

    uint64_t bitmap = __atomic_load_n(
        &m_SlabRegionBitmap[page / 64], __ATOMIC_ACQUIRE);
    return bitmap & (1ULL << (page % 64));
}

SlamAllocator &SlamAllocator::instance()
{
    EMIT_IF(PEDIGREE_BENCHMARK)
    {
        static SlamAllocator instance;
        return instance;
    }
    else
    {
        return m_Instance;
    }
}

void SlamAllocator::free(uintptr_t mem)
{
#if DEBUGGING_SLAB_ALLOCATOR
    NOTICE_NOLOCK("SlabAllocator::free");
#endif

#if SLAM_LOCKED
    LockGuard<Spinlock> guard(m_Lock);
#endif

    // If we're not initialised, fix that
    if (UNLIKELY(!m_bInitialised))
        initialise();
    if (UNLIKELY(!mem))
        return;

#if CRIPPLINGLY_VIGILANT
    if (m_bVigilant)
        for (int i = 0; i < 32; i++)
            m_Caches[i].check();
#endif

// Ensure this pointer is even on the heap...
#if !PEDIGREE_BENCHMARK
    if (!Processor::information().getVirtualAddressSpace().memIsInKernelHeap(
            reinterpret_cast<void *>(mem)))
        FATAL_NOLOCK(
            "SlamAllocator::free - given pointer '"
            << mem << "' was completely invalid.");
#endif

    // Grab the header
    AllocHeader *head =
        reinterpret_cast<AllocHeader *>(mem - sizeof(AllocHeader));

    // If the cache is null, then the pointer is corrupted.
    assert(head->cache != 0);
#if OVERRUN_CHECK
    assert(head->magic == VIGILANT_MAGIC);
    // Footer gets checked in SlamCache::free, as we don't know the object size.

#if VIGILANT_OVERRUN_CHECK
    if (Processor::m_Initialised == 2)
        g_SlamCommand.removeAllocation(head->backtrace, head->requested);
#endif
#endif

    SlamCache *pCache = head->cache;
    head->cache = 0;  // Wipe out the cache - freed page.

// Scribble the freed buffer (both to avoid leaking information, and also
// to ensure anything using a freed object will absolutely fail).
#if SCRIBBLE_FREED_BLOCKS
    size_t size =
        pCache->objectSize() - sizeof(AllocHeader) - sizeof(AllocFooter);
    ByteSet(reinterpret_cast<void *>(mem), 0xAB, size);
#endif

#if THREADS
    if (Processor::m_Initialised == 2)
    {
        Thread *pThread = Processor::information().getCurrentThread();
        if (pThread)
        {
            pThread->getParent()->trackHeap(-pCache->objectSize());
        }
    }
#endif

    // Free now.
    pCache->free(mem - sizeof(AllocHeader));

#if MEMORY_TRACING
    traceAllocation(reinterpret_cast<void *>(mem), MemoryTracing::Free, 0);
#endif
}

bool SlamAllocator::isPointerValid(uintptr_t mem)
#if !SLAM_LOCKED
    const
#endif
{
#if DEBUGGING_SLAB_ALLOCATOR
    NOTICE_NOLOCK("SlabAllocator::isPointerValid");
#endif

#if SLAM_LOCKED
    LockGuard<Spinlock> guard(m_Lock);
#endif

    // If we're not initialised, fix that
    if (UNLIKELY(!m_bInitialised))
    {
        return false;
    }

    // 0 is fine to free.
    if (!mem)
    {
        return true;
    }

// On the heap?
#if !PEDIGREE_BENCHMARK
    if (!Processor::information().getVirtualAddressSpace().memIsInKernelHeap(
            reinterpret_cast<void *>(mem)))
    {
#if VERBOSE_ISPOINTERVALID
        WARNING(
            "SlamAllocator::isPointerValid: memory "
            << Hex << mem << " is not in the heap region.");
#endif
        return false;
    }
#endif

    if (mem < (m_Base + sizeof(AllocHeader)))
    {
        return false;
    }

    uintptr_t headerAddress = mem - sizeof(AllocHeader);
    if (!isAllocatedPage(headerAddress))
    {
        return false;
    }

#if CRIPPLINGLY_VIGILANT
    if (m_bVigilant)
        for (int i = 0; i < 32; i++)
            m_Caches[i].check();
#endif

    // Grab the header
    AllocHeader *head = reinterpret_cast<AllocHeader *>(headerAddress);

#if OVERRUN_CHECK
    if (head->magic != VIGILANT_MAGIC)
    {
#if VERBOSE_ISPOINTERVALID
        WARNING(
            "SlamAllocator::isPointerValid: memory "
            << Hex << mem << " failed magic check (" << head->magic
            << " != " << VIGILANT_MAGIC << ").");
#endif
        return false;
    }
// Footer gets checked in SlamCache::free, as we don't know the object size.
#endif

    // If the cache is null, then the pointer is corrupted.
    if (head->cache == 0)
    {
#if VERBOSE_ISPOINTERVALID
        WARNING(
            "SlamAllocator::isPointerValid: memory "
            << Hex << mem << " does not reference a valid SlamCache.");
#endif
        return false;
    }

    // Check for a valid cache
    bool bValid = false;
    for (int i = 0; i < 32; i++)
    {
        if (head->cache == &m_Caches[i])
        {
            bValid = true;
            break;
        }
    }

    if (!bValid)
    {
        WARNING_NOLOCK(
            "SlamAllocator::isPointerValid - cache pointer '"
            << reinterpret_cast<uintptr_t>(head->cache) << "' is invalid.");
        return false;
    }

    // Final validation.
    return head->cache->isPointerValid(mem - sizeof(AllocHeader));
}

bool SlamAllocator::isWithinHeap(uintptr_t mem) const
{
#if !PEDIGREE_BENCHMARK
    if (!Processor::information().getVirtualAddressSpace().memIsInKernelHeap(
            reinterpret_cast<void *>(mem)))
    {
#if VERBOSE_ISPOINTERVALID
        WARNING(
            "SlamAllocator::isWithinHeap: memory "
            << Hex << mem << " is not in the heap region.");
#endif
        return false;
    }
#endif

    return true;
}

bool _assert_ptr_valid(uintptr_t ptr)
{
    return SlamAllocator::instance().isPointerValid(ptr);
}

#endif  // !SLAM_USE_DEBUG_ALLOCATOR
