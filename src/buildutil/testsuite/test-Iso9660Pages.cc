/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/machine/Disk.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "modules/system/iso9660/Iso9660File.h"
#include "modules/system/iso9660/Iso9660Filesystem.h"
#include <gtest/gtest.h>

namespace {
constexpr size_t Page = TargetInfo::getPageSize();
constexpr uint32_t Extent = 3;
constexpr uint64_t DataOffset = Extent * 2048ULL;

class IsoDisk final : public Disk {
 public:
  struct Read {
    uint64_t location;
    size_t length;
  };

  IsoDisk() : bytes(16 * Page) {
    for (size_t i = 0; i < bytes.size(); ++i)
      bytes[i] = static_cast<uint8_t>(i * 17 + i / 19);
  }

  bool readInto(uint64_t location, void* buffer, size_t length) override {
    if (!buffer || location > bytes.size() || length > bytes.size() - location)
      return false;
    producerReads.push_back({location, length});
    if (location == failedRead) {
      std::copy_n(bytes.data() + location, length / 2, static_cast<uint8_t*>(buffer));
      return false;
    }
    std::copy_n(bytes.data() + location, length, static_cast<uint8_t*>(buffer));
    return true;
  }

  BufferView read(uint64_t location) override {
    legacyReads.push_back(location);
    if (rejectLegacyReads || location >= bytes.size())
      return BufferView();
    ++references;
    return BufferView(bytes.data() + location, Page - location % Page);
  }

  bool pin(uint64_t location) override {
    if (location >= bytes.size())
      return false;
    ++references;
    return true;
  }

  void unpin(uint64_t location) override {
    EXPECT_GT(references, 0U);
    if (references)
      --references;
    unpins.push_back(location);
  }

  void write(uint64_t) override {
    ++writes;
  }

  bool writeFrom(uint64_t, const void*, size_t) override {
    ++writes;
    return false;
  }

  size_t getSize() const override {
    return bytes.size();
  }

  std::vector<uint8_t> bytes;
  std::vector<Read> producerReads;
  std::vector<uint64_t> legacyReads;
  std::vector<uint64_t> unpins;
  uint64_t failedRead = ~uint64_t(0);
  size_t references = 0;
  size_t writes = 0;
  bool rejectLegacyReads = true;
};

class IsoFilesystem final : public Iso9660Filesystem {
 public:
  explicit IsoFilesystem(Disk& disk) {
    m_pDisk = &disk;
    m_BlockSize = 2048;
    m_bReadOnly = true;
  }
};

class IsoFile final : public Iso9660File {
 public:
  IsoFile(IsoFilesystem& filesystem, Iso9660DirRecord& record, bool forceFill = true)
      : Iso9660File(String("extent"), 0, 0, 0, 1, &filesystem, LITTLE_TO_HOST32(record.DataLen_LE),
                    record) {
    m_bForceFillCache = forceFill;
  }

  bool filled() const {
    return useFillCache();
  }

  bool cached(size_t offset) {
    return cacheState().fill.exists(offset, Page);
  }

  bool evict(size_t offset) {
    return cacheState().fill.evict(offset);
  }

  uintptr_t indexed(size_t block) {
    auto result = cacheState().data.lookup(DataCacheKey(block));
    return result.hasValue() ? result.value() : FILE_BAD_BLOCK;
  }

  bool zeroTail(size_t offset, size_t firstZero) {
    const uintptr_t location = cacheState().fill.lookup(offset);
    if (!location)
      return false;
    const auto* bytes = reinterpret_cast<const uint8_t*>(location);
    const bool zeroed =
        std::all_of(bytes + firstZero, bytes + Page, [](uint8_t byte) { return byte == 0; });
    cacheState().fill.release(offset);
    return zeroed;
  }

  bool syncCache() {
    cacheState().fill.timer(CACHE_WRITEBACK_PERIOD * 1000000ULL);
    return cacheState().fill.syncAll();
  }
};

Iso9660DirRecord recordFor(size_t size) {
  Iso9660DirRecord record = {};
  record.ExtentLocation_LE = HOST_TO_LITTLE32(Extent);
  record.DataLen_LE = HOST_TO_LITTLE32(size);
  return record;
}
}  // namespace

TEST(Iso9660Pages, ForcedFillUsesProducerBuffersAndZeroesOnlyTheEofTail) {
  IsoDisk disk;
  IsoFilesystem filesystem(disk);
  auto record = recordFor(2 * Page + 37);
  IsoFile file(filesystem, record);
  ASSERT_TRUE(file.filled());
  std::vector<uint8_t> output(file.getSize(), 0xcc);
  ASSERT_EQ(file.read(0, output.size(), reinterpret_cast<uintptr_t>(output.data())), output.size());
  EXPECT_TRUE(std::equal(output.begin(), output.end(), disk.bytes.begin() + DataOffset));
  ASSERT_EQ(disk.producerReads.size(), 3U);
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(disk.producerReads[i].location, DataOffset + i * Page);
    EXPECT_EQ(disk.producerReads[i].length, i == 2 ? 37U : Page);
  }
  EXPECT_TRUE(file.zeroTail(2 * Page, 37));
  ASSERT_EQ(file.read(0, Page, reinterpret_cast<uintptr_t>(output.data())), Page);
  EXPECT_EQ(disk.producerReads.size(), 3U);
  EXPECT_TRUE(disk.legacyReads.empty());
  EXPECT_EQ(disk.references, 0U);
  EXPECT_EQ(disk.writes, 0U);
}

TEST(Iso9660Pages, FailedProducerFillIsUnpublishedAndRetriesWithoutLosingThePrefix) {
  IsoDisk disk;
  IsoFilesystem filesystem(disk);
  auto record = recordFor(2 * Page);
  IsoFile file(filesystem, record);
  disk.failedRead = DataOffset + Page;
  std::vector<uint8_t> output(2 * Page, 0xcc);
  EXPECT_EQ(file.read(0, output.size(), reinterpret_cast<uintptr_t>(output.data())), Page);
  EXPECT_TRUE(file.cached(0));
  EXPECT_FALSE(file.cached(Page));
  EXPECT_EQ(file.indexed(1), FILE_BAD_BLOCK);
  EXPECT_TRUE(std::equal(output.begin(), output.begin() + Page, disk.bytes.begin() + DataOffset));
  EXPECT_TRUE(
      std::all_of(output.begin() + Page, output.end(), [](uint8_t byte) { return byte == 0xcc; }));
  disk.failedRead = ~uint64_t(0);
  ASSERT_EQ(file.read(Page, Page, reinterpret_cast<uintptr_t>(output.data() + Page)), Page);
  EXPECT_TRUE(std::equal(output.begin(), output.end(), disk.bytes.begin() + DataOffset));
  EXPECT_TRUE(file.cached(Page));
  EXPECT_EQ(disk.producerReads.size(), 4U);
  EXPECT_TRUE(disk.legacyReads.empty());
}

TEST(Iso9660Pages, CleanPagesRetireTheirIndexAndRejectDirectWrites) {
  IsoDisk disk;
  IsoFilesystem filesystem(disk);
  auto record = recordFor(Page);
  IsoFile file(filesystem, record);
  uint8_t value = 0;
  ASSERT_EQ(file.read(0, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  ASSERT_NE(file.indexed(0), FILE_BAD_BLOCK);
  const uint8_t replacement = value ^ 0xff;
  EXPECT_EQ(file.write(0, 1, reinterpret_cast<uintptr_t>(&replacement)), 0U);
  EXPECT_EQ(file.write(Page, 1, reinterpret_cast<uintptr_t>(&replacement)), 0U);
  EXPECT_EQ(file.getSize(), Page);
  EXPECT_TRUE(file.syncCache());
  EXPECT_EQ(disk.writes, 0U);
  ASSERT_TRUE(file.evict(0));
  EXPECT_EQ(file.indexed(0), FILE_BAD_BLOCK);
  ASSERT_EQ(file.read(0, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  EXPECT_EQ(value, disk.bytes[DataOffset]);
  EXPECT_EQ(disk.producerReads.size(), 2U);
  EXPECT_TRUE(disk.legacyReads.empty());
}

#if VFS_NOMMU
TEST(Iso9660Pages, NonMmuReadsKeepTheLegacyViewAndBalancedReferenceContract) {
  IsoDisk disk;
  disk.rejectLegacyReads = false;
  IsoFilesystem filesystem(disk);
  auto record = recordFor(Page);
  IsoFile file(filesystem, record, false);
  ASSERT_FALSE(file.filled());
  std::vector<uint8_t> output(Page);
  ASSERT_EQ(file.read(0, output.size(), reinterpret_cast<uintptr_t>(output.data())), output.size());
  EXPECT_TRUE(std::equal(output.begin(), output.end(), disk.bytes.begin() + DataOffset));
  EXPECT_TRUE(disk.producerReads.empty());
  EXPECT_EQ(disk.legacyReads, disk.unpins);
  EXPECT_EQ(disk.references, 0U);
  EXPECT_EQ(disk.writes, 0U);
}
#endif
