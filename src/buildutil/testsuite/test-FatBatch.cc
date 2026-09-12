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
constexpr size_t FatSectors = 8;
constexpr size_t MirrorLocation = FatLocation + FatSectors * SectorSize;
constexpr size_t DirectoryLocation = 32 * SectorSize;
constexpr size_t FileLocation = 33 * SectorSize;
constexpr size_t GrowthSize = 32 * 1024;

uint32_t directorySize(const std::vector<uint8_t>& image) {
  return LITTLE_TO_HOST32(
      reinterpret_cast<const Dir*>(image.data() + DirectoryLocation)->DIR_FileSize);
}

bool zeroPayload(const std::vector<uint8_t>& image, size_t length) {
  return std::all_of(image.begin() + FileLocation, image.begin() + FileLocation + length,
                     [](uint8_t value) { return value == 0; });
}

class BatchDisk final : public Disk {
 public:
  BatchDisk() : bytes(1100 * SectorSize, 0), submitted(bytes), stored(bytes) {}

  BufferView read(uint64_t location) override {
    if (location >= bytes.size())
      return BufferView();
    if (location >= FileLocation)
      ++payloadReads;
    ++references;
    return BufferView(bytes.data() + location, SectorSize - location % SectorSize);
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
  bool writeFrom(uint64_t location, const void* buffer, size_t length) override {
    if (location > bytes.size() || length > bytes.size() - location || (length && !buffer))
      return false;
    const auto* source = static_cast<const uint8_t*>(buffer);
    std::copy_n(source, length, submitted.data() + location);
    std::copy_n(source, length, bytes.data() + location);
    ++writes;
    if (location >= FileLocation) {
      payloadBytes += length;
      payloadSinceBarrier = true;
    }
    return true;
  }
  bool writeFromBatch(WriteBuffer* buffers, size_t count) override {
    if (count > MaxWriteBuffers || (count && !buffers))
      return false;
    ++batches;
    bool hasPayload = false;
    bool succeeded = true;
    for (size_t i = 0; i < count; ++i) {
      auto& buffer = buffers[i];
      hasPayload = hasPayload || buffer.location >= FileLocation;
      buffer.complete = writeFrom(buffer.location, buffer.buffer, buffer.length);
      succeeded = buffer.complete && succeeded;
    }
    if (hasPayload)
      ++payloadBatches;
    return succeeded;
  }
  bool syncData() override {
    ++barriers;
    if (payloadSinceBarrier)
      ++payloadBarriers;
    payloadSinceBarrier = false;
    if (failBarriers)
      return false;
    stored = submitted;
    durableImages.push_back(stored);
    return true;
  }
  bool syncAll() override {
    return syncData();
  }
  void resetCounters() {
    writes = batches = payloadBatches = payloadBytes = payloadReads = 0;
    barriers = payloadBarriers = 0;
    payloadSinceBarrier = false;
    durableImages.clear();
  }

  std::vector<uint8_t> bytes;
  std::vector<uint8_t> submitted;
  std::vector<uint8_t> stored;
  std::vector<std::vector<uint8_t>> durableImages;
  size_t writes = 0;
  size_t batches = 0;
  size_t payloadBatches = 0;
  size_t payloadBytes = 0;
  size_t payloadReads = 0;
  size_t barriers = 0;
  size_t payloadBarriers = 0;
  int references = 0;
  bool failBarriers = false;
  bool payloadSinceBarrier = false;
};

class BatchFilesystem final : public FatFilesystem {
 public:
  BatchFilesystem(BatchDisk& disk, size_t size) {
    m_pDisk = &disk;
    m_Type = FAT16;
    m_BlockSize = SectorSize;
    m_ClusterCount = 1024;
    m_DataAreaStart = 32;
    m_FatSector = 1;
    m_Superblock.BPB_BytsPerSec = SectorSize;
    m_Superblock.BPB_SecPerClus = 1;
    m_Superblock.BPB_NumFATs = 2;
    m_Superblock.BPB_FATSz16 = FatSectors;
    m_FreeClusterHint = 3;
    const size_t count = std::max(size_t(1), (size + SectorSize - 1) / SectorSize);
    auto* fat = reinterpret_cast<uint16_t*>(disk.bytes.data() + FatLocation);
    fat[2] = HOST_TO_LITTLE16(0xFFF8);
    for (size_t i = 0; i < count; ++i)
      fat[3 + i] = HOST_TO_LITTLE16(i + 1 == count ? 0xFFF8 : 4 + i);
    std::copy_n(disk.bytes.data() + FatLocation, FatSectors * SectorSize,
                disk.bytes.data() + MirrorLocation);
    auto* entry = reinterpret_cast<Dir*>(disk.bytes.data() + DirectoryLocation);
    std::copy_n("BATCH   BIN", 11, entry->DIR_Name);
    entry->DIR_FstClusLO = HOST_TO_LITTLE16(3);
    entry->DIR_FileSize = HOST_TO_LITTLE32(size);
    std::fill(disk.bytes.begin() + FileLocation, disk.bytes.end(), 0xA7);
    disk.submitted = disk.stored = disk.bytes;
  }
  bool release(uint32_t cluster) {
    return releaseClusterChain(cluster);
  }
  bool flushFat() {
    return syncFat();
  }
  uint32_t entry(uint32_t cluster) {
    return getClusterEntry(cluster);
  }
};

class BatchFile final : public FatFile {
 public:
  BatchFile(FatFilesystem& filesystem, size_t size)
      : FatFile(String("batch.bin"), 0, 0, 0, 3, &filesystem, size, 2) {
    m_bForceFillCache = true;
  }
};

struct BatchFixture {
  explicit BatchFixture(size_t size) : filesystem(disk, size), file(filesystem, size) {}
  BatchDisk disk;
  BatchFilesystem filesystem;
  BatchFile file;
};
}  // namespace

TEST(FatBatch, GrowthBatchesAllocationAndZeroingBeforePublishingSize) {
  BatchFixture fixture(0);
  ASSERT_TRUE(fixture.file.resize(GrowthSize));
  EXPECT_LE(fixture.disk.barriers, 4U);
  EXPECT_EQ(fixture.disk.payloadBarriers, 1U);
  EXPECT_EQ(fixture.disk.payloadReads, 0U);
  EXPECT_EQ(fixture.disk.payloadBytes, GrowthSize);
  EXPECT_EQ(directorySize(fixture.disk.stored), GrowthSize);
  EXPECT_TRUE(zeroPayload(fixture.disk.stored, GrowthSize));
  EXPECT_TRUE(std::equal(fixture.disk.stored.begin() + FatLocation,
                         fixture.disk.stored.begin() + FatLocation + FatSectors * SectorSize,
                         fixture.disk.stored.begin() + MirrorLocation));

  bool zeroesDurableBeforeSize = false;
  for (const auto& image : fixture.disk.durableImages) {
    if (directorySize(image) == 0 && zeroPayload(image, GrowthSize))
      zeroesDurableBeforeSize = true;
    if (directorySize(image) == GrowthSize)
      EXPECT_TRUE(zeroesDurableBeforeSize);
  }
  EXPECT_TRUE(zeroesDurableBeforeSize);
  EXPECT_EQ(fixture.disk.references, 0);
}

TEST(FatBatch, EightCachedPagesShareOnePayloadDurabilityBarrier) {
  const size_t size = 8 * TargetInfo::getPageSize();
  BatchFixture fixture(size);
  std::vector<uint8_t> payload(size);
  for (size_t i = 0; i < size; ++i)
    payload[i] = static_cast<uint8_t>((i * 37) ^ (i >> 7));
  ASSERT_EQ(fixture.file.write(0, size, reinterpret_cast<uintptr_t>(payload.data())), size);
  fixture.disk.resetCounters();
  ASSERT_TRUE(fixture.file.sync());
  EXPECT_EQ(fixture.disk.payloadBarriers, 1U);
  EXPECT_LE(fixture.disk.barriers, 3U);
  EXPECT_EQ(fixture.disk.payloadBytes, size);
  const size_t clusterCount = size / SectorSize;
  EXPECT_LE(fixture.disk.payloadBatches,
            (clusterCount + Disk::MaxWriteBuffers - 1) / Disk::MaxWriteBuffers);
  EXPECT_TRUE(
      std::equal(payload.begin(), payload.end(), fixture.disk.stored.begin() + FileLocation));
  EXPECT_EQ(fixture.disk.references, 0);
}

TEST(FatBatch, FailedPayloadBarrierRetainsEveryDirtyPageForRetry) {
  const size_t size = 8 * TargetInfo::getPageSize();
  BatchFixture fixture(size);
  std::vector<uint8_t> payload(size, 0x6D);
  ASSERT_EQ(fixture.file.write(0, size, reinterpret_cast<uintptr_t>(payload.data())), size);
  fixture.disk.resetCounters();
  fixture.disk.failBarriers = true;
  EXPECT_FALSE(fixture.file.sync());
  EXPECT_EQ(fixture.disk.payloadBytes, size);
  EXPECT_TRUE(std::all_of(fixture.disk.stored.begin() + FileLocation,
                          fixture.disk.stored.begin() + FileLocation + size,
                          [](uint8_t value) { return value == 0xA7; }));
  fixture.disk.failBarriers = false;
  fixture.disk.resetCounters();
  ASSERT_TRUE(fixture.file.sync());
  EXPECT_EQ(fixture.disk.payloadBytes, size);
  EXPECT_EQ(fixture.disk.payloadBarriers, 1U);
  EXPECT_TRUE(
      std::equal(payload.begin(), payload.end(), fixture.disk.stored.begin() + FileLocation));
  EXPECT_EQ(fixture.disk.references, 0);
}

TEST(FatBatch, FailedAllocationBarrierKeepsSizeUnpublishedUntilRetry) {
  BatchFixture fixture(0);
  fixture.disk.failBarriers = true;
  EXPECT_FALSE(fixture.file.resize(GrowthSize));
  EXPECT_EQ(fixture.file.getSize(), 0U);
  EXPECT_EQ(directorySize(fixture.disk.stored), 0U);
  EXPECT_EQ(fixture.disk.payloadBytes, 0U);
  fixture.disk.failBarriers = false;
  fixture.disk.resetCounters();
  ASSERT_TRUE(fixture.file.resize(GrowthSize));
  EXPECT_LE(fixture.disk.barriers, 4U);
  EXPECT_EQ(directorySize(fixture.disk.stored), GrowthSize);
  EXPECT_TRUE(zeroPayload(fixture.disk.stored, GrowthSize));
  const auto* fat = reinterpret_cast<const uint16_t*>(fixture.disk.stored.data() + FatLocation);
  size_t allocated = 0;
  for (size_t cluster = 3; cluster < 1026; ++cluster)
    allocated += LITTLE_TO_HOST16(fat[cluster]) != 0;
  EXPECT_EQ(allocated, GrowthSize / SectorSize);
  EXPECT_EQ(fixture.disk.references, 0);
}

TEST(FatBatch, MultiSectorChainReleaseUsesOneBarrierForBothFatCopies) {
  constexpr size_t clusters = 700;
  constexpr size_t changedSectors = 3;
  BatchDisk disk;
  BatchFilesystem filesystem(disk, clusters * SectorSize);
  auto expected = disk.stored;
  for (size_t start : {FatLocation, MirrorLocation}) {
    auto* fat = reinterpret_cast<uint16_t*>(expected.data() + start);
    std::fill_n(fat + 3, clusters, uint16_t(0));
  }

  ASSERT_TRUE(filesystem.release(3));
  EXPECT_EQ(disk.barriers, 1U);
  EXPECT_EQ(disk.batches, 1U);
  EXPECT_EQ(disk.writes, 2 * changedSectors);
  EXPECT_EQ(disk.payloadReads, 0U);
  EXPECT_EQ(disk.payloadBytes, 0U);
  EXPECT_EQ(disk.stored, expected);
  EXPECT_GE(filesystem.entry(2), 0xFFF8U);
  EXPECT_EQ(disk.references, 0);

  disk.resetCounters();
  ASSERT_TRUE(filesystem.flushFat());
  EXPECT_EQ(disk.writes, 0U);
  EXPECT_EQ(disk.barriers, 0U);
}

TEST(FatBatch, FailedChainReleaseBarrierRetainsEveryStagedFreeForRetry) {
  constexpr size_t clusters = 700;
  constexpr size_t changedSectors = 3;
  BatchDisk disk;
  BatchFilesystem filesystem(disk, clusters * SectorSize);
  const auto before = disk.stored;
  auto expected = before;
  for (size_t start : {FatLocation, MirrorLocation}) {
    auto* fat = reinterpret_cast<uint16_t*>(expected.data() + start);
    std::fill_n(fat + 3, clusters, uint16_t(0));
  }

  disk.failBarriers = true;
  EXPECT_FALSE(filesystem.release(3));
  EXPECT_EQ(disk.barriers, 1U);
  EXPECT_EQ(disk.writes, 2 * changedSectors);
  EXPECT_EQ(disk.stored, before);
  EXPECT_EQ(disk.submitted, expected);
  for (uint32_t cluster = 3; cluster < 3 + clusters; ++cluster)
    EXPECT_EQ(filesystem.entry(cluster), 0U) << cluster;

  disk.failBarriers = false;
  disk.resetCounters();
  ASSERT_TRUE(filesystem.flushFat());
  EXPECT_EQ(disk.barriers, 1U);
  EXPECT_EQ(disk.batches, 1U);
  EXPECT_EQ(disk.writes, 2 * changedSectors);
  EXPECT_EQ(disk.payloadReads, 0U);
  EXPECT_EQ(disk.payloadBytes, 0U);
  EXPECT_EQ(disk.stored, expected);
  EXPECT_EQ(disk.references, 0);
}

TEST(FatBatch, ZeroShrinkBatchesWholeChainReleaseAfterClearingDirectoryStart) {
  constexpr size_t clusters = 512;
  BatchFixture fixture(clusters * SectorSize);
  fixture.disk.resetCounters();
  ASSERT_TRUE(fixture.file.resize(0));
  ASSERT_TRUE(fixture.file.sync());
  EXPECT_LE(fixture.disk.barriers, 4U);
  EXPECT_EQ(fixture.disk.payloadBytes, 0U);
  EXPECT_EQ(fixture.disk.payloadReads, 0U);
  EXPECT_EQ(fixture.file.getAttributes().blocks, 0U);
  const auto* entry = reinterpret_cast<const Dir*>(fixture.disk.stored.data() + DirectoryLocation);
  EXPECT_EQ(LITTLE_TO_HOST32(entry->DIR_FileSize), 0U);
  EXPECT_EQ(LITTLE_TO_HOST16(entry->DIR_FstClusLO), 0U);
  EXPECT_EQ(LITTLE_TO_HOST16(entry->DIR_FstClusHI), 0U);
  for (size_t start : {FatLocation, MirrorLocation}) {
    const auto* fat = reinterpret_cast<const uint16_t*>(fixture.disk.stored.data() + start);
    EXPECT_TRUE(
        std::all_of(fat + 3, fat + 3 + clusters, [](uint16_t value) { return value == 0; }));
    EXPECT_GE(LITTLE_TO_HOST16(fat[2]), 0xFFF8U);
  }
  for (const auto& image : fixture.disk.durableImages) {
    const auto* fat = reinterpret_cast<const uint16_t*>(image.data() + FatLocation);
    if (fat[3] == 0) {
      const auto* directory = reinterpret_cast<const Dir*>(image.data() + DirectoryLocation);
      EXPECT_EQ(LITTLE_TO_HOST32(directory->DIR_FileSize), 0U);
      EXPECT_EQ(LITTLE_TO_HOST16(directory->DIR_FstClusLO), 0U);
      EXPECT_EQ(LITTLE_TO_HOST16(directory->DIR_FstClusHI), 0U);
    }
  }
  EXPECT_TRUE(std::all_of(fixture.disk.stored.begin() + FileLocation,
                          fixture.disk.stored.begin() + FileLocation + clusters * SectorSize,
                          [](uint8_t value) { return value == 0xA7; }));
  EXPECT_EQ(fixture.disk.references, 0);
}
