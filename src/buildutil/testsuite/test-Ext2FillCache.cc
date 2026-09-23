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
#include <memory>
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
    FillCacheDisk& backing = *static_cast<FillCacheDisk*>(disk);
    filesystem.m_pSuperblock = disk->read(1024).as<Superblock>();
    filesystem.m_pSuperblock->s_inodes_per_group = HOST_TO_LITTLE32(4);
    filesystem.m_pSuperblock->s_blocks_per_group = HOST_TO_LITTLE32(512);
    filesystem.m_pSuperblock->s_first_data_block = HOST_TO_LITTLE32(blockSize == 1024 ? 1 : 0);
    filesystem.m_InodeSize = sizeof(Inode);
    filesystem.m_nGroupDescriptors = 1;
    const uint32_t descriptorBlock = blockSize == 1024 ? 2 : 1;
    GroupDesc* group = disk->read(descriptorBlock * blockSize).as<GroupDesc>();
    group->bg_inode_table = HOST_TO_LITTLE32(5);
    group->bg_block_bitmap = HOST_TO_LITTLE32(3);
    group->bg_inode_bitmap = HOST_TO_LITTLE32(6);
    filesystem.m_pGroupDescriptors = new GroupDesc*[1]{group};
    filesystem.m_pBlockBitmaps = new Vector<size_t>[1];
    filesystem.m_pInodeBitmaps = new Vector<size_t>[1];
    filesystem.m_pInodeTables = new Vector<size_t>[1];
    filesystem.m_pInodeTables[0].pushBack(disk->read(5 * blockSize).address());
    backing.metadataLocations = {1024, descriptorBlock * blockSize, 3 * blockSize, 5 * blockSize,
                                 6 * blockSize};
    backing.metadataReferences = backing.pageReferences;
    backing.clearActivity();
  }

  static Inode* configureMetadata(Ext2Filesystem& filesystem, FillCacheDisk& disk,
                                  uint32_t blockSize, const Inode& inode) {
    configure(filesystem, &disk, blockSize);
    const uintptr_t table = filesystem.m_pInodeTables[0][0];
    Inode* result = reinterpret_cast<Inode*>(table + 2 * sizeof(Inode));
    *result = inode;
    return result;
  }

  static void forceFillCache(File& file) {
    file.m_bForceFillCache = true;
  }

  static bool usesFillCache(File& file) {
    return file.useFillCache();
  }

  static uintptr_t lookupFillPage(File& file, size_t offset) {
    uintptr_t page = file.cacheState().fill.lookup(offset);
    if (page) {
      // This native fixture borrows the cache without a virtual address
      // space. Account for the loan normally acquired by getPhysicalPage().
      __atomic_add_fetch(&file.physicalPageLoans(), 1, __ATOMIC_RELEASE);
    }
    return page;
  }

  static void releaseFillPage(File& file, size_t offset) {
    file.returnPhysicalPage(offset);
  }

  static bool fillPageExists(File& file, size_t offset) {
    return file.cacheState().fill.exists(offset, TargetInfo::getPageSize());
  }

  static bool evictFillPage(File& file, size_t offset) {
    return file.cacheState().fill.evict(offset);
  }

  static void triggerFillChecksum(File& file, size_t offset) {
    file.cacheState().fill.triggerChecksum(offset);
  }

  static void exposeWritableMapping(File& file, size_t offset) {
    file.cacheState().fill.markExternallyWritable(offset);
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
  FillPageReference(File& file, size_t offset, bool writableMapping = false)
      : file(file), offset(offset), address(Ext2FillCacheTestPeer::lookupFillPage(file, offset)) {
    if (address && writableMapping)
      Ext2FillCacheTestPeer::exposeWritableMapping(file, offset);
  }

  ~FillPageReference() {
    if (address) {
      Ext2FillCacheTestPeer::releaseFillPage(file, offset);
    }
  }

  uintptr_t get() const {
    return address;
  }

  void syncAndReturn(size_t syncOffset, bool async) {
    EXPECT_TRUE(file.syncAndReturnPhysicalPage(syncOffset, async));
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
  EXPECT_TRUE(disk.hasOnlyMetadataPins());
  EXPECT_FALSE(disk.outOfRange);
  EXPECT_FALSE(disk.unbalancedUnpin);
}

void expectProducerTransfers(const FillCacheDisk& disk, const std::vector<uint64_t>& locations,
                             size_t blockSize, bool writing, size_t length = ~size_t{0}) {
  std::vector<FillCacheDisk::Transfer> expected;
  for (uint64_t location : locations) {
    const size_t amount = std::min(blockSize, length);
    if (!expected.empty() && expected.back().first + expected.back().second == location)
      expected.back().second += amount;
    else
      expected.emplace_back(location, amount);
    length -= amount;
  }
  EXPECT_EQ(writing ? disk.dataWrites : disk.dataReads, expected);
  if (writing) {
    EXPECT_EQ(disk.dataBarriers, 1U);
    EXPECT_TRUE(disk.reads.empty());
    EXPECT_TRUE(disk.writes.empty());
    EXPECT_TRUE(disk.flushes.empty());
  }
}

class Ext2FillCacheWriteback : public ::testing::TestWithParam<uint32_t> {};

class ResizeFixture {
 public:
  explicit ResizeFixture(uint32_t blockSize, size_t blockCount = 0) : blockSize(blockSize) {
    if (!blockCount)
      blockCount = 2 * kNativePageSize / blockSize;
    for (size_t i = 0; i < blockCount; ++i) {
      blocks.push_back(8 + static_cast<uint32_t>(i * 3));
      disk.fill(static_cast<uint64_t>(blocks.back()) * blockSize, blockSize,
                static_cast<uint8_t>(0x30 + i));
    }
    const Inode original = makeSubpageInode(disk, blockSize, blocks, blocks.size() * blockSize);
    inode = Ext2FillCacheTestPeer::configureMetadata(filesystem, disk, blockSize, original);
    const uint32_t first = blockSize == 1024 ? 1 : 0;
    auto reserveBlock = [&](uint32_t block) {
      const size_t index = block - first;
      const size_t byte = 3 * blockSize + index / 8;
      disk.storage[byte] |= 1U << (index % 8);
      disk.persisted[byte] = disk.storage[byte];
    };
    for (uint32_t block = first; block < 8; ++block)
      reserveBlock(block);
    for (uint32_t block : blocks)
      reserveBlock(block);
    file.reset(new Ext2File(String("resize"), 3, inode, &filesystem));
    Ext2FillCacheTestPeer::forceFillCache(*file);
  }

  bool fill() {
    std::vector<uint8_t> bytes(file->getSize());
    return file->read(0, bytes.size(), reinterpret_cast<uintptr_t>(bytes.data())) == bytes.size();
  }

  uintptr_t address(size_t offset) {
    FillPageReference page(*file, offset);
    return page.get();
  }

  std::vector<uint8_t> bytes(size_t offset) {
    FillPageReference page(*file, offset);
    if (!page.get())
      return {};
    const uint8_t* first = reinterpret_cast<const uint8_t*>(page.get());
    return std::vector<uint8_t>(first, first + kNativePageSize);
  }

  uint32_t blockSize;
  FillCacheDisk disk;
  Ext2Filesystem filesystem;
  std::vector<uint32_t> blocks;
  Inode* inode;
  std::unique_ptr<Ext2File> file;
};

}  // namespace

TEST_P(Ext2FillCacheWriteback, CachedReadPreservesBytesAndRefillsAfterEviction) {
  ResizeFixture fixture(GetParam());
  std::vector<uint8_t> bytes(kNativePageSize);
  std::vector<uint64_t> locations;
  for (size_t i = 0; i < kNativePageSize / fixture.blockSize; ++i) {
    locations.push_back(static_cast<uint64_t>(fixture.blocks[i]) * fixture.blockSize);
  }

  fixture.disk.clearActivity();
  ASSERT_EQ(fixture.file->read(0, bytes.size(), reinterpret_cast<uintptr_t>(bytes.data())),
            bytes.size());
  expectProducerTransfers(fixture.disk, locations, fixture.blockSize, false);
  expectBalancedPins(fixture.disk, {});
  const std::vector<uint8_t> cached = bytes;
  const uintptr_t address = fixture.address(0);
  ASSERT_NE(address, 0U);

  fixture.disk.fill(locations[0], fixture.blockSize, 0xD3);
  EXPECT_EQ(Ext2FillCacheTestPeer::dataCacheSentinel(*fixture.file, 0), address);
  fixture.disk.clearActivity();
  for (size_t i = 0; i < 4; ++i) {
    ASSERT_EQ(fixture.file->read(0, bytes.size(), reinterpret_cast<uintptr_t>(bytes.data())),
              bytes.size());
    EXPECT_EQ(bytes, cached);
  }
  EXPECT_EQ(Ext2FillCacheTestPeer::dataCacheSentinel(*fixture.file, 0), address);
  expectBalancedPins(fixture.disk, {});
  ASSERT_TRUE(Ext2FillCacheTestPeer::evictFillPage(*fixture.file, 0));

  fixture.disk.clearActivity();
  ASSERT_EQ(fixture.file->read(0, bytes.size(), reinterpret_cast<uintptr_t>(bytes.data())),
            bytes.size());
  std::vector<uint8_t> expected = cached;
  std::fill_n(expected.begin(), fixture.blockSize, 0xD3);
  EXPECT_EQ(bytes, expected);
  expectProducerTransfers(fixture.disk, locations, fixture.blockSize, false);
  expectBalancedPins(fixture.disk, {});
  EXPECT_TRUE(Ext2FillCacheTestPeer::evictFillPage(*fixture.file, 0));
}

TEST_P(Ext2FillCacheWriteback, DirectReadRefreshesResidentPageAndReleasesReference) {
  ResizeFixture fixture(GetParam());
  std::vector<uint8_t> bytes(kNativePageSize);
  ASSERT_EQ(fixture.file->read(0, bytes.size(), reinterpret_cast<uintptr_t>(bytes.data())),
            bytes.size());
  std::vector<uint8_t> expected = bytes;
  std::vector<uint64_t> locations;
  for (size_t i = 0; i < kNativePageSize / fixture.blockSize; ++i) {
    locations.push_back(static_cast<uint64_t>(fixture.blocks[i]) * fixture.blockSize);
  }

  fixture.file->enableDirect();
  for (uint8_t value : {0xD4, 0xE5}) {
    fixture.disk.fill(locations[0], fixture.blockSize, value);
    std::fill_n(expected.begin(), fixture.blockSize, value);
    fixture.disk.clearActivity();
    ASSERT_EQ(fixture.file->read(0, bytes.size(), reinterpret_cast<uintptr_t>(bytes.data())),
              bytes.size());
    EXPECT_EQ(bytes, expected);
    expectProducerTransfers(fixture.disk, locations, fixture.blockSize, false);
    expectBalancedPins(fixture.disk, {});
  }
  fixture.file->disableDirect();

  fixture.disk.clearActivity();
  ASSERT_EQ(fixture.file->read(0, bytes.size(), reinterpret_cast<uintptr_t>(bytes.data())),
            bytes.size());
  EXPECT_EQ(bytes, expected);
  expectBalancedPins(fixture.disk, {});
  EXPECT_TRUE(Ext2FillCacheTestPeer::evictFillPage(*fixture.file, 0));
}

TEST_P(Ext2FillCacheWriteback, DirectReadPersistsDeferredWriteBeforeRefreshingResidentPage) {
  ResizeFixture fixture(GetParam());
  std::vector<uint8_t> expected(kNativePageSize);
  ASSERT_EQ(fixture.file->read(0, expected.size(), reinterpret_cast<uintptr_t>(expected.data())),
            expected.size());
  const auto persistedBefore = fixture.disk.persisted;
  const size_t offset = fixture.blockSize - 7;
  std::vector<uint8_t> input(31, 0xb6);
  std::copy(input.begin(), input.end(), expected.begin() + offset);
  fixture.disk.clearActivity();
  ASSERT_EQ(fixture.file->write(offset, input.size(), reinterpret_cast<uintptr_t>(input.data())),
            input.size());
  EXPECT_TRUE(fixture.disk.dataWrites.empty());
  EXPECT_EQ(fixture.disk.persisted, persistedBefore);

  fixture.file->enableDirect();
  std::vector<uint8_t> actual(kNativePageSize);
  ASSERT_EQ(fixture.file->read(0, actual.size(), reinterpret_cast<uintptr_t>(actual.data())),
            actual.size());
  fixture.file->disableDirect();
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(fixture.disk.dataBarriers, 1U);
  for (size_t i = 0; i < kNativePageSize / fixture.blockSize; ++i) {
    EXPECT_TRUE(fixture.disk.persistedEquals(fixture.blocks[i] * fixture.blockSize,
                                             expected.data() + i * fixture.blockSize,
                                             fixture.blockSize));
  }
  EXPECT_TRUE(fixture.disk.hasOnlyMetadataPins());
  EXPECT_FALSE(fixture.disk.unbalancedUnpin);
}

TEST_P(Ext2FillCacheWriteback, FailedShrinkTailReadPreservesCachedPrefixAndSuffix) {
  ResizeFixture fixture(GetParam());
  ASSERT_TRUE(fixture.fill());
  const auto prefix = fixture.bytes(0);
  const auto suffix = fixture.bytes(kNativePageSize);
  const uintptr_t prefixAddress = fixture.address(0);
  const uintptr_t suffixAddress = fixture.address(kNativePageSize);
  const Inode before = *fixture.inode;
  fixture.disk.failedReadLocation = fixture.blocks[0] * fixture.blockSize;
  EXPECT_FALSE(fixture.file->resize(137));
  fixture.disk.failedReadLocation = ~uint64_t(0);
  EXPECT_EQ(fixture.file->getSize(), 2 * kNativePageSize);
  EXPECT_EQ(fixture.inode->i_size, before.i_size);
  EXPECT_EQ(fixture.inode->i_blocks, before.i_blocks);
  EXPECT_EQ(fixture.address(0), prefixAddress);
  EXPECT_EQ(fixture.address(kNativePageSize), suffixAddress);
  EXPECT_EQ(fixture.bytes(0), prefix);
  EXPECT_EQ(fixture.bytes(kNativePageSize), suffix);
  EXPECT_TRUE(fixture.disk.hasOnlyMetadataPins());
  EXPECT_FALSE(fixture.disk.unbalancedUnpin);
}

TEST_P(Ext2FillCacheWriteback, FailedShrinkIndirectReadCancelsPreparedSuffixDiscard) {
  ResizeFixture fixture(GetParam(), 16);
  ASSERT_TRUE(fixture.fill());
  const uintptr_t prefixAddress = fixture.address(0);
  const uintptr_t suffixAddress = fixture.address(kNativePageSize);
  const auto suffix = fixture.bytes(kNativePageSize);
  const size_t oldSize = fixture.file->getSize();
  const Inode before = *fixture.inode;
  fixture.disk.failedReadLocation = kIndirectBlock * fixture.blockSize;
  EXPECT_FALSE(fixture.file->resize(137));
  fixture.disk.failedReadLocation = ~uint64_t(0);
  EXPECT_EQ(fixture.file->getSize(), oldSize);
  EXPECT_EQ(fixture.inode->i_block[12], before.i_block[12]);
  EXPECT_EQ(fixture.inode->i_blocks, before.i_blocks);
  EXPECT_EQ(fixture.address(0), prefixAddress);
  EXPECT_EQ(fixture.address(kNativePageSize), suffixAddress);
  EXPECT_EQ(fixture.bytes(kNativePageSize), suffix);
  EXPECT_TRUE(fixture.disk.hasOnlyMetadataPins());
  EXPECT_FALSE(fixture.disk.unbalancedUnpin);
}

TEST_P(Ext2FillCacheWriteback, FailedShrinkBitmapReadPreservesVisibleInodeAndData) {
  ResizeFixture fixture(GetParam());
  ASSERT_TRUE(fixture.fill());
  const uintptr_t prefixAddress = fixture.address(0);
  const uintptr_t suffixAddress = fixture.address(kNativePageSize);
  const auto prefix = fixture.bytes(0);
  const auto suffix = fixture.bytes(kNativePageSize);
  const Inode before = *fixture.inode;
  fixture.disk.failedReadLocation = 3 * fixture.blockSize;
  EXPECT_FALSE(fixture.file->resize(137));
  fixture.disk.failedReadLocation = ~uint64_t(0);
  EXPECT_EQ(fixture.file->getSize(), 2 * kNativePageSize);
  EXPECT_EQ(fixture.inode->i_size, before.i_size);
  EXPECT_EQ(fixture.inode->i_blocks, before.i_blocks);
  for (size_t i = 0; i < 15; ++i) {
    const uint32_t actualBlock = fixture.inode->i_block[i];
    const uint32_t expectedBlock = before.i_block[i];
    EXPECT_EQ(actualBlock, expectedBlock);
  }
  EXPECT_EQ(fixture.address(0), prefixAddress);
  EXPECT_EQ(fixture.address(kNativePageSize), suffixAddress);
  EXPECT_EQ(fixture.bytes(0), prefix);
  EXPECT_EQ(fixture.bytes(kNativePageSize), suffix);
  EXPECT_FALSE(fixture.disk.unbalancedUnpin);
}

TEST_P(Ext2FillCacheWriteback, ShrinkRetainsBoundaryPageAndZerosItsEntireNativeTail) {
  ResizeFixture fixture(GetParam());
  ASSERT_TRUE(fixture.fill());
  const uintptr_t prefixAddress = fixture.address(0);
  const uintptr_t boundaryAddress = fixture.address(kNativePageSize);
  const auto prefix = fixture.bytes(0);
  auto boundary = fixture.bytes(kNativePageSize);
  ASSERT_EQ(boundary.size(), kNativePageSize);
  ASSERT_TRUE(fixture.file->resize(kNativePageSize + 137));
  EXPECT_EQ(fixture.address(0), prefixAddress);
  EXPECT_EQ(fixture.address(kNativePageSize), boundaryAddress);
  EXPECT_EQ(fixture.bytes(0), prefix);
  std::fill(boundary.begin() + 137, boundary.end(), 0);
  EXPECT_EQ(fixture.bytes(kNativePageSize), boundary);
  ASSERT_TRUE(fixture.file->resize(2 * kNativePageSize));
  EXPECT_EQ(fixture.address(kNativePageSize), boundaryAddress);
  EXPECT_EQ(fixture.bytes(kNativePageSize), boundary);
  EXPECT_FALSE(fixture.disk.unbalancedUnpin);
}

TEST_P(Ext2FillCacheWriteback, ShrinkDiscardsDirtySuffixWithoutWritingRemovedData) {
  ResizeFixture fixture(GetParam());
  ASSERT_TRUE(fixture.fill());
  const uintptr_t prefixAddress = fixture.address(0);
  {
    FillPageReference page(*fixture.file, kNativePageSize, true);
    ASSERT_NE(page.get(), 0U);
    std::fill_n(reinterpret_cast<uint8_t*>(page.get()), kNativePageSize, 0xE7);
  }
  Ext2FillCacheTestPeer::triggerFillChecksum(*fixture.file, kNativePageSize);
  fixture.disk.failedSyncLocation =
      fixture.blocks[kNativePageSize / fixture.blockSize] * fixture.blockSize;
  ASSERT_TRUE(fixture.file->resize(137));
  EXPECT_EQ(fixture.address(0), prefixAddress);
  EXPECT_FALSE(Ext2FillCacheTestPeer::fillPageExists(*fixture.file, kNativePageSize));
  EXPECT_TRUE(fixture.disk.failedSyncs.empty());
  fixture.disk.failedSyncLocation = ~uint64_t(0);
  ASSERT_TRUE(fixture.file->resize(2 * kNativePageSize));
  std::vector<uint8_t> regrown(2 * kNativePageSize - 137, 0xFF);
  ASSERT_EQ(fixture.file->read(137, regrown.size(), reinterpret_cast<uintptr_t>(regrown.data())),
            regrown.size());
  EXPECT_EQ(regrown, std::vector<uint8_t>(regrown.size(), 0));
  EXPECT_FALSE(fixture.disk.unbalancedUnpin);
}

TEST_P(Ext2FillCacheWriteback, NonmappingLoanRejectsShrinkWithoutChangingCache) {
  ResizeFixture fixture(GetParam());
  ASSERT_TRUE(fixture.fill());
  const auto prefix = fixture.bytes(0);
  const auto suffix = fixture.bytes(kNativePageSize);
  {
    FillPageReference borrowed(*fixture.file, kNativePageSize);
    ASSERT_NE(borrowed.get(), 0U);
    EXPECT_FALSE(fixture.file->resize(137));
    EXPECT_EQ(fixture.file->getSize(), 2 * kNativePageSize);
    EXPECT_EQ(fixture.bytes(0), prefix);
    EXPECT_EQ(fixture.bytes(kNativePageSize), suffix);
  }
  EXPECT_TRUE(fixture.file->resize(137));
  EXPECT_FALSE(fixture.disk.unbalancedUnpin);
}

TEST_P(Ext2FillCacheWriteback, OrdinaryWriteDefersProducerTransfersUntilSync) {
  const uint32_t blockSize = GetParam();
  const std::vector<uint32_t> blocks = makeBlocks(blockSize, TargetLayout::CoResident);
  FillCacheDisk disk;
  std::vector<uint8_t> expected;
  initialiseBlocks(disk, blocks, blockSize, expected);

  Ext2Filesystem filesystem;
  Ext2FillCacheTestPeer::configure(filesystem, &disk, blockSize);
  Inode inode = makeSubpageInode(disk, blockSize, blocks);
  Ext2File file(String("fill-write"), 3, &inode, &filesystem);
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
  expectProducerTransfers(disk, locations, blockSize, false);
  std::vector<uint64_t> metadataReads;
  if (blocks.size() > 12)
    metadataReads.push_back(static_cast<uint64_t>(kIndirectBlock) * blockSize);
  expectBalancedPins(disk, metadataReads);
  EXPECT_TRUE(disk.dataWrites.empty());
  EXPECT_TRUE(disk.writes.empty());
  EXPECT_EQ(disk.dataBarriers, 0U);
  std::vector<uint8_t> observed(source.size());
  ASSERT_EQ(file.read(kTargetPage + kInset, observed.size(),
                      reinterpret_cast<uintptr_t>(observed.data())),
            observed.size());
  EXPECT_EQ(observed, source);
  EXPECT_FALSE(disk.persistedEquals(locations[0] + kInset, source.data(), blockSize - kInset));

  disk.clearActivity();
  ASSERT_TRUE(file.sync(kTargetPage, false));
  expectProducerTransfers(disk, locations, blockSize, true);
  expectBalancedPins(disk, {});
  expectTargetBytes(disk, blocks, blockSize, expected, false);
  expectTargetBytes(disk, blocks, blockSize, expected, true);
  Ext2FillCacheTestPeer::triggerFillChecksum(file, kTargetPage);
  EXPECT_TRUE(Ext2FillCacheTestPeer::evictFillPage(file, kTargetPage));
}

TEST_P(Ext2FillCacheWriteback, WholePageOverwriteSkipsPayloadReadsButPartialWriteRequiresThem) {
  const size_t blockSize = GetParam();
  const size_t blocksPerPage = kNativePageSize / blockSize;
  ResizeFixture fixture(blockSize, 16 + blocksPerPage);
  const size_t firstBlock = (12 / blocksPerPage) * blocksPerPage;
  const size_t target = firstBlock * blockSize;
  std::vector<uint8_t> expected(kNativePageSize);
  for (size_t i = 0; i < expected.size(); ++i)
    expected[i] = static_cast<uint8_t>(0x40 + i % 113);

  fixture.disk.failedReadLocation = fixture.blocks[firstBlock] * blockSize;
  fixture.disk.clearActivity();
  ASSERT_EQ(
      fixture.file->write(target, expected.size(), reinterpret_cast<uintptr_t>(expected.data())),
      expected.size());
  EXPECT_TRUE(fixture.disk.dataReads.empty());
  EXPECT_TRUE(fixture.disk.dataWrites.empty());
  EXPECT_TRUE(Ext2FillCacheTestPeer::fillPageExists(*fixture.file, target));
  std::vector<uint8_t> actual(kNativePageSize);
  ASSERT_EQ(fixture.file->read(target, actual.size(), reinterpret_cast<uintptr_t>(actual.data())),
            actual.size());
  EXPECT_EQ(actual, expected);
  EXPECT_TRUE(fixture.disk.dataReads.empty());
  ASSERT_TRUE(fixture.file->sync(target, false));
  for (size_t i = 0; i < blocksPerPage; ++i)
    EXPECT_TRUE(fixture.disk.persistedEquals(fixture.blocks[firstBlock + i] * blockSize,
                                             expected.data() + i * blockSize, blockSize));
  ASSERT_TRUE(Ext2FillCacheTestPeer::evictFillPage(*fixture.file, target));

  const auto persisted = fixture.disk.persisted;
  const auto storage = fixture.disk.storage;
  const size_t inset = 17;
  std::vector<uint8_t> partial(31, 0xb7);
  fixture.disk.clearActivity();
  EXPECT_EQ(fixture.file->write(target + inset, partial.size(),
                                reinterpret_cast<uintptr_t>(partial.data())),
            0U);
  EXPECT_FALSE(Ext2FillCacheTestPeer::fillPageExists(*fixture.file, target));
  EXPECT_TRUE(fixture.disk.dataWrites.empty());
  EXPECT_EQ(fixture.disk.storage, storage);
  EXPECT_EQ(fixture.disk.persisted, persisted);
  EXPECT_TRUE(fixture.disk.hasOnlyMetadataPins());

  fixture.disk.failedReadLocation = ~uint64_t{0};
  ASSERT_EQ(fixture.file->write(target + inset, partial.size(),
                                reinterpret_cast<uintptr_t>(partial.data())),
            partial.size());
  EXPECT_FALSE(fixture.disk.dataReads.empty());
  std::copy(partial.begin(), partial.end(), expected.begin() + inset);
  ASSERT_TRUE(fixture.file->sync(target, false));
  for (size_t i = 0; i < blocksPerPage; ++i)
    EXPECT_TRUE(fixture.disk.persistedEquals(fixture.blocks[firstBlock + i] * blockSize,
                                             expected.data() + i * blockSize, blockSize));
  EXPECT_TRUE(fixture.disk.hasOnlyMetadataPins());
  EXPECT_FALSE(fixture.disk.unbalancedUnpin);
}

TEST_P(Ext2FillCacheWriteback, AsynchronousMappedSyncCompletesBackendWritesInWorker) {
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

  FillPageReference page(file, kTargetPage, true);
  ASSERT_NE(page.get(), 0U);
  for (size_t i = 0; i < expected.size(); ++i) {
    expected[i] = static_cast<uint8_t>(0x40 + ((i / blockSize) * 11));
  }
  std::copy(expected.begin(), expected.end(), reinterpret_cast<uint8_t*>(page.get()));
  page.syncAndReturn(kTargetPage + 137, true);

  const std::vector<uint64_t> locations = targetLocations(blocks, blockSize);
  expectProducerTransfers(disk, locations, blockSize, true);
  expectBalancedPins(disk, {});
  expectTargetBytes(disk, blocks, blockSize, expected, false);
  expectTargetBytes(disk, blocks, blockSize, expected, true);

  Ext2FillCacheTestPeer::triggerFillChecksum(file, kTargetPage);
  EXPECT_TRUE(Ext2FillCacheTestPeer::evictFillPage(file, kTargetPage));
}

TEST_P(Ext2FillCacheWriteback, HardlinkAliasesSharePagesAndTransferWritebackOwnership) {
  const uint32_t blockSize = GetParam();
  const std::vector<uint32_t> blocks = makeBlocks(blockSize, TargetLayout::CoResident);
  FillCacheDisk disk;
  std::vector<uint8_t> expected;
  initialiseBlocks(disk, blocks, blockSize, expected);
  Ext2Filesystem filesystem;
  Ext2FillCacheTestPeer::configure(filesystem, &disk, blockSize);
  Inode inode = makeSubpageInode(disk, blockSize, blocks);
  auto first = std::make_unique<Ext2File>(String("first"), 3, &inode, &filesystem);
  Ext2File second(String("second"), 3, &inode, &filesystem);
  Ext2FillCacheTestPeer::forceFillCache(*first);
  Ext2FillCacheTestPeer::forceFillCache(second);
  uint8_t value = 0;
  ASSERT_EQ(first->read(kTargetPage, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  ASSERT_EQ(second.read(kTargetPage, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  FillPageReference retained(second, kTargetPage, true);
  ASSERT_NE(retained.get(), 0U);
  {
    FillPageReference original(*first, kTargetPage, true);
    ASSERT_EQ(original.get(), retained.get());
    reinterpret_cast<uint8_t*>(original.get())[23] = 0x7d;
  }
  ASSERT_EQ(second.read(kTargetPage + 23, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  EXPECT_EQ(value, 0x7d);
  value = 0x9e;
  ASSERT_EQ(first->write(kTargetPage + 29, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  EXPECT_EQ(reinterpret_cast<uint8_t*>(retained.get())[29], 0x9e);
  expected[23] = 0x7d;
  expected[29] = 0x9e;
  first.reset();
  reinterpret_cast<uint8_t*>(retained.get())[31] = 0x4a;
  expected[31] = 0x4a;
  EXPECT_TRUE(second.sync(kTargetPage, false));
  expectTargetBytes(disk, blocks, blockSize, expected, true);
  retained.syncAndReturn(kTargetPage, false);
  expectTargetBytes(disk, blocks, blockSize, expected, true);
  Ext2FillCacheTestPeer::triggerFillChecksum(second, kTargetPage);
  EXPECT_TRUE(Ext2FillCacheTestPeer::evictFillPage(second, kTargetPage));
  EXPECT_TRUE(disk.hasOnlyMetadataPins());
  EXPECT_FALSE(disk.unbalancedUnpin);
}

TEST_P(Ext2FillCacheWriteback, SynchronousMappedSyncTransfersEveryExtentBeforeOneBarrier) {
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

  FillPageReference page(file, kTargetPage, true);
  ASSERT_NE(page.get(), 0U);
  for (size_t i = 0; i < expected.size(); ++i) {
    expected[i] = static_cast<uint8_t>(0x60 + ((i / blockSize) * 7));
  }
  std::copy(expected.begin(), expected.end(), reinterpret_cast<uint8_t*>(page.get()));
  page.syncAndReturn(kTargetPage + 313, false);

  const std::vector<uint64_t> locations = targetLocations(blocks, blockSize);
  expectProducerTransfers(disk, locations, blockSize, true);
  expectBalancedPins(disk, {});
  const auto barrier = std::find(disk.operations.begin(), disk.operations.end(), 'B');
  EXPECT_EQ(static_cast<size_t>(std::count(disk.operations.begin(), barrier, 'O')),
            disk.dataWrites.size());
  expectTargetBytes(disk, blocks, blockSize, expected, true);
  constexpr uintptr_t kSentinel = 0x12345000;
  const size_t dataCacheBlock = kTargetPage / kNativePageSize;
  Ext2FillCacheTestPeer::setDataCacheSentinel(file, dataCacheBlock, kSentinel);
  Ext2FillCacheTestPeer::triggerFillChecksum(file, kTargetPage);
  EXPECT_TRUE(Ext2FillCacheTestPeer::evictFillPage(file, kTargetPage));
  EXPECT_EQ(Ext2FillCacheTestPeer::dataCacheSentinel(file, dataCacheBlock), FILE_BAD_BLOCK);
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
    FillPageReference page(file, kTargetPage, true);
    ASSERT_NE(page.get(), 0U);
    std::fill(reinterpret_cast<uint8_t*>(page.get()),
              reinterpret_cast<uint8_t*>(page.get()) + kNativePageSize, 0xD7);
    page.syncAndReturn(kTargetPage + 91, false);

    const std::vector<uint64_t> locations = targetLocations(blocks, blockSize);
    expectProducerTransfers(disk, locations, blockSize, true);
    expectBalancedPins(disk, {});
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
    FillPageReference page(file, kTargetPage, true);
    ASSERT_NE(page.get(), 0U);
    std::fill(reinterpret_cast<uint8_t*>(page.get()),
              reinterpret_cast<uint8_t*>(page.get()) + kNativePageSize, 0xE3);
    std::fill(expected.begin(), expected.begin() + blockSize + (blockSize / 2), 0xE3);
    page.syncAndReturn(kTargetPage + 19, false);

    const std::vector<uint64_t> locations = targetLocations(blocks, blockSize);
    expectProducerTransfers(disk, locations, blockSize, true, fileSize - kTargetPage);
    expectBalancedPins(disk, {});
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
    FillPageReference page(*file, kTargetPage, true);
    ASSERT_NE(page.get(), 0U);
    for (size_t i = 0; i < expected.size(); ++i) {
      expected[i] = static_cast<uint8_t>(0xA0 + ((i / blockSize) * 3));
    }
    std::copy(expected.begin(), expected.end(), reinterpret_cast<uint8_t*>(page.get()));
  }
  disk.clearActivity();

  delete file;

  const std::vector<uint64_t> locations = targetLocations(blocks, blockSize);
  expectProducerTransfers(disk, locations, blockSize, true);
  expectBalancedPins(disk, {});
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

TEST_P(Ext2FillCacheWriteback, FailedConstituentSyncRetainsPageAndRetriesPartialProgress) {
  const uint32_t blockSize = GetParam();
  const auto blocks = makeBlocks(blockSize, TargetLayout::Noncontiguous);
  FillCacheDisk disk;
  std::vector<uint8_t> expected;
  initialiseBlocks(disk, blocks, blockSize, expected);
  Ext2Filesystem filesystem;
  Ext2FillCacheTestPeer::configure(filesystem, &disk, blockSize);
  Inode inode = makeSubpageInode(disk, blockSize, blocks);
  Ext2File file(String("fill-failure"), 3, &inode, &filesystem);
  Ext2FillCacheTestPeer::forceFillCache(file);
  uint8_t byte = 0;
  ASSERT_EQ(file.read(kTargetPage, 1, reinterpret_cast<uintptr_t>(&byte)), 1U);
  {
    FillPageReference page(file, kTargetPage, true);
    ASSERT_NE(page.get(), 0U);
    std::fill(expected.begin(), expected.end(), 0xED);
    std::copy(expected.begin(), expected.end(), reinterpret_cast<uint8_t*>(page.get()));
  }
  const auto locations = targetLocations(blocks, blockSize);
  disk.failedSyncLocation = locations[1];
  EXPECT_FALSE(file.sync(kTargetPage, false));
  EXPECT_TRUE(disk.persistedEquals(locations[0], expected.data(), blockSize));
  EXPECT_FALSE(disk.persistedEquals(locations[1], expected.data() + blockSize, blockSize));
  EXPECT_FALSE(file.sync(kTargetPage, false));
  EXPECT_FALSE(Ext2FillCacheTestPeer::evictFillPage(file, kTargetPage));
  EXPECT_TRUE(Ext2FillCacheTestPeer::fillPageExists(file, kTargetPage));
  EXPECT_EQ(disk.failedSyncs.size(), 3U);
  EXPECT_TRUE(disk.hasOnlyMetadataPins());
  disk.failedSyncLocation = ~uint64_t(0);
  EXPECT_TRUE(file.sync(kTargetPage, false));
  expectTargetBytes(disk, blocks, blockSize, expected, true);
  EXPECT_TRUE(Ext2FillCacheTestPeer::evictFillPage(file, kTargetPage));
  EXPECT_FALSE(disk.unbalancedUnpin);
}

TEST_P(Ext2FillCacheWriteback, FailedProducerWriteSurvivesLastCloseAndReopen) {
  const uint32_t blockSize = GetParam();
  const auto blocks = makeBlocks(blockSize, TargetLayout::Noncontiguous);
  FillCacheDisk disk;
  std::vector<uint8_t> expected;
  initialiseBlocks(disk, blocks, blockSize, expected);
  Ext2Filesystem filesystem;
  Ext2FillCacheTestPeer::configure(filesystem, &disk, blockSize);
  Inode inode = makeSubpageInode(disk, blockSize, blocks);
  const auto locations = targetLocations(blocks, blockSize);
  {
    Ext2File file(String("closed-failure"), 3, &inode, &filesystem);
    Ext2FillCacheTestPeer::forceFillCache(file);
    uint8_t byte = 0;
    ASSERT_EQ(file.read(kTargetPage, 1, reinterpret_cast<uintptr_t>(&byte)), 1U);
    {
      FillPageReference page(file, kTargetPage, true);
      ASSERT_NE(page.get(), 0U);
      std::fill(expected.begin(), expected.end(), 0xAB);
      std::copy(expected.begin(), expected.end(), reinterpret_cast<uint8_t*>(page.get()));
    }
    disk.failedWriteLocation = locations[0];
    EXPECT_FALSE(file.sync(kTargetPage, false));
  }
  Ext2File reopened(String("reopened"), 3, &inode, &filesystem);
  Ext2FillCacheTestPeer::forceFillCache(reopened);
  uint8_t byte = 0;
  ASSERT_EQ(reopened.read(kTargetPage, 1, reinterpret_cast<uintptr_t>(&byte)), 1U);
  EXPECT_EQ(byte, 0xAB);
  EXPECT_FALSE(reopened.sync(kTargetPage, false));
  disk.failedWriteLocation = ~uint64_t(0);
  EXPECT_TRUE(reopened.sync(kTargetPage, false));
  expectTargetBytes(disk, blocks, blockSize, expected, true);
  EXPECT_TRUE(disk.hasOnlyMetadataPins());
  EXPECT_FALSE(disk.unbalancedUnpin);
}

TEST_P(Ext2FillCacheWriteback, BackgroundRetryKeepsFailedAsynchronousDataDirty) {
  const uint32_t blockSize = GetParam();
  const auto blocks = makeBlocks(blockSize, TargetLayout::Noncontiguous);
  FillCacheDisk disk;
  std::vector<uint8_t> expected;
  initialiseBlocks(disk, blocks, blockSize, expected);
  Ext2Filesystem filesystem;
  Ext2FillCacheTestPeer::configure(filesystem, &disk, blockSize);
  Inode inode = makeSubpageInode(disk, blockSize, blocks);
  Ext2File file(String("async-failure"), 3, &inode, &filesystem);
  Ext2FillCacheTestPeer::forceFillCache(file);
  uint8_t byte = 0;
  ASSERT_EQ(file.read(kTargetPage, 1, reinterpret_cast<uintptr_t>(&byte)), 1U);
  {
    FillPageReference page(file, kTargetPage, true);
    ASSERT_NE(page.get(), 0U);
    std::fill(expected.begin(), expected.end(), 0xD9);
    std::copy(expected.begin(), expected.end(), reinterpret_cast<uint8_t*>(page.get()));
  }
  const auto locations = targetLocations(blocks, blockSize);
  disk.failedSyncLocation = locations[0];
  // Standalone CacheManager executes accepted asynchronous work inline. Its
  // return value remains admission success; the failed page must stay dirty.
  EXPECT_TRUE(file.sync(kTargetPage, true));
  ASSERT_EQ(disk.failedSyncs.size(), 1U);
  EXPECT_FALSE(Ext2FillCacheTestPeer::evictFillPage(file, kTargetPage));
  const uint64_t period = CACHE_WRITEBACK_PERIOD * 1000000ULL;
  CacheManager::instance().timer(period);
  const size_t attempts = disk.failedSyncs.size();
  CacheManager::instance().timer(period);
  EXPECT_EQ(disk.failedSyncs.size(), attempts + 1);
  EXPECT_TRUE(Ext2FillCacheTestPeer::fillPageExists(file, kTargetPage));
  disk.failedSyncLocation = ~uint64_t(0);
  CacheManager::instance().timer(period);
  expectTargetBytes(disk, blocks, blockSize, expected, true);
  EXPECT_TRUE(Ext2FillCacheTestPeer::evictFillPage(file, kTargetPage));
  EXPECT_TRUE(disk.hasOnlyMetadataPins());
  EXPECT_FALSE(disk.unbalancedUnpin);
}

TEST_P(Ext2FillCacheWriteback, FullSyncReportsInodeFlushFailureAndRetries) {
  const uint32_t blockSize = GetParam();
  const auto blocks = makeBlocks(blockSize, TargetLayout::Noncontiguous);
  FillCacheDisk disk;
  std::vector<uint8_t> expected;
  initialiseBlocks(disk, blocks, blockSize, expected);
  const Inode original = makeSubpageInode(disk, blockSize, blocks);
  {
    Ext2Filesystem filesystem;
    Inode* inode = Ext2FillCacheTestPeer::configureMetadata(filesystem, disk, blockSize, original);
    Ext2File file(String("metadata-failure"), 3, inode, &filesystem);
    Ext2FillCacheTestPeer::forceFillCache(file);
    uint8_t byte = 0;
    ASSERT_EQ(file.read(kTargetPage, 1, reinterpret_cast<uintptr_t>(&byte)), 1U);
    {
      FillPageReference page(file, kTargetPage, true);
      ASSERT_NE(page.get(), 0U);
      std::fill(expected.begin(), expected.end(), 0xBC);
      std::copy(expected.begin(), expected.end(), reinterpret_cast<uint8_t*>(page.get()));
    }
    disk.failedSyncLocation = 5 * blockSize;
    EXPECT_FALSE(file.sync());
    expectTargetBytes(disk, blocks, blockSize, expected, true);
    EXPECT_FALSE(file.sync());
    disk.failedSyncLocation = 3 * blockSize;
    EXPECT_FALSE(file.sync());
    EXPECT_FALSE(file.sync());
    if (blocks.size() > 12) {
      disk.failedSyncLocation = kIndirectBlock * blockSize;
      EXPECT_FALSE(file.sync());
    }
    disk.failedSyncLocation = ~uint64_t(0);
    EXPECT_TRUE(file.sync());
    EXPECT_TRUE(disk.persistedEquals(5 * blockSize + 2 * sizeof(Inode),
                                     reinterpret_cast<const uint8_t*>(inode), sizeof(Inode)));
  }
  EXPECT_TRUE(disk.hasNoPins());
  EXPECT_FALSE(disk.unbalancedUnpin);
}

#if PEDIGREE_TARGET_PAGE_SIZE > 2048
INSTANTIATE_TEST_CASE_P(BlockSizes, Ext2FillCacheWriteback, ::testing::Values(1024U, 2048U));
#elif PEDIGREE_TARGET_PAGE_SIZE > 1024
INSTANTIATE_TEST_CASE_P(BlockSizes, Ext2FillCacheWriteback, ::testing::Values(1024U));
#endif
