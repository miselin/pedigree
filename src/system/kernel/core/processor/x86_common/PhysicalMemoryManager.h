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

#ifndef KERNEL_PROCESSOR_X86_COMMON_PHYSICALMEMORYMANAGER_H
#define KERNEL_PROCESSOR_X86_COMMON_PHYSICALMEMORYMANAGER_H

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/HashTable.h"
#include "pedigree/kernel/utilities/RangeList.h"
#include "pedigree/kernel/utilities/utility.h"

class BootstrapStruct_t;
class MemoryRegion;

/** @addtogroup kernelprocessorx86common
 * @{ */

extern size_t g_AllocedPages;
extern size_t g_FreePages;

/** The common x86 implementation of the PhysicalMemoryManager
 *\brief Implementation of the PhysicalMemoryManager for common x86 */
class X86CommonPhysicalMemoryManager : public PhysicalMemoryManager
{
    friend class CacheManager;
    friend class Cache;

#if HOSTED
    friend class HostedPhysicalMemoryManager;
#endif

  public:
    /** Get the X86CommonPhysicalMemoryManager instance
     *\return instance of the X86CommonPhysicalMemoryManager */
    inline static X86CommonPhysicalMemoryManager &instance()
    {
        return m_Instance;
    }

    //
    // PhysicalMemoryManager Interface
    //
    virtual physical_uintptr_t allocatePage(size_t pageConstraints = 0);
    virtual void freePage(physical_uintptr_t page);
    virtual bool allocateRegion(
        MemoryRegion &Region, size_t cPages, size_t pageConstraints,
        size_t Flags, physical_uintptr_t start = -1);

    virtual void pin(physical_uintptr_t page);

    /** Initialise the page stack
     *\param[in] Info reference to the multiboot information structure */
    void initialise(const BootstrapStruct_t &Info) INITIALISATION_ONLY;

    /** Initialise the page stack, with ranges above 4 GB. Requires ranges
     *  below 4 GB to be available (call initialise first).
     *\param[in] Info reference to the multiboot information structure */
    void initialise64(const BootstrapStruct_t &Info) INITIALISATION_ONLY;

    /** Unmap & free the .init section */
    void initialisationDone();

    /** Clean up tracking structures. */
    void shutdown();

    const RangeList<uint64_t> &getAcpiRanges() const
    {
        return m_AcpiRanges;
    }

    /** Specifies the number of pages that remain free on the system. */
    virtual size_t freePageCount() const;

  protected:
    /** The constructor */
    X86CommonPhysicalMemoryManager() INITIALISATION_ONLY;
    /** The destructor */
    virtual ~X86CommonPhysicalMemoryManager();

  private:
    /** The copy-constructor
     *\note Not implemented (singleton) */
    X86CommonPhysicalMemoryManager(const X86CommonPhysicalMemoryManager &);
    /** The copy-constructor
     *\note Not implemented (singleton) */
    X86CommonPhysicalMemoryManager &
    operator=(const X86CommonPhysicalMemoryManager &);

    void unmapRegion(MemoryRegion *pRegion);

    /** Same as freePage, but without the lock. Will panic if the lock is
     * unlocked. \note Use in the wrong place and you die. */
    virtual void freePageUnlocked(physical_uintptr_t page);

    /** The actual page stack contains is a Stack of the pages with the
     *constraints below4GB and below64GB and those pages without address size
     *constraints. \brief The Stack of pages (below4GB, below64GB, no
     *constraint). */
    class PageStack
    {
        friend class X86CommonPhysicalMemoryManager;
#if HOSTED
        friend class HostedPhysicalMemoryManager;
#endif

      public:
        /** Default constructor does nothing */
        PageStack() INITIALISATION_ONLY;
        /** Allocate a page with certain constraints
         *\param[in] constraints either below4GB or below64GB or 0
         *\return The physical address of the allocated page or 0 */
        physical_uintptr_t allocate(size_t constraints);
        /** Free a physical page
         *\param[in] physicalAddress physical address of the page */
        void free(uint64_t physicalAddress, size_t length);
        /** The destructor does nothing */
        inline ~PageStack()
        {
        }

        inline size_t freePages() const
        {
            return m_FreePages;
        }

        void setCapacity(size_t newCapacity)
        {
            m_DesiredCapacity = newCapacity;
        }

        void increaseCapacity(size_t by)
        {
            m_DesiredCapacity += by;
        }

      protected:
        /** Mark all stacks tracking pages above 4GB mark ready. */
        void markAbove4GReady();

        /** Mark stacks tracking pages below 4GB mark ready. */
        void markBelow4GReady();

      private:
        /** The copy-constructor
         *\note Not implemented */
        PageStack(const PageStack &);
        /** The copy-constructor
         *\note Not implemented */
        PageStack &operator=(const PageStack &);

        /**
         * Initialize the stacks if needed.
         * This is needed to avoid a dependency on the order of construction of
         * VirtualAddressSpace objects for the kernel address space. Otherwise
         * we can try to get page stack addresses from an unconstructed
         * VirtualAddressSpace object.
         */
        void initialise();

        /**
         * Potentially use the given page to map paging structures for future
         * stack frees.
         * \return true if the page was consumed, false otherwise.
         */
        bool maybeMap(size_t index, uint64_t physicalAddress);

        /** The number of Stacks */
        static const size_t StackCount = 3;

        /** Pointer to the base address of the stack. The stack grows upwards.
         */
        void *m_Stack[StackCount];
        /** Size of the currently mapped stack */
        size_t m_StackMax[StackCount];
        /** Currently used size of the stack */
        size_t m_StackSize[StackCount];
        /** Current pages available. */
        size_t m_FreePages;
        /** Current capacity (i.e. mapped pages). */
        size_t m_Capacity;
        /** Desired capacity. New pages will be mapped until demand is met. */
        size_t m_DesiredCapacity;

        /** Whether or not a particular stack is ready for use. */
        Atomic<bool> m_StackReady[StackCount];
    };

    /** The page stack */
    PageStack m_PageStack;

    /** RangeList for the usable memory below 1MB */
    RangeList<uint32_t> m_RangeBelow1MB;
    /** RangeList for the usable memory below 16MB */
    RangeList<uint32_t> m_RangeBelow16MB;

    /** RangeList of free physical memory */
    RangeList<uint64_t> m_PhysicalRanges;

    /** RangeList of ACPI memory */
    RangeList<uint64_t> m_AcpiRanges;

    /** Virtual-memory available for MemoryRegions
     *\todo rename this member (conflicts with
     *PhysicalMemoryManager::m_MemoryRegions) */
    RangeList<uintptr_t> m_MemoryRegions;

    /** The X86CommonPhysicalMemoryManager class instance */
    static X86CommonPhysicalMemoryManager m_Instance;

    /** To guard against multiprocessor reentrancy. */
    Spinlock m_Lock, m_RegionLock;

    /** Utility to wrap a physical address and hash it. */
    class PageHashable
    {
      public:
        PageHashable()
        {
            m_Hash = m_Page = 0;
        }

        PageHashable(physical_uintptr_t p)
        {
            m_Hash = p / getPageSize();
            m_Page = p;
        }

        size_t hash() const
        {
            return m_Hash;
        }

        bool operator==(const PageHashable &p) const
        {
            return p.m_Page == m_Page;
        }

      private:
        size_t m_Hash;
        physical_uintptr_t m_Page;
    };

    /** Physical page metadata. */
    struct page
    {
        page() : active(false), refcount(0)
        {
        }

        bool active;
        size_t refcount;
    };

    typedef HashTable<PageHashable, struct page> MetadataTable;

    /** Page metadata table */
    HashTable<PageHashable, struct page> m_PageMetadata;
};

/** @} */

#endif
