/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/utilities/assert.h"

#include "File.h"
#include "MemoryMappedFile.h"

struct MemoryMapManager::PreparedFileResize::Data {
  struct Mapping {
    VirtualAddressSpace* space;
    MemoryMappedFile* object;
  };
  UniqueArray<Mapping> mappings;
  UniqueArray<Cache::DiscardReference> loans;
  size_t mappingCount = 0;
  size_t loanCount = 0;
  size_t totalLoans = 0;
  size_t newSize = 0;
  bool committed = false;
};

MemoryMapManager::PreparedFileResize::PreparedFileResize(Data* data)
    : m_Data(UniquePointer<Data>::adopt(data)) {}

MemoryMapManager::PreparedFileResize::~PreparedFileResize() = default;

const Cache::DiscardReference* MemoryMapManager::PreparedFileResize::loans() const {
  return m_Data.get()->loans.get();
}

size_t MemoryMapManager::PreparedFileResize::loanCount() const {
  return m_Data.get()->loanCount;
}

size_t MemoryMapManager::PreparedFileResize::totalLoans() const {
  return m_Data.get()->totalLoans;
}

void MemoryMapManager::PreparedFileResize::commit() {
  Data& data = *m_Data.get();
  assert(!data.committed);
  for (size_t i = 0; i < data.mappingCount; ++i) {
    const auto& mapping = data.mappings.get()[i];
    mapping.object->discardFilePages(*mapping.space, data.newSize);
  }
  data.committed = true;
}

MemoryMapManager::ResizeStatus MemoryMapManager::prepareFileResize(
    File* file, size_t oldSize, size_t newSize, UniquePointer<PreparedFileResize>& result) {
  result.reset();
  if (!file || newSize >= oldSize || file->getSize() != oldSize) {
    return ResizeStatus::Invalid;
  }
  OperationGuard operation(*this);
  constexpr size_t MaximumMappings = 4096;
  constexpr size_t MaximumTrackedPages = 65536;
  const uintptr_t identity = file->futexIdentity();
  size_t mappingCount = 0;
  size_t trackedCount = 0;
  for (auto spaces = m_MmObjectLists.count() ? m_MmObjectLists.begin() : m_MmObjectLists.end();
       spaces != m_MmObjectLists.end(); ++spaces) {
    for (auto objects = spaces.value()->begin(); objects != spaces.value()->end(); ++objects) {
      if (!(*objects)->usesBacking(identity)) {
        continue;
      }
#if !X64 && !HOSTED
      return ResizeStatus::Unsupported;
#endif
      auto* object = static_cast<MemoryMappedFile*>(*objects);
      LockGuard<Mutex> guard(object->m_Lock);
      if (++mappingCount > MaximumMappings ||
          object->m_Mappings.count() > MaximumTrackedPages - trackedCount) {
        return ResizeStatus::NoMemory;
      }
      trackedCount += object->m_Mappings.count();
    }
  }

  auto* data = new PreparedFileResize::Data;
  if (!data) {
    return ResizeStatus::NoMemory;
  }
  auto plan = UniquePointer<PreparedFileResize>::adopt(new PreparedFileResize(data));
  if (!plan) {
    delete data;
    return ResizeStatus::NoMemory;
  }
  if (mappingCount) {
    data->mappings = UniqueArray<PreparedFileResize::Data::Mapping>::allocate(mappingCount);
    if (!data->mappings) {
      return ResizeStatus::NoMemory;
    }
  }
  data->newSize = newSize;

  Tree<uintptr_t, size_t> loanCounts;
  for (auto spaces = m_MmObjectLists.count() ? m_MmObjectLists.begin() : m_MmObjectLists.end();
       spaces != m_MmObjectLists.end(); ++spaces) {
    for (auto objects = spaces.value()->begin(); objects != spaces.value()->end(); ++objects) {
      if (!(*objects)->usesBacking(identity)) {
        continue;
      }
      auto* object = static_cast<MemoryMappedFile*>(*objects);
      data->mappings.get()[data->mappingCount++] = {spaces.key(), object};
      LockGuard<Mutex> guard(object->m_Lock);
      uintptr_t cursor = object->m_Address;
      uintptr_t address = 0;
      physical_uintptr_t tracked = 0;
      while (object->m_Mappings.lowerBound(cursor, address, tracked)) {
        if (address == ~uintptr_t(0)) {
          return ResizeStatus::Invalid;
        }
        cursor = address + 1;
        if (tracked != ~static_cast<physical_uintptr_t>(0)) {
          continue;
        }
        if (address < object->m_Address || address - object->m_Address >= object->m_Length ||
            address - object->m_Address > ~size_t(0) - object->m_Offset) {
          return ResizeStatus::Invalid;
        }
        const uintptr_t offset = (object->m_Offset + (address - object->m_Address)) &
                                 ~(static_cast<uintptr_t>(TargetInfo::getPageSize()) - 1);
        // Generic CoW may replace a PTE while its original cache loan remains
        // tracked here. Count that loan, independently of the current PTE.
        const size_t count = loanCounts.lookup(offset);
        if (!loanCounts.tryInsert(offset, count + 1)) {
          return ResizeStatus::NoMemory;
        }
        ++data->totalLoans;
      }
    }
  }
  data->loanCount = loanCounts.count();
  if (data->loanCount) {
    data->loans = UniqueArray<Cache::DiscardReference>::allocate(data->loanCount);
    if (!data->loans) {
      return ResizeStatus::NoMemory;
    }
    size_t i = 0;
    for (auto loan = loanCounts.begin(); loan != loanCounts.end(); ++loan) {
      data->loans.get()[i++] = {loan.key(), loan.value()};
    }
  }
  result = pedigree_std::move(plan);
  return ResizeStatus::Ready;
}
