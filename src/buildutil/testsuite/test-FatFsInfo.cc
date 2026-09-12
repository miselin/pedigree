/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/machine/Disk.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "modules/system/fat/FatFilesystem.h"
#include <gtest/gtest.h>

namespace {
class FsInfoDisk final : public Disk {
 public:
  struct Event {
    bool barrier;
    uint64_t location;
  };
  explicit FsInfoDisk(size_t sectorSize = 512)
      : sectorSize(sectorSize), bytes(64 * sectorSize, 0), transferred(bytes), stored(bytes) {}
  BufferView read(uint64_t location) override {
    if (location >= bytes.size() || location == failedRead)
      return BufferView();
    ++references;
    return BufferView(bytes.data() + location, 512 - location % 512);
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
  bool writeFrom(uint64_t location, const void* source, size_t length) override {
    events.push_back({false, location});
    if (location > bytes.size() || length > bytes.size() - location)
      return false;
    std::memcpy(bytes.data() + location, source, length);
    std::memcpy(transferred.data() + location, source, length);
    // Failed transfers may have already updated both producer and device caches.
    return location != failedWrite;
  }
  bool syncData() override {
    events.push_back({true, 0});
    if (++barriers == failedBarrier)
      return false;
    stored = transferred;
    return true;
  }
  size_t getSize() const override {
    return bytes.size();
  }
  size_t getBlockSize() const override {
    return 512;
  }
  void seed(size_t sector) {
    uint8_t* data = bytes.data() + sector * sectorSize;
    for (size_t i = 0; i < sectorSize; ++i)
      data[i] = static_cast<uint8_t>(0x35 + i * 17);
    auto* info = reinterpret_cast<FSInfo32*>(data);
    info->FSI_LeadSig = HOST_TO_LITTLE32(0x41615252);
    info->FSI_StrucSig = HOST_TO_LITTLE32(0x61417272);
    info->FSI_TrailSig = HOST_TO_LITTLE32(0xAA550000);
    info->FSI_Free_Count = HOST_TO_LITTLE32(1234);
    info->FSI_NxtFree = HOST_TO_LITTLE32(19);
    transferred = stored = bytes;
  }
  size_t writesAt(size_t sector) const {
    return std::count_if(events.begin(), events.end(), [&](const Event& event) {
      return !event.barrier && event.location == sector * sectorSize;
    });
  }
  void expectInvalidated(size_t sector, const std::vector<uint8_t>& before) const {
    std::vector<uint8_t> expected(before.begin() + sector * sectorSize,
                                  before.begin() + (sector + 1) * sectorSize);
    auto* info = reinterpret_cast<FSInfo32*>(expected.data());
    info->FSI_Free_Count = HOST_TO_LITTLE32(UINT32_MAX);
    info->FSI_NxtFree = HOST_TO_LITTLE32(UINT32_MAX);
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), stored.begin() + sector * sectorSize));
  }
  size_t sectorSize;
  std::vector<uint8_t> bytes, transferred, stored;
  std::vector<Event> events;
  uint64_t failedRead = UINT64_MAX, failedWrite = UINT64_MAX;
  size_t barriers = 0, failedBarrier = ~size_t(0);
  int references = 0;
};

class FsInfoFilesystem final : public FatFilesystem {
 public:
  explicit FsInfoFilesystem(FsInfoDisk& disk, FatType type = FAT32) {
    m_pDisk = &disk;
    m_Type = type;
    m_BlockSize = disk.sectorSize;
    m_ClusterCount = 16;
    m_FatSector = 32;
    m_DataAreaStart = 34;
    m_Superblock.BPB_BytsPerSec = HOST_TO_LITTLE16(disk.sectorSize);
    m_Superblock.BPB_RsvdSecCnt = HOST_TO_LITTLE16(32);
    m_Superblock.BPB_SecPerClus = 1;
    m_Superblock.BPB_NumFATs = 2;
    m_Superblock.BPB_FATSz16 = HOST_TO_LITTLE16(1);
    m_Superblock32.BPB_FATSz32 = HOST_TO_LITTLE32(1);
    m_Superblock32.BPB_FsInfo = HOST_TO_LITTLE16(1);
    m_Superblock32.BPB_BkBootSec = HOST_TO_LITTLE16(6);
    disk.seed(1);
    disk.seed(7);
  }
  bool stage(uint32_t cluster = 3) {
    return setClusterEntry(cluster, eofValue(), true, false);
  }
  bool flush() {
    return syncFat();
  }
  bool invalidated() const {
    return m_FsInfoInvalidated;
  }
  void primary(uint16_t sector) {
    m_Superblock32.BPB_FsInfo = HOST_TO_LITTLE16(sector);
  }
  void backup(uint16_t sector) {
    m_Superblock32.BPB_BkBootSec = HOST_TO_LITTLE16(sector);
  }
};
}  // namespace

TEST(FatFsInfo, InvalidatesBothCopiesDurablyBeforeFirstFatWriteAndOnlyOnce) {
  for (size_t sectorSize : {512, 1024}) {
    FsInfoDisk disk(sectorSize);
    FsInfoFilesystem fs(disk);
    const auto before = disk.stored;
    ASSERT_TRUE(fs.flush());
    EXPECT_TRUE(disk.events.empty());
    ASSERT_TRUE(fs.stage());
    EXPECT_TRUE(disk.events.empty());
    ASSERT_TRUE(fs.flush());
    ASSERT_GE(disk.events.size(), 7U);
    EXPECT_FALSE(disk.events[0].barrier);
    EXPECT_EQ(disk.events[0].location, sectorSize);
    EXPECT_TRUE(disk.events[1].barrier);
    EXPECT_FALSE(disk.events[2].barrier);
    EXPECT_EQ(disk.events[2].location, 7 * sectorSize);
    EXPECT_TRUE(disk.events[3].barrier);
    EXPECT_FALSE(disk.events[4].barrier);
    EXPECT_EQ(disk.events[4].location, 32 * sectorSize);
    disk.expectInvalidated(1, before);
    disk.expectInvalidated(7, before);
    EXPECT_TRUE(fs.invalidated());
    ASSERT_TRUE(fs.stage(4));
    ASSERT_TRUE(fs.flush());
    EXPECT_EQ(disk.writesAt(1), 1U);
    EXPECT_EQ(disk.writesAt(7), 1U);
    EXPECT_EQ(disk.references, 0);
  }
}

TEST(FatFsInfo, FailedConfiguredCopyReadBlocksFatWritesAndRetries) {
  for (size_t sector : {1, 7}) {
    FsInfoDisk disk;
    FsInfoFilesystem fs(disk);
    const auto before = disk.stored;
    ASSERT_TRUE(fs.stage());
    disk.failedRead = sector * disk.sectorSize;
    EXPECT_FALSE(fs.flush());
    EXPECT_FALSE(fs.invalidated());
    EXPECT_EQ(disk.writesAt(32), 0U);
    EXPECT_EQ(disk.writesAt(33), 0U);
    disk.failedRead = UINT64_MAX;
    ASSERT_TRUE(fs.flush());
    EXPECT_TRUE(fs.invalidated());
    disk.expectInvalidated(1, before);
    disk.expectInvalidated(7, before);
    EXPECT_EQ(disk.references, 0);
  }
}

TEST(FatFsInfo, FailedTransferRewritesAlreadyUnknownCachedHintsOnRetry) {
  for (size_t sector : {1, 7}) {
    FsInfoDisk disk;
    FsInfoFilesystem fs(disk);
    const auto before = disk.stored;
    ASSERT_TRUE(fs.stage());
    disk.failedWrite = sector * disk.sectorSize;
    EXPECT_FALSE(fs.flush());
    EXPECT_FALSE(fs.invalidated());
    EXPECT_EQ(disk.writesAt(32), 0U);
    disk.failedWrite = UINT64_MAX;
    ASSERT_TRUE(fs.flush());
    EXPECT_EQ(disk.writesAt(sector), 2U);
    disk.expectInvalidated(1, before);
    disk.expectInvalidated(7, before);
  }
}

TEST(FatFsInfo, FailedBarrierRetainsInvalidationForRetryBeforeFatPersistence) {
  for (size_t barrier : {1, 2}) {
    FsInfoDisk disk;
    FsInfoFilesystem fs(disk);
    const auto before = disk.stored;
    ASSERT_TRUE(fs.stage());
    disk.failedBarrier = barrier;
    EXPECT_FALSE(fs.flush());
    EXPECT_FALSE(fs.invalidated());
    EXPECT_EQ(disk.writesAt(32), 0U);
    disk.failedBarrier = ~size_t(0);
    ASSERT_TRUE(fs.flush());
    EXPECT_TRUE(fs.invalidated());
    EXPECT_EQ(disk.writesAt(1), 2U);
    disk.expectInvalidated(1, before);
    disk.expectInvalidated(7, before);
  }
}

TEST(FatFsInfo, InvalidSignaturesArePreservedWhileValidBackupIsInvalidated) {
  FsInfoDisk disk;
  FsInfoFilesystem fs(disk);
  disk.bytes[disk.sectorSize] ^= 1;
  disk.transferred = disk.stored = disk.bytes;
  const auto before = disk.stored;
  ASSERT_TRUE(fs.stage());
  ASSERT_TRUE(fs.flush());
  EXPECT_EQ(disk.writesAt(1), 0U);
  EXPECT_TRUE(std::equal(before.begin() + disk.sectorSize, before.begin() + 2 * disk.sectorSize,
                         disk.stored.begin() + disk.sectorSize));
  disk.expectInvalidated(7, before);
}

TEST(FatFsInfo, MissingFsInfoAndOlderFatFormatsDoNotRewriteReservedSectors) {
  for (FatType type : {FAT12, FAT16, FAT32}) {
    FsInfoDisk disk;
    FsInfoFilesystem fs(disk, type);
    if (type == FAT32)
      fs.primary(0xFFFF);
    const auto before = disk.stored;
    ASSERT_TRUE(fs.stage());
    ASSERT_TRUE(fs.flush());
    EXPECT_EQ(disk.writesAt(1), 0U);
    EXPECT_EQ(disk.writesAt(7), 0U);
    EXPECT_TRUE(
        std::equal(before.begin(), before.begin() + 32 * disk.sectorSize, disk.stored.begin()));
  }
}

TEST(FatFsInfo, AbsentBackupDoesNotReadOrWriteAnUnconfiguredCopy) {
  FsInfoDisk disk;
  FsInfoFilesystem fs(disk);
  fs.backup(0);
  disk.failedRead = 7 * disk.sectorSize;
  ASSERT_TRUE(fs.stage());
  ASSERT_TRUE(fs.flush());
  EXPECT_TRUE(fs.invalidated());
  EXPECT_EQ(disk.writesAt(1), 1U);
  EXPECT_EQ(disk.writesAt(7), 0U);
}
