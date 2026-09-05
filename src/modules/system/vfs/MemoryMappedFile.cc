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

#include "MemoryMappedFile.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/errors.h"
#include "pedigree/kernel/process/MemoryPressureManager.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/Uninterruptible.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/MemoryAllocator.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#include "File.h"
#include "VFS.h"

MemoryMapManager MemoryMapManager::m_Instance;

physical_uintptr_t AnonymousMemoryMap::m_Zero = 0;

// #define DEBUG_MMOBJECTS

namespace {
class AddressSpaceRestorer {
 public:
  explicit AddressSpaceRestorer(VirtualAddressSpace& addressSpace) : m_AddressSpace(addressSpace) {}

  ~AddressSpaceRestorer() {
    Processor::switchAddressSpace(m_AddressSpace);
  }

 private:
  NOT_COPYABLE_OR_ASSIGNABLE(AddressSpaceRestorer);

  VirtualAddressSpace& m_AddressSpace;
};

class TemporaryPhysicalMapping {
 public:
  TemporaryPhysicalMapping(physical_uintptr_t page, const char* name)
      : m_Region(name), m_Mapped(false) {
    PhysicalMemoryManager& memory = PhysicalMemoryManager::instance();
    if (!memory.allocateRegion(
            m_Region, 1, PhysicalMemoryManager::virtualOnly | PhysicalMemoryManager::anonymous,
            VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write)) {
      return;
    }

    VirtualAddressSpace& kernelSpace = VirtualAddressSpace::getKernelAddressSpace();
    m_Mapped = kernelSpace.map(page, m_Region.virtualAddress(),
                               VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write);
    if (!m_Mapped) {
      m_Region.free();
    }
  }

  ~TemporaryPhysicalMapping() {
    if (m_Mapped) {
      // A virtual-only MemoryRegion owns pages left mapped inside it. Remove
      // this alias first so releasing the reservation does not free the page.
      VirtualAddressSpace::getKernelAddressSpace().unmap(m_Region.virtualAddress());
    }
    m_Region.free();
  }

  bool valid() const {
    return m_Mapped;
  }

  void* address() const {
    return m_Region.virtualAddress();
  }

 private:
  NOT_COPYABLE_OR_ASSIGNABLE(TemporaryPhysicalMapping);

  MemoryRegion m_Region;
  bool m_Mapped;
};

size_t protectionFlags(size_t flags, MemoryMappedObject::Permissions permissions,
                       bool sharedWritable) {
  flags &= ~(VirtualAddressSpace::Write | VirtualAddressSpace::Execute |
             VirtualAddressSpace::NoAccess | VirtualAddressSpace::WriteProtected);
  if (permissions == MemoryMappedObject::None) {
    flags |= VirtualAddressSpace::NoAccess;
  }
  if (permissions & MemoryMappedObject::Exec) {
    flags |= VirtualAddressSpace::Execute;
  }
  if (!(permissions & MemoryMappedObject::Write)) {
    flags |= VirtualAddressSpace::WriteProtected;
  } else if (!(flags & VirtualAddressSpace::CopyOnWrite) &&
             (!(flags & VirtualAddressSpace::Shared) || sharedWritable)) {
    flags |= VirtualAddressSpace::Write;
  }
  return flags;
}

void* currentOperationOwner() {
  ProcessorInformation& information = Processor::information();
  Thread* thread = information.getCurrentThread();
  return thread ? static_cast<void*>(thread) : static_cast<void*>(&information);
}
}  // namespace

MemoryMappedObject::~MemoryMappedObject() {}

AnonymousMemoryMap::AnonymousMemoryMap(uintptr_t address, size_t length,
                                       MemoryMappedObject::Permissions perms)
    : MemoryMappedObject(address, true, length, perms), m_Mappings(), m_Lock(false) {
  LockGuard<Spinlock> guard(m_Lock);

  if (m_Zero == 0) {
    m_Zero = PhysicalMemoryManager::instance().allocatePage();
    if (!m_Zero) {
      FATAL("AnonymousMemoryMap: could not allocate the shared zero page");
      return;
    }

    TemporaryPhysicalMapping temporary(m_Zero, "Anonymous Shared Zero Page");
    if (!temporary.valid()) {
      PhysicalMemoryManager::instance().freePage(m_Zero);
      m_Zero = 0;
      FATAL("AnonymousMemoryMap: could not map the shared zero page");
      return;
    }

    ByteSet(temporary.address(), 0, PhysicalMemoryManager::getPageSize());
    PhysicalMemoryManager::instance().pin(m_Zero);
  }
}

MemoryMappedObject* AnonymousMemoryMap::clone() {
  LockGuard<Spinlock> guard(m_Lock);

  AnonymousMemoryMap* pResult = new AnonymousMemoryMap(m_Address, m_Length, m_Permissions);
  pResult->m_Mappings = m_Mappings;
  return pResult;
}

MemoryMappedObject* AnonymousMemoryMap::split(uintptr_t at) {
  LockGuard<Spinlock> guard(m_Lock);

  if (at < m_Address || at >= (m_Address + m_Length)) {
    ERROR("AnonymousMemoryMap::split() given bad at parameter (at="
          << at << ", address=" << m_Address << ", end=" << (m_Address + m_Length) << ")");
    return 0;
  }

  if (at == m_Address) {
    ERROR("AnonymousMemoryMap::split() misused, at == base address");
    return 0;
  }

  // Change our own object to fit in the new region.
  size_t oldLength = m_Length;
  m_Length = at - m_Address;

  // New object.
  AnonymousMemoryMap* pResult = new AnonymousMemoryMap(at, oldLength - m_Length, m_Permissions);

  // Fix up mapping metadata.
  for (List<void*>::Iterator it = m_Mappings.begin(); it != m_Mappings.end();) {
    uintptr_t v = reinterpret_cast<uintptr_t>(*it);
    if (v >= at) {
      pResult->m_Mappings.pushBack(*it);
      it = m_Mappings.erase(it);
    } else
      ++it;
  }

  return pResult;
}

bool AnonymousMemoryMap::remove(size_t length) {
  LockGuard<Spinlock> guard(m_Lock);

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  size_t pageSz = PhysicalMemoryManager::getPageSize();

  if (length & (pageSz - 1)) {
    length += pageSz;
    length &= ~(pageSz - 1);
  }

  if (length >= m_Length) {
    unmapUnlocked();
    return true;
  }

  m_Address += length;
  m_Length -= length;

  // Remove any existing mappings in this range.
  for (List<void*>::Iterator it = m_Mappings.begin(); it != m_Mappings.end();) {
    uintptr_t virt = reinterpret_cast<uintptr_t>(*it);
    if (virt >= m_Address) {
      ++it;
      continue;
    }

    void* v = *it;
    if (va.isMapped(v)) {
      size_t flags;
      physical_uintptr_t phys;

      va.getMapping(v, phys, flags);

      va.unmap(v);
      PhysicalMemoryManager::instance().freePage(phys);
    }

    it = m_Mappings.erase(it);
  }

  return false;
}

void AnonymousMemoryMap::setPermissions(MemoryMappedObject::Permissions perms) {
  LockGuard<Spinlock> guard(m_Lock);
  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  for (List<void*>::Iterator it = m_Mappings.begin(); it != m_Mappings.end(); ++it) {
    if (va.isMapped(*it)) {
      physical_uintptr_t physical;
      size_t flags;
      va.getMapping(*it, physical, flags);
      va.setFlags(*it, protectionFlags(flags, perms, false));
    }
  }
  m_Permissions = perms;
}

void AnonymousMemoryMap::unmap() {
  LockGuard<Spinlock> guard(m_Lock);

  unmapUnlocked();
}

bool AnonymousMemoryMap::trap(uintptr_t address, bool bWrite) {
  LockGuard<Spinlock> guard(m_Lock);

#ifdef DEBUG_MMOBJECTS
  NOTICE("AnonymousMemoryMap::trap(" << address << ", " << bWrite << ")");
#endif

  size_t pageSz = PhysicalMemoryManager::getPageSize();
  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();

  // Page-align the trap address
  address = address & ~(pageSz - 1);

  // Skip out on a few things if we can.
  if (bWrite && !(m_Permissions & Write)) {
#ifdef DEBUG_MMOBJECTS
    NOTICE("  -> no write permission");
#endif
    return false;
  } else if ((!bWrite) && !(m_Permissions & Read)) {
#ifdef DEBUG_MMOBJECTS
    NOTICE("  -> no read permission");
#endif
    return false;
  }

  // Add execute flag.
  size_t extraFlags = 0;
  if (m_Permissions & Exec)
    extraFlags |= VirtualAddressSpace::Execute;

  if (!bWrite) {
    if (va.isMapped(reinterpret_cast<void*>(address))) {
      ERROR("trapped on a currently-mapped page!");
      return false;
    }
    PhysicalMemoryManager::instance().pin(m_Zero);
    if (!va.map(m_Zero, reinterpret_cast<void*>(address),
                VirtualAddressSpace::Shared | extraFlags)) {
      ERROR("map() failed for AnonymousMemoryMap::trap() - read @" << Hex << address);
      PhysicalMemoryManager::instance().freePage(m_Zero);
      return false;
    }

    m_Mappings.pushBack(reinterpret_cast<void*>(address));
  } else {
    // "Copy" on write... but not really :)
    physical_uintptr_t newPage = PhysicalMemoryManager::instance().allocatePage();
    if (!newPage) {
      ERROR("allocatePage() failed in AnonymousMemoryMap::trap() - write");
      return false;
    }

    {
      TemporaryPhysicalMapping temporary(newPage, "Anonymous Page Initialisation");
      if (!temporary.valid()) {
        ERROR("temporary map failed in AnonymousMemoryMap::trap() - write");
        PhysicalMemoryManager::instance().freePage(newPage);
        return false;
      }
      ByteSet(temporary.address(), 0, pageSz);
    }

    // Publish only after the page is fully initialised. Other processors can
    // use a present userspace mapping without entering this trap handler.
    if (va.isMapped(reinterpret_cast<void*>(address))) {
      va.unmap(reinterpret_cast<void*>(address));

      // Drop the refcount on the zero page.
      PhysicalMemoryManager::instance().freePage(m_Zero);
    }
    for (List<void*>::Iterator it = m_Mappings.begin(); it != m_Mappings.end(); ++it) {
      if (*it == reinterpret_cast<void*>(address)) {
        m_Mappings.erase(it);
        break;
      }
    }

    if (!va.map(newPage, reinterpret_cast<void*>(address),
                VirtualAddressSpace::Write | extraFlags)) {
      ERROR("map() failed in AnonymousMemoryMap::trap() - write");
      PhysicalMemoryManager::instance().freePage(newPage);
      return false;
    }
    m_Mappings.pushBack(reinterpret_cast<void*>(address));
  }

  return true;
}

void AnonymousMemoryMap::unmapUnlocked() {
#ifdef DEBUG_MMOBJECTS
  NOTICE("AnonymousMemoryMap::unmap()");
#endif

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();

  for (List<void*>::Iterator it = m_Mappings.begin(); it != m_Mappings.end(); ++it) {
    void* v = *it;
    if (va.isMapped(v)) {
      size_t flags;
      physical_uintptr_t phys;

      va.getMapping(v, phys, flags);

      // Clean up. Shared read-only zero page will only have its refcount
      // decreased by this - it will not hit zero.
      va.unmap(v);
      PhysicalMemoryManager::instance().freePage(phys);
    }
  }

  m_Mappings.clear();
}

MemoryMappedFile::MemoryMappedFile(uintptr_t address, size_t length, size_t offset, File* backing,
                                   bool bCopyOnWrite, MemoryMappedObject::Permissions perms,
                                   MemoryMappedObject::Permissions maximumPerms,
                                   const SharedPointer<MappingAttachment>& attachment)
    : MemoryMappedObject(address, bCopyOnWrite, length, perms, maximumPerms),
      m_pBacking(backing),
      m_Offset(offset),
      m_Mappings(),
      m_Lock(),
      m_bVfsLease(backing && VFS::instance().retainTrackedFile(backing)) {
  assert(m_pBacking);
  m_Attachment = attachment;
}

MemoryMappedFile::~MemoryMappedFile() {
  unmap();
  if (m_bVfsLease) {
    m_bVfsLease = false;
    VFS::instance().untrackFile(m_pBacking);
  }
}

MemoryMappedObject* MemoryMappedFile::clone() {
  TerminationDeferral terminationDeferral;
  LockGuard<Mutex> guard(m_Lock);

  MemoryMappedFile* pResult =
      new MemoryMappedFile(m_Address, m_Length, m_Offset, m_pBacking, m_bCopyOnWrite, m_Permissions,
                           m_MaximumPermissions, m_Attachment);
  pResult->m_Mappings = m_Mappings;

  for (auto it = m_Mappings.begin(); it != m_Mappings.end(); ++it) {
    // Bump reference count on backing file page if needed.
    size_t fileOffset = (it.key() - m_Address) + m_Offset;
    if (it.value() == ~0UL)
      m_pBacking->getPhysicalPage(fileOffset);
  }

  return pResult;
}

MemoryMappedObject* MemoryMappedFile::split(uintptr_t at) {
  TerminationDeferral terminationDeferral;
  LockGuard<Mutex> guard(m_Lock);

  size_t pageSz = PhysicalMemoryManager::getPageSize();

  if (at < m_Address || at >= (m_Address + m_Length)) {
    ERROR("MemoryMappedFile::split() given bad at parameter (at="
          << at << ", address=" << m_Address << ", end=" << (m_Address + m_Length) << ")");
    return 0;
  }

  if (at == m_Address) {
    ERROR("MemoryMappedFile::split() misused, at == base address");
    return 0;
  }

  uintptr_t oldEnd = m_Address + m_Length;

  // Change our own object to fit in the new region.
  size_t oldLength = m_Length;
  m_Length = at - m_Address;

  // New object.
  MemoryMappedFile* pResult =
      new MemoryMappedFile(at, oldLength - m_Length, m_Offset + m_Length, m_pBacking,
                           m_bCopyOnWrite, m_Permissions, m_MaximumPermissions, m_Attachment);

  // Fix up mapping metadata.
  for (uintptr_t virt = at; virt < oldEnd; virt += pageSz) {
    if (m_Mappings.contains(virt)) {
      physical_uintptr_t old = getMapping(virt);
      untrackMapping(virt);
      pResult->trackMapping(virt, old);
    }
  }

  return pResult;
}

bool MemoryMappedFile::remove(size_t length) {
  TerminationDeferral terminationDeferral;
  LockGuard<Mutex> guard(m_Lock);

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  size_t pageSz = PhysicalMemoryManager::getPageSize();

  if (length & (pageSz - 1)) {
    length += pageSz;
    length &= ~(pageSz - 1);
  }

  if (length >= m_Length) {
    unmapUnlocked();
    return true;
  }

  uintptr_t oldStart = m_Address;
  size_t oldOffset = m_Offset;

  m_Address += length;
  m_Offset += length;
  m_Length -= length;

  // Remove any existing mappings in this range.
  for (uintptr_t virt = oldStart; virt < m_Address; virt += pageSz) {
    void* v = reinterpret_cast<void*>(virt);
    if (va.isMapped(v)) {
      size_t flags;
      physical_uintptr_t phys;

      va.getMapping(v, phys, flags);
      va.unmap(v);

      physical_uintptr_t p = getMapping(virt);
      if (p == ~0UL) {
        size_t fileOffset = (virt - oldStart) + oldOffset;

        if (!m_bCopyOnWrite) {
          m_pBacking->syncAndReturnPhysicalPage(fileOffset, true);
        } else {
          m_pBacking->returnPhysicalPage(fileOffset);
        }
      } else
        PhysicalMemoryManager::instance().freePage(phys);
    }

    untrackMapping(virt);
  }

  return false;
}

void MemoryMappedFile::setPermissions(MemoryMappedObject::Permissions perms) {
  TerminationDeferral terminationDeferral;
  LockGuard<Mutex> guard(m_Lock);
  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  for (auto it = m_Mappings.begin(); it != m_Mappings.end(); ++it) {
    void* address = reinterpret_cast<void*>(it.key());
    if (va.isMapped(address)) {
      physical_uintptr_t physical;
      size_t flags;
      va.getMapping(address, physical, flags);
      va.setFlags(address, protectionFlags(flags, perms, !m_bCopyOnWrite));
    }
  }
  m_Permissions = perms;
}

bool MemoryMappedFile::preparePermissions(uintptr_t base, size_t length, Permissions perms) {
  if (m_bCopyOnWrite || !(perms & Write) || (m_Permissions & Write)) {
    return true;
  }
  LockGuard<Mutex> guard(m_Lock);
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  for (auto it = m_Mappings.begin(); it != m_Mappings.end(); ++it) {
    if (it.key() >= base && it.key() - base < length && it.value() == ~0UL &&
        !m_pBacking->prepareSharedMapping(m_Offset + (it.key() - m_Address), pageSize)) {
      return false;
    }
  }
  return true;
}

bool MemoryMappedFile::sharedBacking(uintptr_t at, uintptr_t& identity, size_t& offset) const {
  if (m_bCopyOnWrite || at < m_Address || at - m_Address >= m_Length) {
    return false;
  }
  identity = m_pBacking->futexIdentity();
  offset = m_Offset + (at - m_Address);
  return true;
}

bool MemoryMappedFile::usesBacking(uintptr_t identity) const {
  return m_pBacking->futexIdentity() == identity;
}

bool MemoryMappedFile::beyondBackingEnd(uintptr_t at) const {
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const uintptr_t page = at & ~(pageSize - 1);
  const size_t displacement = page - m_Address;
  return displacement > ~static_cast<size_t>(0) - m_Offset ||
         m_Offset + displacement >= m_pBacking->getSize();
}

void MemoryMappedFile::discardFilePages(VirtualAddressSpace& space, size_t end, bool borrowedOnly) {
  LockGuard<Mutex> guard(m_Lock);
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t firstDiscardedPage = end / pageSize + (end % pageSize != 0);
  uintptr_t cursor = m_Address;
  uintptr_t address = 0;
  physical_uintptr_t tracked = 0;
  while (m_Mappings.lowerBound(cursor, address, tracked)) {
    if (address < m_Address || address - m_Address >= m_Length) {
      break;
    }
    // The copied key survives rotations when this entry is removed below.
    cursor = address + 1;
    const size_t displacement = address - m_Address;
    const size_t fileOffset = m_Offset + displacement;
    if (!borrowedOnly && fileOffset / pageSize < firstDiscardedPage) {
      continue;
    }
    const bool loan = tracked == ~0UL;
    if (borrowedOnly && !loan) {
      continue;
    }
    physical_uintptr_t physical = 0;
    size_t flags = 0;
    const bool detached = space.detachMapping(reinterpret_cast<void*>(address), physical, flags,
                                              borrowedOnly ? VirtualAddressSpace::Borrowed : 0);
    if (detached && (flags & VirtualAddressSpace::Borrowed) && !m_bCopyOnWrite) {
      m_pBacking->sync(fileOffset, false);
    }
    // A generic COW replacement may leave the object's original cache loan
    // outstanding. Retire that loan without discarding the private replacement
    // during the fallible preparation phase.
    if (loan || (detached && (flags & VirtualAddressSpace::Borrowed))) {
      m_pBacking->returnPhysicalPage(fileOffset);
    }
    if (!detached && physical) {
      trackMapping(address, physical);
      continue;
    }
    if (detached && !(flags & VirtualAddressSpace::Borrowed)) {
      PhysicalMemoryManager::instance().freePage(physical);
    }
    untrackMapping(address);
  }
}

static physical_uintptr_t getBackingPage(File* pBacking, size_t fileOffset) {
  size_t pageSz = PhysicalMemoryManager::getPageSize();
  const size_t fileSize = pBacking->getSize();
  if (fileOffset >= fileSize) {
    return ~0UL;
  }
  const size_t expected = fileSize - fileOffset < pageSz ? fileSize - fileOffset : pageSz;

  physical_uintptr_t phys = pBacking->getPhysicalPage(fileOffset);
  if (phys == ~0UL) {
    // No page found, trigger a read to fix that!
    uint64_t actual = 0;

    if ((actual = pBacking->read(fileOffset, expected, 0)) != expected) {
      ERROR("Short read of " << pBacking->getName() << " in getBackingPage() - wanted " << expected
                             << " bytes but got " << actual << " instead");
    }
    phys = pBacking->getPhysicalPage(fileOffset);
    if (phys == ~0UL) {
      ERROR(
          "*** Could not manage to get a physical page for a "
          "MemoryMappedFile ("
          << pBacking->getName() << ") - read got " << actual << " bytes!");
    }
  }

  return phys;
}

bool MemoryMappedFile::sync(uintptr_t at, bool async) {
  TerminationDeferral terminationDeferral;
  LockGuard<Mutex> guard(m_Lock);
  if (!m_bCopyOnWrite && at >= m_Address && at - m_Address < m_Length && getMapping(at) == ~0UL) {
    // Write permission can have been removed since the page was dirtied.
    return m_pBacking->sync(m_Offset + (at - m_Address), async);
  }
  return true;
}

void MemoryMappedFile::invalidate(uintptr_t at) {
  // Shared mappings already alias the page cache. Invalidating must not discard
  // private modifications, which never belong to the backing file.
}

void MemoryMappedFile::unmap() {
  TerminationDeferral terminationDeferral;
  LockGuard<Mutex> guard(m_Lock);

  unmapUnlocked();
}

bool MemoryMappedFile::trap(uintptr_t address, bool bWrite) {
  TerminationDeferral terminationDeferral;
  LockGuard<Mutex> guard(m_Lock);

#ifdef DEBUG_MMOBJECTS
  NOTICE("MemoryMappedFile::trap(" << address << ", " << bWrite << ")");
#endif

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  size_t pageSz = PhysicalMemoryManager::getPageSize();

  // Page-align the trap address
  address = address & ~(pageSz - 1);
  size_t mappingOffset = (address - m_Address);
  size_t fileOffset = m_Offset + mappingOffset;

  if (beyondBackingEnd(address)) {
    return false;
  }

  bool bWillEof = (mappingOffset + pageSz) > m_Length;
  bool bShouldCopy = m_bCopyOnWrite && (bWillEof || bWrite);

  // Skip out on a few things if we can.
  if (bWrite && !(m_Permissions & Write)) {
#ifdef DEBUG_MMOBJECTS
    DEBUG_LOG(" -> ignoring, was a write and this is not a writable mapping.");
#endif
    return false;
  } else if ((!bWrite) && !(m_Permissions & Read)) {
#ifdef DEBUG_MMOBJECTS
    DEBUG_LOG(" -> ignoring, was a read and this is not a readable mapping.");
#endif
    return false;
  }

#ifdef DEBUG_MMOBJECTS
  DEBUG_LOG(" -> mapping offset is " << mappingOffset << ", file offset: " << fileOffset);
  DEBUG_LOG(" -> will eof: " << bWillEof << ", should copy: " << bShouldCopy);
#endif

  // Add execute flag.
  size_t extraFlags = 0;
  if (m_Permissions & Exec)
    extraFlags |= VirtualAddressSpace::Execute;

  if (!bShouldCopy) {
    if (!m_bCopyOnWrite && (m_Permissions & Write) &&
        !m_pBacking->prepareSharedMapping(fileOffset, pageSz)) {
      return false;
    }
    // No need to lock this section - only accessing m_Mappings once
    physical_uintptr_t phys = getBackingPage(m_pBacking, fileOffset);
    if (phys == ~0UL) {
      ERROR("MemoryMappedFile::trap couldn't get a backing page");
      return false;  // Fail.
    }

    size_t flags = VirtualAddressSpace::Shared | VirtualAddressSpace::Borrowed;
    if (!m_bCopyOnWrite && (m_Permissions & Write)) {
      flags |= VirtualAddressSpace::Write;
    }

    bool r = va.map(phys, reinterpret_cast<void*>(address), flags | extraFlags);
    if (!r) {
      ERROR("map() failed in MemoryMappedFile::trap (no-copy)");
      m_pBacking->returnPhysicalPage(fileOffset);
      return false;
    }

    trackMapping(address, ~0);
  } else {
    // Prepare the private page before exposing it to userspace.
    physical_uintptr_t newPhys = PhysicalMemoryManager::instance().allocatePage();
    if (!newPhys) {
      ERROR("allocatePage() failed in MemoryMappedFile::trap (copy)");
      return false;
    }

    size_t nBytes = m_Length - mappingOffset;
    if (nBytes > pageSz)
      nBytes = pageSz;

    bool readSucceeded = false;
    {
      TemporaryPhysicalMapping temporary(newPhys, "Mapped File Page Initialisation");
      if (!temporary.valid()) {
        ERROR("temporary map failed in MemoryMappedFile::trap (copy)");
        PhysicalMemoryManager::instance().freePage(newPhys);
        return false;
      }

      uintptr_t temporaryAddress = reinterpret_cast<uintptr_t>(temporary.address());
      size_t nRead = m_pBacking->read(fileOffset, nBytes, temporaryAddress);
      readSucceeded = nRead && nRead <= nBytes;
      if (readSucceeded && nRead < pageSz) {
        // Couldn't quite read in a page - zero out what's left.
        ByteSet(reinterpret_cast<void*>(temporaryAddress + nRead), 0, pageSz - nRead);
      }
    }
    if (!readSucceeded) {
      PhysicalMemoryManager::instance().freePage(newPhys);
      return false;
    }

    // Ditch an existing mapping only once its replacement is ready.
    if (va.isMapped(reinterpret_cast<void*>(address))) {
      va.unmap(reinterpret_cast<void*>(address));

      // One less reference to the backing page.
      m_pBacking->returnPhysicalPage(fileOffset);
      untrackMapping(address);
    }

    bool r = va.map(newPhys, reinterpret_cast<void*>(address),
                    ((m_Permissions & Write) ? VirtualAddressSpace::Write : 0) | extraFlags);
    if (!r) {
      ERROR("map() failed in MemoryMappedFile::trap (copy)");
      PhysicalMemoryManager::instance().freePage(newPhys);
      return false;
    }

    trackMapping(address, newPhys);
  }

  return true;
}

bool MemoryMappedFile::compact() {
  TerminationDeferral terminationDeferral;
  if (!m_Lock.tryAcquire()) {
    return false;
  }

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  bool released = false;
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  for (uintptr_t address = m_Address; address < m_Address + m_Length; address += pageSize) {
    if (getMapping(address) != ~0UL) {
      continue;
    }
    void* page = reinterpret_cast<void*>(address);
    if (!va.isMapped(page)) {
      continue;
    }
    if (!m_pBacking->tryBeginMappingRelease()) {
      break;
    }
    const size_t offset = m_Offset + (address - m_Address);
    va.unmap(page);
    if (!m_bCopyOnWrite) {
      m_pBacking->sync(offset, false);
    }
    m_pBacking->returnPhysicalPage(offset);
    m_pBacking->endMappingRelease();
    untrackMapping(address);
    released = true;
  }
  m_Lock.release();
  return released;
}

void MemoryMappedFile::unmapUnlocked() {
#ifdef DEBUG_MMOBJECTS
  NOTICE("MemoryMappedFile::unmap()");
#endif

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();

  if (!getMappingCount())
    return;

  for (auto it = m_Mappings.begin(); it != m_Mappings.end(); ++it) {
    void* v = reinterpret_cast<void*>(it.key());
    if (!va.isMapped(v))
      continue;  // Already unmapped...

    size_t flags = 0;
    physical_uintptr_t phys = 0;
    va.getMapping(v, phys, flags);
    va.unmap(v);

    physical_uintptr_t p = it.value();
    if (p == ~0UL) {
      size_t fileOffset = (it.key() - m_Address) + m_Offset;

      if (!m_bCopyOnWrite) {
        m_pBacking->syncAndReturnPhysicalPage(fileOffset, true);
      } else {
        m_pBacking->returnPhysicalPage(fileOffset);
      }
    } else
      PhysicalMemoryManager::instance().freePage(phys);
  }

  clearMappings();
}

void MemoryMappedFile::trackMapping(uintptr_t addr, physical_uintptr_t phys) {
  m_Mappings.insert(addr, phys);
}

void MemoryMappedFile::untrackMapping(uintptr_t addr) {
  m_Mappings.remove(addr);
}

physical_uintptr_t MemoryMappedFile::getMapping(uintptr_t addr) {
  return m_Mappings.lookup(addr);
}

size_t MemoryMappedFile::getMappingCount() {
  return m_Mappings.count();
}

void MemoryMappedFile::clearMappings() {
  m_Mappings.clear();
}

MemoryMapManager::OperationGuard::OperationGuard(MemoryMapManager& manager, bool tryOnly)
    : m_EventDeferral(),
      m_TerminationDeferral(),
      m_Manager(manager),
      m_Acquired(!tryOnly || manager.tryEnterOperation()) {
  if (!tryOnly) {
    m_Manager.enterOperation();
  }
}

MemoryMapManager::OperationGuard::~OperationGuard() {
  if (m_Acquired) {
    m_Manager.leaveOperation();
  }
}

MemoryMapManager::MemoryMapManager()
    : m_MmObjectLists(),
      m_Lock(),
      m_LifecycleLock(),
      m_LifecycleStateLock(false),
      m_pLifecycleOwner(nullptr),
      m_LifecycleDepth(0) {
  const bool registered = PageFaultHandler::instance().registerHandler(this);
  assert(registered);
  MemoryPressureManager::instance().registerHandler(MemoryPressureManager::HighPriority, this);
}

void MemoryMapManager::enterOperation() {
  void* owner = currentOperationOwner();
  {
    LockGuard<Spinlock> guard(m_LifecycleStateLock);
    if (m_pLifecycleOwner == owner) {
      ++m_LifecycleDepth;
      return;
    }
  }

  const bool acquired = m_LifecycleLock.acquire();
  assert(acquired);

  LockGuard<Spinlock> guard(m_LifecycleStateLock);
  assert(!m_pLifecycleOwner);
  assert(!m_LifecycleDepth);
  m_pLifecycleOwner = owner;
  m_LifecycleDepth = 1;
}

bool MemoryMapManager::tryEnterOperation() {
  void* owner = currentOperationOwner();
  {
    LockGuard<Spinlock> guard(m_LifecycleStateLock);
    if (m_pLifecycleOwner == owner) {
      // Pressure recovery must not revoke pages preflighted by an outer copy.
      return false;
    }
  }

  if (!m_LifecycleLock.tryAcquire()) {
    return false;
  }

  LockGuard<Spinlock> guard(m_LifecycleStateLock);
  assert(!m_pLifecycleOwner);
  assert(!m_LifecycleDepth);
  m_pLifecycleOwner = owner;
  m_LifecycleDepth = 1;
  return true;
}

void MemoryMapManager::leaveOperation() {
  bool release = false;
  {
    LockGuard<Spinlock> guard(m_LifecycleStateLock);
    assert(m_pLifecycleOwner == currentOperationOwner());
    assert(m_LifecycleDepth);
    if (!--m_LifecycleDepth) {
      m_pLifecycleOwner = nullptr;
      release = true;
    }
  }

  if (release) {
    m_LifecycleLock.release();
  }
}

MemoryMapManager::~MemoryMapManager() {
  const bool unregistered = PageFaultHandler::instance().unregisterHandler(this);
  assert(unregistered);
  MemoryPressureManager::instance().removeHandler(this);
}

MemoryMappedObject* MemoryMapManager::mapFile(File* pFile, uintptr_t& address, size_t length,
                                              MemoryMappedObject::Permissions perms, size_t offset,
                                              bool bCopyOnWrite) {
  return mapFile(pFile, address, length, perms, offset, bCopyOnWrite, Placement::FixedReplace,
                 nullptr);
}

MemoryMappedObject* MemoryMapManager::mapFile(File* pFile, uintptr_t& address, size_t length,
                                              MemoryMappedObject::Permissions perms, size_t offset,
                                              bool bCopyOnWrite, Placement placement,
                                              MapStatus* status,
                                              MemoryMappedObject::Permissions maximumPerms,
                                              const SharedPointer<MappingAttachment>& attachment) {
  OperationGuard operation(*this);

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  size_t pageSz = PhysicalMemoryManager::getPageSize();

  // Make sure the size is page aligned. (we'll fill any space that is past
  // the end of the extent with zeroes).
  size_t actualLength = length;
  if (length > ~static_cast<size_t>(0) - (pageSz - 1)) {
    if (status) {
      *status = MapStatus::NoMemory;
    }
    return 0;
  }
  if (length & (pageSz - 1)) {
    length = (length + pageSz - 1) & ~(pageSz - 1);
  }

  const MapStatus placementStatus = sanitiseAddress(address, length, placement);
  if (status) {
    *status = placementStatus;
  }
  if (placementStatus != MapStatus::Success) {
    return 0;
  }

  if (placement == Placement::FixedReplace) {
    remove(address, length);
  }

#ifdef DEBUG_MMOBJECTS
  NOTICE("MemoryMapManager::mapFile: " << address << " length " << actualLength << " for "
                                       << pFile->getName());
#endif
  MemoryMappedFile* pMappedFile = new MemoryMappedFile(
      address, actualLength, offset, pFile, bCopyOnWrite, perms, maximumPerms, attachment);

  MmObjectList* pMmObjectList = m_MmObjectLists.lookup(&va);
  if (!pMmObjectList) {
    pMmObjectList = new MmObjectList();
    m_MmObjectLists.insert(&va, pMmObjectList);
  }

  pMmObjectList->pushBack(pMappedFile);

  // Success.
  return pMappedFile;
}

MemoryMappedObject* MemoryMapManager::mapAnon(uintptr_t& address, size_t length,
                                              MemoryMappedObject::Permissions perms) {
  return mapAnon(address, length, perms, Placement::FixedReplace, nullptr);
}

MemoryMappedObject* MemoryMapManager::mapAnon(uintptr_t& address, size_t length,
                                              MemoryMappedObject::Permissions perms,
                                              Placement placement, MapStatus* status) {
  OperationGuard operation(*this);

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  size_t pageSz = PhysicalMemoryManager::getPageSize();

  // Make sure the size is page aligned. (we'll fill any space that is past
  // the end of the extent with zeroes).
  if (length > ~static_cast<size_t>(0) - (pageSz - 1)) {
    if (status) {
      *status = MapStatus::NoMemory;
    }
    return 0;
  }
  if (length & (pageSz - 1)) {
    length = (length + pageSz - 1) & ~(pageSz - 1);
  }

  const MapStatus placementStatus = sanitiseAddress(address, length, placement);
  if (status) {
    *status = placementStatus;
  }
  if (placementStatus != MapStatus::Success) {
    return 0;
  }

  if (placement == Placement::FixedReplace) {
    remove(address, length);
  }

#ifdef DEBUG_MMOBJECTS
  NOTICE("MemoryMapManager::mapAnon: " << address << " length " << length);
#endif
  AnonymousMemoryMap* pMap = new AnonymousMemoryMap(address, length, perms);

  MmObjectList* pMmObjectList = m_MmObjectLists.lookup(&va);
  if (!pMmObjectList) {
    pMmObjectList = new MmObjectList();
    m_MmObjectLists.insert(&va, pMmObjectList);
  }

  pMmObjectList->pushBack(pMap);

  // Success.
  return pMap;
}

void MemoryMapManager::clone(Process* pProcess) {
  OperationGuard operation(*this);

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  VirtualAddressSpace* pOtherVa = pProcess->getAddressSpace();

  MmObjectList* pMmObjectList = m_MmObjectLists.lookup(&va);
  if (!pMmObjectList)
    return;

  MmObjectList* pMmObjectList2 = m_MmObjectLists.lookup(pOtherVa);
  if (!pMmObjectList2) {
    pMmObjectList2 = new MmObjectList();
    m_MmObjectLists.insert(pOtherVa, pMmObjectList2);
  }

  Tree<MappingAttachment*, SharedPointer<MappingAttachment>> clonedAttachments;
  for (List<MemoryMappedObject*>::Iterator it = pMmObjectList->begin(); it != pMmObjectList->end();
       it++) {
    MemoryMappedObject* obj = *it;
    MemoryMappedObject* pNewObject = obj->clone();
    if (obj->m_Attachment) {
      auto attachment = clonedAttachments.lookup(obj->m_Attachment.get());
      if (!attachment) {
        attachment = obj->m_Attachment->clone(pProcess);
        clonedAttachments.insert(obj->m_Attachment.get(), attachment);
      }
      pNewObject->m_Attachment = attachment;
    }
    pMmObjectList2->pushBack(pNewObject);
  }
}

size_t MemoryMapManager::remove(uintptr_t base, size_t length) {
  return removeInternal(base, length, false);
}

size_t MemoryMapManager::removeAndRelease(uintptr_t base, size_t length) {
  return removeInternal(base, length, true);
}

SharedPointer<MappingAttachment> MemoryMapManager::findAttachment(uintptr_t base) {
  OperationGuard operation(*this);
  VirtualAddressSpace& space = Processor::information().getVirtualAddressSpace();
  MmObjectList* objects = m_MmObjectLists.lookup(&space);
  MemoryMappedObject* first = nullptr;
  if (objects) {
    for (auto it = objects->begin(); it != objects->end(); ++it) {
      MemoryMappedObject* object = *it;
      if (object->m_Attachment && object->m_Attachment->baseAddress() == base &&
          (!first || object->address() < first->address())) {
        // A newer attachment may reuse the original base of an older suffix.
        first = object;
      }
    }
  }
  return first ? first->m_Attachment : SharedPointer<MappingAttachment>();
}

size_t MemoryMapManager::removeAttachment(const SharedPointer<MappingAttachment>& attachment) {
  OperationGuard operation(*this);
  VirtualAddressSpace& space = Processor::information().getVirtualAddressSpace();
  Process* process = Processor::information().getCurrentThread()->getParent();
  MmObjectList* objects = m_MmObjectLists.lookup(&space);
  if (!objects || !attachment) {
    return 0;
  }
  size_t removed = 0;
  const size_t pageMask = PhysicalMemoryManager::getPageSize() - 1;
  for (auto it = objects->begin(); it != objects->end();) {
    MemoryMappedObject* object = *it;
    if (object->m_Attachment != attachment) {
      ++it;
      continue;
    }
    const uintptr_t base = object->address();
    const size_t length = (object->length() + pageMask) & ~pageMask;
    it = objects->erase(it);
    object->unmap();
    delete object;
    releaseReservation(process, space, base, length);
    ++removed;
  }
  return removed;
}

size_t MemoryMapManager::removeInternal(uintptr_t base, size_t length, bool releaseReservations) {
  OperationGuard operation(*this);

#ifdef DEBUG_MMOBJECTS
  NOTICE("MemoryMapManager::remove(" << base << ", " << length << ")");
#endif

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  size_t pageSz = PhysicalMemoryManager::getPageSize();
  Process* process = Processor::information().getCurrentThread()->getParent();

  size_t nAffected = 0;

  if (!length || length > ~static_cast<size_t>(0) - (pageSz - 1)) {
    return 0;
  }
  if (length & (pageSz - 1)) {
    length += pageSz;
    length &= ~(pageSz - 1);
  }

  if (base > ~static_cast<uintptr_t>(0) - length) {
    return 0;
  }

  uintptr_t removeEnd = base + length;

  MmObjectList* pMmObjectList = m_MmObjectLists.lookup(&va);
  if (!pMmObjectList) {
    return 0;
  }

  for (List<MemoryMappedObject*>::Iterator it = pMmObjectList->begin();
       it != pMmObjectList->end();) {
    MemoryMappedObject* pObject = *it;

    // Whether or not  it = x.erase() was called - because we should not
    // increment an iterator if so.
    bool bErased = false;

    uintptr_t objEnd = pObject->address() + pObject->length();

#ifdef DEBUG_MMOBJECTS
    NOTICE("MemoryMapManager::remove() - object at " << pObject->address() << " -> " << objEnd
                                                     << ".");
#endif

    uintptr_t objAlignEnd = objEnd;
    if (objAlignEnd & (pageSz - 1)) {
      objAlignEnd += pageSz;
      objAlignEnd &= ~(pageSz - 1);
    }

    // Capture the owned intersection before split/remove rewrites the object.
    // Holes have no object and must not be inserted into the allocator again.
    const uintptr_t releasedStart = base > pObject->address() ? base : pObject->address();
    const uintptr_t releasedEnd = removeEnd < objAlignEnd ? removeEnd : objAlignEnd;
    bool affected = false;

    // Avoid?
    if (pObject->address() == removeEnd) {
      ++it;
      continue;
    }

    // Direct removal?
    else if (pObject->address() == base) {
#ifdef DEBUG_MMOBJECTS
      NOTICE("MemoryMapManager::remove() - a direct removal");
#endif
      bool bAll = pObject->remove(length);
      affected = true;
      if (bAll) {
        it = pMmObjectList->erase(it);
        delete pObject;
        bErased = true;
      }
    }

    // Object fully contains parameters.
    else if ((pObject->address() < base) && (removeEnd <= objAlignEnd)) {
#ifdef DEBUG_MMOBJECTS
      NOTICE("MemoryMapManager::remove() - fully enclosed removal");
#endif
      MemoryMappedObject* pNewObject = pObject->split(base);
      bool bAll = pNewObject->remove(removeEnd - base);
      affected = true;
      if (!bAll) {
        // Remainder not fully removed - add to housekeeping.
        pMmObjectList->pushBack(pNewObject);
      } else {
        delete pNewObject;
      }
    }

    // Object in the middle of the parameters (neither begin or end inside)
    else if ((pObject->address() > base) && (objEnd >= base) && (objEnd <= removeEnd)) {
#ifdef DEBUG_MMOBJECTS
      NOTICE(
          "MemoryMapManager::remove() - begin before start, end after "
          "object end");
#endif
      // Outright unmap.
      pObject->unmap();
      affected = true;

      it = pMmObjectList->erase(it);
      delete pObject;
      bErased = true;
    }

    // End is within the object, start is before the object.
    else if ((pObject->address() > base) && (removeEnd >= pObject->address()) &&
             (removeEnd <= objEnd)) {
#ifdef DEBUG_MMOBJECTS
      NOTICE("MemoryMapManager::remove() - begin outside, end inside");
#endif
      MemoryMappedObject* pNewObject = pObject->split(removeEnd);

      pObject->unmap();
      affected = true;

      it = pMmObjectList->erase(it);
      delete pObject;
      bErased = true;

      pMmObjectList->pushBack(pNewObject);
    }

    // Start is within the object, end is past the end of the object.
    else if ((pObject->address() < base) && (base < objEnd) && (removeEnd >= objEnd)) {
#ifdef DEBUG_MMOBJECTS
      NOTICE("MemoryMapManager::remove() - begin inside, end outside");
#endif
      MemoryMappedObject* pNewObject = pObject->split(base);
      pNewObject->unmap();
      delete pNewObject;
      affected = true;
    }

    // Nothing!
    else {
#ifdef DEBUG_MMOBJECTS
      NOTICE("MemoryMapManager::remove() - doing nothing!");
#endif
      ++it;
      continue;
    }

    if (releaseReservations && affected && releasedStart < releasedEnd) {
      releaseReservation(process, va, releasedStart, releasedEnd - releasedStart);
    }

    if (!bErased) {
      ++it;
    }

    ++nAffected;
  }

  return nAffected;
}

void MemoryMapManager::releaseReservation(Process* process, VirtualAddressSpace& addressSpace,
                                          uintptr_t base, size_t length) {
  const uintptr_t end = base + length;

  auto releaseIntersection = [base, end](MemoryAllocator& allocator, uintptr_t regionStart,
                                         uintptr_t regionEnd) {
    const uintptr_t releaseStart = base > regionStart ? base : regionStart;
    const uintptr_t releaseEnd = end < regionEnd ? end : regionEnd;
    if (releaseStart < releaseEnd) {
      allocator.free(releaseStart, releaseEnd - releaseStart);
    }
  };

  const uintptr_t dynamicStart = addressSpace.getDynamicStart();
  const uintptr_t dynamicEnd = addressSpace.getDynamicEnd();
  if (dynamicStart && dynamicStart < dynamicEnd) {
    releaseIntersection(process->getDynamicSpaceAllocator(), dynamicStart, dynamicEnd);
  }
  releaseIntersection(process->getSpaceAllocator(), addressSpace.getUserStart(),
                      addressSpace.getUserReservedStart());
}

size_t MemoryMapManager::setPermissions(uintptr_t base, size_t length,
                                        MemoryMappedObject::Permissions perms,
                                        ProtectStatus* status) {
  OperationGuard operation(*this);
  const size_t pageMask = PhysicalMemoryManager::getPageSize() - 1;
  if (status) {
    *status = ProtectStatus::InvalidRange;
  }
  if (!length || (base & pageMask) || length > ~static_cast<size_t>(0) - pageMask ||
      (perms &
       ~(MemoryMappedObject::Read | MemoryMappedObject::Write | MemoryMappedObject::Exec))) {
    return 0;
  }
  length = (length + pageMask) & ~pageMask;
  if (length > ~static_cast<uintptr_t>(0) - base) {
    return 0;
  }
#if !(X64 || HOSTED)
  // These ports do not yet retain inaccessible physical mappings.
  if (perms == MemoryMappedObject::None) {
    if (status) {
      *status = ProtectStatus::Unsupported;
    }
    return 0;
  }
#endif
  if (!allows(base, length, MemoryMappedObject::None)) {
    if (status) {
      *status = ProtectStatus::Unmapped;
    }
    return 0;
  }

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  MmObjectList* objects = m_MmObjectLists.lookup(&va);
  const uintptr_t end = base + length;
  for (auto it = objects->begin(); it != objects->end(); ++it) {
    MemoryMappedObject* object = *it;
    const uintptr_t objectEnd = (object->address() + object->length() + pageMask) & ~pageMask;
    if (object->address() < end && objectEnd > base &&
        (object->maximumPermissions() & perms) != perms) {
      if (status) {
        *status = ProtectStatus::AccessDenied;
      }
      return 0;
    }
  }

  size_t affected = 0;
  for (auto it = objects->begin(); it != objects->end(); ++it) {
    MemoryMappedObject* object = *it;
    const uintptr_t objectEnd = (object->address() + object->length() + pageMask) & ~pageMask;
    if (object->address() < end && objectEnd > base &&
        !object->preparePermissions(base, length, perms)) {
      if (status) {
        *status = ProtectStatus::NoMemory;
      }
      return 0;
    }
  }
  size_t remaining = objects->count();
  for (auto it = objects->begin(); remaining; ++it, --remaining) {
    MemoryMappedObject* object = *it;
    const uintptr_t objectEnd = (object->address() + object->length() + pageMask) & ~pageMask;
    if (object->address() >= end || objectEnd <= base) {
      continue;
    }
    if (object->address() < base) {
      object = object->split(base);
      objects->pushBack(object);
    }
    if (objectEnd > end) {
      objects->pushBack(object->split(end));
    }
    object->setPermissions(perms);
    ++affected;
  }
  if (status) {
    *status = ProtectStatus::Success;
  }
  return affected;
}

bool MemoryMapManager::contains(uintptr_t base, size_t length) {
  OperationGuard operation(*this);

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  size_t pageSz = PhysicalMemoryManager::getPageSize();

  MmObjectList* pMmObjectList = m_MmObjectLists.lookup(&va);
  if (!pMmObjectList) {
    return false;
  }

  for (uintptr_t address = base; address < (base + length); address += pageSz) {
    for (List<MemoryMappedObject*>::Iterator it = pMmObjectList->begin();
         it != pMmObjectList->end(); it++) {
      MemoryMappedObject* pObject = *it;
      if (pObject->matches(address & ~(pageSz - 1))) {
        return true;
      }
    }
  }

  return false;
}

bool MemoryMapManager::allows(uintptr_t base, size_t length,
                              MemoryMappedObject::Permissions permissions) {
  OperationGuard operation(*this);

  if (!length || length > (~static_cast<uintptr_t>(0) - base)) {
    return false;
  }

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  uintptr_t end = base + length;

  MmObjectList* pMmObjectList = m_MmObjectLists.lookup(&va);
  if (!pMmObjectList) {
    return false;
  }

  uintptr_t cursor = base;
  while (cursor < end) {
    uintptr_t coveredUntil = cursor;
    for (List<MemoryMappedObject*>::Iterator it = pMmObjectList->begin();
         it != pMmObjectList->end(); ++it) {
      MemoryMappedObject* pObject = *it;
      const size_t pageMask = PhysicalMemoryManager::getPageSize() - 1;
      uintptr_t objectEnd = (pObject->address() + pObject->length() + pageMask) & ~pageMask;
      if (cursor >= pObject->address() && cursor < objectEnd &&
          (pObject->permissions() & permissions) == permissions) {
        if (objectEnd > coveredUntil) {
          coveredUntil = objectEnd;
        }
      }
    }

    if (coveredUntil == cursor) {
      return false;
    }
    cursor = coveredUntil < end ? coveredUntil : end;
  }

  return true;
}

bool MemoryMapManager::op(MemoryMapManager::Ops what, uintptr_t base, size_t length, bool async) {
  OperationGuard operation(*this);

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  size_t pageSz = PhysicalMemoryManager::getPageSize();

  MmObjectList* pMmObjectList = m_MmObjectLists.lookup(&va);
  if (!pMmObjectList) {
    return false;
  }

  bool success = true;
  for (uintptr_t address = base; address < (base + length); address += pageSz) {
    for (List<MemoryMappedObject*>::Iterator it = pMmObjectList->begin();
         it != pMmObjectList->end(); it++) {
      MemoryMappedObject* pObject = *it;
      if (pObject->matches(address & ~(pageSz - 1))) {
        switch (what) {
          case Sync:
            success = pObject->sync(address, async) && success;
            break;
          case Invalidate:
            pObject->invalidate(address);
            break;
          default:
            WARNING("Bad 'what' in MemoryMapManager::op()");
        }
        break;
      }
    }
  }
  return success;
}

bool MemoryMapManager::sync(uintptr_t base, size_t length, bool async, int* error) {
  OperationGuard operation(*this);
  if (error) {
    *error = static_cast<int>(Error::OutOfMemory);
  }
  const size_t pageMask = PhysicalMemoryManager::getPageSize() - 1;
  if (!length || (base & pageMask) || length > ~static_cast<size_t>(0) - pageMask) {
    return false;
  }
  length = (length + pageMask) & ~pageMask;
  if (!allows(base, length, MemoryMappedObject::None)) {
    return false;
  }
  const bool success = op(Sync, base, length, async);
  if (error) {
    *error = success ? 0 : static_cast<int>(Error::IoError);
  }
  return success;
}

bool MemoryMapManager::sharedBacking(Process* process, uintptr_t address, uintptr_t& identity,
                                     size_t& offset) {
  OperationGuard operation(*this);
  if (!process) {
    return false;
  }
  MmObjectList* objects = m_MmObjectLists.lookup(process->getAddressSpace());
  if (!objects) {
    return false;
  }
  for (auto it = objects->begin(); it != objects->end(); ++it) {
    if ((*it)->matches(address)) {
      return (*it)->sharedBacking(address, identity, offset);
    }
  }
  return false;
}

bool MemoryMapManager::faultIn(uintptr_t address, bool write) {
  OperationGuard operation(*this);
  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  void* page = reinterpret_cast<void*>(address & ~(PhysicalMemoryManager::getPageSize() - 1));
  const bool present = va.isMapped(page);
  if (present) {
    physical_uintptr_t physical;
    size_t flags;
    va.getMapping(page, physical, flags);
    if ((flags & (VirtualAddressSpace::KernelMode | VirtualAddressSpace::NoAccess |
                  VirtualAddressSpace::Swapped)) ||
        (write && (flags & VirtualAddressSpace::WriteProtected))) {
      return false;
    }
    if (!write || (flags & VirtualAddressSpace::Write)) {
      return true;
    }
    if (flags & VirtualAddressSpace::CopyOnWrite) {
      return va.handleCopyOnWriteFault(page, true);
    }
  }
  return handleTrap(address, write, present);
}

void MemoryMapManager::invalidate(uintptr_t base, size_t length) {
  op(Invalidate, base, length, false);
}

void MemoryMapManager::unmap(MemoryMappedObject* pObj) {
  OperationGuard operation(*this);

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();

  MmObjectList* pMmObjectList = m_MmObjectLists.lookup(&va);
  if (!pMmObjectList)
    return;

  for (List<MemoryMappedObject*>::Iterator it = pMmObjectList->begin(); it != pMmObjectList->end();
       ++it) {
    if ((*it) != pObj)
      continue;

    MemoryMappedObject* object = *it;
    pMmObjectList->erase(it);

    object->unmap();
    delete object;
    return;
  }
}

void MemoryMapManager::unmapAll() {
  OperationGuard operation(*this);

  unmapAllUnlocked();
}

bool MemoryMapManager::trap(InterruptState& state, uintptr_t address, bool bIsWrite,
                            bool bWasPresent) {
  OperationGuard operation(*this);
  if (handleTrap(address, bIsWrite, bWasPresent)) {
    return true;
  }
  if (state.kernelMode()) {
    return false;
  }
  VirtualAddressSpace& space = Processor::information().getVirtualAddressSpace();
  MmObjectList* objects = m_MmObjectLists.lookup(&space);
  if (!objects) {
    return false;
  }
  const uintptr_t page = address & ~(PhysicalMemoryManager::getPageSize() - 1);
  const auto required = bIsWrite ? MemoryMappedObject::Write : MemoryMappedObject::Read;
  for (auto it = objects->begin(); it != objects->end(); ++it) {
    MemoryMappedObject* object = *it;
    if (object->matches(page) && (object->permissions() & required) &&
        object->beyondBackingEnd(page)) {
      Thread* thread = Processor::information().getCurrentThread();
      return thread &&
             thread->deferSubsystemException(Subsystem::FileMappingFault, address,
                                             4 | (bWasPresent ? 1 : 0) | (bIsWrite ? 2 : 0));
    }
  }
  return false;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
bool MemoryMapManager::trapForHostedTest(uintptr_t address, bool bIsWrite, bool bWasPresent) {
  return handleTrap(address, bIsWrite, bWasPresent);
}
#endif

bool MemoryMapManager::handleTrap(uintptr_t address, bool bIsWrite, bool bWasPresent) {
  // Can't take an event while we're trapping, as the event would otherwise
  // be in a minefield (can't touch *any* trap pages in userspace).
  Uninterruptible while_trapping;
  OperationGuard operation(*this);

#ifdef DEBUG_MMOBJECTS
  NOTICE("Trap start: " << Hex << address << ", pid:tid " << Dec
                        << Processor::information().getCurrentThread()->getParent()->getId() << ":"
                        << Processor::information().getCurrentThread()->getId());
#endif

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  size_t pageSz = PhysicalMemoryManager::getPageSize();
  const uintptr_t pageAddress = address & ~(pageSz - 1);

  m_Lock.acquire();
#ifdef DEBUG_MMOBJECTS
  NOTICE_NOLOCK("trap: got lock");
#endif

  MmObjectList* pMmObjectList = m_MmObjectLists.lookup(&va);
  if (!pMmObjectList) {
    m_Lock.release();
    return false;
  }

#ifdef DEBUG_MMOBJECTS
  NOTICE_NOLOCK("trap: lookup complete " << reinterpret_cast<uintptr_t>(pMmObjectList));
#endif

  MemoryMappedObject* pObject = nullptr;
  for (List<MemoryMappedObject*>::Iterator it = pMmObjectList->begin(); it != pMmObjectList->end();
       it++) {
    MemoryMappedObject* candidate = *it;
#ifdef DEBUG_MMOBJECTS
    NOTICE_NOLOCK("mmobj=" << reinterpret_cast<uintptr_t>(candidate));
    if (!candidate) {
      NOTICE_NOLOCK("bad mmobj, should create a real #PF and backtrace");
      break;
    }
#endif

    // Passing in a page-aligned address means we handle the case where
    // a mapping ends midway through a page and a trap happens after this.
    // Because we map in terms of pages, but store unaligned 'actual'
    // lengths (for proper page zeroing etc), this is necessary.
    if (candidate->matches(pageAddress)) {
      pObject = candidate;
      break;
    }
  }

  m_Lock.release();
  if (!pObject) {
#ifdef DEBUG_MMOBJECTS
    ERROR("MemoryMapManager::trap() could not find an object for " << address);
#endif
    return false;
  }

  const MemoryMappedObject::Permissions required =
      bIsWrite ? MemoryMappedObject::Write : MemoryMappedObject::Read;
  if (!(pObject->permissions() & required)) {
    return false;
  }

  // The original fault bits remain authoritative after waiting for the
  // lifecycle gate. A mapping visible here was completed by another operation
  // on this object, so retry and let the processor re-evaluate permissions.
  if (va.isMapped(reinterpret_cast<void*>(pageAddress))) {
    if (!bWasPresent) {
      return true;
    }

    if (bIsWrite) {
      physical_uintptr_t physicalAddress = 0;
      size_t flags = 0;
      va.getMapping(reinterpret_cast<void*>(pageAddress), physicalAddress, flags);
      if (flags & VirtualAddressSpace::Write) {
        return true;
      }
      if (flags & VirtualAddressSpace::CopyOnWrite) {
        return va.handleCopyOnWriteFault(reinterpret_cast<void*>(pageAddress), true);
      }
    }
  }

  return pObject->trap(address, bIsWrite);
}

MemoryMapManager::MapStatus MemoryMapManager::sanitiseAddress(uintptr_t& address, size_t length,
                                                              Placement placement) {
  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  size_t pageSz = PhysicalMemoryManager::getPageSize();

  if (length > ~static_cast<size_t>(0) - pageSz ||
      (address && address > ~static_cast<uintptr_t>(0) - length)) {
    return MapStatus::NoMemory;
  }

  auto allocateAnywhere = [&]() -> bool {
    const size_t allocationLength = length + pageSz - 1;
    uintptr_t allocationBase = 0;
    MemoryAllocator* allocator = &pProcess->getDynamicSpaceAllocator();
    bool allocated = allocator->allocate(allocationLength, allocationBase);
    if (!allocated) {
      allocator = &pProcess->getSpaceAllocator();
      allocated = allocator->allocate(allocationLength, allocationBase);
    }
    if (!allocated) {
      return false;
    }

    address = (allocationBase + pageSz - 1) & ~(pageSz - 1);
    const size_t prefixLength = address - allocationBase;
    if (prefixLength) {
      allocator->free(allocationBase, prefixLength);
    }
    const uintptr_t allocationEnd = allocationBase + allocationLength;
    const uintptr_t mappingEnd = address + length;
    if (mappingEnd < allocationEnd) {
      allocator->free(mappingEnd, allocationEnd - mappingEnd);
    }
    return true;
  };

  auto allocateSpecific = [&]() -> bool {
    const uintptr_t end = address + length;
    const uintptr_t dynamicStart = va.getDynamicStart();
    const uintptr_t dynamicEnd = va.getDynamicEnd();
    if (dynamicStart && address >= dynamicStart && end <= dynamicEnd) {
      return pProcess->getDynamicSpaceAllocator().allocateSpecific(address, length);
    }
    if (address >= va.getUserStart() && end <= va.getUserReservedStart()) {
      return pProcess->getSpaceAllocator().allocateSpecific(address, length);
    }
    return false;
  };

  auto reserveFreeSubranges = [&](MemoryAllocator& allocator) {
    const uintptr_t requestedEnd = address + length;
    while (true) {
      bool reserved = false;
      for (size_t i = 0; i < allocator.size(); ++i) {
        MemoryAllocator::Range range(0, 0);
        if (!allocator.getRange(i, range)) {
          continue;
        }

        const uintptr_t rangeEnd = range.length > ~static_cast<uintptr_t>(0) - range.address
                                       ? ~static_cast<uintptr_t>(0)
                                       : range.address + range.length;
        const uintptr_t overlapStart = address > range.address ? address : range.address;
        const uintptr_t overlapEnd = requestedEnd < rangeEnd ? requestedEnd : rangeEnd;
        if (overlapStart < overlapEnd &&
            allocator.allocateSpecific(overlapStart, overlapEnd - overlapStart)) {
          reserved = true;
          break;
        }
      }
      if (!reserved) {
        return;
      }
    }
  };

  if (address == 0) {
    return allocateAnywhere() ? MapStatus::Success : MapStatus::NoMemory;
  }

  if (allocateSpecific()) {
    return MapStatus::Success;
  }

  if (placement == Placement::Hint) {
    address = 0;
    return allocateAnywhere() ? MapStatus::Success : MapStatus::NoMemory;
  }
  if (placement == Placement::FixedNoReplace) {
    return MapStatus::AddressInUse;
  }

  // Preserve reservations already covered by the replaced mapping while
  // claiming any previously-free parts of a larger fixed range.
  reserveFreeSubranges(pProcess->getDynamicSpaceAllocator());
  reserveFreeSubranges(pProcess->getSpaceAllocator());

  // Fixed mappings may target a range which an internal caller reserved
  // before asking the memory-map manager to publish the object.
  return MapStatus::Success;
}

bool MemoryMapManager::prepareFileResize(File* file) {
  OperationGuard operation(*this);
  const uintptr_t identity = file->futexIdentity();
  for (auto spaces = m_MmObjectLists.begin(); spaces != m_MmObjectLists.end(); ++spaces) {
    for (auto objects = spaces.value()->begin(); objects != spaces.value()->end(); ++objects) {
      if ((*objects)->usesBacking(identity)) {
#if X64 || HOSTED
        (*objects)->discardFilePages(*spaces.key(), 0, true);
#else
        return false;
#endif
      }
    }
  }
  return true;
}

void MemoryMapManager::finishFileResize(File* file, size_t newSize) {
  OperationGuard operation(*this);
  const uintptr_t identity = file->futexIdentity();
  for (auto spaces = m_MmObjectLists.begin(); spaces != m_MmObjectLists.end(); ++spaces) {
    for (auto objects = spaces.value()->begin(); objects != spaces.value()->end(); ++objects) {
      if ((*objects)->usesBacking(identity)) {
        (*objects)->discardFilePages(*spaces.key(), newSize, false);
      }
    }
  }
}

bool MemoryMapManager::compact() {
  // Allocation may enter from a file mutation that another mapping operation
  // is waiting for. Pressure recovery cannot wait for that operation's gate.
  OperationGuard operation(*this, true);
  if (!operation) {
    return false;
  }

  // Track current address space as we need to switch into each known address
  // space in order to compact them.
  VirtualAddressSpace& currva = Processor::information().getVirtualAddressSpace();
  AddressSpaceRestorer restoreAddressSpace(currva);

  bool bCompact = false;
  for (Tree<VirtualAddressSpace*, MmObjectList*>::Iterator it = m_MmObjectLists.begin();
       it != m_MmObjectLists.end(); ++it) {
    Processor::switchAddressSpace(*it.key());

    for (MmObjectList::Iterator it2 = it.value()->begin(); it2 != it.value()->end(); ++it2) {
      bCompact = (*it2)->compact();
      if (bCompact)
        break;
    }

    if (bCompact)
      break;
  }

  // Memory mapped files tend to un-pin pages for the Cache system to
  // release, so we never return success (as we never actually released
  // pages and therefore didn't resolve any memory pressure).
  if (bCompact)
    NOTICE("    -> success, hoping for Cache eviction...");
  return false;
}

void MemoryMapManager::unmapAllUnlocked() {
  TerminationDeferral terminationDeferral;

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();

  MmObjectList* pMmObjectList = m_MmObjectLists.lookup(&va);
  if (!pMmObjectList)
    return;

  // Detach first so a backing-store callback that re-enters the manager
  // cannot observe objects which are already being destroyed.
  m_MmObjectLists.remove(&va);

  for (List<MemoryMappedObject*>::Iterator it = pMmObjectList->begin(); it != pMmObjectList->end();
       it = pMmObjectList->begin()) {
    MemoryMappedObject* object = *it;
    pMmObjectList->erase(it);

    object->unmap();
    delete object;
  }

  delete pMmObjectList;
}

void MemoryMapManager::acquireLock() {
  enterOperation();
}

void MemoryMapManager::releaseLock() {
  leaveOperation();
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void MemoryMapManager::acquireLifecycleGateForHostedTest() {
  acquireLock();
}

void MemoryMapManager::releaseLifecycleGateForHostedTest() {
  releaseLock();
}
#endif
