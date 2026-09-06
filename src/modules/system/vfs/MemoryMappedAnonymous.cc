/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/utility.h"

#include "MemoryMappedFile.h"

namespace {
size_t anonymousFlags(size_t flags, MemoryMappedObject::Permissions permissions) {
  flags &= ~(VirtualAddressSpace::Write | VirtualAddressSpace::Execute |
             VirtualAddressSpace::NoAccess | VirtualAddressSpace::WriteProtected);
  if (!permissions)
    flags |= VirtualAddressSpace::NoAccess;
  if (permissions & MemoryMappedObject::Exec)
    flags |= VirtualAddressSpace::Execute;
  if (!(permissions & MemoryMappedObject::Write))
    flags |= VirtualAddressSpace::WriteProtected;
  else if (!(flags & (VirtualAddressSpace::CopyOnWrite | VirtualAddressSpace::Shared)))
    flags |= VirtualAddressSpace::Write;
  return flags;
}
bool admitOwner(Process* identity, VirtualAddressSpace& space, Scheduler::ProcessLease& owner) {
  return identity && Scheduler::instance().acquireProcess(owner, identity) &&
         owner->getAddressSpace() == &space;
}
void accountPages(Process& owner, ssize_t virtualPages, ssize_t physicalPages) {
#if X64 && !HOSTED
  // Existing PMM operations charge the executor. Transfer only committed
  // data and new user-table ownership; failed allocations balance themselves.
  auto* thread = Processor::information().getCurrentThread();
  auto* caller = thread ? thread->getParent() : nullptr;
  if (caller != &owner) {
    if (caller)
      caller->trackPages(0, -physicalPages, 0);
    owner.trackPages(virtualPages, physicalPages, 0);
  } else
    owner.trackPages(virtualPages, 0, 0);
#endif
}
PopulationStatus populationStatus(SwapStatus status) {
  return status == SwapStatus::Success    ? PopulationStatus::Success
         : status == SwapStatus::NoMemory ? PopulationStatus::NoMemory
                                          : PopulationStatus::IoError;
}
}  // namespace

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
namespace {
ssize_t anonymousCloneFailure = -1;
}
void AnonymousMemoryMap::setCloneFailureForTest(ssize_t after) {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  anonymousCloneFailure = after;
}
#endif
AnonymousMemoryMap::~AnonymousMemoryMap() {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  if (m_OwnsMappings)
    for (auto& page : m_Mappings)
      SwapStore::instance().release(page.slot);
}
MemoryMappedObject* AnonymousMemoryMap::clone() {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  auto* result = new AnonymousMemoryMap(m_Address, m_Length, m_Permissions);
  if (!result)
    return nullptr;
  result->m_OwnerProcess = m_OwnerProcess;
  result->m_MaximumPermissions = m_MaximumPermissions;
  for (const auto& page : m_Mappings) {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    if (!anonymousCloneFailure) {
      delete result;
      return nullptr;
    }
    if (anonymousCloneFailure > 0)
      --anonymousCloneFailure;
#endif
    if (!result->m_Mappings.tryPushBack(page)) {
      delete result;
      return nullptr;
    }
    if (page.slot && !SwapStore::instance().retain(page.slot)) {
      result->m_Mappings.popBack();
      delete result;
      return nullptr;
    }
  }
  return result;
}
MemoryMappedObject* AnonymousMemoryMap::stageSlice(uintptr_t source, size_t sourceLength,
                                                   uintptr_t destination,
                                                   size_t destinationLength) {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  auto* result = new AnonymousMemoryMap(destination, destinationLength, m_Permissions);
  if (!result)
    return nullptr;
  result->m_OwnerProcess = m_OwnerProcess;
  result->m_OwnsMappings = false;
  result->m_LockMode = m_LockMode;
  if (sourceLength == destinationLength && source < m_Address + m_Length &&
      sourceLength > m_Address + m_Length - source)
    result->m_Length = m_Address + m_Length - source;
  result->m_bCopyOnWrite = m_bCopyOnWrite;
  result->m_MaximumPermissions = m_MaximumPermissions;
  result->m_Attachment = m_Attachment;
  const size_t preserved = sourceLength < destinationLength ? sourceLength : destinationLength;
  for (const auto& page : m_Mappings) {
    if (page.address < source || page.address - source >= preserved)
      continue;
    Page moved = page;
    moved.address = destination + page.address - source;
    if (!result->m_Mappings.tryPushBack(moved)) {
      delete result;
      return nullptr;
    }
  }
  return result;
}
MemoryMappedObject* AnonymousMemoryMap::split(uintptr_t at) {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  if (at <= m_Address || at >= m_Address + m_Length)
    return nullptr;
  auto* result = static_cast<AnonymousMemoryMap*>(
      stageSlice(at, m_Address + m_Length - at, at, m_Address + m_Length - at));
  if (!result)
    return nullptr;
  for (auto it = m_Mappings.begin(); it != m_Mappings.end();)
    if ((*it).address >= at)
      it = m_Mappings.erase(it);
    else
      ++it;
  m_Length = at - m_Address;
  result->m_OwnsMappings = m_OwnsMappings;
  return result;
}
void AnonymousMemoryMap::releaseDetachedPage(uintptr_t oldAddress,
                                             const VirtualAddressSpace::DetachedPage& page) {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  for (auto it = m_Mappings.begin(); it != m_Mappings.end(); ++it) {
    if ((*it).address == oldAddress) {
      SwapStore::instance().release((*it).slot);
      m_Mappings.erase(it);
      break;
    }
  }
  if (page.mapped)
    PhysicalMemoryManager::instance().freePage(page.physical);
}
void AnonymousMemoryMap::discardRange(VirtualAddressSpace& space, uintptr_t base, size_t length) {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  const size_t bytes = PhysicalMemoryManager::getPageSize();
  for (uintptr_t address = base; address < base + length; address += bytes) {
    VirtualAddressSpace::DetachedPage page{address, 0, 0, false};
    page.mapped = space.detachMapping(reinterpret_cast<void*>(address), page.physical, page.flags);
    releaseDetachedPage(address, page);
  }
}
bool AnonymousMemoryMap::remove(size_t length) {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  const size_t mask = PhysicalMemoryManager::getPageSize() - 1;
  length = (length + mask) & ~mask;
  if (length >= m_Length) {
    unmapUnlocked();
    return true;
  }
  auto& space = Processor::information().getVirtualAddressSpace();
  discardRange(space, m_Address, length);
  m_Address += length;
  m_Length -= length;
  return false;
}
void AnonymousMemoryMap::setPermissions(Permissions permissions) {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  auto& space = Processor::information().getVirtualAddressSpace();
  for (auto& page : m_Mappings) {
    if (page.slot || !space.isMapped(reinterpret_cast<void*>(page.address)))
      continue;
    physical_uintptr_t physical;
    size_t flags;
    space.getMapping(reinterpret_cast<void*>(page.address), physical, flags);
    space.setFlags(reinterpret_cast<void*>(page.address), anonymousFlags(flags, permissions));
    page.pagingBlocked = false;
  }
  m_Permissions = permissions;
}
void AnonymousMemoryMap::unmap() {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  unmapUnlocked();
}
void AnonymousMemoryMap::unmapUnlocked() {
  auto& space = Processor::information().getVirtualAddressSpace();
  while (m_Mappings.count()) {
    const Page entry = *m_Mappings.begin();
    VirtualAddressSpace::DetachedPage page{entry.address, 0, 0, false};
    page.mapped =
        space.detachMapping(reinterpret_cast<void*>(entry.address), page.physical, page.flags);
    releaseDetachedPage(entry.address, page);
  }
}
SwapStatus AnonymousMemoryMap::restorePage(VirtualAddressSpace& space, Page& page) {
  auto* address = reinterpret_cast<void*>(page.address);
  if (!page.slot && !page.pagingBlocked)
    return SwapStatus::Success;
  Scheduler::ProcessLease owner;
  if (!admitOwner(m_OwnerProcess, space, owner))
    return SwapStatus::Busy;
  if (!page.slot) {
    physical_uintptr_t physical;
    size_t flags;
    if (!space.isMapped(address))
      return SwapStatus::Invalid;
    space.getMapping(address, physical, flags);
    if (!space.trySetFlags(address, anonymousFlags(flags, m_Permissions)))
      return SwapStatus::IoError;
    page.pagingBlocked = false;
    return SwapStatus::Success;
  }
  auto& memory = PhysicalMemoryManager::instance();
  const auto physical = memory.tryAllocatePage();
  if (!physical)
    return SwapStatus::NoMemory;
  SwapStatus status = SwapStore::instance().readPage(page.slot, physical);
  size_t committedTables = 0;
  if (status == SwapStatus::Success &&
      !space.tryMapUserPage(physical, address, anonymousFlags(0, m_Permissions), &committedTables))
    status = SwapStatus::NoMemory;
  if (status != SwapStatus::Success) {
    memory.freePage(physical);
    return status;
  }
  accountPages(*owner.get(), 1, 1 + static_cast<ssize_t>(committedTables));
  SwapStore::instance().release(page.slot);
  page.pagingBlocked = false;
  return SwapStatus::Success;
}
bool AnonymousMemoryMap::trap(VirtualAddressSpace& space, uintptr_t address, bool write,
                              PopulationStatus* population) {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  if (population)
    *population = PopulationStatus::NoMemory;
  const size_t bytes = PhysicalMemoryManager::getPageSize();
  address &= ~(bytes - 1);
  if ((write && !(m_Permissions & Write)) || (!write && !(m_Permissions & Read) && !population))
    return false;
  for (auto& page : m_Mappings) {
    if (page.address == address && (page.slot || page.pagingBlocked)) {
      const auto status = restorePage(space, page);
      if (population)
        *population = populationStatus(status);
      return status == SwapStatus::Success;
    }
  }
  Scheduler::ProcessLease owner;
  if (!admitOwner(m_OwnerProcess, space, owner))
    return false;
  auto* virtualAddress = reinterpret_cast<void*>(address);
  const size_t execute = (m_Permissions & Exec) ? VirtualAddressSpace::Execute : 0;
  auto& memory = PhysicalMemoryManager::instance();
  if (!write) {
    if (space.isMapped(virtualAddress))
      return false;
    memory.pin(m_Zero);
    if (!space.map(m_Zero, virtualAddress, VirtualAddressSpace::Shared | execute)) {
      memory.freePage(m_Zero);
      return false;
    }
    bool tracked = false;
    for (const auto& page : m_Mappings)
      tracked = tracked || page.address == address;
    if (!tracked && !m_Mappings.tryPushBack({address, {}, false})) {
      space.unmap(virtualAddress);
      memory.freePage(m_Zero);
      return false;
    }
  } else {
#if X64 || HOSTED
    const auto physical = memory.tryAllocatePage();
#else
    const auto physical = memory.allocatePage();
#endif
    if (!physical)
      return false;
    if (!initialisePhysicalPage(physical)) {
      memory.freePage(physical);
      return false;
    }
    // Reserve tracking before replacing a zero-page mapping. The existing
    // address entry is reused, so allocation failure cannot orphan the PTE.
    Page* entry = nullptr;
    for (auto& page : m_Mappings)
      if (page.address == address)
        entry = &page;
    if (!entry) {
      if (!m_Mappings.tryPushBack({address, {}, false})) {
        memory.freePage(physical);
        return false;
      }
      entry = &*m_Mappings.rbegin();
    }
    if (space.isMapped(virtualAddress)) {
      physical_uintptr_t old;
      size_t flags;
      space.getMapping(virtualAddress, old, flags);
      if (old != m_Zero) {
        memory.freePage(physical);
        return false;
      }
      space.unmap(virtualAddress);
      memory.freePage(old);
    }
#if X64 || HOSTED
    size_t committedTables = 0;
    const bool mapped = space.tryMapUserPage(
        physical, virtualAddress, VirtualAddressSpace::Write | execute, &committedTables);
#else
    const bool mapped = space.map(physical, virtualAddress, VirtualAddressSpace::Write | execute);
#endif
    if (!mapped) {
      memory.freePage(physical);
      // The retained empty entry still belongs to this object and may retry.
      return false;
    }
#if X64 || HOSTED
    accountPages(*owner.get(), 1, 1 + static_cast<ssize_t>(committedTables));
#endif
  }
  if (population)
    *population = PopulationStatus::Success;
  return true;
}
bool AnonymousMemoryMap::supportsPageOut(VirtualAddressSpace& space, uintptr_t address) {
  if (!m_OwnerProcess || !m_OwnsMappings || m_LockMode != MemoryLockMode::None || !m_bCopyOnWrite ||
      m_Attachment)
    return false;
  for (auto& page : m_Mappings) {
    if (page.address != address)
      continue;
    if (page.slot)
      return true;
    if (page.pagingBlocked)
      return false;
    auto* at = reinterpret_cast<void*>(address);
    if (!space.isMapped(at))
      return true;
    physical_uintptr_t physical;
    size_t flags;
    space.getMapping(at, physical, flags);
    if (physical == m_Zero)
      return !(flags & VirtualAddressSpace::NoAccess);
    return (flags & VirtualAddressSpace::Write) &&
           !(flags & (VirtualAddressSpace::CopyOnWrite | VirtualAddressSpace::Shared |
                      VirtualAddressSpace::Borrowed | VirtualAddressSpace::NoAccess |
                      VirtualAddressSpace::KernelMode | VirtualAddressSpace::RuntimeMapping));
  }
  return !space.isMapped(reinterpret_cast<void*>(address));
}
SwapStatus AnonymousMemoryMap::pageOutAt(VirtualAddressSpace& space, uintptr_t address,
                                         bool& released) {
  released = false;
  if (!MemoryMapManager::instance().operationOwnedByCurrentExecution() ||
      !supportsPageOut(space, address))
    return SwapStatus::Unsupported;
  Scheduler::ProcessLease owner;
  if (!admitOwner(m_OwnerProcess, space, owner))
    return SwapStatus::Busy;
  for (auto& page : m_Mappings) {
    auto* at = reinterpret_cast<void*>(address);
    if (page.address != address || page.slot || !space.isMapped(at))
      continue;
    physical_uintptr_t physical;
    size_t flags;
    space.getMapping(at, physical, flags);
    if (!space.trySetFlags(at, flags | VirtualAddressSpace::NoAccess))
      return SwapStatus::IoError;
    page.pagingBlocked = true;
    SwapReference slot;
    const bool zero = physical == m_Zero;
    const auto status =
        zero ? SwapStatus::Success : SwapStore::instance().writePage(physical, slot);
    if (status == SwapStatus::Success && space.tryDetachUserPage(at, physical)) {
      page.slot = slot;
      page.pagingBlocked = false;
      PhysicalMemoryManager::instance().freePage(physical);
      accountPages(*owner.get(), -1, zero ? 0 : -1);
      released = !zero;
      return SwapStatus::Success;
    }
    SwapStore::instance().release(slot);
    if (space.trySetFlags(at, flags))
      page.pagingBlocked = false;
    return status == SwapStatus::Success ? SwapStatus::IoError : status;
  }
  return SwapStatus::Success;
}
bool AnonymousMemoryMap::pageOut(VirtualAddressSpace& space) {
  if (!MemoryMapManager::instance().operationOwnedByCurrentExecution() ||
      !SwapStore::instance().snapshot().active)
    return false;
  for (auto& page : m_Mappings) {
    if (!supportsPageOut(space, page.address))
      continue;
    bool released = false;
    if (pageOutAt(space, page.address, released) != SwapStatus::Success)
      return false;
    if (released)
      return true;
  }
  return false;
}
SwapStatus AnonymousMemoryMap::restoreAll(VirtualAddressSpace& space) {
  for (auto& page : m_Mappings) {
    const auto status = restorePage(space, page);
    if (status != SwapStatus::Success)
      return status;
  }
  return SwapStatus::Success;
}

PopulationStatus AnonymousMemoryMap::prepareResidentAccess(VirtualAddressSpace& space,
                                                           uintptr_t address) {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  for (auto& page : m_Mappings)
    if (page.address == address && page.pagingBlocked)
      return populationStatus(restorePage(space, page));
  return PopulationStatus::Success;
}
