/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/machine/Disk.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "modules/system/fat/FatFilesystem.h"
#include <gtest/gtest.h>

namespace {
constexpr size_t SectorSize = 512;
constexpr size_t FatLocation = SectorSize;
constexpr size_t DirectoryLocation = 8 * SectorSize;
constexpr size_t FileLocation = 9 * SectorSize;

class ResizeDisk final : public Disk {
 public:
  ResizeDisk() : bytes(256 * SectorSize, 0), stored(bytes) {}

  BufferView read(uint64_t location) override {
    if (location >= bytes.size() || location == failedRead)
      return BufferView();
    ++references;
    return BufferView(bytes.data() + location, SectorSize - location % SectorSize);
  }
  void write(uint64_t) override {}
  bool sync(uint64_t location, bool) override {
    if (location == failedSync || (location == FatLocation && fatSyncs++ >= failFatAfter))
      return false;
    const size_t start = location - location % SectorSize;
    std::copy_n(bytes.data() + start, SectorSize, stored.data() + start);
    return true;
  }
  bool pin(uint64_t location) override {
    if (location >= bytes.size())
      return false;
    ++references;
    return true;
  }
  void unpin(uint64_t) override {
    --references;
  }
  size_t getSize() const override {
    return bytes.size();
  }
  size_t getBlockSize() const override {
    return SectorSize;
  }
  uint32_t storedSize() const {
    return LITTLE_TO_HOST32(
        reinterpret_cast<const Dir*>(stored.data() + DirectoryLocation)->DIR_FileSize);
  }
  uint32_t storedCluster() const {
    const Dir* entry = reinterpret_cast<const Dir*>(stored.data() + DirectoryLocation);
    return LITTLE_TO_HOST16(entry->DIR_FstClusLO) |
           (uint32_t(LITTLE_TO_HOST16(entry->DIR_FstClusHI)) << 16);
  }

  std::vector<uint8_t> bytes;
  std::vector<uint8_t> stored;
  uint64_t failedRead = UINT64_MAX;
  uint64_t failedSync = UINT64_MAX;
  size_t fatSyncs = 0;
  size_t failFatAfter = ~size_t(0);
  int references = 0;
};

class ResizeFilesystem final : public FatFilesystem {
 public:
  void configure(ResizeDisk& disk, size_t size) {
    m_pDisk = &disk;
    m_Type = FAT16;
    m_BlockSize = SectorSize;
    m_ClusterCount = 220;
    m_DataAreaStart = 8;
    m_FatSector = 1;
    m_Superblock.BPB_BytsPerSec = SectorSize;
    m_Superblock.BPB_SecPerClus = 1;
    m_Superblock.BPB_NumFATs = 1;
    m_Superblock.BPB_FATSz16 = 1;
    m_FreeClusterHint = 3;
    const size_t count = (size + SectorSize - 1) / SectorSize;
    auto* fat = reinterpret_cast<uint16_t*>(disk.bytes.data() + FatLocation);
    fat[2] = HOST_TO_LITTLE16(0xFFF8);
    for (size_t i = 0; i < count; ++i)
      fat[3 + i] = HOST_TO_LITTLE16(i + 1 == count ? 0xFFF8 : 4 + i);
    auto* entry = reinterpret_cast<Dir*>(disk.bytes.data() + DirectoryLocation);
    entry->DIR_FstClusLO = HOST_TO_LITTLE16(3);
    entry->DIR_FileSize = HOST_TO_LITTLE32(size);
    std::fill_n(disk.bytes.data() + FileLocation, count * SectorSize, 0xA7);
    disk.stored = disk.bytes;
  }
  uint32_t entry(uint32_t cluster) {
    return getClusterEntry(cluster);
  }
  uint32_t reserve() {
    return findFreeCluster();
  }
  void detach(File* file) {
    unlinkNode(file);
  }
};

class ResizeFile final : public FatFile {
 public:
  ResizeFile(FatFilesystem& filesystem, size_t size)
      : FatFile(String("resize"), 0, 0, 0, 3, &filesystem, size, 2) {
    m_bForceFillCache = true;
  }
  uintptr_t page(size_t offset) {
    return cacheState().fill.lookup(offset);
  }
  void release(size_t offset) {
    cacheState().fill.release(offset);
  }
  bool resident(size_t offset) {
    return cacheState().fill.exists(offset, TargetInfo::getPageSize());
  }
};

struct ResizeFixture {
  explicit ResizeFixture(size_t size) : file(filesystem, size) {
    filesystem.configure(disk, size);
  }
  ResizeDisk disk;
  ResizeFilesystem filesystem;
  ResizeFile file;
};
}  // namespace

TEST(FatResize, PartialShrinkPreservesDirtyPrefixAndZeroesRegrowth) {
  const size_t pageSize = TargetInfo::getPageSize();
  ResizeFixture fixture(3 * pageSize);
  uint8_t value = 0;
  ASSERT_EQ(fixture.file.read(0, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  ASSERT_EQ(fixture.file.read(2 * pageSize, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  const uintptr_t page = fixture.file.page(0);
  ASSERT_NE(page, 0U);
  fixture.file.markPageExternallyWritable(0);
  reinterpret_cast<uint8_t*>(page)[7] = 0x52;
  fixture.file.release(0);
  ASSERT_TRUE(fixture.file.resize(SectorSize + 17));
  EXPECT_FALSE(fixture.file.resident(2 * pageSize));
  ASSERT_TRUE(fixture.file.resize(pageSize + 31));
  ASSERT_TRUE(fixture.file.sync());
  std::vector<uint8_t> bytes(pageSize + 31);
  ASSERT_EQ(fixture.file.read(0, bytes.size(), reinterpret_cast<uintptr_t>(bytes.data())),
            bytes.size());
  EXPECT_EQ(bytes[7], 0x52);
  EXPECT_TRUE(std::all_of(bytes.begin() + SectorSize + 17, bytes.end(),
                          [](uint8_t value) { return value == 0; }));
  EXPECT_TRUE(std::all_of(fixture.disk.stored.begin() + FileLocation + SectorSize + 17,
                          fixture.disk.stored.begin() + FileLocation + bytes.size(),
                          [](uint8_t value) { return value == 0; }));
  EXPECT_EQ(fixture.disk.references, 0);
}

TEST(FatResize, PreparationFailurePreservesResidentSuffixAndSize) {
  const size_t pageSize = TargetInfo::getPageSize();
  ResizeFixture fixture(3 * pageSize);
  uint8_t value = 0x43;
  ASSERT_EQ(fixture.file.write(2 * pageSize + 9, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  fixture.disk.failedRead = DirectoryLocation;
  EXPECT_FALSE(fixture.file.resize(17));
  EXPECT_EQ(fixture.file.getSize(), 3 * pageSize);
  EXPECT_TRUE(fixture.file.resident(2 * pageSize));
  value = 0;
  EXPECT_EQ(fixture.file.read(2 * pageSize + 9, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  EXPECT_EQ(value, 0x43);
  fixture.disk.failedRead = UINT64_MAX;
  EXPECT_TRUE(fixture.file.sync());
  EXPECT_EQ(fixture.disk.references, 0);
}

TEST(FatResize, DirectoryFailureRetainsAllocationUntilMetadataPersists) {
  ResizeFixture fixture(3 * SectorSize);
  ASSERT_TRUE(fixture.file.resize(17));
  fixture.disk.failedSync = DirectoryLocation;
  EXPECT_FALSE(fixture.file.sync());
  EXPECT_EQ(fixture.file.getSize(), 17U);
  EXPECT_EQ(fixture.disk.storedSize(), 3 * SectorSize);
  EXPECT_EQ(fixture.filesystem.entry(3), 4U);
  EXPECT_EQ(fixture.filesystem.entry(4), 5U);
  fixture.disk.failedSync = UINT64_MAX;
  ASSERT_TRUE(fixture.file.sync());
  EXPECT_EQ(fixture.disk.storedSize(), 17U);
  EXPECT_GE(fixture.filesystem.entry(3), 0xFFF8U);
  EXPECT_EQ(fixture.filesystem.entry(4), 0U);
  EXPECT_EQ(fixture.filesystem.entry(5), 0U);
}

TEST(FatResize, FailedDetachmentPreservesSuffixOwnershipForRetry) {
  ResizeFixture fixture(3 * SectorSize);
  ASSERT_TRUE(fixture.file.resize(SectorSize));
  fixture.disk.failedSync = FatLocation;
  EXPECT_FALSE(fixture.file.sync());
  EXPECT_EQ(fixture.disk.storedSize(), SectorSize);
  EXPECT_EQ(fixture.filesystem.entry(3), 4U);
  EXPECT_EQ(fixture.filesystem.entry(4), 5U);
  fixture.disk.failedSync = UINT64_MAX;
  ASSERT_TRUE(fixture.file.sync());
  EXPECT_GE(fixture.filesystem.entry(3), 0xFFF8U);
  EXPECT_EQ(fixture.filesystem.entry(4), 0U);
  EXPECT_EQ(fixture.filesystem.entry(5), 0U);
}

TEST(FatResize, FailedFreeRetryCannotFreeAReusedCluster) {
  ResizeFixture fixture(3 * SectorSize);
  ASSERT_TRUE(fixture.file.resize(SectorSize));
  fixture.disk.failFatAfter = 1;
  EXPECT_FALSE(fixture.file.sync());
  EXPECT_GE(fixture.filesystem.entry(3), 0xFFF8U);
  EXPECT_EQ(fixture.filesystem.entry(4), 0U);
  EXPECT_EQ(fixture.filesystem.entry(5), 0U);
  fixture.disk.failFatAfter = ~size_t(0);
  const uint32_t reused = fixture.filesystem.reserve();
  ASSERT_TRUE(reused == 4 || reused == 5);
  ASSERT_TRUE(fixture.file.sync());
  EXPECT_GE(fixture.filesystem.entry(reused), 0xFFF8U);
}

TEST(FatResize, VirtualTruncateUsesPreparedZeroShrink) {
  ResizeFixture fixture(3 * SectorSize);
  File& file = fixture.file;
  file.truncate();
  EXPECT_EQ(file.getSize(), 0U);
  ASSERT_TRUE(file.sync());
  EXPECT_EQ(fixture.disk.storedSize(), 0U);
  EXPECT_EQ(fixture.disk.storedCluster(), 0U);
  EXPECT_EQ(file.getInode(), 0U);
  EXPECT_EQ(file.getAttributes().blocks, 0U);
  EXPECT_EQ(fixture.filesystem.entry(3), 0U);
  EXPECT_EQ(fixture.filesystem.entry(4), 0U);
  EXPECT_EQ(fixture.filesystem.entry(5), 0U);
}

TEST(FatResize, ZeroShrinkKeepsChainUntilDirectoryDetachmentPersists) {
  ResizeFixture fixture(3 * SectorSize);
  ASSERT_TRUE(fixture.file.resize(0));
  fixture.disk.failedSync = DirectoryLocation;
  EXPECT_FALSE(fixture.file.sync());
  EXPECT_EQ(fixture.disk.storedCluster(), 3U);
  EXPECT_EQ(fixture.file.getInode(), 3U);
  EXPECT_EQ(fixture.filesystem.entry(3), 4U);
  fixture.disk.failedSync = UINT64_MAX;
  ASSERT_TRUE(fixture.file.sync());
  EXPECT_EQ(fixture.disk.storedCluster(), 0U);
  EXPECT_EQ(fixture.file.getAttributes().blocks, 0U);
  EXPECT_EQ(fixture.filesystem.entry(3), 0U);
}

TEST(FatResize, FailedZeroShrinkFreeCannotReclaimAReusedFirstCluster) {
  ResizeFixture fixture(3 * SectorSize);
  ASSERT_TRUE(fixture.file.resize(0));
  fixture.disk.failedSync = FatLocation;
  EXPECT_FALSE(fixture.file.sync());
  EXPECT_EQ(fixture.disk.storedSize(), 0U);
  EXPECT_EQ(fixture.disk.storedCluster(), 0U);
  EXPECT_EQ(fixture.file.getInode(), 0U);
  EXPECT_EQ(fixture.filesystem.entry(3), 0U);
  EXPECT_EQ(fixture.filesystem.entry(4), 0U);
  EXPECT_EQ(fixture.filesystem.entry(5), 0U);
  fixture.disk.failedSync = UINT64_MAX;
  ASSERT_EQ(fixture.filesystem.reserve(), 3U);
  ASSERT_TRUE(fixture.file.sync());
  EXPECT_GE(fixture.filesystem.entry(3), 0xFFF8U);
  EXPECT_EQ(fixture.disk.storedCluster(), 0U);
  EXPECT_EQ(fixture.file.getAttributes().blocks, 0U);
}

TEST(FatResize, OpenUnlinkedFileShrinksWithoutReadingDeletedDirectoryEntry) {
  ResizeFixture fixture(3 * SectorSize);
  fixture.filesystem.detach(&fixture.file);
  fixture.disk.bytes[DirectoryLocation] = 0xE5;
  fixture.disk.stored[DirectoryLocation] = 0xE5;
  const auto directoryBefore = fixture.disk.stored;
  fixture.disk.failedRead = DirectoryLocation;
  ASSERT_TRUE(fixture.file.resize(17));
  ASSERT_TRUE(fixture.file.resize(0));
  ASSERT_TRUE(fixture.file.sync());
  EXPECT_EQ(fixture.file.getInode(), 0U);
  EXPECT_EQ(fixture.file.getAttributes().blocks, 0U);
  EXPECT_EQ(fixture.filesystem.entry(3), 0U);
  EXPECT_TRUE(std::equal(directoryBefore.begin() + DirectoryLocation,
                         directoryBefore.begin() + DirectoryLocation + SectorSize,
                         fixture.disk.stored.begin() + DirectoryLocation));
}

TEST(FatResize, GrowthPastFormatLimitPreservesFile) {
  ResizeFixture fixture(SectorSize);
  EXPECT_FALSE(fixture.file.resize(uint64_t(UINT32_MAX) + 1));
  EXPECT_EQ(fixture.file.getSize(), SectorSize);
  EXPECT_EQ(fixture.disk.storedSize(), SectorSize);
}
