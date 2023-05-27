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

#ifndef SLAM_ALLOCATOR_H
#define SLAM_ALLOCATOR_H

/** The SLAM allocator (SLAB à la James Molloy) is based on the Slab
    allocator (Bonwick94).

    \see http://www.pedigree-project.org/r/projects/pedigree/wiki/SlabDraft
**/

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"

#if PEDIGREE_BENCHMARK
namespace SlamSupport
{
uintptr_t getHeapBase();
uintptr_t getHeapEnd();
void getPageAt(void *addr);
void unmapPage(void *page);
void unmapAll();
}  // namespace SlamSupport
#endif

class SlamAllocator;
class SlamCache;

/// Size of each slab in 4096-byte pages
#define SLAB_SIZE 1

/// Minimum slab size in bytes
#define SLAB_MINIMUM_SIZE (4096 * SLAB_SIZE)

/// Define if using the magic number method of slab recovery.
/// This turns recovery into an O(n) instead of O(n^2) algorithm,
/// but relies on a magic number which introduces false positives
/// (depending on number length and value), and requires a
/// doubly linked list instead of a singly.
#define USING_MAGIC 1

/// Used only if USING_MAGIC. Type of the magic number.
#define MAGIC_TYPE uintptr_t

/// Used only if USING_MAGIC. Magic value.
#define MAGIC_VALUE 0xb00b1e55ULL

/// Minimum size of an object.
#define ABSOLUTE_MINIMUM_SIZE 64
#define ALL_HEADERS_SIZE                                            \
    (sizeof(SlamCache::Node) + sizeof(SlamAllocator::AllocHeader) + \
     sizeof(SlamAllocator::AllocFooter))
#define OBJECT_MINIMUM_SIZE                                             \
    (ALL_HEADERS_SIZE < ABSOLUTE_MINIMUM_SIZE ? ABSOLUTE_MINIMUM_SIZE : \
                                                ALL_HEADERS_SIZE)

/// Outputs information during each function call
#define DEBUGGING_SLAB_ALLOCATOR 0

/// Temporary magic used during allocation.
#define TEMP_MAGIC 0x67845753

/// Adds magic numbers to the start of free blocks, to check for
/// buffer overruns.
#if USE_DEBUG_ALLOCATOR
#define OVERRUN_CHECK 0
#else
#define OVERRUN_CHECK 1
#endif

/// Adds magic numbers to the start and end of allocated chunks, increasing
/// object size. Also adds a small amount of backtrace information.
#define VIGILANT_OVERRUN_CHECK 0

#define VIGILANT_MAGIC 0x1337cafe

/// This will check EVERY object on EVERY alloc/free.
/// It will cripple your performance.
#define CRIPPLINGLY_VIGILANT 0

/// If you're using a modified version of Bochs which supports magic
/// watchpoints (xchg cx, cx), this will set and remove watchpoints
/// on all allocations. This means you find out exactly where each
/// overrun occurs (EIP and all) rather than guessing.
#define BOCHS_MAGIC_WATCHPOINTS 0

/// Scribble in freed memory; can be useful for finding bugs which are caused
/// by reuse of freed objects (that would otherwise look like valid objects).
/// It can also avoid leaking information in heap objects.
#define SCRIBBLE_FREED_BLOCKS 1

/// Block allocations larger than or equal to the native page size.
#define WARN_PAGE_SIZE_OR_LARGER 0

/// Return slabs directly for allocations page sized or above.
/// This will result in better memory usage but does not hold onto freed blocks
/// so can be slow if allocation patterns include a large number of allocations
/// and frees of blocks bigger than the native page size.
#define SLABS_FOR_HUGE_ALLOCS 0

/// Be verbose about reasons for invalidity in isPointerValid
#define VERBOSE_ISPOINTERVALID 0

/// Turn every allocation into a slab, and unmap it without returning when
/// freeing. This makes the kernel heap enormous (as it never truly frees
/// address space), but allows for rapid discovery of use-after-frees as the
/// freed allocations are completely unmapped.
#define EVERY_ALLOCATION_IS_A_SLAB 0

#ifndef SLAM_USE_DEBUG_ALLOCATOR
/// Define this to enable the debug allocator (which is basically placement new).
#define SLAM_USE_DEBUG_ALLOCATOR 0
#endif

/// Use a lock to protect SlamAllocator. This comes with a performance cost,
/// but guarantees only one thread is ever in the allocator at one time.
#define SLAM_LOCKED SLAM_USE_DEBUG_ALLOCATOR ? 1 : 0  // need the lock for the debug allocator only

#ifndef SLAM_BT_FRAMES
/// Number of frames to include in allocation header backtraces.
#define SLAM_BT_FRAMES 3
#endif

/** A cache allocates objects of a constant size. */
class SlamCache
{
    // struct Node must be public so that sizeof(SlamCache::Node) is available.
  public:
    /** The structure inside a free object (list node) */
    struct Node
    {
        Node *next;
#if USING_MAGIC
        MAGIC_TYPE magic;
#endif
    } __attribute__((aligned(16)));

    /** Default constructor, does nothing. */
    SlamCache();
    /** Destructor is not designed to be called. There is no cleanup,
        this is a kernel heap! */
    virtual ~SlamCache();

    /** Main init function. */
    void initialise(SlamAllocator *parent, size_t objectSize);

    /** Allocates an object. */
    uintptr_t allocate();

    /** Frees an object. */
    void free(uintptr_t object);

    /** Attempt to recover slabs from this cache. */
    size_t recovery(size_t maxSlabs);

    bool isPointerValid(uintptr_t object) const;

    inline size_t objectSize() const
    {
        return m_ObjectSize;
    }

    inline size_t slabSize() const
    {
        return m_SlabSize;
    }

    void trackSlab(uintptr_t slab);
    void check();

  private:
    SlamCache(const SlamCache &);
    const SlamCache &operator=(const SlamCache &);

    static constexpr const int NUM_LISTS = MULTIPROCESSOR ? 255 : 1;

    typedef volatile Node *alignedNode;
    alignedNode m_PartialLists[NUM_LISTS];

    Node *pop(alignedNode *head);
    /* newHead = 0 to use newTail. */
    void push(alignedNode *head, Node *newTail, Node *newHead = 0);

    uintptr_t getSlab();
    void freeSlab(uintptr_t slab);

    Node *initialiseSlab(uintptr_t slab);

    size_t m_ObjectSize;
    size_t m_SlabSize;

    // This version of the allocator doesn't have a free list, instead
    // the reap() function returns memory directly to the VMM. This
    // avoids needing to lock the free list on MP systems.

    uintptr_t m_FirstSlab;

    /**
     * Recovery cannot be done trivially.
     * Spinlock disables interrupts as part of its operation, so we can
     * use it to ensure recovery isn't interrupted. Note recovery is a
     * per-CPU thing.
     */
    Spinlock m_RecoveryLock;

    /** Pointer back to the associated SlamAllocator. */
    SlamAllocator *m_pParentAllocator;

    struct Node m_EmptyNode;
};

class SlamAllocator
{
  public:
    SlamAllocator();
    virtual ~SlamAllocator();

    void initialise();

    // quickly clear all allocations from the allocator
    void clearAll();

    uintptr_t allocate(size_t nBytes);
    void free(uintptr_t mem);

    size_t recovery(size_t maxSlabs = 1);

    bool isPointerValid(uintptr_t mem)
#if !SLAM_LOCKED
        const
#endif
        ;
    bool isWithinHeap(uintptr_t mem) const;

    size_t allocSize(uintptr_t mem);

    static EXPORTED_PUBLIC SlamAllocator &instance();

    size_t heapPageCount() const
    {
        return m_HeapPageCount;
    }

    uintptr_t getSlab(size_t fullSize);
    void freeSlab(uintptr_t address, size_t length);

    size_t headerSize() const
    {
        return sizeof(AllocHeader);
    }
    size_t footerSize() const
    {
        return sizeof(AllocFooter);
    }

    void setVigilance(bool b)
    {
        m_bVigilant = b;
    }
    bool getVigilance() const
    {
        return m_bVigilant;
    }

  private:
    SlamAllocator(const SlamAllocator &);
    const SlamAllocator &operator=(const SlamAllocator &);

    static SlamAllocator m_Instance;

    /** Wipe out all memory used by the allocator. */
    void wipe();

    SlamCache m_Caches[32];

  public:
    struct AllocHeader_VigilantOverrunCheck_Empty
    {};
    struct AllocHeader_VigilantOverrunCheck
    {
        uintptr_t backtrace[SLAM_BT_FRAMES];
        size_t requested;
    };

    struct OverrunCheck_Magic_Empty
    {};
    struct OverrunCheck_Magic : pedigree_std::conditional<VIGILANT_OVERRUN_CHECK, AllocHeader_VigilantOverrunCheck, AllocHeader_VigilantOverrunCheck_Empty>::type
    {
        size_t magic;
    };

    /// Prepended to all allocated data. Basically just information to make
    /// freeing slightly less performance-intensive...
    struct AllocHeader : pedigree_std::conditional<OVERRUN_CHECK, OverrunCheck_Magic, OverrunCheck_Magic_Empty>::type
    {
        // Already-present and embedded Node fields.
        SlamCache::Node node;
        SlamCache *cache;
    } __attribute__((aligned(16)));

    struct AllocFooter : pedigree_std::conditional<OVERRUN_CHECK, OverrunCheck_Magic, OverrunCheck_Magic_Empty>::type
    {
    } __attribute__((aligned(16)));

  private:
    bool m_bInitialised;

    bool m_bVigilant;

    Spinlock m_SlabRegionLock;

    size_t m_HeapPageCount;

    uint64_t *m_SlabRegionBitmap;
    size_t m_SlabRegionBitmapEntries;

    uintptr_t m_Base;

    Spinlock m_Lock;
};

#endif
