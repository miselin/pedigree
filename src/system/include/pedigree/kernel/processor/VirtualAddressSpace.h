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

#ifndef KERNEL_PROCESSOR_VIRTUALADDRESSSPACE_H
#define KERNEL_PROCESSOR_VIRTUALADDRESSSPACE_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/utilities/utility.h"
#include "pedigree/kernel/processor/types.h"

/** @addtogroup kernelprocessor
 * @{ */

/** The VirtualAddressSpace encapsulates all the functionality of a virtual
 *memory- management. This includes management of the mapping between physical
 *and virtual memory, management of allocated physical memory pages and
 *management of free/allocated virtual memory. \note If
 *KERNEL_NEEDS_ADDRESS_SPACE_SWITCH is set defined to 1, you have to switch to
 *the VirtualAddressSpace you want to change yourself before you call any of the
 *following functions: expandHeap, isMapped, map, getMapping, setFlags,
 *unmap */
class VirtualAddressSpace
{
  public:
    /** Debugger can access our private members for statistics reporting. */
    friend class Debugger;

    class Stack;

    /** The page is only accessible from kernel-mode. If this flag is not set
     * the page is also accessible from user-mode. */
    static const size_t KernelMode = 0x01;
    /** The page is writeable. If this flag is not set the page is read-only. */
    static const size_t Write = 0x02;
    /** The page is executable. If this flag is not set the page is not
     * executable. */
    static const size_t Execute = 0x04;
    /** If this flag is set, the cache strategy is write-through. */
    static const size_t WriteThrough = 0x08;
    /** If this flag is set, the cache is disabled. */
    static const size_t CacheDisable = 0x10;
    /** If this flag is set, the page is copy-on-write */
    static const size_t CopyOnWrite = 0x20;
    /** If this flag is set, the page is swapped out */
    static const size_t Swapped = 0x40;
    /** If this flag is set, the page is memory coherent - only applicable to
     * PPC */
    static const size_t MemoryCoherent = 0x80;
    /** If this flag is set, the page is guarded - only applicable to PPC */
    static const size_t Guarded = 0x100;
    /** If this flag is set, the mapped physical page is shared. */
    static const size_t Shared = 0x200;
    /** If this flag is set, the cache strategy is write-combine. */
    static const size_t WriteCombine = 0x400;
    /** Whether the page has been accessed. */
    static const size_t Accessed = 0x800;
    /** Whether the page has been dirtied. */
    static const size_t Dirty = 0x1000;
    /** Clear the dirty flag set by the above. */
    static const size_t ClearDirty = 0x2000;

    /** Get the kernel virtual address space
     *\return reference to the kernel virtual address space */
    EXPORTED_PUBLIC static VirtualAddressSpace &getKernelAddressSpace();

    /** Expand the heap
     *\param[in] pageCount the number of pages that should be allocated and
     *mapped to the heap end \param[in] flags flags that describe which accesses
     *should be allowed on the page \return pointer to the beginning of the
     *newly allocated/mapped heap, 0 otherwise */
    virtual void *expandHeap(ssize_t incr, size_t flags);

    /** Is a particular virtual address valid?
     *\param[in] virtualAddress the virtual address to check
     *\return true, if the address is valid, false otherwise */
    virtual bool isAddressValid(void *virtualAddress) = 0;
    /** Checks whether a mapping the the specific virtual address exists. Pages
     *marked as swapped out are not considered mapped. \note This function must
     *be valid on all the valid addresses within the virtual address space.
     *\param[in] virtualAddress the virtual address
     *\return true, if a mapping exists, false otherwise */
    virtual bool isMapped(void *virtualAddress) = 0;

    /** Map a specific physical page (of size
     *PhysicalMemoryManager::getPageSize()) at a specific location into the
     *virtual address space. \note This function must also work on pages marked
     *as swapped out. \param[in] physicalAddress the address of the physical
     *page that should be mapped into the virtual address space. \param[in]
     *virtualAddress the virtual address at which the page apears within the
     *virtual address space. \param[in] flags flags that describe which accesses
     *should be allowed on the page. \return true, if successfull, false
     *otherwise */
    virtual bool
    map(physical_uintptr_t physicalAddress, void *virtualAddress,
        size_t flags) = 0;
    /** Map a region of memory using the largest possible frame size.
     * Where possible the largest page size should be used, degrading into
     * smaller page sizes as needed (e.g. 1 GB, 2 MB, 4K) to reduce the
     * overhead of the mapping on the system.
     * Default implementation calls map() in a loop.
     * \return true if successful, false otherwise.
     */
    virtual bool mapHuge(
        physical_uintptr_t physAddress, void *virtualAddress, size_t count,
        size_t flags);
    /** Get the physical address and the flags associated with the specific
     *virtual address. \note This function is only valid on memory that was
     *mapped with VirtualAddressSpace::map() and that is still mapped or marked
     *as swapped out. \param[in] virtualAddress the address in the virtual
     *address space \param[out] flags the flags \param[out] physicalAddress the
     *physical address */
    virtual void getMapping(
        void *virtualAddress, physical_uintptr_t &physicalAddress,
        size_t &flags) = 0;
    /** Set the flags of the page at a specific virtual address.
     *\note The page must have been mapped with VirtualAddressSpace::map() and
     *the page must still be mapped or marked as swapped out. \param[in]
     *virtualAddress the virtual address \param[in] newFlags the flags */
    virtual void setFlags(void *virtualAddress, size_t newFlags) = 0;
    /** Remove the page at the specific virtual address from the virtual address
     *space. \note This function is only valid on memory that was mapped with
     *VirtualAddressSpace::map() and that is still mapped or marked as swapped
     *out. \param[in] virtualAddress the virtual address */
    virtual void unmap(void *virtualAddress) = 0;

    /** Allocates a single stack for a thread. Will use the default kernel
     * thread size. */
    virtual Stack *allocateStack() = 0;
    /** Allocates a single stack of the given size for a thread. */
    virtual Stack *allocateStack(size_t stackSz)
    {
        // Default implementation just ignores the stack size.
        return allocateStack();
    }
    /** Frees a stack allocated with allocateStack. */
    virtual void freeStack(Stack *pStack) = 0;

    /** Create a new VirtualAddressSpace. Only the kernel is mapped into that
     *virtual address space \return pointer to the new VirtualAddressSpace, 0
     *otherwise */
    static VirtualAddressSpace *create();

    /** Clone this VirtualAddressSpace. That means that we copy-on-write-map the
     application *  image. * \param[in] copyOnWrite whether the new address
     space copies pages on write, or shares them with the parent address space.
     * \return pointer to the new VirtualAddressSpace
     */
    virtual VirtualAddressSpace *clone(bool copyOnWrite = true) = 0;

    /** Undo a clone() - this happens when an application is Exec()'d - we
       destroy all mappings not in the kernel address space so the space is
       'clean'.*/
    virtual void revertToKernelAddressSpace() = 0;

    /** The destructor does nothing */
    inline virtual ~VirtualAddressSpace()
    {
    }

    /** Setter for the internal Heap and HeapEnd variables */
    void setHeap(void *heap, void *heapEnd)
    {
        m_Heap = heap;
        m_HeapEnd = heapEnd;
    }

    /** Determines whether a given address is within the kernel's heap region.
     */
    virtual bool memIsInKernelHeap(void *pMem) = 0;

    /** Determines if a given point is within the virtual address space region
     * dedicated to the kernel heap. */
    virtual bool memIsInHeap(void *pMem) = 0;

    /** Gets a pointer to the byte after the end of the heap. */
    virtual void *getEndOfHeap() = 0;

    /** Gets start address of the kernel in the address space. */
    virtual uintptr_t getKernelStart() const = 0;

    /** Gets start address of the region usable and cloneable for userspace. */
    virtual uintptr_t getUserStart() const = 0;

    /** Gets start address of reserved areas of the userpace address space. */
    virtual uintptr_t getUserReservedStart() const = 0;

    /** Gets address of the dynamic linker in the address space. */
    virtual uintptr_t getDynamicLinkerAddress() const = 0;

    /** Gets address of the start of the kernel's heap region. */
    virtual uintptr_t getKernelHeapStart() const = 0;

    /** Gets address of the end of the kernel's heap region. */
    virtual uintptr_t getKernelHeapEnd() const = 0;

    /** Gets address of the start of the kernel's cache region. */
    virtual uintptr_t getKernelCacheStart() const = 0;

    /** Gets address of the end of the kernel's cache region. */
    virtual uintptr_t getKernelCacheEnd() const = 0;

    /** Gets address of the start of the kernel's event handling block. */
    virtual uintptr_t getKernelEventBlockStart() const = 0;

    /** Gets address of the start of the kernel's module region. */
    virtual uintptr_t getKernelModulesStart() const = 0;

    /** Gets address of the end of the kernel's module region. */
    virtual uintptr_t getKernelModulesEnd() const = 0;

    /** Gets address of the physical page stack, if one exists. */
    virtual uintptr_t getKernelVirtualPagestack() const
    {
        return 0;
    }

    /** Gets address of the first additional page stack, if one exists. */
    virtual uintptr_t getKernelVirtualPagestackAdd1() const
    {
        return 0;
    }

    /** Gets address of the second additional page stack, if one exists. */
    virtual uintptr_t getKernelVirtualPagestackAdd2() const
    {
        return 0;
    }

    /**
     * Gets address of the start of the dynamic memory mapping area.
     * This is an area in which memory mappings can be created for userspace
     * applications.
     * Systems that do not have room in the address space for this may return
     * zero.
     */
    virtual uintptr_t getDynamicStart() const
    {
        return 0;
    }
    /** Gets address of the end of the dynamic memory mapping area. */
    virtual uintptr_t getDynamicEnd() const
    {
        return 0;
    }

    /** Gets address of the global info block location. */
    virtual uintptr_t getGlobalInfoBlock() const
    {
        return 0;
    }

    /** Pointer to the beginning of the heap */
    void *m_Heap;
    /** Pointer to the current heap end */
    void *m_HeapEnd;
    /** Physical address of a zero page, always page-aligned. */
    static physical_uintptr_t m_ZeroPage;

    /** Abstracts a stack. */
    class Stack
    {
      public:
        Stack(void *top, size_t size) : m_Top(top), m_Size(size)
        {
        }

        void *getTop() const
        {
            return m_Top;
        }

        void *getBase() const
        {
            if (m_Top)
            {
                return adjust_pointer(m_Top, -m_Size);
            }
            else
            {
                return nullptr;
            }
        }

        size_t getSize() const
        {
            return m_Size;
        }

        operator void *() const
        {
            return m_Top;
        }

      private:
        void *m_Top;
        size_t m_Size;
    };

  protected:
    /** The constructor does nothing */
    inline VirtualAddressSpace(void *Heap) : m_Heap(Heap), m_HeapEnd(Heap)
    {
    }

  private:
    /** The default constructor */
    VirtualAddressSpace();
    /** The copy-constructor
     *\note NOT implemented */
    VirtualAddressSpace(const VirtualAddressSpace &);
    /** The copy-constructor
     *\note Not implemented */
    VirtualAddressSpace &operator=(const VirtualAddressSpace &);

    /** Reverts the heap expansion, that was begun with expandHeap
     *\param[in] virtualAddress current heap address
     *\param[in] pageCount number of mapped pages to unmap and free */
    void rollbackHeapExpansion(void *virtualAddress, size_t pageCount);
};

/** @} */

#endif
