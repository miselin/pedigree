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

#ifndef KERNEL_PROCESSOR_PHYSICALMEMORYMANAGER_H
#define KERNEL_PROCESSOR_PHYSICALMEMORYMANAGER_H

#include <utilities/Vector.h>
#include <processor/types.h>

/** @addtogroup kernelprocessor
 * @{ */

class MemoryRegion;

/** The PhysicalMemoryManager manages the physical address space. That means it provides
 *  functions to allocate and free pages. */
class PhysicalMemoryManager
{
  friend class CacheManager;
  friend class Cache;
  public:
    /** MemoryRegion can access our unmapRegion function. */
    friend class MemoryRegion;

    /** If this flag is set the pages are physically continuous */
    static const size_t continuous   = 1 << 0;
    /** If this flag is set we allocate pages that are not in RAM */
    static const size_t nonRamMemory = 1 << 1;
    /** If this flag is set we try to remove the range from the PhysicalMemoryManager, but
        if that fails, we still map the physical memory region */
    static const size_t force        = 1 << 2;
    /** Only allocate virtual address space for the region. */
    static const size_t virtualOnly  = 1 << 3;

    // x86/x64 specific flags
    #if defined(X86_COMMON)
      /** The allocated pages should be below the 1MB mark */
      static const size_t below1MB   = 1 << 4;
      /** The allocated pages should be below the 16MB mark */
      static const size_t below16MB  = 2 << 4;
      /** The allocated pages should be below the 4GB mark */
      static const size_t below4GB   = 3 << 4;
      /** The allocated pages should be below the 64GB mark */
      static const size_t below64GB  = 4 << 4;

      /** All address size constraints */
      static const size_t addressConstraints = below1MB | below16MB | below4GB | below64GB;
    #endif

    /** Get the PhysicalMemoryManager instance
     *\return instance of the PhysicalMemoryManager */
    static PhysicalMemoryManager &instance();

    /** Get the size of one page
     *\return size of one page in bytes */
    inline static size_t getPageSize()
      {return PAGE_SIZE;}
    /** Allocate a 'normal' page. Normal means that the page does not need to fullfill any
     *  constraints. These kinds of pages can be used to map normal memory into a virtual
     *  address space.
     *\return physical address of the page or 0 if no page available */
    virtual physical_uintptr_t allocatePage() = 0;
    /** Free a page allocated with the allocatePage() function
     *\param[in] page physical address of the page */
    virtual void freePage(physical_uintptr_t page) = 0;

    /**
     * "Pin" a page, increasing its refcount.
     *
     * Every time a pinned page is passed to freePage, its refcount is decreased,
     * and the page is only freed once the refcount hits zero.
     *
     * Note that an unpin function is not provided, as freePage offers the
     * right semantics.
     */
    virtual void pin(physical_uintptr_t page) = 0;

    /** Allocate a memory-region with specific constraints the pages need to fullfill.
     *\param[in] Region reference to the MemoryRegion object
     *\param[in] cPages the number of pages to allocate for the MemoryRegion object
     *\param[in] pageConstraints the constraints the pages have to fullfill
     *\param[in] Flags flags from the VirtualAddressSpace class namespace
     *\param[in] start the physical address of the beginning of the region (optional)
     *\return true, if a valid MemoryRegion object is created, false otherwise */
    virtual bool allocateRegion(MemoryRegion &Region,
                                size_t cPages,
                                size_t pageConstraints,
                                size_t Flags,
                                physical_uintptr_t start = -1) = 0;

    /** Structure containing information about one memory region. */
    struct MemoryRegionInfo
    {
      /** Constructor initialises all structure members
       *\param[in] pVirtualAddress virtual address of the beginning of the memory region
       *\param[in] PhysicalAddress physical address of the beginning of the memory region (or 0)
       *\param[in] sAddress size (in bytes) of the memory region
       *\param[in] Name user-visible name of the memory region */
      inline MemoryRegionInfo(void *VirtualAddress,
                              physical_uintptr_t PhysicalAddress,
                              size_t size,
                              const char *name)
        : pVirtualAddress(VirtualAddress), physicalAddress(PhysicalAddress), sVirtualAddress(size), pName(name){}

      /** Virtual address of the memory region */
      void *pVirtualAddress;
      /** Physical address of the memory region (or 0) */
      physical_uintptr_t physicalAddress;
      /** Size (in bytes) of the memory region */
      size_t sVirtualAddress;
      /** Pointer to the user-visible name of the memory region */
      const char *pName;
    };

    /** Copy the memory region list
     *\param[in,out] MemoryRegions container for the copy of the memory regions */
    void allocateMemoryRegionList(Vector<MemoryRegionInfo*> &MemoryRegions);
      /** Free the memory region list created with allocateMemoryRegionList.
       *\param[in,out] MemoryRegions container of the copy of the memory regions */
    void freeMemoryRegionList(Vector<MemoryRegionInfo*> &MemoryRegions);

    /** Specifies the number of pages that remain free on the system. */
    virtual size_t freePageCount() const
    {
        return ~0UL;
    };

  protected:
    /** The constructor */
    inline PhysicalMemoryManager()
      : m_MemoryRegions(){}
    /** The destructor */
    inline virtual ~PhysicalMemoryManager(){}

    /** List of memory-regions */
    Vector<MemoryRegion*> m_MemoryRegions;

  private:
    /** The copy-constructor
     *\note Not implemented (singleton) */
    PhysicalMemoryManager(const PhysicalMemoryManager &);
    /** The copy-constructor
     *\note Not implemented (singleton) */
    PhysicalMemoryManager &operator = (const PhysicalMemoryManager &);
    
    /** Same as freePage, but without the lock. Will panic if the lock is unlocked.
      * \note Use in the wrong place and you die. */
    virtual void freePageUnlocked(physical_uintptr_t page) = 0;

    /** Unmaps a memory region - called ONLY from MemoryRegion's destructor. */
    virtual void unmapRegion(MemoryRegion *pRegion) = 0;
};

/** @} */

#endif
