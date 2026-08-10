/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/system/fat/FatFilesystem.h"

namespace {
class TrackingDisk final : public Disk {
 public:
  static constexpr size_t DataSize = 8192;

  TrackingDisk()
      : m_ReadCount(0),
        m_WriteCount(0),
        m_UnpinCount(0),
        m_Data(new uint8_t[DataSize]),
        m_BalanceError(false) {
    ByteSet(m_Data, 0xA5, DataSize);
    ByteSet(m_ReadLocations, 0, sizeof(m_ReadLocations));
    ByteSet(m_WriteLocations, 0, sizeof(m_WriteLocations));
    ByteSet(m_UnpinLocations, 0, sizeof(m_UnpinLocations));
    ByteSet(m_PageReferences, 0, sizeof(m_PageReferences));
  }

  ~TrackingDisk() override {
    delete[] m_Data;
  }

  uintptr_t read(uint64_t location) override {
    if (location >= DataSize) {
      return 0;
    }

    if (m_ReadCount < 4) {
      m_ReadLocations[m_ReadCount] = location;
    }
    ++m_ReadCount;
    ++m_PageReferences[location / 4096];
    return reinterpret_cast<uintptr_t>(m_Data + location);
  }

  void write(uint64_t location) override {
    if (m_WriteCount < 4) {
      m_WriteLocations[m_WriteCount] = location;
    }
    ++m_WriteCount;
  }

  size_t getSize() const override {
    return DataSize;
  }

  size_t getBlockSize() const override {
    // Device I/O extent size must not change native-page ownership.
    return 65536;
  }

  bool pin(uint64_t location) override {
    if (location >= DataSize) {
      return false;
    }
    ++m_PageReferences[location / 4096];
    return true;
  }

  void unpin(uint64_t location) override {
    if (m_UnpinCount < 4) {
      m_UnpinLocations[m_UnpinCount] = location;
    }
    ++m_UnpinCount;

    size_t& references = m_PageReferences[location / 4096];
    if (!references) {
      m_BalanceError = true;
      return;
    }
    --references;
  }

  bool balanced() const {
    return !m_BalanceError && !m_PageReferences[0] && !m_PageReferences[1];
  }

  uint8_t* data() {
    return m_Data;
  }

  size_t m_ReadCount;
  size_t m_WriteCount;
  size_t m_UnpinCount;
  uint64_t m_ReadLocations[4];
  uint64_t m_WriteLocations[4];
  uint64_t m_UnpinLocations[4];

 private:
  uint8_t* m_Data;
  size_t m_PageReferences[2];
  bool m_BalanceError;
};

class FatFilesystemHarness : public FatFilesystem {
 public:
  void configure(Disk* disk) {
    m_pDisk = disk;
    m_Superblock.BPB_BytsPerSec = 512;
  }

  bool writeSectors(uint32_t sector, size_t size, uintptr_t source) {
    return writeSectorBlock(sector, size, source);
  }
};

class ShortReadFatFilesystem final : public FatFilesystem {
 public:
  ShortReadFatFilesystem() : m_ReadCalls(0) {}

  uint64_t read(File*, uint64_t, uint64_t size, uintptr_t buffer, bool = true) override {
    ++m_ReadCalls;
    ByteSet(reinterpret_cast<void*>(buffer), 0x5A, size);
    return m_ReadCalls == 1 ? size - 1 : size;
  }

  size_t m_ReadCalls;
};

bool fatSectorPageBoundary() {
  constexpr size_t TransferSize = 4096;
  uint8_t* source = new uint8_t[TransferSize];
  for (size_t i = 0; i < TransferSize; ++i) {
    source[i] = static_cast<uint8_t>((i * 37) ^ (i >> 3));
  }

  TrackingDisk disk;
  FatFilesystemHarness filesystem;
  filesystem.configure(&disk);

  const bool wrote = filesystem.writeSectors(1, TransferSize, reinterpret_cast<uintptr_t>(source));
  const bool bytesMatch = !MemoryCompare(disk.data() + 512, source, TransferSize);
  delete[] source;

  const bool splitAtPage = disk.m_ReadCount == 2 && disk.m_WriteCount == 2 &&
                           disk.m_ReadLocations[0] == 512 && disk.m_ReadLocations[1] == 4096 &&
                           disk.m_WriteLocations[0] == 512 && disk.m_WriteLocations[1] == 4096;
  const bool balanced = disk.m_UnpinCount == 2 && disk.m_UnpinLocations[0] == 512 &&
                        disk.m_UnpinLocations[1] == 4096 && disk.balanced();

  const bool passed = wrote && bytesMatch && splitAtPage && balanced;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS fat-sector-page-boundary");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL fat-sector-page-boundary: "
        "unaligned transfer crossed an unowned cache page or leaked a "
        "read reference");
  }
  return passed;
}

bool fatShortReadPublication() {
  ShortReadFatFilesystem filesystem;
  FatFile file(String("short-read"), 0, 0, 0, 1, &filesystem, 4096);

  const uintptr_t failed = file.readBlock(0);
  const uintptr_t retried = file.readBlock(0);
  if (retried) {
    file.unpinBlock(0);
  }

  const bool passed = !failed && retried && filesystem.m_ReadCalls == 2;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS fat-short-read-publication");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL fat-short-read-publication: "
        "a short read was published or its failed Editing page blocked "
        "retry");
  }
  return passed;
}
}  // namespace

EXPORTED_PUBLIC bool runHostedFatSectorRegressions() {
  return fatSectorPageBoundary() && fatShortReadPublication();
}
