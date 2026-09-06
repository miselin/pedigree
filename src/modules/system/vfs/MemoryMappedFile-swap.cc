/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

#include "MemoryMappedFile.h"

SwapStatus MemoryMapManager::activateSwap(uint32_t endpoint) {
  OperationGuard operation(*this);
  return SwapStore::instance().activate(endpoint);
}
SwapStatus MemoryMapManager::deactivateSwap(uint32_t endpoint) {
  OperationGuard operation(*this);
  auto& store = SwapStore::instance();
  if (!endpoint || store.endpointId() != endpoint)
    return SwapStatus::NotActive;
  auto& original = Processor::information().getVirtualAddressSpace();
  SwapStatus status = SwapStatus::Success;
  for (auto spaces = m_MmObjectLists.begin(); spaces != m_MmObjectLists.end(); ++spaces) {
    Processor::switchAddressSpace(*spaces.key());
    for (auto* object : *spaces.value()) {
      status = object->restoreSwapPages(*spaces.key());
      if (status != SwapStatus::Success)
        break;
    }
    if (status != SwapStatus::Success)
      break;
  }
  Processor::switchAddressSpace(original);
  return status == SwapStatus::Success ? store.finishDeactivate(endpoint) : status;
}
SwapSnapshot MemoryMapManager::swapSnapshot() {
  OperationGuard operation(*this);
  return SwapStore::instance().snapshot();
}

SwapStatus MemoryMapManager::pageOutRange(uintptr_t base, size_t length) {
  OperationGuard operation(*this);
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  if ((base & (pageSize - 1)) || (length & (pageSize - 1)) || length > ~uintptr_t(0) - base)
    return SwapStatus::Invalid;
  if (!SwapStore::instance().snapshot().active)
    return SwapStatus::NotActive;
  auto& space = Processor::information().getVirtualAddressSpace();
  auto* objects = m_MmObjectLists.lookup(&space);
  // Validate every owner before revocation; mixed or pinned ranges cannot
  // silently turn an unsupported part of the range into success.
  for (size_t pass = 0; pass < 2; ++pass) {
    for (uintptr_t address = base; address < base + length; address += pageSize) {
      MemoryMappedObject* owner = nullptr;
      if (objects)
        for (auto* object : *objects)
          if (object->matches(address)) {
            owner = object;
            break;
          }
      if (!owner)
        return SwapStatus::Unmapped;
      if (!owner->supportsPageOut(space, address))
        return SwapStatus::Unsupported;
      if (pass) {
        bool released = false;
        const auto status = owner->pageOutAt(space, address, released);
        if (status != SwapStatus::Success)
          return status;
      }
    }
  }
  return SwapStatus::Success;
}
