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

#ifndef KERNEL_PROCESSOR_X64_VIRTUALADDRESSSPACE_H
#define KERNEL_PROCESSOR_X64_VIRTUALADDRESSSPACE_H

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

/**
 * Virtual address space layout
 * NOTE: the kernel and all modules must exist in the final 2GB of the address
 * space so that the 'kernel' mcmodel works as expected. If these are outside
 * that region, things like sign-extended relocations fall over. This also
 * ensures all jumps between the kernel and modules are within 2GB of each
 * other.
 */
#define USERSPACE_DYNAMIC_LINKER_LOCATION reinterpret_cast<void *>(0x4FA00000)

#define USERSPACE_VIRTUAL_START reinterpret_cast<void *>(0x400000)
#define USERSPACE_VIRTUAL_HEAP reinterpret_cast<void *>(0x50000000)
#define USERSPACE_RESERVED_START USERSPACE_DYNAMIC_LINKER_LOCATION
#define USERSPACE_VIRTUAL_STACK_SIZE 0x100000
#define USERSPACE_VIRTUAL_MAX_STACK_SIZE 0x100000
#define USERSPACE_DYNAMIC_START reinterpret_cast<void *>(0x100000000)
#define USERSPACE_DYNAMIC_END reinterpret_cast<void *>(0x00000FFFFFFFFFFF)
#define USERSPACE_VIRTUAL_LOWEST_STACK \
    reinterpret_cast<void *>(          \
        USERSPACE_DYNAMIC_END + USERSPACE_VIRTUAL_MAX_STACK_SIZE)
#define USERSPACE_VIRTUAL_STACK \
    reinterpret_cast<void *>(0x00007FFFEFFFF000)  // right below the Event base
#define KERNEL_VIRTUAL_EVENT_BASE \
    reinterpret_cast<void *>(0x00007FFFF0000000)  // right above the stacks

#define KERNEL_SPACE_START reinterpret_cast<void *>(0xFFFF800000000000)
#define KERNEL_VIRTUAL_PAGESTACK_ABV4GB1 \
    reinterpret_cast<void *>(0xFFFF800100000000)
#define KERNEL_VIRTUAL_PAGESTACK_ABV4GB2 \
    reinterpret_cast<void *>(0xFFFF801000000000)
#define KERNEL_VIRTUAL_HEAP reinterpret_cast<void *>(0xFFFF900000000000)
#define KERNEL_VIRTUAL_CACHE reinterpret_cast<void *>(0xFFFFB00000000000)
#define KERNEL_VIRTUAL_MEMORYREGION_ADDRESS \
    reinterpret_cast<void *>(0xFFFFF00000000000)
#define KERNEL_VIRTUAL_PAGESTACK_4GB \
    reinterpret_cast<void *>(0xFFFFFFFF7FC00000)
#define KERNEL_VIRTUAL_ADDRESS reinterpret_cast<void *>(0xFFFFFFFF7FF00000)
#define KERNEL_VIRTUAL_INFO_BLOCK reinterpret_cast<void *>(0xFFFFFFFF8FFF0000)
#define KERNEL_VIRTUAL_MODULE_BASE reinterpret_cast<void *>(0xFFFFFFFF90000000)
#define KERNEL_VIRTUAL_LOWEST_STACK reinterpret_cast<void *>(0xFFFFFFFFE0000000)
#define KERNEL_VIRTUAL_STACK reinterpret_cast<void *>(0xFFFFFFFFFFFF7000)

#define KERNEL_VIRTUAL_MODULE_SIZE \
    pointer_diff_const(KERNEL_VIRTUAL_MODULE_BASE, KERNEL_VIRTUAL_LOWEST_STACK)
#define KERNEL_VIRTUAL_HEAP_SIZE \
    pointer_diff_const(KERNEL_VIRTUAL_HEAP, KERNEL_VIRTUAL_CACHE)
#define KERNEL_VIRTUAL_CACHE_SIZE \
    pointer_diff_const(           \
        KERNEL_VIRTUAL_CACHE, KERNEL_VIRTUAL_MEMORYREGION_ADDRESS)
#define KERNEL_VIRTUAL_MEMORYREGION_SIZE \
    pointer_diff_const(                  \
        KERNEL_VIRTUAL_MEMORYREGION_ADDRESS, KERNEL_VIRTUAL_PAGESTACK_4GB)
#define KERNEL_STACK_SIZE 0x8000

/** @addtogroup kernelprocessorx64
 * @{ */

/** The X64VirtualAddressSpace implements the VirtualAddressSpace class for the
 * x64 processor architecture, that means it wraps around the processor's paging
 * functionality. */
class X64VirtualAddressSpace : public VirtualAddressSpace
{
    /** Processor::switchAddressSpace() needs access to m_PhysicalPML4 */
    friend class ProcessorBase;
    /** Multiprocessor::initialise() needs access to m_PhysicalPML4 */
    friend class Multiprocessor;
    /** VirtualAddressSpace::getKernelAddressSpace() needs access to
     * m_KernelSpace */
    friend VirtualAddressSpace &VirtualAddressSpace::getKernelAddressSpace();
    /** VirtualAddressSpace::create needs access to the constructor */
    friend VirtualAddressSpace *VirtualAddressSpace::create();

  public:
    //
    // VirtualAddressSpace Interface
    //
    virtual bool isAddressValid(void *virtualAddress);
    virtual bool isMapped(void *virtualAddress);

    virtual bool
    map(physical_uintptr_t physAddress, void *virtualAddress, size_t flags);
    virtual bool mapHuge(
        physical_uintptr_t physAddress, void *virtualAddress, size_t count,
        size_t flags);
    virtual void getMapping(
        void *virtualAddress, physical_uintptr_t &physAddress, size_t &flags);
    virtual void setFlags(void *virtualAddress, size_t newFlags);
    virtual void unmap(void *virtualAddress);
    virtual Stack *allocateStack();
    virtual Stack *allocateStack(size_t stackSz);
    virtual void freeStack(Stack *pStack);

    virtual bool memIsInKernelHeap(void *pMem);
    virtual bool memIsInHeap(void *pMem);
    virtual void *getEndOfHeap();

    virtual VirtualAddressSpace *clone(bool copyOnWrite = true);
    virtual void revertToKernelAddressSpace();

    //
    // Needed for the PhysicalMemoryManager
    //
    /** Map the page directory pointer table, the page directory, the page table
     *or the page frame if none is currently present \note This should only be
     *used from the PhysicalMemoryManager \param[in] physAddress the physical
     *page that should be used as page table or page frame \param[in]
     *virtualAddress the virtual address that should be checked for the
     *existance of a page directory pointer table, page directory, page table or
     *page frame \param[in] flags the flags used for
     *the mapping \return true, if a page table/frame is already mapped for that
     *address, false if the physicalAddress has been used as a page directory
     *pointer table, page directory, page table or page frame. */
    bool mapPageStructures(
        physical_uintptr_t physAddress, void *virtualAddress, size_t flags);
    bool mapPageStructuresAbove4GB(
        physical_uintptr_t physAddress, void *virtualAddress, size_t flags);

    /** The destructor cleans up the address space */
    virtual ~X64VirtualAddressSpace();

    /** Gets start address of the kernel in the address space. */
    virtual uintptr_t getKernelStart() const
    {
        return reinterpret_cast<uintptr_t>(KERNEL_SPACE_START);
    }

    /** Gets start address of the region usable and cloneable for userspace. */
    virtual uintptr_t getUserStart() const
    {
        return reinterpret_cast<uintptr_t>(USERSPACE_VIRTUAL_START);
    }

    /** Gets start address of reserved areas of the userpace address space. */
    virtual uintptr_t getUserReservedStart() const
    {
        return reinterpret_cast<uintptr_t>(USERSPACE_RESERVED_START);
    }

    /** Gets address of the dynamic linker in the address space. */
    virtual uintptr_t getDynamicLinkerAddress() const
    {
        return reinterpret_cast<uintptr_t>(USERSPACE_DYNAMIC_LINKER_LOCATION);
    }

    /** Gets address of the start of the kernel's heap region. */
    virtual uintptr_t getKernelHeapStart() const
    {
        return reinterpret_cast<uintptr_t>(KERNEL_VIRTUAL_HEAP);
    }

    /** Gets address of the end of the kernel's heap region. */
    virtual uintptr_t getKernelHeapEnd() const
    {
        return reinterpret_cast<uintptr_t>(KERNEL_VIRTUAL_HEAP) +
               KERNEL_VIRTUAL_HEAP_SIZE;
    }

    /** Gets address of the start of dynamic memory mapping area. */
    virtual uintptr_t getDynamicStart() const
    {
        return reinterpret_cast<uintptr_t>(USERSPACE_DYNAMIC_START);
    }

    /** Gets address of the end of dynamic memory mapping area. */
    virtual uintptr_t getDynamicEnd() const
    {
        return reinterpret_cast<uintptr_t>(USERSPACE_DYNAMIC_END);
    }

    /** Gets address of the global info block location. */
    virtual uintptr_t getGlobalInfoBlock() const
    {
        return reinterpret_cast<uintptr_t>(KERNEL_VIRTUAL_INFO_BLOCK);
    }

    /** Gets address of the start of the kernel's cache region. */
    virtual uintptr_t getKernelCacheStart() const
    {
        return reinterpret_cast<uintptr_t>(KERNEL_VIRTUAL_CACHE);
    }

    /** Gets address of the end of the kernel's cache region. */
    virtual uintptr_t getKernelCacheEnd() const
    {
        return reinterpret_cast<uintptr_t>(KERNEL_VIRTUAL_CACHE) +
               KERNEL_VIRTUAL_CACHE_SIZE;
    }

    /** Gets address of the start of the kernel's event handling block. */
    virtual uintptr_t getKernelEventBlockStart() const
    {
        return reinterpret_cast<uintptr_t>(KERNEL_VIRTUAL_EVENT_BASE);
    }

    /** Gets address of the start of the kernel's module region. */
    virtual uintptr_t getKernelModulesStart() const
    {
        return reinterpret_cast<uintptr_t>(KERNEL_VIRTUAL_MODULE_BASE);
    }

    /** Gets address of the end of the kernel's module region. */
    virtual uintptr_t getKernelModulesEnd() const
    {
        return reinterpret_cast<uintptr_t>(KERNEL_VIRTUAL_MODULE_BASE) +
               KERNEL_VIRTUAL_MODULE_SIZE;
    }

    /** Gets address of the physical page stack, if one exists. */
    virtual uintptr_t getKernelVirtualPagestack() const
    {
        return reinterpret_cast<uintptr_t>(KERNEL_VIRTUAL_PAGESTACK_4GB);
    }

    /** Gets address of the first additional page stack, if one exists. */
    virtual uintptr_t getKernelVirtualPagestackAdd1() const
    {
        return reinterpret_cast<uintptr_t>(KERNEL_VIRTUAL_PAGESTACK_ABV4GB1);
    }

    /** Gets address of the second additional page stack, if one exists. */
    virtual uintptr_t getKernelVirtualPagestackAdd2() const
    {
        return reinterpret_cast<uintptr_t>(KERNEL_VIRTUAL_PAGESTACK_ABV4GB2);
    }

  private:
    /** The default constructor */
    X64VirtualAddressSpace();
    /** The constructor for already present paging structures
     *\param[in] Heap virtual address of the beginning of the heap
     *\param[in] PhysicalPageDirectory physical address of the page directory
     *\param[in] VirtualStack virtual address of the top of the next kernel
     *stack \note This constructor is only used to construct the kernel
     *VirtualAddressSpace */
    X64VirtualAddressSpace(
        void *Heap, physical_uintptr_t PhysicalPML4, void *VirtualStack);

    /** The copy-constructor
     *\note NOT implemented */
    X64VirtualAddressSpace(const X64VirtualAddressSpace &);
    /** The copy-constructor
     *\note Not implemented */
    X64VirtualAddressSpace &operator=(const X64VirtualAddressSpace &);

    /** Get the page table entry, if it exists and check whether a page is
     *mapped or marked as swapped out. \param[in] virtualAddress the virtual
     *address \param[out] pageTableEntry pointer to the page table entry \return
     *true, if the page table is present and the page mapped or marked swapped
     *out, false otherwise */
    bool
    getPageTableEntry(void *virtualAddress, uint64_t *&pageTableEntry) const;
    /**
     * \brief Possibly cleans up tables for the given address.
     *
     * This is used when unmapping pages to opportunistically unmap paging
     * structures that are no longer necessary.
     * \param[in] virtualAddress the virtual address
     * \param[out] pageTableEntry pointer to the page table entry
     */
    void maybeFreeTables(void *virtualAddress);
    /** Convert the processor independant flags to the processor's
     *representation of the flags \param[in] flags the processor independant
     *flag representation \param[in] bFinal whether this is for the actual page
     *or just an intermediate PTE/PDE \return the proessor specific flag
     *representation */
    uint64_t toFlags(size_t flags, bool bFinal = false) const PURE;
    /** Convert processor's representation of the flags to the processor
     *independant representation \param[in] Flags the processor specific flag
     *representation \param[in] bFinal whether this is for the actual page or
     *just an intermediate PTE/PDE \return the proessor independant flag
     *representation */
    size_t fromFlags(uint64_t Flags, bool bFinal = false) const PURE;
    /** Allocate and map the table entry if none is present
     *\param[in] tableEntry pointer to the current table entry
     *\param[in] flags flags that are used for the mapping
     *\return true, if successfull, false otherwise */
    bool conditionalTableEntryAllocation(uint64_t *tableEntry, uint64_t flags);
    /** map the table entry if none is present
     *\param[in] tableEntry pointer to the current table entry
     *\param[in] physAddress physical address of the page used as the table
     *\param[in] flags flags that are used for the mapping
     *\return true, if the page has been used, false otherwise */
    bool conditionalTableEntryMapping(
        uint64_t *tableEntry, uint64_t physAddress, uint64_t flags);

    /**
     * Perform a mapping proper, without taking the lock.
     * \param[in] locked whether the lock was taken before calling or not.
     */
    bool mapUnlocked(
        physical_uintptr_t physAddress, void *virtualAddress, size_t flags,
        bool locked = false);

    /**
     * Perform an unmap without taking the lock.
     */
    void unmapUnlocked(void *virtualAddress, bool requireMapped = true);

    /** Allocates a stack with a given size. */
    Stack *doAllocateStack(size_t sSize);

    /** Physical address of the Page Map Level 4 */
    physical_uintptr_t m_PhysicalPML4;
    /** Current top of the stacks */
    void *m_pStackTop;
    /** List of free stacks */
    Vector<Stack *> m_freeStacks;
    /** Is this the kernel space? */
    bool m_bKernelSpace;
    /** Lock to guard against multiprocessor reentrancy. */
    Spinlock m_Lock;
    /** Lock to guard against multiprocessor reentrancy for stack reuse. */
    Spinlock m_StacksLock;

    /** The kernel virtual address space */
    static X64VirtualAddressSpace m_KernelSpace;
};

/** @} */

#endif
