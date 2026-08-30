/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/utilities/Cache.h"
#include "pedigree/kernel/utilities/utility.h"

namespace {
constexpr size_t PageSize = TargetInfo::getPageSize();
constexpr size_t TerminalBytes = 512;
static_assert(PageSize >= TerminalBytes, "disk regression requires one sector per target page");

bool cacheRangeGeometry() {
  constexpr uintptr_t EditingKey = 0x57A6000;
  constexpr uintptr_t PublishedKey = EditingKey + (4 * PageSize);
  Cache cache;

  bool alreadyExisted = true;
  const uintptr_t invalid = cache.insert(EditingKey, PageSize + TerminalBytes, &alreadyExisted);
  const bool insertionRejected = !invalid && !alreadyExisted && !cache.exists(EditingKey, PageSize);

  const uintptr_t editing = cache.insert(EditingKey);
  cache.markNoLongerEditing(EditingKey, PageSize + TerminalBytes);
  const bool invalidPublishLeftEditing = editing && cache.discardEditing(EditingKey);

  const uintptr_t published = cache.insert(PublishedKey);
  cache.markNoLongerEditing(PublishedKey);
  cache.markEditing(PublishedKey, PageSize + TerminalBytes);
  const bool invalidEditLeftPublished = published && !cache.discardEditing(PublishedKey);
  cache.empty();

  const bool passed = insertionRejected && invalidPublishLeftEditing && invalidEditLeftPublished;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS cache-range-geometry");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL cache-range-geometry: a partial cache range "
        "changed cache state");
  }
  return passed;
}

class SequenceDisk final : public Disk {
 public:
  static constexpr size_t DataSize = (2 * PageSize) + TerminalBytes;

  SequenceDisk() : m_Data(new uint8_t[DataSize]), m_BalanceError(false) {
    for (size_t i = 0; i < DataSize; ++i) {
      m_Data[i] = static_cast<uint8_t>((i * 31) ^ (i >> 3));
    }
    ByteSet(m_References, 0, sizeof(m_References));
  }

  ~SequenceDisk() override {
    delete[] m_Data;
  }

  BufferView read(uint64_t location) override {
    if (location >= DataSize) {
      return BufferView();
    }

    const uint64_t page = location - (location % PageSize);
    const size_t offset = location - page;
    const size_t validLength = PageSize < (DataSize - page) ? PageSize : (DataSize - page);
    ++m_References[page / PageSize];
    return BufferView(m_Data + location, validLength - offset);
  }

  size_t getSize() const override {
    return DataSize;
  }

  size_t getBlockSize() const override {
    return TerminalBytes;
  }

  bool pin(uint64_t location) override {
    if (location >= DataSize) {
      return false;
    }
    ++m_References[location / PageSize];
    return true;
  }

  void unpin(uint64_t location) override {
    if (location >= DataSize || !m_References[location / PageSize]) {
      m_BalanceError = true;
      return;
    }
    --m_References[location / PageSize];
  }

  const uint8_t* data() const {
    return m_Data;
  }

  bool balanced() const {
    return !m_BalanceError && !m_References[0] && !m_References[1] && !m_References[2];
  }

 private:
  uint8_t* m_Data;
  size_t m_References[3];
  bool m_BalanceError;
};

bool diskViewSequencePageSpan() {
  constexpr size_t ReadLength = 2 * PageSize;
  SequenceDisk disk;
  BufferView storage[3];
  BufferViewSequence views(storage, 3);
  uint8_t* copied = new uint8_t[ReadLength];

  const bool read = disk.readViews(TerminalBytes, ReadLength, views);
  const bool geometry = read && views.count() == 3 &&
                        views[0].size() == (PageSize - TerminalBytes) &&
                        views[1].size() == PageSize && views[2].size() == TerminalBytes;
  const bool copiedAll = geometry && views.copyTo(copied, ReadLength) &&
                         !MemoryCompare(copied, disk.data() + TerminalBytes, ReadLength);
  if (read) {
    disk.unpinViews(TerminalBytes, views);
  }
  const bool released = disk.balanced() && views.empty();

  BufferView shortStorage[2];
  BufferViewSequence shortViews(shortStorage, 2);
  const bool capacityFailure = !disk.readViews(TerminalBytes, ReadLength, shortViews) &&
                               shortViews.empty() && disk.balanced();
  delete[] copied;

  const bool passed = copiedAll && released && capacityFailure;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS disk-view-sequence-page-span");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL disk-view-sequence-page-span: a multi-page "
        "read lost bounds or cache ownership");
  }
  return passed;
}
}  // namespace

bool runHostedStoragePageRegressions() {
  return cacheRangeGeometry() && diskViewSequencePageSpan();
}
