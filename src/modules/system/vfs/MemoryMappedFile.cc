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
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#include "File.h"
#include "VFS.h"

MemoryMapManager MemoryMapManager::m_Instance;

physical_uintptr_t AnonymousMemoryMap::m_Zero = 0;

#ifdef DEBUG_MMOBJECTS
static constexpr bool DebugMemoryMappings = true;
#else
static constexpr bool DebugMemoryMappings = false;
#endif

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
    : MemoryMappedObject(address, true, length, perms), m_Mappings() {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());

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

bool AnonymousMemoryMap::initialisePhysicalPage(physical_uintptr_t physical) {
#if X64 || HOSTED
  return SwapStore::instance().zeroPage(physical);
#else
  TemporaryPhysicalMapping temporary(physical, "Anonymous Page Initialisation");
  if (!temporary.valid())
    return false;
  ByteSet(temporary.address(), 0, PhysicalMemoryManager::getPageSize());
  return true;
#endif
}

MemoryMappedFile::MemoryMappedFile(uintptr_t address, size_t length, size_t offset, File* backing,
                                   bool bCopyOnWrite, MemoryMappedObject::Permissions perms,
                                   MemoryMappedObject::Permissions maximumPerms,
                                   const SharedPointer<MappingAttachment>& attachment,
                                   const FileMappingOrigin& origin, bool executableUse)
    : MemoryMappedObject(address, bCopyOnWrite, length, perms, maximumPerms),
      m_pBacking(backing),
      m_Offset(offset),
      m_Mappings(),
      m_Origin(origin),
      m_Lock(),
      m_bVfsLease(backing && VFS::instance().retainTrackedFile(backing)),
      m_ExecutableUse(executableUse || (bCopyOnWrite && (perms & Exec))),
      m_SharedWriteUse(!bCopyOnWrite && (maximumPerms & Write)),
      m_UseAdmitted(backing && backing->acquireMappingUse(m_ExecutableUse, m_SharedWriteUse)) {
  assert(m_pBacking);
  m_Attachment = attachment;
}

MemoryMappedFile::~MemoryMappedFile() {
  TerminationDeferral lifetime;
  if (m_OwnsMappings)
    unmap();
  if (m_UseAdmitted)
    m_pBacking->releaseMappingUse(m_ExecutableUse, m_SharedWriteUse);
  if (m_bVfsLease) {
    m_bVfsLease = false;
    VFS::instance().untrackFile(m_pBacking);
  }
  m_Origin.openingPath.reset();
}

MemoryMappedObject* MemoryMappedFile::clone() {
  TerminationDeferral terminationDeferral;
  LockGuard<Mutex> guard(m_Lock);

  MemoryMappedFile* pResult =
      new MemoryMappedFile(m_Address, m_Length, m_Offset, m_pBacking, m_bCopyOnWrite, m_Permissions,
                           m_MaximumPermissions, m_Attachment, m_Origin, m_ExecutableUse);
  assert(pResult->m_UseAdmitted);
  pResult->m_OwnerProcess = m_OwnerProcess;
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
  MemoryMappedFile* pResult = new MemoryMappedFile(
      at, oldLength - m_Length, m_Offset + m_Length, m_pBacking, m_bCopyOnWrite, m_Permissions,
      m_MaximumPermissions, m_Attachment, m_Origin, m_ExecutableUse);
  assert(pResult->m_UseAdmitted);

  pResult->m_OwnerProcess = m_OwnerProcess;
  pResult->m_LockMode = m_LockMode;

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
  if (m_bCopyOnWrite && (perms & Exec) && !m_ExecutableUse) {
    // The manager preflights every backing while holding its operation gate,
    // so no shared-write mapping can appear before this permission commit.
    const bool admitted = m_pBacking->acquireMappingUse(true, false);
    assert(admitted);
    m_ExecutableUse = true;
  }
  LockGuard<Mutex> guard(m_Lock);
  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  for (auto it = m_Mappings.begin(); it != m_Mappings.end(); ++it) {
    void* address = reinterpret_cast<void*>(it.key());
    if (va.isMapped(address)) {
      physical_uintptr_t physical;
      size_t flags;
      va.getMapping(address, physical, flags);
      const size_t newFlags = protectionFlags(flags, perms, !m_bCopyOnWrite);
      if (!m_bCopyOnWrite && it.value() == ~0UL && (newFlags & VirtualAddressSpace::Write) &&
          !(flags & VirtualAddressSpace::Write)) {
        m_pBacking->markPageExternallyWritable(m_Offset + (it.key() - m_Address));
      }
      va.setFlags(address, newFlags);
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

void MemoryMappedFile::discardFilePages(VirtualAddressSpace& space, size_t end) {
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
    if (fileOffset / pageSize < firstDiscardedPage) {
      continue;
    }
    const bool loan = tracked == ~0UL;
    physical_uintptr_t physical = 0;
    size_t flags = 0;
    const bool detached = space.detachMapping(reinterpret_cast<void*>(address), physical, flags);
    // CoW can replace the PTE without updating its original loan tracker.
    if (loan || (detached && (flags & VirtualAddressSpace::Borrowed))) {
      m_pBacking->returnPhysicalPage(fileOffset);
    }
    if (detached && !(flags & VirtualAddressSpace::Borrowed)) {
      PhysicalMemoryManager::instance().freePage(physical);
    }
    untrackMapping(address);
  }
}

physical_uintptr_t MemoryMappedFile::getBackingPage(size_t fileOffset, size_t mappingBytes) {
  File* pBacking = m_pBacking;
  size_t pageSz = PhysicalMemoryManager::getPageSize();
  const size_t fileSize = pBacking->getSize();
  if (fileOffset >= fileSize) {
    return ~0UL;
  }
  size_t expected = fileSize - fileOffset < pageSz ? fileSize - fileOffset : pageSz;
  if (expected > mappingBytes)
    expected = mappingBytes;

  physical_uintptr_t phys = pBacking->getPhysicalPage(fileOffset);
  if (phys == ~0UL) {
    // Grow only when faults consume the preceding window. A jump starts small
    // to avoid reading large unused portions of executables and random mappings.
    m_ReadAheadPages =
        fileOffset == m_ReadAheadEnd ? (m_ReadAheadPages < 32 ? m_ReadAheadPages * 2 : 32) : 4;
    const size_t window =
        mappingBytes < m_ReadAheadPages * pageSz ? mappingBytes : m_ReadAheadPages * pageSz;
    auto* thread = Processor::information().getCurrentThread();
    const size_t previousError = thread ? thread->getErrno() : 0;
    const size_t actual = pBacking->populateRange(fileOffset, window);
    m_ReadAheadEnd = fileOffset + actual;
    if (actual < expected) {
      ERROR("Short read of " << pBacking->getName() << " in getBackingPage() - wanted " << expected
                             << " bytes but got " << actual << " instead");
    }
    phys = pBacking->getPhysicalPage(fileOffset);
    if (phys == ~0UL) {
      ERROR(
          "*** Could not manage to get a physical page for a "
          "MemoryMappedFile ("
          << pBacking->getName() << ") - read got " << actual << " bytes!");
    } else if (thread) {
      // Failure of a speculative neighbour must not change a successful fault.
      thread->setErrno(previousError);
    }
  }

  return phys;
}

bool MemoryMappedObject::syncRange(uintptr_t at, size_t length, bool async) {
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  bool succeeded = true;
  for (size_t offset = 0; offset < length; offset += pageSize)
    succeeded = sync(at + offset, async) && succeeded;
  return succeeded;
}

bool MemoryMappedFile::syncRange(uintptr_t at, size_t length, bool async) {
  TerminationDeferral terminationDeferral;
  LockGuard<Mutex> guard(m_Lock);
  if (m_bCopyOnWrite || !length)
    return true;
  if (at < m_Address || at - m_Address >= m_Length)
    return false;
  const size_t available = m_Length - (at - m_Address);
  if (length > available)
    length = available;
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  uint64_t offsets[Cache::MaxWritebackPages];
  size_t count = 0;
  bool succeeded = true;
  for (size_t offset = 0; offset < length; offset += pageSize) {
    const uintptr_t address = at + offset;
    if (getMapping(address) != ~0UL)
      continue;
    const size_t fileOffset = m_Offset + address - m_Address;
    if (async) {
      succeeded = m_pBacking->sync(fileOffset, true) && succeeded;
    } else {
      offsets[count++] = fileOffset;
      if (count == Cache::MaxWritebackPages) {
        succeeded = m_pBacking->syncPages(offsets, count) && succeeded;
        count = 0;
      }
    }
  }
  if (count)
    succeeded = m_pBacking->syncPages(offsets, count) && succeeded;
  return succeeded;
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

bool MemoryMappedFile::trap(VirtualAddressSpace& va, uintptr_t address, bool bWrite,
                            PopulationStatus* population) {
  if (population)
    *population = PopulationStatus::NoMemory;
  TerminationDeferral terminationDeferral;
  LockGuard<Mutex> guard(m_Lock);

  EMIT_IF(DebugMemoryMappings) {
    NOTICE("MemoryMappedFile::trap(" << address << ", " << bWrite << ")");
  }

  size_t pageSz = PhysicalMemoryManager::getPageSize();

  // Page-align the trap address
  address = address & ~(pageSz - 1);
  size_t mappingOffset = (address - m_Address);
  size_t fileOffset = m_Offset + mappingOffset;

  if (beyondBackingEnd(address)) {
    if (population)
      *population = PopulationStatus::Inaccessible;
    return false;
  }

  bool bWillEof = (mappingOffset + pageSz) > m_Length;
  bool bShouldCopy = m_bCopyOnWrite && (bWillEof || bWrite);

  // Skip out on a few things if we can.
  if (bWrite && !(m_Permissions & Write)) {
    EMIT_IF(DebugMemoryMappings) {
      DEBUG_LOG(" -> ignoring, was a write and this is not a writable mapping.");
    }
    return false;
  } else if ((!bWrite) && !(m_Permissions & Read) && !population) {
    EMIT_IF(DebugMemoryMappings) {
      DEBUG_LOG(" -> ignoring, was a read and this is not a readable mapping.");
    }
    return false;
  }

  EMIT_IF(DebugMemoryMappings) {
    DEBUG_LOG(" -> mapping offset is " << mappingOffset << ", file offset: " << fileOffset);
    DEBUG_LOG(" -> will eof: " << bWillEof << ", should copy: " << bShouldCopy);
  }

  // Add execute flag.
  size_t extraFlags = 0;
  if (m_Permissions & Exec)
    extraFlags |= VirtualAddressSpace::Execute;

  if (!bShouldCopy) {
    if (!m_bCopyOnWrite && (m_Permissions & Write) &&
        !m_pBacking->prepareSharedMapping(fileOffset, pageSz)) {
      if (population) {
        auto* thread = Processor::information().getCurrentThread();
        *population = thread && thread->getErrno() == Error::OutOfMemory
                          ? PopulationStatus::NoMemory
                          : PopulationStatus::IoError;
      }
      return false;
    }
    // No need to lock this section - only accessing m_Mappings once
    physical_uintptr_t phys = getBackingPage(fileOffset, m_Length - mappingOffset);
    if (phys == ~0UL) {
      if (population) {
        auto* thread = Processor::information().getCurrentThread();
        *population = thread && thread->getErrno() == Error::OutOfMemory
                          ? PopulationStatus::NoMemory
                          : PopulationStatus::IoError;
      }
      ERROR("MemoryMappedFile::trap couldn't get a backing page");
      return false;  // Fail.
    }

    size_t flags = VirtualAddressSpace::Shared | VirtualAddressSpace::Borrowed;
    if (!m_bCopyOnWrite && (m_Permissions & Write)) {
      m_pBacking->markPageExternallyWritable(fileOffset);
      flags |= VirtualAddressSpace::Write;
    }

    bool r = va.map(phys, reinterpret_cast<void*>(address), flags | extraFlags);
    if (!r) {
      ERROR("map() failed in MemoryMappedFile::trap (no-copy)");
      m_pBacking->returnPhysicalPage(fileOffset);
      return false;
    }

    if (!m_Mappings.tryInsert(address, ~0UL)) {
      va.unmap(reinterpret_cast<void*>(address));
      m_pBacking->returnPhysicalPage(fileOffset);
      return false;
    }
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
      if (population) {
        auto* thread = Processor::information().getCurrentThread();
        *population = thread && thread->getErrno() == Error::OutOfMemory
                          ? PopulationStatus::NoMemory
                          : PopulationStatus::IoError;
      }
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

    if (!m_Mappings.tryInsert(address, newPhys)) {
      va.unmap(reinterpret_cast<void*>(address));
      PhysicalMemoryManager::instance().freePage(newPhys);
      return false;
    }
  }

  return true;
}

bool MemoryMappedFile::compact() {
  if (m_LockMode != MemoryLockMode::None)
    return false;
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
    if (!m_pBacking->tryBeginMappingRelease()) {
      break;
    }
    const size_t offset = m_Offset + (address - m_Address);
    size_t flags = 0;
    physical_uintptr_t physical = 0;
    if (!va.detachMapping(page, physical, flags)) {
      m_pBacking->endMappingRelease();
      continue;
    }
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
  EMIT_IF(DebugMemoryMappings) {
    NOTICE("MemoryMappedFile::unmap()");
  }

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
    : m_EventDeferral(), m_Manager(manager), m_Acquired(!tryOnly || manager.tryEnterOperation()) {
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
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  Thread* diagnosticThread = Processor::information().getCurrentThread();
  Process* diagnosticProcess = diagnosticThread ? diagnosticThread->getParent() : nullptr;
  if (diagnosticProcess) {
    diagnosticProcess->recordBenchmarkVmCounter(Process::VmGuardEntries);
  }
#endif
  {
    LockGuard<Spinlock> guard(m_LifecycleStateLock);
    if (m_pLifecycleOwner == owner) {
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
      if (diagnosticProcess) {
        diagnosticProcess->recordBenchmarkVmCounter(Process::VmGuardRecursiveEntries);
      }
#endif
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

bool MemoryMapManager::operationOwnedByCurrentExecution() {
  LockGuard<Spinlock> guard(m_LifecycleStateLock);
  return m_LifecycleDepth && m_pLifecycleOwner == currentOperationOwner();
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

bool MemoryMapManager::clone(Process* pProcess) {
  OperationGuard operation(*this);

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  VirtualAddressSpace* pOtherVa = pProcess->getAddressSpace();

  MmObjectList* pMmObjectList = m_MmObjectLists.lookup(&va);
  if (!pMmObjectList)
    return true;

  MmObjectList* pMmObjectList2 = m_MmObjectLists.lookup(pOtherVa);
  if (!pMmObjectList2) {
    pMmObjectList2 = new MmObjectList();
    if (!pMmObjectList2 || !m_MmObjectLists.tryInsert(pOtherVa, pMmObjectList2)) {
      delete pMmObjectList2;
      return false;
    }
  }

  Tree<MappingAttachment*, SharedPointer<MappingAttachment>> clonedAttachments;
  for (List<MemoryMappedObject*>::Iterator it = pMmObjectList->begin(); it != pMmObjectList->end();
       it++) {
    MemoryMappedObject* obj = *it;
    if (!pMmObjectList2->reserveBack(obj->address()))
      return false;
    MemoryMappedObject* pNewObject = obj->clone();
    if (!pNewObject) {
      pMmObjectList2->popBack();
      return false;
    }
    pNewObject->m_OwnerProcess = pProcess;
    pMmObjectList2->publishBack(pNewObject);
    if (obj->m_Attachment) {
      auto attachment = clonedAttachments.lookup(obj->m_Attachment.get());
      if (!attachment) {
        attachment = obj->m_Attachment->clone(pProcess);
        clonedAttachments.insert(obj->m_Attachment.get(), attachment);
      }
      pNewObject->m_Attachment = attachment;
    }
  }
  return true;
}

size_t MemoryMapManager::remove(uintptr_t base, size_t length) {
  return removeInternal(base, length, false);
}

size_t MemoryMapManager::removeAndRelease(uintptr_t base, size_t length, VmStatus* status) {
  OperationGuard operation(*this);
  if (status)
    *status = VmStatus::InvalidRange;
  const size_t mask = PhysicalMemoryManager::getPageSize() - 1;
  if ((base & mask) || !length || length > ~size_t(0) - mask)
    return 0;
  length = (length + mask) & ~mask;
  if (length > ~uintptr_t(0) - base)
    return 0;
  auto& space = Processor::information().getVirtualAddressSpace();
  if (space.runtimeMappingPages(base, length)) {
    if (status)
      *status = VmStatus::Unsupported;
    return 0;
  }
  UniquePointer<PreparedMemoryLock> raw;
  const auto prepared = space.rawUserMemory().prepareReplacement(base, length, raw);
  if (prepared != MemoryLockStatus::Success) {
    if (status)
      *status =
          prepared == MemoryLockStatus::Unsupported ? VmStatus::Unsupported : VmStatus::NoMemory;
    return 0;
  }
  VmStatus removedStatus;
  const size_t affected = removeInternal(base, length, true, &removedStatus);
  if (removedStatus != VmStatus::Success) {
    if (status)
      *status = removedStatus;
    return 0;
  }
  if (raw) {
    raw.get()->commit();
    auto* process = Processor::information().getCurrentThread()->getParent();
    for (size_t i = 0; i < raw.get()->removedRangeCount(); ++i) {
      const auto& range = raw.get()->removedRanges()[i];
      releaseReservation(process, space, range.base, range.length);
    }
    if (auto* account = space.memoryLockAccount()) {
      auto charge = account->charge();
      assert(raw.get()->removedPages() <= charge.rawPages);
      charge.rawPages -= raw.get()->removedPages();
      account->publish(charge, account->futureMode());
    }
  }
  if (status)
    *status = VmStatus::Success;
  return affected;
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
    if (object->m_LockMode != MemoryLockMode::None)
      retireLockedPages(space, length / (pageMask + 1));
    object->unmap();
    delete object;
    releaseReservation(process, space, base, length);
    ++removed;
  }
  return removed;
}

void MemoryMapManager::releaseReservation(Process* process, VirtualAddressSpace& addressSpace,
                                          uintptr_t base, size_t length) {
  const uintptr_t end = base + length;

  auto releaseIntersection = [process, base, end](Process::UserRegion region, uintptr_t regionStart,
                                                  uintptr_t regionEnd) {
    const uintptr_t releaseStart = base > regionStart ? base : regionStart;
    const uintptr_t releaseEnd = end < regionEnd ? end : regionEnd;
    if (releaseStart < releaseEnd) {
      process->freeUserRange(region, releaseStart, releaseEnd - releaseStart);
    }
  };

  const uintptr_t dynamicStart = addressSpace.getDynamicStart();
  const uintptr_t dynamicEnd = addressSpace.getDynamicEnd();
  if (dynamicStart && dynamicStart < dynamicEnd) {
    releaseIntersection(Process::UserRegion::Dynamic, dynamicStart, dynamicEnd);
  }
  releaseIntersection(Process::UserRegion::Normal, addressSpace.getUserStart(),
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
    if (object->address() < end && objectEnd > base && object->m_bCopyOnWrite &&
        (perms & MemoryMappedObject::Exec)) {
      File* backing = object->backingFile();
      if (backing) {
        if (!backing->acquireMappingUse(true, false)) {
          if (status)
            *status = ProtectStatus::TextBusy;
          return 0;
        }
        backing->releaseMappingUse(true, false);
      }
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
      if (!objects->reserveBack(base)) {
        if (status)
          *status = ProtectStatus::NoMemory;
        return affected;
      }
      auto* split = object->split(base);
      if (!split) {
        objects->popBack();
        if (status)
          *status = ProtectStatus::NoMemory;
        return affected;
      }
      objects->publishBack(split);
      object = split;
    }
    if (objectEnd > end) {
      if (!objects->reserveBack(end)) {
        if (status)
          *status = ProtectStatus::NoMemory;
        return affected;
      }
      auto* split = object->split(end);
      if (!split) {
        objects->popBack();
        if (status)
          *status = ProtectStatus::NoMemory;
        return affected;
      }
      objects->publishBack(split);
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

#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  Process* process = Processor::information().getCurrentThread()->getParent();
  process->recordBenchmarkVmCounter(Process::VmAllowsCalls);
  process->recordBenchmarkVmCounter(Process::VmAllowsObjectCount, pMmObjectList->count());
  size_t objectVisits = 0;
#endif

  const size_t pageMask = PhysicalMemoryManager::getPageSize() - 1;
  uintptr_t cursor = base;
  while (cursor < end) {
    uintptr_t coveredUntil = cursor;
    for (List<MemoryMappedObject*>::Iterator it = pMmObjectList->begin();
         it != pMmObjectList->end(); ++it) {
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
      ++objectVisits;
#endif
      MemoryMappedObject* pObject = *it;
      uintptr_t objectEnd = (pObject->address() + pObject->length() + pageMask) & ~pageMask;
      if (cursor >= pObject->address() && cursor < objectEnd &&
          (pObject->permissions() & permissions) == permissions) {
        if (objectEnd > coveredUntil) {
          coveredUntil = objectEnd;
        }
        if (coveredUntil >= end) {
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
          process->recordBenchmarkVmCounter(Process::VmAllowsObjectVisits, objectVisits);
#endif
          return true;
        }
      }
    }

    if (coveredUntil == cursor) {
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
      process->recordBenchmarkVmCounter(Process::VmAllowsObjectVisits, objectVisits);
#endif
      return false;
    }
    cursor = coveredUntil < end ? coveredUntil : end;
  }

#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  process->recordBenchmarkVmCounter(Process::VmAllowsObjectVisits, objectVisits);
#endif
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
  if (what == Sync) {
    const uintptr_t end = base + length;
    for (MemoryMappedObject* object : *pMmObjectList) {
      const uintptr_t objectEnd =
          (object->address() + object->length() + pageSz - 1) & ~(pageSz - 1);
      const uintptr_t start = base > object->address() ? base : object->address();
      const uintptr_t stop = end < objectEnd ? end : objectEnd;
      if (start < stop)
        success = object->syncRange(start, stop - start, async) && success;
    }
    return success;
  }
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
  auto* object = objects->find(address);
  return object && object->sharedBacking(address, identity, offset);
}

bool MemoryMapManager::faultInUnlocked(uintptr_t address, bool write, MemoryMappedObject*& selected,
                                       bool residentAnonymousOnly) {
  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  Process* diagnosticProcess = Processor::information().getCurrentThread()->getParent();
#endif
  const uintptr_t pageAddress = address & ~(PhysicalMemoryManager::getPageSize() - 1);
  void* page = reinterpret_cast<void*>(pageAddress);
  const bool pageAligned = address == pageAddress;

  if (!selected || !selected->matches(address)) {
    selected = nullptr;
    auto* objects = m_MmObjectLists.lookup(&va);
    if (objects) {
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
      size_t objectVisits = 0;
#endif
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
      selected = objects->find(address, &objectVisits);
      Processor::information().getCurrentThread()->getParent()->recordBenchmarkVmCounter(
          Process::VmFaultInObjectVisits, objectVisits);
#else
      selected = objects->find(address);
#endif
    }
  }

  if (residentAnonymousOnly && (!selected || selected->backingFile())) {
    return false;
  }
  const auto required = write ? MemoryMappedObject::Write : MemoryMappedObject::Read;
  if (selected && !(selected->permissions() & required))
    return false;

  physical_uintptr_t physical = 0;
  size_t flags = 0;
  bool present = va.getMapping(page, physical, flags);
  if (residentAnonymousOnly) {
    return present && (flags & VirtualAddressSpace::Write) &&
           !(flags & (VirtualAddressSpace::KernelMode | VirtualAddressSpace::NoAccess |
                      VirtualAddressSpace::Swapped | VirtualAddressSpace::WriteProtected |
                      VirtualAddressSpace::CopyOnWrite));
  }
  if (selected && (!present || (flags & VirtualAddressSpace::NoAccess))) {
    if (selected->prepareResidentAccess(va, reinterpret_cast<uintptr_t>(page)) !=
        PopulationStatus::Success)
      return false;
    present = va.getMapping(page, physical, flags);
  }
  if (present) {
    if ((flags & (VirtualAddressSpace::KernelMode | VirtualAddressSpace::NoAccess |
                  VirtualAddressSpace::Swapped)) ||
        (write && (flags & VirtualAddressSpace::WriteProtected))) {
      return false;
    }
    if (!write || (flags & VirtualAddressSpace::Write)) {
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
      diagnosticProcess->recordBenchmarkVmCounter(Process::VmFaultInPresent);
#endif
      return true;
    }
    if (flags & VirtualAddressSpace::CopyOnWrite) {
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
      diagnosticProcess->recordBenchmarkVmCounter(Process::VmFaultInCopyOnWrite);
#endif
      return va.handleCopyOnWriteFault(page, true);
    }
  }

  // A page-aligned range keeps the selected object aligned with handleTrap's
  // own mapping lookup. Preserve the old fallback for unaligned direct calls.
  MemoryMappedObject* trapObject = pageAligned ? selected : nullptr;
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  diagnosticProcess->recordBenchmarkVmCounter(Process::VmFaultInTrap);
#endif
  return handleTrapUnlocked(address, write, present, false, trapObject);
}

bool MemoryMapManager::faultIn(uintptr_t address, bool write) {
  OperationGuard operation(*this);
  MemoryMappedObject* selected = nullptr;
  return faultInUnlocked(address, write, selected);
}

bool MemoryMapManager::faultInRange(uintptr_t address, size_t length, bool write) {
  return accessRange(address, length, write, false);
}

bool MemoryMapManager::writableAnonymousRange(uintptr_t address, size_t length) {
  return accessRange(address, length, true, true);
}

bool MemoryMapManager::accessRange(uintptr_t address, size_t length, bool write,
                                   bool residentAnonymousOnly) {
  if (!length) {
    return true;
  }
  if (length - 1 > (~static_cast<uintptr_t>(0) - address)) {
    return false;
  }

  auto faultRange = [&]() {
    const size_t pageSize = PhysicalMemoryManager::getPageSize();
    const uintptr_t lastPage = (address + length - 1) & ~(pageSize - 1);
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
    Process* process = Processor::information().getCurrentThread()->getParent();
    process->recordBenchmarkVmCounter(Process::VmFaultInRangeCalls);
    process->recordBenchmarkVmCounter(Process::VmFaultInRangePages,
                                      (lastPage - (address & ~(pageSize - 1))) / pageSize + 1);
#endif
    MemoryMappedObject* selected = nullptr;
    for (uintptr_t page = address & ~(pageSize - 1);; page += pageSize) {
      if (!faultInUnlocked(page, write, selected, residentAnonymousOnly)) {
        return false;
      }
      if (page == lastPage) {
        return true;
      }
    }
  };

  if (operationOwnedByCurrentExecution())
    return faultRange();
  OperationGuard operation(*this);
  return faultRange();
}

MemoryMapManager::FaultResolution MemoryMapManager::resolveUserFault(uintptr_t address, bool write,
                                                                     bool wasPresent,
                                                                     bool execute) {
  if (!Processor::getInterrupts() || !Processor::information().getCurrentThread() ||
      (write && execute))
    return FaultResolution::Unhandled;
  OperationGuard operation(*this);
  VirtualAddressSpace& space = Processor::information().getVirtualAddressSpace();
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  Process* process = Processor::information().getCurrentThread()->getParent();
#endif
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  process->recordBenchmarkVmCounter(Process::VmFaultCalls);
#endif
  const bool normal = address >= space.getUserStart() && address < space.getUserReservedStart();
  const bool dynamic = space.getDynamicStart() && address >= space.getDynamicStart() &&
                       address < space.getDynamicEnd();
  if (!normal && !dynamic) {
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
    process->recordBenchmarkVmCounter(Process::VmFaultUnhandled);
#endif
    return FaultResolution::Unhandled;
  }
  auto* objects = m_MmObjectLists.lookup(&space);
  if (!objects) {
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
    process->recordBenchmarkVmCounter(Process::VmFaultUnhandled);
#endif
    return FaultResolution::Unhandled;
  }
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  process->recordBenchmarkVmCounter(Process::VmFaultObjectCount, objects->count());
  size_t objectVisits = 0;
#endif
  const uintptr_t pageAddress = address & ~(PhysicalMemoryManager::getPageSize() - 1);
  const auto required = execute ? MemoryMappedObject::Exec
                        : write ? MemoryMappedObject::Write
                                : MemoryMappedObject::Read;
  MemoryMappedObject* selected = nullptr;
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  selected = objects->find(pageAddress, &objectVisits);
#else
  selected = objects->find(pageAddress);
#endif
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  process->recordBenchmarkVmCounter(Process::VmFaultObjectVisits, objectVisits);
#endif
  if (!selected || !(selected->permissions() & required)) {
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
    process->recordBenchmarkVmCounter(Process::VmFaultUnhandled);
#endif
    return FaultResolution::Unhandled;
  }
  if (selected->beyondBackingEnd(pageAddress)) {
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
    process->recordBenchmarkVmCounter(Process::VmFaultBacking);
#endif
    return FaultResolution::BackingFault;
  }
  if (!handleTrapUnlocked(address, write, wasPresent, execute, selected)) {
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
    process->recordBenchmarkVmCounter(Process::VmFaultUnhandled);
#endif
    return FaultResolution::Unhandled;
  }
  void* page = reinterpret_cast<void*>(pageAddress);
  if (!space.isMapped(page)) {
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
    process->recordBenchmarkVmCounter(Process::VmFaultUnhandled);
#endif
    return FaultResolution::Unhandled;
  }
  physical_uintptr_t physical;
  size_t flags;
  space.getMapping(page, physical, flags);
  const bool accessible =
      !(flags & (VirtualAddressSpace::KernelMode | VirtualAddressSpace::NoAccess |
                 VirtualAddressSpace::Swapped)) &&
      (!write ||
       ((flags & VirtualAddressSpace::Write) && !(flags & VirtualAddressSpace::WriteProtected))) &&
      (!execute || (flags & VirtualAddressSpace::Execute));
#if PEDIGREE_BENCHMARK_VM_DIAGNOSTICS
  process->recordBenchmarkVmCounter(accessible ? Process::VmFaultResolved
                                               : Process::VmFaultUnhandled);
#endif
  return accessible ? FaultResolution::Resolved : FaultResolution::Unhandled;
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

    if (object->m_LockMode != MemoryLockMode::None) {
      const size_t pageSize = PhysicalMemoryManager::getPageSize();
      retireLockedPages(va, (object->length() + pageSize - 1) / pageSize);
    }
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
  // User faults must not wait for the mapping gate, allocation or backing I/O
  // while the architecture's raw interrupt and accounting scopes are active.
  if (!state.kernelMode())
    return false;
  return handleTrap(address, bIsWrite, bWasPresent);
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
bool MemoryMapManager::trapForHostedTest(uintptr_t address, bool bIsWrite, bool bWasPresent) {
  return handleTrap(address, bIsWrite, bWasPresent);
}
#endif

bool MemoryMapManager::handleTrap(uintptr_t address, bool bIsWrite, bool bWasPresent, bool execute,
                                  MemoryMappedObject* selected) {
  // Can't take an event while we're trapping, as the event would otherwise
  // be in a minefield (can't touch *any* trap pages in userspace).
  Uninterruptible while_trapping;
  OperationGuard operation(*this);

  return handleTrapUnlocked(address, bIsWrite, bWasPresent, execute, selected);
}

bool MemoryMapManager::handleTrapUnlocked(uintptr_t address, bool bIsWrite, bool bWasPresent,
                                          bool execute, MemoryMappedObject* selected) {
  // Callers already own OperationGuard, so do not re-enter the event and
  // lifetime deferral scopes for every page fault in a user copy.

  EMIT_IF(DebugMemoryMappings) {
    NOTICE("Trap start: " << Hex << address << ", pid:tid " << Dec
                          << Processor::information().getCurrentThread()->getParent()->getId()
                          << ":" << Processor::information().getCurrentThread()->getId());
  }

  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  size_t pageSz = PhysicalMemoryManager::getPageSize();
  const uintptr_t pageAddress = address & ~(pageSz - 1);

  MemoryMappedObject* pObject = selected;
  if (!pObject) {
    m_Lock.acquire();
    EMIT_IF(DebugMemoryMappings) {
      NOTICE_NOLOCK("trap: got lock");
    }

    MmObjectList* pMmObjectList = m_MmObjectLists.lookup(&va);
    if (!pMmObjectList) {
      m_Lock.release();
      return false;
    }

    EMIT_IF(DebugMemoryMappings) {
      NOTICE_NOLOCK("trap: lookup complete " << reinterpret_cast<uintptr_t>(pMmObjectList));
    }

    // The final page can extend beyond the stored byte length of a file.
    pObject = pMmObjectList->find(pageAddress);

    m_Lock.release();
  }
  if (!pObject) {
    EMIT_IF(DebugMemoryMappings) {
      ERROR("MemoryMapManager::trap() could not find an object for " << address);
    }
    return false;
  }

  const MemoryMappedObject::Permissions required = execute    ? MemoryMappedObject::Exec
                                                   : bIsWrite ? MemoryMappedObject::Write
                                                              : MemoryMappedObject::Read;
  if (!(pObject->permissions() & required)) {
    return false;
  }

  if (pObject->prepareResidentAccess(va, pageAddress) != PopulationStatus::Success)
    return false;

  // A mapping published while this fault waited is only a completed resolution
  // if it permits the access which originally faulted.
  if (va.isMapped(reinterpret_cast<void*>(pageAddress))) {
    physical_uintptr_t physicalAddress = 0;
    size_t flags = 0;
    va.getMapping(reinterpret_cast<void*>(pageAddress), physicalAddress, flags);
    const bool userAccessible =
        !(flags & (VirtualAddressSpace::KernelMode | VirtualAddressSpace::NoAccess |
                   VirtualAddressSpace::Swapped));
    if (userAccessible && (!execute || (flags & VirtualAddressSpace::Execute))) {
      if (!bIsWrite && !bWasPresent) {
        return true;
      }
      if (bIsWrite && !(flags & VirtualAddressSpace::WriteProtected)) {
        if (flags & VirtualAddressSpace::Write) {
          return true;
        }
        if (flags & VirtualAddressSpace::CopyOnWrite) {
          return va.handleCopyOnWriteFault(reinterpret_cast<void*>(pageAddress), true);
        }
      }
    }
  }

  PopulationStatus population = PopulationStatus::NoMemory;
  return pObject->trap(va, address, bIsWrite, execute ? &population : nullptr);
}

bool MemoryMapManager::hasSharedWriteCapability(File* backing) {
  OperationGuard operation(*this);
  const uintptr_t identity = backing->futexIdentity();
  for (auto spaces = m_MmObjectLists.begin(); spaces != m_MmObjectLists.end(); ++spaces) {
    for (auto objects = spaces.value()->begin(); objects != spaces.value()->end(); ++objects) {
      const auto* object = *objects;
      if (!object->m_bCopyOnWrite && (object->maximumPermissions() & MemoryMappedObject::Write) &&
          object->usesBacking(identity)) {
        return true;
      }
    }
  }
  return false;
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
      if ((*it2)->reclaimAnonymousPage(*it.key()))
        return true;
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

    if (object->m_LockMode != MemoryLockMode::None) {
      const size_t pageSize = PhysicalMemoryManager::getPageSize();
      retireLockedPages(va, (object->length() + pageSize - 1) / pageSize);
    }
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
