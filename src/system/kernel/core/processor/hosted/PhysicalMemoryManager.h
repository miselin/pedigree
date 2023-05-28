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

#ifndef KERNEL_PROCESSOR_HOSTED_PHYSICALMEMORYMANAGER_H
#define KERNEL_PROCESSOR_HOSTED_PHYSICALMEMORYMANAGER_H

#include "pedigree/kernel/BootstrapInfo.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/utilities/HashTable.h"
#include "pedigree/kernel/utilities/RangeList.h"
#include "../x86_common/PhysicalMemoryManager.h"

/** @addtogroup kernelprocessorhosted
 * @{ */

#define HOSTED_PHYSICAL_MEMORY_SIZE (1UL << 31)

/** The common x86 implementation of the PhysicalMemoryManager
 *\brief Implementation of the PhysicalMemoryManager for common x86 */
class HostedPhysicalMemoryManager : public PhysicalMemoryManager
{
    friend class CacheManager;
    friend class Cache;
    /** For getBackingFile() */
    friend class HostedVirtualAddressSpace;

  public:
    /** Get the HostedPhysicalMemoryManager instance
     *\return instance of the HostedPhysicalMemoryManager */
    static HostedPhysicalMemoryManager &instance();

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

    /** Unmap & free the .init section */
    void initialisationDone();

    const RangeList<uint64_t> &getAcpiRanges() const
    {
        return m_AcpiRanges;
    }

  protected:
    /** The constructor */
    HostedPhysicalMemoryManager() INITIALISATION_ONLY;
    /** The destructor */
    virtual ~HostedPhysicalMemoryManager();

  private:
    /** The copy-constructor
     *\note Not implemented (singleton) */
    HostedPhysicalMemoryManager(const HostedPhysicalMemoryManager &);
    /** The copy-constructor
     *\note Not implemented (singleton) */
    HostedPhysicalMemoryManager &operator=(const HostedPhysicalMemoryManager &);

    inline int getBackingFile() const
    {
        return m_BackingFile;
    }

    void unmapRegion(MemoryRegion *pRegion);

    /** Same as freePage, but without the lock. Will panic if the lock is
     * unlocked. \note Use in the wrong place and you die. */
    virtual void freePageUnlocked(physical_uintptr_t page);

    using PageStack = X86CommonPhysicalMemoryManager::PageStack;

    /** The page stack */
    PageStack m_PageStack;

    /** RangeList of free physical memory */
    RangeList<uint64_t> m_PhysicalRanges;

    /** Virtual-memory available for MemoryRegions
     *\todo rename this member (conflicts with
     *PhysicalMemoryManager::m_MemoryRegions) */
    RangeList<uintptr_t> m_MemoryRegions;

    RangeList<uint64_t> m_AcpiRanges;

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

    /** Hosted: backing file for physical memory. */
    int m_BackingFile;

    /** Static instance. */
    static HostedPhysicalMemoryManager *m_Instance;
};

/** @} */

#endif
