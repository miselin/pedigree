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

#include "VirtualAddressSpace.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/utilities/utility.h"

#include "PhysicalMemoryManager.h"

#include <dlfcn.h>
#include <errno.h>
#include <sys/mman.h>

VirtualAddressSpace *g_pCurrentlyCloning = 0;

HostedVirtualAddressSpace HostedVirtualAddressSpace::m_KernelSpace(
    KERNEL_VIRTUAL_HEAP, KERNEL_VIRTUAL_STACK);

typedef void *(*malloc_t)(size_t);
typedef void *(*realloc_t)(void *, size_t);
typedef void (*free_t)(void *);

#include <stdio.h>
void *__libc_malloc(size_t n)
{
    static malloc_t local = (malloc_t) dlsym(RTLD_NEXT, "malloc");
    return local(n);
}

void *__libc_realloc(void *p, size_t n)
{
    static realloc_t local = (realloc_t) dlsym(RTLD_NEXT, "realloc");
    return local(p, n);
}

void __libc_free(void *p)
{
    static free_t local = (free_t) dlsym(RTLD_NEXT, "free");
    local(p);
}

VirtualAddressSpace &VirtualAddressSpace::getKernelAddressSpace()
{
    return HostedVirtualAddressSpace::m_KernelSpace;
}

VirtualAddressSpace *VirtualAddressSpace::create()
{
    return new HostedVirtualAddressSpace();
}

bool HostedVirtualAddressSpace::memIsInHeap(void *pMem)
{
    if (pMem < m_Heap)
        return false;
    else if (pMem >= getEndOfHeap())
        return false;
    else
        return true;
}

bool HostedVirtualAddressSpace::memIsInKernelHeap(void *pMem)
{
    if (pMem < KERNEL_VIRTUAL_HEAP)
        return false;
    else if (pMem >= adjust_pointer(KERNEL_VIRTUAL_HEAP, KERNEL_VIRTUAL_HEAP_SIZE))
        return false;
    else
        return true;
}

void *HostedVirtualAddressSpace::getEndOfHeap()
{
    return adjust_pointer(KERNEL_VIRTUAL_HEAP, KERNEL_VIRTUAL_HEAP_SIZE);
}

bool HostedVirtualAddressSpace::isAddressValid(void *virtualAddress)
{
    if (reinterpret_cast<uint64_t>(virtualAddress) < 0x0008000000000000ULL ||
        reinterpret_cast<uint64_t>(virtualAddress) >= 0xFFF8000000000000ULL)
        return true;
    return false;
}

bool HostedVirtualAddressSpace::isMapped(void *virtualAddress)
{
    LockGuard<Spinlock> guard(m_Lock);

    virtualAddress = page_align(virtualAddress);

    int r =
        msync(virtualAddress, PhysicalMemoryManager::getPageSize(), MS_ASYNC);
    if (r < 0)
    {
        if (errno == ENOMEM)
        {
            return false;
        }
    }

    if (this != &getKernelAddressSpace())
    {
        bool r = getKernelAddressSpace().isMapped(virtualAddress);
        if (r)
            return r;
    }

    // Find this mapping if we can.
    for (size_t i = 0; i < m_KnownMapsSize; ++i)
    {
        if (m_pKnownMaps[i].active && m_pKnownMaps[i].vaddr == virtualAddress)
            return true;
    }

    return false;
}

bool HostedVirtualAddressSpace::map(
    physical_uintptr_t physAddress, void *virtualAddress, size_t flags)
{
    virtualAddress = page_align(virtualAddress);

    // If this should be a kernel mapping, use the kernel address space.
    if (this != &getKernelAddressSpace())
        if ((virtualAddress >= KERNEL_SPACE_START) || (flags & KernelMode))
            return getKernelAddressSpace().map(
                physAddress, virtualAddress, flags);

    // mmap() won't fail if the address is already mapped, but we need to.
    if (isMapped(virtualAddress))
    {
        return false;
    }

    LockGuard<Spinlock> guard(m_Lock);

    // Map, backed onto the "physical memory" of the system.
    int prot = toFlags(flags, true);
    void *r = mmap(
        virtualAddress, PhysicalMemoryManager::getPageSize(), prot,
        MAP_FIXED | MAP_SHARED,
        HostedPhysicalMemoryManager::instance().getBackingFile(), physAddress);

    if (UNLIKELY(r == MAP_FAILED))
        return false;

    assert(r == virtualAddress);

    // Extend list of known maps if we can't fit this one in.
    if (m_numKnownMaps == m_KnownMapsSize)
    {
        size_t oldSize = m_KnownMapsSize;
        if (m_KnownMapsSize == 0)
            m_KnownMapsSize = 1;

        m_KnownMapsSize *= 2;

        size_t newSizeBytes = sizeof(mapping_t) * m_KnownMapsSize;
        if (!m_pKnownMaps)
            m_pKnownMaps = (mapping_t *) __libc_malloc(newSizeBytes);
        else
            m_pKnownMaps =
                (mapping_t *) __libc_realloc(m_pKnownMaps, newSizeBytes);

        // Mark all inactive.
        for (size_t i = oldSize; i < m_KnownMapsSize; ++i)
            m_pKnownMaps[i].active = false;
    }

    // Register in the list of known mappings.
    bool bRegistered = false;
    size_t idx = m_nLastUnmap;
    for (; idx < m_KnownMapsSize; ++idx)
    {
        if (m_pKnownMaps[idx].active)
            continue;

        bRegistered = true;
        break;
    }
    if (!bRegistered)
    {
        // Try again from the beginning.
        for (idx = 0; idx < m_nLastUnmap; ++idx)
        {
            if (m_pKnownMaps[idx].active)
                continue;

            bRegistered = true;
            break;
        }
    }

    if (!bRegistered)
        panic("Fatal algorithmic error in HostedVirtualAddressSpace::map");

    m_pKnownMaps[idx].active = true;
    m_pKnownMaps[idx].vaddr = virtualAddress;
    m_pKnownMaps[idx].paddr = physAddress;
    m_pKnownMaps[idx].flags = flags;

    ++m_numKnownMaps;

    return true;
}

void HostedVirtualAddressSpace::getMapping(
    void *virtualAddress, physical_uintptr_t &physAddress, size_t &flags)
{
    LockGuard<Spinlock> guard(m_Lock);

    virtualAddress = page_align(virtualAddress);

    // Handle kernel mappings, if needed.
    if (this != &getKernelAddressSpace())
    {
        if (getKernelAddressSpace().isMapped(virtualAddress))
        {
            getKernelAddressSpace().getMapping(
                virtualAddress, physAddress, flags);
            return;
        }
    }

    size_t pageSize = PhysicalMemoryManager::getPageSize();
    uintptr_t alignedVirtualAddress =
        reinterpret_cast<uintptr_t>(virtualAddress) & ~(pageSize - 1);
    virtualAddress = reinterpret_cast<void *>(alignedVirtualAddress);

    // Find this mapping if we can.
    for (size_t i = 0; i < m_KnownMapsSize; ++i)
    {
        if (m_pKnownMaps[i].active && m_pKnownMaps[i].vaddr == virtualAddress)
        {
            physAddress = m_pKnownMaps[i].paddr;
            flags = fromFlags(m_pKnownMaps[i].flags, true);
            return;
        }
    }

    panic("HostedVirtualAddressSpace::getMapping - function misused");
}

void HostedVirtualAddressSpace::setFlags(void *virtualAddress, size_t newFlags)
{
    LockGuard<Spinlock> guard(m_Lock);

    virtualAddress = page_align(virtualAddress);

    // Check for kernel mappings.
    if (this != &getKernelAddressSpace())
    {
        if (getKernelAddressSpace().isMapped(virtualAddress))
        {
            getKernelAddressSpace().setFlags(virtualAddress, newFlags);
            return;
        }
        else if (newFlags & KernelMode)
            WARNING("setFlags called with KernelMode as a flag, page is not "
                    "mapped in kernel.");
    }

    for (size_t i = 0; i < m_KnownMapsSize; ++i)
    {
        if (m_pKnownMaps[i].active && m_pKnownMaps[i].vaddr == virtualAddress)
        {
            m_pKnownMaps[i].flags = newFlags;
            break;
        }
    }

    size_t flags = toFlags(newFlags, true);
    mprotect(virtualAddress, PhysicalMemoryManager::getPageSize(), flags);
}

void HostedVirtualAddressSpace::unmap(void *virtualAddress)
{
    LockGuard<Spinlock> guard(m_Lock);

    virtualAddress = page_align(virtualAddress);

    // Check for kernel mappings.
    if (this != &getKernelAddressSpace())
    {
        if (getKernelAddressSpace().isMapped(virtualAddress))
        {
            getKernelAddressSpace().unmap(virtualAddress);
            return;
        }
    }

    for (size_t i = 0; i < m_KnownMapsSize; ++i)
    {
        if (m_pKnownMaps[i].active && m_pKnownMaps[i].vaddr == virtualAddress)
        {
            m_pKnownMaps[i].active = false;
            m_nLastUnmap = i;
            break;
        }
    }

    munmap(virtualAddress, PhysicalMemoryManager::getPageSize());
}

VirtualAddressSpace *HostedVirtualAddressSpace::clone(bool copyOnWrite)
{
    HostedVirtualAddressSpace *pNew =
        static_cast<HostedVirtualAddressSpace *>(VirtualAddressSpace::create());

    {
        LockGuard<Spinlock> guard(m_Lock);

        // Copy over the known maps so the new address space can find them.
        pNew->m_pKnownMaps =
            (mapping_t *) __libc_malloc(m_KnownMapsSize * sizeof(mapping_t));
        MemoryCopy(
            pNew->m_pKnownMaps, m_pKnownMaps,
            m_KnownMapsSize * sizeof(mapping_t));
        pNew->m_KnownMapsSize = m_KnownMapsSize;
        pNew->m_numKnownMaps = m_numKnownMaps;
        pNew->m_nLastUnmap = m_nLastUnmap;

        // Readjust flags on the new mappings if needed.
        for (size_t i = 0; i < pNew->m_KnownMapsSize; ++i)
        {
            mapping_t *mapping = &pNew->m_pKnownMaps[i];
            if (!mapping->active)
                continue;

            PhysicalMemoryManager::instance().pin(mapping->paddr);

            if (mapping->flags & Shared)
            {
                continue;
            }

            if (!(mapping->flags & CopyOnWrite))
                PhysicalMemoryManager::instance().pin(mapping->paddr);

            if (mapping->flags & Write)
            {
                mapping->flags |= CopyOnWrite;
            }
            mapping->flags &= ~Write;
        }
    }

    if (m_pStackTop < KERNEL_SPACE_START)
    {
        pNew->m_pStackTop = m_pStackTop;
        for (Vector<Stack *>::Iterator it = m_freeStacks.begin();
             it != m_freeStacks.end(); ++it)
        {
            Stack *pNewStack = new Stack(**it);
            pNew->m_freeStacks.pushBack(pNewStack);
        }
    }

    if (m_Heap < KERNEL_SPACE_START)
    {
        pNew->m_Heap = m_Heap;
        pNew->m_HeapEnd = m_HeapEnd;
        NOTICE("clone: heap=" << m_Heap << " end=" << m_HeapEnd);
    }

    return pNew;
}

void HostedVirtualAddressSpace::revertToKernelAddressSpace()
{
    LockGuard<Spinlock> guard(m_Lock);

    for (size_t i = 0; i < m_KnownMapsSize; ++i)
    {
        if (m_pKnownMaps[i].active)
        {
            if (getKernelAddressSpace().isMapped(m_pKnownMaps[i].vaddr))
            {
                m_pKnownMaps[i].active = false;
                m_nLastUnmap = i;
                continue;
            }
            else if (m_pKnownMaps[i].vaddr > KERNEL_SPACE_START)
                continue;

            munmap(m_pKnownMaps[i].vaddr, PhysicalMemoryManager::getPageSize());

            // Clean up references to physical memory as needed.
            if ((m_pKnownMaps[i].flags & (Shared | Swapped)) == 0)
                PhysicalMemoryManager::instance().freePage(
                    m_pKnownMaps[i].paddr);

            m_pKnownMaps[i].active = false;
        }
    }
}

VirtualAddressSpace::Stack *HostedVirtualAddressSpace::allocateStack()
{
    size_t sz = USERSPACE_VIRTUAL_STACK_SIZE;
    if (this == &getKernelAddressSpace())
        sz = KERNEL_STACK_SIZE;
    return doAllocateStack(sz);
}

VirtualAddressSpace::Stack *
HostedVirtualAddressSpace::allocateStack(size_t stackSz)
{
    if (stackSz == 0)
        return allocateStack();
    return doAllocateStack(stackSz);
}

VirtualAddressSpace::Stack *
HostedVirtualAddressSpace::doAllocateStack(size_t sSize)
{
    size_t flags = 0;
    bool bMapAll = true;
    if (this == &m_KernelSpace)
    {
        // Don't demand map kernel mode stacks.
        flags = VirtualAddressSpace::KernelMode;
        bMapAll = true;
    }

    m_Lock.acquire();

    size_t pageSz = PhysicalMemoryManager::getPageSize();

    // Grab a new stack pointer. Use the list of freed stacks if we can,
    // otherwise adjust the internal stack pointer. Using the list of freed
    // stacks helps avoid having the virtual address creep downwards.
    void *pStack = 0;
    if (m_freeStacks.count() != 0)
    {
        Stack *poppedStack = m_freeStacks.popBack();
        if (poppedStack->getSize() >= sSize)
        {
            pStack = poppedStack->getTop();
        }
        delete poppedStack;
    }
    else
    {
        pStack = m_pStackTop;

        // Always leave one page unmapped between each stack to catch overflow.
        m_pStackTop =
            adjust_pointer(m_pStackTop, -static_cast<ssize_t>(sSize + pageSz));
    }

    m_Lock.release();

    // Map the top of the stack in proper.
    uintptr_t firstPage = reinterpret_cast<uintptr_t>(pStack) - pageSz;
    physical_uintptr_t phys = PhysicalMemoryManager::instance().allocatePage();
    if (!bMapAll)
        PhysicalMemoryManager::instance().pin(phys);
    if (!map(
            phys, reinterpret_cast<void *>(firstPage),
            flags | VirtualAddressSpace::Write))
        WARNING("map() failed in doAllocateStack");

    // Bring in the rest of the stack as CoW.
    uintptr_t stackBottom = reinterpret_cast<uintptr_t>(pStack) - sSize;
    for (uintptr_t addr = stackBottom; addr < firstPage; addr += pageSz)
    {
        size_t map_flags = 0;

        if (!bMapAll)
        {
            // Copy first stack page on write.
            PhysicalMemoryManager::instance().pin(phys);
            map_flags = VirtualAddressSpace::CopyOnWrite;
        }
        else
        {
            phys = PhysicalMemoryManager::instance().allocatePage();
            map_flags = VirtualAddressSpace::Write;
        }

        if (!map(phys, reinterpret_cast<void *>(addr), flags | map_flags))
            WARNING("CoW map() failed in doAllocateStack");
    }

    Stack *stackInfo = new Stack(pStack, sSize);
    return stackInfo;
}

void HostedVirtualAddressSpace::freeStack(Stack *pStack)
{
    size_t pageSz = PhysicalMemoryManager::getPageSize();

    // Clean up the stack
    uintptr_t stackTop = reinterpret_cast<uintptr_t>(pStack->getTop());
    for (size_t i = 0; i < pStack->getSize(); i += pageSz)
    {
        stackTop -= pageSz;
        void *v = reinterpret_cast<void *>(stackTop);
        if (!isMapped(v))
            break;  // Hit end of stack.

        size_t flags = 0;
        physical_uintptr_t phys = 0;
        getMapping(v, phys, flags);

        unmap(v);
        PhysicalMemoryManager::instance().freePage(phys);
    }

    // Add the stack to the list
    m_Lock.acquire();
    m_freeStacks.pushBack(pStack);
    m_Lock.release();
}

HostedVirtualAddressSpace::~HostedVirtualAddressSpace()
{
    // TODO: Free other things, perhaps in VirtualAddressSpace
    //       We can't do this in VirtualAddressSpace destructor though!
}

HostedVirtualAddressSpace::HostedVirtualAddressSpace()
    : VirtualAddressSpace(USERSPACE_VIRTUAL_HEAP),
      m_pStackTop(USERSPACE_VIRTUAL_STACK), m_freeStacks(),
      m_bKernelSpace(false), m_Lock(false, true), m_pKnownMaps(0),
      m_numKnownMaps(0), m_nLastUnmap(0)
{
}

HostedVirtualAddressSpace::HostedVirtualAddressSpace(
    void *Heap, void *VirtualStack)
    : VirtualAddressSpace(Heap), m_pStackTop(VirtualStack), m_freeStacks(),
      m_bKernelSpace(true), m_Lock(false, true), m_pKnownMaps(0),
      m_numKnownMaps(0), m_nLastUnmap(0)
{
}

uint64_t HostedVirtualAddressSpace::toFlags(size_t flags, bool bFinal)
{
    uint64_t Flags = 0;
    if (flags & Write)
        Flags |= PROT_WRITE;
    if (flags & Swapped)
        Flags |= PROT_NONE;
    else
        Flags |= PROT_READ;
    if (flags & Execute)
        Flags |= PROT_EXEC;
    return Flags;
}

size_t HostedVirtualAddressSpace::fromFlags(uint64_t Flags, bool bFinal)
{
    return Flags;
}

void HostedVirtualAddressSpace::switchAddressSpace(
    VirtualAddressSpace &a, VirtualAddressSpace &b)
{
    HostedVirtualAddressSpace &oldSpace =
        static_cast<HostedVirtualAddressSpace &>(a);
    HostedVirtualAddressSpace &newSpace =
        static_cast<HostedVirtualAddressSpace &>(b);

    if (&oldSpace != &getKernelAddressSpace())
    {
        for (size_t i = 0; i < oldSpace.m_KnownMapsSize; ++i)
        {
            if (oldSpace.m_pKnownMaps[i].active)
            {
                if (getKernelAddressSpace().isMapped(
                        oldSpace.m_pKnownMaps[i].vaddr))
                {
                    continue;
                }
                else if (oldSpace.m_pKnownMaps[i].flags & KernelMode)
                {
                    continue;
                }

                munmap(
                    oldSpace.m_pKnownMaps[i].vaddr,
                    PhysicalMemoryManager::getPageSize());
            }
        }
    }

    for (size_t i = 0; i < newSpace.m_KnownMapsSize; ++i)
    {
        if (newSpace.m_pKnownMaps[i].active)
        {
            if (getKernelAddressSpace().isMapped(
                    newSpace.m_pKnownMaps[i].vaddr))
            {
                continue;
            }

            mmap(
                newSpace.m_pKnownMaps[i].vaddr,
                PhysicalMemoryManager::getPageSize(),
                newSpace.toFlags(newSpace.m_pKnownMaps[i].flags, true),
                MAP_FIXED | MAP_SHARED,
                HostedPhysicalMemoryManager::instance().getBackingFile(),
                newSpace.m_pKnownMaps[i].paddr);
        }
    }
}
