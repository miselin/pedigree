/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"

#include <algorithm>
#include <vector>

#include "modules/system/vfs/File.h"
#include <gtest/gtest.h>

namespace {
class RangeSyncFile final : public File {
 public:
  explicit RangeSyncFile(size_t pageCount = 3)
      : pageSize(PhysicalMemoryManager::getPageSize()),
        bytes(pageCount * pageSize, 0),
        persisted(pageCount * pageSize, 0) {
    setSize(bytes.size());
  }

  using File::sync;

  bool sync(size_t offset, bool async) override {
    EXPECT_FALSE(async);
    EXPECT_GT(pins, 0U);
    synced.push_back(offset);
    if (offset == failedOffset)
      return false;
    std::copy_n(bytes.begin() + offset, pageSize, persisted.begin() + offset);
    return true;
  }

  bool syncPages(const uint64_t* offsets, size_t count) override {
    EXPECT_EQ(pins, count);
    batchSizes.push_back(count);
    return File::syncPages(offsets, count);
  }

  void prime() {
    ASSERT_EQ(read(0, bytes.size(), 0), bytes.size());
    ASSERT_EQ(pins, 0U);
  }

  void resetCalls() {
    writes.clear();
    synced.clear();
    batchSizes.clear();
  }

  const size_t pageSize;
  std::vector<unsigned char> bytes;
  std::vector<unsigned char> persisted;
  std::vector<size_t> writes;
  std::vector<size_t> synced;
  std::vector<size_t> batchSizes;
  size_t failedOffset = ~size_t(0);
  size_t pins = 0;

 protected:
  size_t getBlockSize() const override {
    return pageSize;
  }

  bool useFillCache() const override {
    return false;
  }

  uintptr_t readBlock(uint64_t offset) override {
    ++pins;
    return reinterpret_cast<uintptr_t>(bytes.data() + offset);
  }

  void writeBlock(uint64_t offset, uintptr_t buffer) override {
    EXPECT_GT(pins, 0U);
    EXPECT_EQ(buffer, reinterpret_cast<uintptr_t>(bytes.data() + offset));
    writes.push_back(offset);
  }

  bool pinBlock(uint64_t) override {
    ++pins;
    return true;
  }

  void unpinBlock(uint64_t) override {
    ASSERT_GT(pins, 0U);
    --pins;
  }
};
}  // namespace

TEST(FileRangeSync, FlushesOnlyPagesOverlappingUnalignedRange) {
  RangeSyncFile file;
  file.prime();
  file.bytes[0] = 1;
  file.bytes[file.pageSize] = 2;
  file.bytes[2 * file.pageSize] = 3;
  ASSERT_TRUE(file.syncRange(file.pageSize + 7, 1));
  EXPECT_EQ(file.synced, (std::vector<size_t>{file.pageSize}));
  EXPECT_EQ(file.writes, file.synced);
  EXPECT_EQ(file.persisted[0], 0);
  EXPECT_EQ(file.persisted[file.pageSize], 2);
  EXPECT_EQ(file.persisted[2 * file.pageSize], 0);
  EXPECT_EQ(file.pins, 0U);

  file.resetCalls();
  ASSERT_TRUE(file.syncRange(file.pageSize - 1, 2));
  std::sort(file.synced.begin(), file.synced.end());
  EXPECT_EQ(file.synced, (std::vector<size_t>{0, file.pageSize}));
  EXPECT_EQ(file.persisted[0], 1);
  EXPECT_EQ(file.persisted[2 * file.pageSize], 0);
  EXPECT_EQ(file.pins, 0U);
}

TEST(FileRangeSync, ZeroLengthFlushesSuffixAndFullSyncStillFlushesEveryPage) {
  RangeSyncFile file;
  file.prime();
  ASSERT_TRUE(file.syncRange(file.pageSize + 1, 0));
  std::sort(file.synced.begin(), file.synced.end());
  EXPECT_EQ(file.synced, (std::vector<size_t>{file.pageSize, 2 * file.pageSize}));
  file.resetCalls();
  ASSERT_TRUE(file.sync());
  std::sort(file.synced.begin(), file.synced.end());
  EXPECT_EQ(file.synced, (std::vector<size_t>{0, file.pageSize, 2 * file.pageSize}));
  EXPECT_EQ(file.pins, 0U);
}

TEST(FileRangeSync, FailedPageStaysAvailableForRetryAndOutsideFailureIsExcluded) {
  RangeSyncFile file;
  file.prime();
  file.bytes[file.pageSize] = 29;
  file.bytes[2 * file.pageSize] = 43;
  file.failedOffset = file.pageSize;
  EXPECT_FALSE(file.syncRange(file.pageSize, 0));
  std::sort(file.synced.begin(), file.synced.end());
  EXPECT_EQ(file.synced, (std::vector<size_t>{file.pageSize, 2 * file.pageSize}));
  EXPECT_EQ(file.persisted[file.pageSize], 0);
  EXPECT_EQ(file.persisted[2 * file.pageSize], 43);
  EXPECT_EQ(file.pins, 0U);

  file.resetCalls();
  EXPECT_TRUE(file.syncRange(2 * file.pageSize, 1));
  EXPECT_EQ(file.synced, (std::vector<size_t>{2 * file.pageSize}));
  file.failedOffset = ~size_t(0);
  file.resetCalls();
  EXPECT_TRUE(file.syncRange(file.pageSize, 1));
  EXPECT_EQ(file.synced, (std::vector<size_t>{file.pageSize}));
  EXPECT_EQ(file.persisted[file.pageSize], 29);
  EXPECT_EQ(file.pins, 0U);
}

TEST(FileRangeSync, EmptySuffixAndWrappedRangesPerformNoIo) {
  RangeSyncFile file;
  file.prime();
  EXPECT_TRUE(file.syncRange(file.bytes.size(), 0));
  EXPECT_TRUE(file.syncRange(~size_t(0), 1));
  EXPECT_FALSE(file.syncRange(~size_t(0), 2));
  EXPECT_TRUE(file.synced.empty());
  EXPECT_TRUE(file.writes.empty());
  EXPECT_EQ(file.pins, 0U);
}

TEST(FileRangeSync, BoundedBatchesRetainAllPinsAndContinueAfterFailure) {
  RangeSyncFile file(Disk::MaxSyncPages + 3);
  file.prime();
  file.failedOffset = 0;
  EXPECT_FALSE(file.syncRange(0, 0));
  EXPECT_EQ(file.batchSizes, (std::vector<size_t>{Disk::MaxSyncPages, 3}));
  EXPECT_EQ(file.synced.size(), Disk::MaxSyncPages + 3);
  EXPECT_EQ(file.writes, file.synced);
  EXPECT_EQ(file.pins, 0U);
  file.failedOffset = ~size_t(0);
  file.resetCalls();
  EXPECT_TRUE(file.syncRange(0, 0));
  EXPECT_EQ(file.pins, 0U);
}

TEST(FileRangeSync, BatchFallbackRejectsInvalidListBeforeAnySync) {
  RangeSyncFile file;
  const uint64_t offsets[] = {0, file.bytes.size()};
  EXPECT_FALSE(file.File::syncPages(offsets, 2));
  EXPECT_FALSE(file.File::syncPages(nullptr, 1));
  EXPECT_FALSE(file.File::syncPages(offsets, Disk::MaxSyncPages + 1));
  EXPECT_TRUE(file.File::syncPages(nullptr, 0));
  EXPECT_TRUE(file.synced.empty());
  EXPECT_EQ(file.pins, 0U);
}

namespace {
class FillRangeSyncFile final : public File {
 public:
  FillRangeSyncFile() : pageSize(PhysicalMemoryManager::getPageSize()), bytes(3 * pageSize, 0) {
    setSize(bytes.size());
    enableFillCacheWriteback();
  }

  void prime() {
    ASSERT_EQ(read(0, bytes.size(), 0), bytes.size());
    ASSERT_EQ(lowerPins, 0U);
  }

  bool syncPages(const uint64_t* offsets, size_t count) override {
    EXPECT_EQ(count, 3U);
    for (size_t i = 0; i < count; ++i) {
      // This dispatch precedes any backend claim. The range collector itself
      // must retain every selected upper page until the batch result returns.
      EXPECT_FALSE(cacheState().fill.evict(offsets[i]));
      ++evictionAttempts;
    }
    return !fail;
  }

  bool evictPage(size_t offset) {
    return cacheState().fill.evict(offset);
  }

  const size_t pageSize;
  std::vector<unsigned char> bytes;
  size_t lowerPins = 0;
  size_t evictionAttempts = 0;
  bool fail = false;

 protected:
  uintptr_t readBlock(uint64_t offset) override {
    ++lowerPins;
    return reinterpret_cast<uintptr_t>(bytes.data() + offset);
  }

  void unpinBlock(uint64_t) override {
    ASSERT_GT(lowerPins, 0U);
    --lowerPins;
  }

  bool useFillCache() const override {
    return true;
  }

  size_t getBlockSize() const override {
    return pageSize;
  }
};
}  // namespace

TEST(FileRangeSync, FillBatchRetainsSnapshotPagesAndReleasesPinsAfterEitherResult) {
  for (bool fail : {false, true}) {
    FillRangeSyncFile file;
    file.prime();
    file.fail = fail;
    EXPECT_EQ(file.syncRange(0, 0), !fail);
    EXPECT_EQ(file.evictionAttempts, 3U);
    for (size_t i = 0; i < 3; ++i)
      EXPECT_TRUE(file.evictPage(i * file.pageSize));
  }
}
