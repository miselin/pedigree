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

#include "Ext2FillCacheTestDisk.h"
#include "modules/system/ext2/Ext2File.h"
#include "modules/system/ext2/Ext2Filesystem.h"
#include "modules/system/ext2/ext2.h"
#include <gtest/gtest.h>

class Ext2FillCacheTestPeer {
 public:
  static void configure(Ext2Filesystem& filesystem, Disk* disk, uint32_t blockSize) {
    filesystem.m_pDisk = disk;
    filesystem.m_BlockSize = blockSize;
  }

  static void forceFillCache(File& file) {
    file.m_bForceFillCache = true;
  }

  static bool usesFillCache(File& file) {
    return file.useFillCache();
  }

  static uintptr_t lookupFillPage(File& file, size_t offset) {
    return file.m_FillCache.lookup(offset);
  }

  static void releaseFillPage(File& file, size_t offset) {
    file.m_FillCache.release(offset);
  }

  static bool fillPageExists(File& file, size_t offset) {
    return file.m_FillCache.exists(offset, TargetInfo::getPageSize());
  }

  static bool evictFillPage(File& file, size_t offset) {
    return file.m_FillCache.evict(offset);
  }

  static void triggerFillChecksum(File& file, size_t offset) {
    file.m_FillCache.triggerChecksum(offset);
  }

  static void setDataCacheSentinel(File& file, size_t block, uintptr_t value) {
    file.setCachedPage(block, value);
  }

  static uintptr_t dataCacheSentinel(File& file, size_t block) {
    return file.getCachedPage(block);
  }
};

namespace {
constexpr size_t kNativePageSize = TargetInfo::getPageSize();
constexpr size_t kTargetPage = kNativePageSize;
constexpr uint32_t kIndirectBlock = 4;
enum class TargetLayout { CoResident, Noncontiguous, Sparse };

std::vector<uint32_t> makeBlocks(uint32_t blockSize, TargetLayout layout) {
  const size_t blocksPerPage = kNativePageSize / blockSize;
  std::vector<uint32_t> blocks(2 * blocksPerPage);
  for (size_t i = 0; i < blocksPerPage; ++i) {
    blocks[i] = 8 + static_cast<uint32_t>(i * 3);
  }

  const uint32_t coResidentBase = static_cast<uint32_t>((9 * kNativePageSize) / blockSize);
  for (size_t i = 0; i < blocksPerPage; ++i) {
    blocks[blocksPerPage + i] = layout == TargetLayout::CoResident
                                    ? coResidentBase + static_cast<uint32_t>(i)
                                    : coResidentBase + 1 + static_cast<uint32_t>(i * blocksPerPage);
  }
  if (layout == TargetLayout::Sparse) {
    blocks[blocksPerPage + 1] = 0;
  }
  return blocks;
}

Inode makeSubpageInode(FillCacheDisk& disk, uint32_t blockSize, const std::vector<uint32_t>& blocks,
                       size_t size = 2 * kNativePageSize) {
  Inode inode = {};
  inode.i_mode = HOST_TO_LITTLE16(EXT2_S_IFREG);
  inode.i_size = HOST_TO_LITTLE32(size);
  const size_t metadataBlocks = blocks.size() > 12 ? 1 : 0;
  inode.i_blocks = HOST_TO_LITTLE32(((blocks.size() + metadataBlocks) * blockSize) / 512);
  for (size_t i = 0; i < blocks.size() && i < 12; ++i) {
    inode.i_block[i] = HOST_TO_LITTLE32(blocks[i]);
  }
  if (metadataBlocks) {
    inode.i_block[12] = HOST_TO_LITTLE32(kIndirectBlock);
    for (size_t i = 12; i < blocks.size(); ++i) {
      disk.store32((kIndirectBlock * blockSize) + ((i - 12) * sizeof(uint32_t)),
                   HOST_TO_LITTLE32(blocks[i]));
    }
  }
  return inode;
}

void initialiseBlocks(FillCacheDisk& disk, const std::vector<uint32_t>& blocks, uint32_t blockSize,
                      std::vector<uint8_t>& expectedPage) {
  const size_t blocksPerPage = kNativePageSize / blockSize;
  expectedPage.resize(kNativePageSize);
  for (size_t i = 0; i < blocks.size(); ++i) {
    if (!blocks[i]) {
      continue;
    }
    const uint8_t value = static_cast<uint8_t>(0x20 + i);
    disk.fill(static_cast<uint64_t>(blocks[i]) * blockSize, blockSize, value);
    if (i >= blocksPerPage) {
      std::fill(expectedPage.begin() + ((i - blocksPerPage) * blockSize),
                expectedPage.begin() + ((i - blocksPerPage + 1) * blockSize), value);
    }
  }
}

std::vector<uint64_t> targetLocations(const std::vector<uint32_t>& blocks, uint32_t blockSize) {
  const size_t blocksPerPage = kNativePageSize / blockSize;
  std::vector<uint64_t> result;
  for (size_t i = blocksPerPage; i < blocks.size(); ++i) {
    if (blocks[i]) {
      result.push_back(static_cast<uint64_t>(blocks[i]) * blockSize);
    }
  }
  return result;
}

class FillPageReference {
 public:
  FillPageReference(File& file, size_t offset)
      : file(file), offset(offset), address(Ext2FillCacheTestPeer::lookupFillPage(file, offset)) {}

  ~FillPageReference() {
    if (address) {
      Ext2FillCacheTestPeer::releaseFillPage(file, offset);
    }
  }

  uintptr_t get() const {
    return address;
  }

  void syncAndReturn(size_t syncOffset, bool async) {
    file.syncAndReturnPhysicalPage(syncOffset, async);
    address = 0;
  }

 private:
  File& file;
  size_t offset;
  uintptr_t address;
};

void expectTargetBytes(const FillCacheDisk& disk, const std::vector<uint32_t>& blocks,
                       uint32_t blockSize, const std::vector<uint8_t>& expected, bool persisted) {
  const size_t blocksPerPage = kNativePageSize / blockSize;
  for (size_t i = blocksPerPage; i < blocks.size(); ++i) {
    if (!blocks[i]) {
      continue;
    }
    const uint64_t location = static_cast<uint64_t>(blocks[i]) * blockSize;
    const uint8_t* blockExpected = expected.data() + ((i - blocksPerPage) * blockSize);
    EXPECT_TRUE(persisted ? disk.persistedEquals(location, blockExpected, blockSize)
                          : disk.equals(location, blockExpected, blockSize));
  }
}

void expectBalancedPins(const FillCacheDisk& disk, const std::vector<uint64_t>& expectedReads) {
  EXPECT_EQ(disk.reads, expectedReads);
  EXPECT_EQ(disk.writePins.size(), disk.writes.size());
  EXPECT_TRUE(
      std::all_of(disk.writePins.begin(), disk.writePins.end(), [](bool pin) { return pin; }));
  EXPECT_EQ(std::count(disk.operations.begin(), disk.operations.end(), 'R'),
            std::count(disk.operations.begin(), disk.operations.end(), 'U'));
  EXPECT_TRUE(disk.hasNoPins());
  EXPECT_FALSE(disk.outOfRange);
  EXPECT_FALSE(disk.unbalancedUnpin);
}

class Ext2FillCacheWriteback : public ::testing::TestWithParam<uint32_t> {};

}  // namespace

TEST_P(Ext2FillCacheWriteback, OrdinaryWriteCopiesAllBlocksBeforeSchedulingWriteback) {
  const uint32_t blockSize = GetParam();
  const std::vector<uint32_t> blocks = makeBlocks(blockSize, TargetLayout::CoResident);
  FillCacheDisk disk;
  std::vector<uint8_t> expected;
  initialiseBlocks(disk, blocks, blockSize, expected);

  Ext2Filesystem filesystem;
  Ext2FillCacheTestPeer::configure(filesystem, &disk, blockSize);
  Inode inode = makeSubpageInode(disk, blockSize, blocks);
  Ext2File file(String("fill-write"), 3, &inode, &filesystem);
  EXPECT_FALSE(Ext2FillCacheTestPeer::usesFillCache(file));
  Ext2FillCacheTestPeer::forceFillCache(file);
  EXPECT_EQ(file.getSize(), 2 * kNativePageSize);
  EXPECT_TRUE(Ext2FillCacheTestPeer::usesFillCache(file));

  constexpr size_t kInset = 17;
  std::vector<uint8_t> source(kNativePageSize - (2 * kInset));
  for (size_t i = 0; i < source.size(); ++i) {
    source[i] = static_cast<uint8_t>(0x80 + (i % 73));
  }
  std::copy(source.begin(), source.end(), expected.begin() + kInset);

  EXPECT_EQ(
      file.write(kTargetPage + kInset, source.size(), reinterpret_cast<uintptr_t>(source.data())),
      source.size());
  EXPECT_TRUE(Ext2FillCacheTestPeer::fillPageExists(file, kTargetPage));

  const std::vector<uint64_t> locations = targetLocations(blocks, blockSize);
  std::vector<uint64_t> reads = locations;
  if (blocks.size() > 12) {
    const size_t blocksPerPage = kNativePageSize / blockSize;
    const size_t directTargetBlocks = blocksPerPage < 12 ? 12 - blocksPerPage : 0;
    reads.insert(reads.begin() + directTargetBlocks,
                 static_cast<uint64_t>(kIndirectBlock) * blockSize);
  }
  reads.insert(reads.end(), locations.begin(), locations.end());
  EXPECT_EQ(disk.writes, locations);
  expectBalancedPins(disk, reads);
  expectTargetBytes(disk, blocks, blockSize, expected, false);
  expectTargetBytes(disk, blocks, blockSize, expected, true);
  Ext2FillCacheTestPeer::triggerFillChecksum(file, kTargetPage);
  EXPECT_TRUE(Ext2FillCacheTestPeer::evictFillPage(file, kTargetPage));
}

TEST_P(Ext2FillCacheWriteback, AsynchronousMappedSyncCopiesAllBlocksWithoutFlushing) {
  const uint32_t blockSize = GetParam();
  const std::vector<uint32_t> blocks = makeBlocks(blockSize, TargetLayout::CoResident);
  FillCacheDisk disk;
  std::vector<uint8_t> expected;
  initialiseBlocks(disk, blocks, blockSize, expected);

  Ext2Filesystem filesystem;
  Ext2FillCacheTestPeer::configure(filesystem, &disk, blockSize);
  Inode inode = makeSubpageInode(disk, blockSize, blocks);
  Ext2File file(String("fill-async"), 3, &inode, &filesystem);
  Ext2FillCacheTestPeer::forceFillCache(file);

  std::vector<uint8_t> original(kNativePageSize);
  ASSERT_EQ(file.read(kTargetPage, kNativePageSize, reinterpret_cast<uintptr_t>(original.data())),
            kNativePageSize);
  disk.clearActivity();

  FillPageReference page(file, kTargetPage);
  ASSERT_NE(page.get(), 0U);
  for (size_t i = 0; i < expected.size(); ++i) {
    expected[i] = static_cast<uint8_t>(0x40 + ((i / blockSize) * 11));
  }
  std::copy(expected.begin(), expected.end(), reinterpret_cast<uint8_t*>(page.get()));
  page.syncAndReturn(kTargetPage + 137, true);

  const std::vector<uint64_t> locations = targetLocations(blocks, blockSize);
  EXPECT_EQ(disk.writes, locations);
  EXPECT_TRUE(disk.flushes.empty());
  expectBalancedPins(disk, locations);
  expectTargetBytes(disk, blocks, blockSize, expected, false);
  expectTargetBytes(disk, blocks, blockSize, expected, true);

  Ext2FillCacheTestPeer::triggerFillChecksum(file, kTargetPage);
  EXPECT_TRUE(Ext2FillCacheTestPeer::evictFillPage(file, kTargetPage));
}

TEST_P(Ext2FillCacheWriteback, SynchronousMappedSyncCopiesThenFlushesEveryPhysicalBlock) {
  const uint32_t blockSize = GetParam();
  const std::vector<uint32_t> blocks = makeBlocks(blockSize, TargetLayout::Noncontiguous);
  FillCacheDisk disk;
  std::vector<uint8_t> expected;
  initialiseBlocks(disk, blocks, blockSize, expected);

  Ext2Filesystem filesystem;
  Ext2FillCacheTestPeer::configure(filesystem, &disk, blockSize);
  Inode inode = makeSubpageInode(disk, blockSize, blocks);
  Ext2File file(String("fill-sync"), 3, &inode, &filesystem);
  Ext2FillCacheTestPeer::forceFillCache(file);

  std::vector<uint8_t> original(kNativePageSize);
  ASSERT_EQ(file.read(kTargetPage, kNativePageSize, reinterpret_cast<uintptr_t>(original.data())),
            kNativePageSize);
  disk.clearActivity();

  FillPageReference page(file, kTargetPage);
  ASSERT_NE(page.get(), 0U);
  for (size_t i = 0; i < expected.size(); ++i) {
    expected[i] = static_cast<uint8_t>(0x60 + ((i / blockSize) * 7));
  }
  std::copy(expected.begin(), expected.end(), reinterpret_cast<uint8_t*>(page.get()));
  page.syncAndReturn(kTargetPage + 313, false);

  const std::vector<uint64_t> locations = targetLocations(blocks, blockSize);
  EXPECT_EQ(disk.writes, locations);
  EXPECT_EQ(disk.flushes, locations);
  expectBalancedPins(disk, locations);
  const auto firstWrite = std::find(disk.operations.begin(), disk.operations.end(), 'W');
  EXPECT_EQ(static_cast<size_t>(std::count(disk.operations.begin(), firstWrite, 'R')),
            locations.size());
  const auto firstFlush = std::find(disk.operations.begin(), disk.operations.end(), 'F');
  EXPECT_EQ(static_cast<size_t>(std::count(disk.operations.begin(), firstFlush, 'W')),
            locations.size());
  expectTargetBytes(disk, blocks, blockSize, expected, true);
  constexpr uintptr_t kSentinel = 0x12345000;
  const size_t dataCacheBlock = kTargetPage / blockSize;
  Ext2FillCacheTestPeer::setDataCacheSentinel(file, dataCacheBlock, kSentinel);
  Ext2FillCacheTestPeer::triggerFillChecksum(file, kTargetPage);
  EXPECT_TRUE(Ext2FillCacheTestPeer::evictFillPage(file, kTargetPage));
  EXPECT_EQ(Ext2FillCacheTestPeer::dataCacheSentinel(file, dataCacheBlock), kSentinel);
}

TEST_P(Ext2FillCacheWriteback, SkipsSparseAndPastEofBlocks) {
  const uint32_t blockSize = GetParam();

  {
    const std::vector<uint32_t> blocks = makeBlocks(blockSize, TargetLayout::Sparse);
    FillCacheDisk disk;
    std::vector<uint8_t> expected;
    initialiseBlocks(disk, blocks, blockSize, expected);
    Ext2Filesystem filesystem;
    Ext2FillCacheTestPeer::configure(filesystem, &disk, blockSize);
    Inode inode = makeSubpageInode(disk, blockSize, blocks);
    Ext2File file(String("fill-sparse"), 3, &inode, &filesystem);
    Ext2FillCacheTestPeer::forceFillCache(file);

    std::vector<uint8_t> original(kNativePageSize);
    ASSERT_EQ(file.read(kTargetPage, kNativePageSize, reinterpret_cast<uintptr_t>(original.data())),
              kNativePageSize);
    disk.clearActivity();
    FillPageReference page(file, kTargetPage);
    ASSERT_NE(page.get(), 0U);
    std::fill(reinterpret_cast<uint8_t*>(page.get()),
              reinterpret_cast<uint8_t*>(page.get()) + kNativePageSize, 0xD7);
    page.syncAndReturn(kTargetPage + 91, false);

    const std::vector<uint64_t> locations = targetLocations(blocks, blockSize);
    EXPECT_EQ(disk.writes, locations);
    EXPECT_EQ(disk.flushes, locations);
    expectBalancedPins(disk, locations);
    uintptr_t sparse = file.readBlock(kTargetPage + blockSize);
    ASSERT_NE(sparse, 0U);
    const bool sparseIsZero = std::all_of(reinterpret_cast<uint8_t*>(sparse),
                                          reinterpret_cast<uint8_t*>(sparse) + blockSize,
                                          [](uint8_t value) { return value == 0; });
    EXPECT_TRUE(sparseIsZero);
    std::fill(reinterpret_cast<uint8_t*>(sparse), reinterpret_cast<uint8_t*>(sparse) + blockSize,
              0);
    file.unpinBlock(kTargetPage + blockSize);
    Ext2FillCacheTestPeer::triggerFillChecksum(file, kTargetPage);
    EXPECT_TRUE(Ext2FillCacheTestPeer::evictFillPage(file, kTargetPage));
  }

  {
    std::vector<uint32_t> blocks = makeBlocks(blockSize, TargetLayout::Noncontiguous);
    const size_t blocksPerPage = kNativePageSize / blockSize;
    blocks.resize(blocksPerPage + 2);
    const size_t fileSize = kTargetPage + blockSize + (blockSize / 2);
    FillCacheDisk disk;
    std::vector<uint8_t> expected;
    initialiseBlocks(disk, blocks, blockSize, expected);
    Ext2Filesystem filesystem;
    Ext2FillCacheTestPeer::configure(filesystem, &disk, blockSize);
    Inode inode = makeSubpageInode(disk, blockSize, blocks, fileSize);
    Ext2File file(String("fill-tail"), 3, &inode, &filesystem);
    Ext2FillCacheTestPeer::forceFillCache(file);

    std::vector<uint8_t> original(kNativePageSize);
    ASSERT_EQ(file.read(kTargetPage, kNativePageSize, reinterpret_cast<uintptr_t>(original.data())),
              fileSize - kTargetPage);
    disk.clearActivity();
    FillPageReference page(file, kTargetPage);
    ASSERT_NE(page.get(), 0U);
    std::fill(reinterpret_cast<uint8_t*>(page.get()),
              reinterpret_cast<uint8_t*>(page.get()) + kNativePageSize, 0xE3);
    std::fill(expected.begin(), expected.begin() + blockSize + (blockSize / 2), 0xE3);
    page.syncAndReturn(kTargetPage + 19, false);

    const std::vector<uint64_t> locations = targetLocations(blocks, blockSize);
    EXPECT_EQ(disk.writes, locations);
    EXPECT_EQ(disk.flushes, locations);
    expectBalancedPins(disk, locations);
    expectTargetBytes(disk, blocks, blockSize, expected, false);
    expectTargetBytes(disk, blocks, blockSize, expected, true);
    Ext2FillCacheTestPeer::triggerFillChecksum(file, kTargetPage);
    EXPECT_TRUE(Ext2FillCacheTestPeer::evictFillPage(file, kTargetPage));
  }
}

TEST_P(Ext2FillCacheWriteback, DestructorDrainsDirtyFillPageWhileDerivedTypeIsAlive) {
  const uint32_t blockSize = GetParam();
  const std::vector<uint32_t> blocks = makeBlocks(blockSize, TargetLayout::CoResident);
  FillCacheDisk disk;
  std::vector<uint8_t> expected;
  initialiseBlocks(disk, blocks, blockSize, expected);
  Ext2Filesystem filesystem;
  Ext2FillCacheTestPeer::configure(filesystem, &disk, blockSize);
  Inode inode = makeSubpageInode(disk, blockSize, blocks);
  Ext2File* file = new Ext2File(String("fill-destroy"), 3, &inode, &filesystem);
  Ext2FillCacheTestPeer::forceFillCache(*file);

  std::vector<uint8_t> original(kNativePageSize);
  ASSERT_EQ(file->read(kTargetPage, kNativePageSize, reinterpret_cast<uintptr_t>(original.data())),
            kNativePageSize);
  {
    FillPageReference page(*file, kTargetPage);
    ASSERT_NE(page.get(), 0U);
    for (size_t i = 0; i < expected.size(); ++i) {
      expected[i] = static_cast<uint8_t>(0xA0 + ((i / blockSize) * 3));
    }
    std::copy(expected.begin(), expected.end(), reinterpret_cast<uint8_t*>(page.get()));
  }
  disk.clearActivity();

  delete file;

  const std::vector<uint64_t> locations = targetLocations(blocks, blockSize);
  EXPECT_EQ(disk.writes, locations);
  expectBalancedPins(disk, locations);
  expectTargetBytes(disk, blocks, blockSize, expected, true);
}

TEST_P(Ext2FillCacheWriteback, MissingFillPageFallsBackToOneFilesystemBlock) {
  const uint32_t blockSize = GetParam();
  const std::vector<uint32_t> blocks = makeBlocks(blockSize, TargetLayout::Noncontiguous);
  const size_t blocksPerPage = kNativePageSize / blockSize;
  const uint32_t physicalBlock = blocks[blocksPerPage + 1];
  const uint64_t physicalLocation = static_cast<uint64_t>(physicalBlock) * blockSize;
  FillCacheDisk disk;
  Ext2Filesystem filesystem;
  Ext2FillCacheTestPeer::configure(filesystem, &disk, blockSize);
  Inode inode = makeSubpageInode(disk, blockSize, blocks);
  Ext2File file(String("fill-missing"), 3, &inode, &filesystem);
  Ext2FillCacheTestPeer::forceFillCache(file);

  file.sync(kTargetPage + blockSize + 27, true);
  EXPECT_EQ(disk.writes, std::vector<uint64_t>({physicalLocation}));
  EXPECT_TRUE(disk.flushes.empty());

  disk.clearActivity();
  file.sync(kTargetPage + blockSize + 31, false);
  EXPECT_TRUE(disk.writes.empty());
  EXPECT_EQ(disk.flushes, std::vector<uint64_t>({physicalLocation}));
  EXPECT_FALSE(Ext2FillCacheTestPeer::fillPageExists(file, kTargetPage));
}

INSTANTIATE_TEST_CASE_P(BlockSizes, Ext2FillCacheWriteback, ::testing::Values(1024U, 2048U));
