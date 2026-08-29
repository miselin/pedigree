/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/system/rawfs/RawFs.h"
#include "modules/system/rawfs/RawFsFile.h"

namespace {
class TrackingDisk final : public Disk {
 public:
  static constexpr size_t PageSize = TargetInfo::getPageSize();
  static constexpr size_t DataSize = 3 * PageSize;
  static constexpr size_t PageSlots = 4;

  TrackingDisk()
      : m_ReadCount(0),
        m_UnpinCount(0),
        m_DataAllocation(new uint8_t[DataSize + PageSize - 1]),
        m_Data(reinterpret_cast<uint8_t*>(
            (reinterpret_cast<uintptr_t>(m_DataAllocation) + PageSize - 1) & ~(PageSize - 1))),
        m_PageAllocation(new uint8_t[(PageSlots * PageSize) + PageSize - 1]),
        m_PageData(reinterpret_cast<uint8_t*>(
            (reinterpret_cast<uintptr_t>(m_PageAllocation) + PageSize - 1) & ~(PageSize - 1))),
        m_AlignmentPoint(0),
        m_BalanceError(false) {
    for (size_t i = 0; i < DataSize; ++i) {
      m_Data[i] = static_cast<uint8_t>((i * 29) ^ (i >> 2));
    }
    ByteSet(m_ReadLocations, 0, sizeof(m_ReadLocations));
    ByteSet(m_UnpinLocations, 0, sizeof(m_UnpinLocations));
    ByteSet(m_PageReferences, 0, sizeof(m_PageReferences));
    ByteSet(m_PageKeys, 0xFF, sizeof(m_PageKeys));
  }

  ~TrackingDisk() override {
    delete[] m_PageAllocation;
    delete[] m_DataAllocation;
  }

  BufferView read(uint64_t location) override {
    const uint64_t page = pageLocation(location);
    if (page >= DataSize || PageSize > (DataSize - page)) {
      return BufferView();
    }

    const size_t slot = findPage(page, true);
    if (slot == PageSlots) {
      return BufferView();
    }

    if (m_ReadCount < 8) {
      m_ReadLocations[m_ReadCount] = location;
    }
    ++m_ReadCount;
    ++m_PageReferences[slot];
    return BufferView(m_PageData + (slot * PageSize) + (location - page),
                      PageSize - (location - page));
  }

  size_t getSize() const override {
    return DataSize;
  }

  size_t getBlockSize() const override {
    // A readahead extent is not a lifetime guarantee for every page in it.
    return 65536;
  }

  bool pin(uint64_t location) override {
    const size_t slot = findPage(pageLocation(location), false);
    if (slot == PageSlots) {
      return false;
    }
    ++m_PageReferences[slot];
    return true;
  }

  void unpin(uint64_t location) override {
    if (m_UnpinCount < 8) {
      m_UnpinLocations[m_UnpinCount] = location;
    }
    ++m_UnpinCount;

    const size_t slot = findPage(pageLocation(location), false);
    if (slot == PageSlots || !m_PageReferences[slot]) {
      m_BalanceError = true;
      return;
    }
    --m_PageReferences[slot];
  }

  void align(uint64_t location) override {
    m_AlignmentPoint = location;
  }

  bool balanced() const {
    if (m_BalanceError) {
      return false;
    }
    for (size_t i = 0; i < PageSlots; ++i) {
      if (m_PageReferences[i]) {
        return false;
      }
    }
    return true;
  }

  const uint8_t* data() const {
    return m_Data;
  }

  size_t m_ReadCount;
  size_t m_UnpinCount;
  uint64_t m_ReadLocations[8];
  uint64_t m_UnpinLocations[8];

 private:
  uint64_t pageLocation(uint64_t location) const {
    const uint64_t alignPoint = m_AlignmentPoint <= location ? m_AlignmentPoint : 0;
    return location - ((location - alignPoint) % PageSize);
  }

  size_t findPage(uint64_t page, bool create) {
    size_t freeSlot = PageSlots;
    for (size_t i = 0; i < PageSlots; ++i) {
      if (m_PageKeys[i] == page) {
        return i;
      }
      if (freeSlot == PageSlots && m_PageKeys[i] == ~uint64_t(0)) {
        freeSlot = i;
      }
    }

    if (!create || freeSlot == PageSlots) {
      return PageSlots;
    }

    m_PageKeys[freeSlot] = page;
    MemoryCopy(m_PageData + (freeSlot * PageSize), m_Data + page, PageSize);
    return freeSlot;
  }

  uint8_t* m_DataAllocation;
  uint8_t* m_Data;
  uint8_t* m_PageAllocation;
  uint8_t* m_PageData;
  uint64_t m_PageKeys[PageSlots];
  size_t m_PageReferences[PageSlots];
  uint64_t m_AlignmentPoint;
  bool m_BalanceError;
};

bool rawFsNativePageOwnership() {
  constexpr size_t Start = 512;
  constexpr size_t TransferSize = TrackingDisk::PageSize;

  TrackingDisk disk;
  RawFs filesystem;
  RawFsFile file(String("raw-disk"), &filesystem, nullptr, &disk);
  uint8_t* destination = new uint8_t[TransferSize];

  const uint64_t bytesRead =
      file.read(Start, TransferSize, reinterpret_cast<uintptr_t>(destination));
  const bool bytesMatch = !MemoryCompare(destination, disk.data() + Start, TransferSize);
  delete[] destination;

  const bool splitAtPage = disk.m_ReadCount == 2 && disk.m_ReadLocations[0] == 0 &&
                           disk.m_ReadLocations[1] == TrackingDisk::PageSize;
  const bool balanced = disk.m_UnpinCount == 2 && disk.m_UnpinLocations[0] == 0 &&
                        disk.m_UnpinLocations[1] == TrackingDisk::PageSize && disk.balanced();

  const bool passed = bytesRead == TransferSize && bytesMatch && splitAtPage && balanced;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS rawfs-native-page-ownership");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL rawfs-native-page-ownership: "
        "one raw read crossed an unowned native page or leaked its "
        "reference");
  }
  return passed;
}

bool rawFsParentAlignmentIsolation() {
  constexpr size_t Start = TrackingDisk::PageSize;
  constexpr size_t TransferSize = TrackingDisk::PageSize;

  TrackingDisk disk;
  disk.align(512);
  RawFs filesystem;
  RawFsFile file(String("aligned-raw-disk"), &filesystem, nullptr, &disk);
  uint8_t* destination = new uint8_t[TransferSize];

  const uint64_t bytesRead =
      file.read(Start, TransferSize, reinterpret_cast<uintptr_t>(destination));
  const bool bytesMatch = !MemoryCompare(destination, disk.data() + Start, TransferSize);
  delete[] destination;

  const bool splitAtParentPage = disk.m_ReadCount == 2 && disk.m_ReadLocations[0] == Start &&
                                 disk.m_ReadLocations[1] == (Start + 512);
  const bool balanced = disk.m_UnpinCount == 2 && disk.m_UnpinLocations[0] == Start &&
                        disk.m_UnpinLocations[1] == (Start + 512) && disk.balanced();

  const bool passed = bytesRead == TransferSize && bytesMatch && splitAtParentPage && balanced;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS rawfs-parent-alignment-isolation");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL rawfs-parent-alignment-isolation: "
        "a parent alignment point exposed bytes beyond one owned page");
  }
  return passed;
}

bool filePastEofDoesNotRead() {
  TrackingDisk disk;
  RawFs filesystem;
  RawFsFile file(String("raw-disk"), &filesystem, nullptr, &disk);
  uint8_t destination = 0;

  const uint64_t bytesRead =
      file.read(TrackingDisk::DataSize + 1, 1, reinterpret_cast<uintptr_t>(&destination));
  const bool passed = !bytesRead && !disk.m_ReadCount && !disk.m_UnpinCount && disk.balanced();
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS file-past-eof-no-read");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL file-past-eof-no-read: "
        "an out-of-range read reached the backing object");
  }
  return passed;
}
}  // namespace

EXPORTED_PUBLIC bool runHostedRawFsContractRegressions() {
  return rawFsNativePageOwnership() && rawFsParentAlignmentIsolation() && filePastEofDoesNotRead();
}
