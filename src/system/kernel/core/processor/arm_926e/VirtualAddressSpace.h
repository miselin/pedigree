/*
 * Copyright (c) 2008 James Molloy, Jörg Pfähler, Matthew Iselin
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

#ifndef KERNEL_PROCESSOR_ARM926E_VIRTUALADDRESSSPACE_H
#define KERNEL_PROCESSOR_ARM926E_VIRTUALADDRESSSPACE_H

#include <processor/types.h>
#include <processor/VirtualAddressSpace.h>

/** @addtogroup kernelprocessorARM926E
 * @{ */

/** The Arm926EVirtualAddressSpace implements the VirtualAddressSpace class for the mip32
    processor, which means it encompasses paging (KUSEG) and KSEG0, KSEG1, KSEG2.*/
class Arm926EVirtualAddressSpace : public VirtualAddressSpace
{
  /** Processor::switchAddressSpace() needs access to m_PhysicalPageDirectory */
  friend class Processor;
  friend VirtualAddressSpace &VirtualAddressSpace::getKernelAddressSpace();
  public:
    //
    // VirtualAddressSpace Interface
    //
    virtual bool isAddressValid(void *virtualAddress);
    virtual bool isMapped(void *virtualAddress);

    virtual bool map(physical_uintptr_t physicalAddress,
                     void *virtualAddress,
                     size_t flags);
    virtual void getMapping(void *virtualAddress,
                            physical_uintptr_t &physicalAddress,
                            size_t &flags);
    virtual void setFlags(void *virtualAddress, size_t newFlags);
    virtual void unmap(void *virtualAddress);
    virtual void *allocateStack();
    virtual void freeStack(void *pStack);

    //
    // Needed for the PhysicalMemoryManager
    //
    /** Map the page table or the page frame if none is currently present
     *\note This should only be used from the PhysicalMemoryManager
     *\param[in] physicalAddress the physical page that should be used as page table or
     *                           page frame
     *\param[in] virtualAddress the virtual address that should be checked for the existance
     *                          of a page table and page frame
     *\param[in] flags the flags used for the mapping
     *\return true, if a page table/frame is already mapped for that address, false if the
     *        physicalAddress has been used as a page table or as a page frame. */
    bool mapPageStructures(physical_uintptr_t physicalAddress,
                           void *virtualAddress,
                           size_t flags);

  protected:
    /** The destructor does nothing */
    virtual ~Arm926EVirtualAddressSpace();

  private:
    /** The constructor for already present paging structures
     *\param[in] Heap virtual address of the beginning of the heap
     *\param[in] PhysicalPageDirectory physical address of the page directory
     *\param[in] VirtualPageDirectory virtual address of the page directory
     *\param[in] VirtualPageTables virtual address of the page tables */
    Arm926EVirtualAddressSpace(void *Heap,
                           physical_uintptr_t PhysicalPageDirectory,
                           void *VirtualPageDirectory,
                           void *VirtualPageTables);

    /** The default constructor
     *\note NOT implemented */
    Arm926EVirtualAddressSpace();
    /** The copy-constructor
     *\note NOT implemented */
    Arm926EVirtualAddressSpace(const Arm926EVirtualAddressSpace &);
    /** The copy-constructor
     *\note Not implemented */
    Arm926EVirtualAddressSpace &operator = (const Arm926EVirtualAddressSpace &);

    /** Initialises the kernel address space, called by Processor. */
    bool initialise();

    /** Get the page table entry, if it exists and check whether a page is mapped or marked as
     *  swapped out.
     *\param[in] virtualAddress the virtual address
     *\param[out] pageTableEntry pointer to the page table entry
     *\return true, if the page table is present and the page mapped or marked swapped out, false
     *        otherwise */
    bool getPageTableEntry(void *virtualAddress,
                           uint32_t *&pageTableEntry);
    /** Convert the processor independant flags to the processor's representation of the flags
     *\param[in] flags the processor independant flag representation
     *\return the proessor specific flag representation */
    uint32_t toFlags(size_t flags);
    /** Convert processor's representation of the flags to the processor independant representation
     *\param[in] Flags the processor specific flag representation
     *\return the proessor independant flag representation */
    size_t fromFlags(uint32_t Flags);

    /** Physical address of the page directory */
    physical_uintptr_t m_PhysicalPageDirectory;
    /** Virtual address of the page directory */
    void *m_VirtualPageDirectory;
    /** Virtual address of the page tables */
    void *m_VirtualPageTables;

    /** The kernel virtual address space */
    static Arm926EVirtualAddressSpace m_KernelSpace;
};

/** @} */

#endif
