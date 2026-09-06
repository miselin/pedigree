/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/TargetInfo.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "Ext2FillCacheTestDisk.h"
#include "modules/system/ext2/Ext2File.h"
#include "modules/system/ext2/Ext2Filesystem.h"
#include "modules/system/ext2/ext2.h"
#include "modules/system/ramfs/RamFs.h"
#include <gtest/gtest.h>

class Ext2AllocationTestPeer {
 public:
  static Inode* configure(Ext2Filesystem& fs, FillCacheDisk& disk) {
    fs.m_pDisk = &disk;
    fs.m_BlockSize = 1024;
    fs.m_InodeSize = sizeof(Inode);
    fs.m_nGroupDescriptors = 1;
    fs.m_pSuperblock = disk.read(1024).as<Superblock>();
    fs.m_pSuperblock->s_rev_level = HOST_TO_LITTLE32(1);
    fs.m_pSuperblock->s_blocks_count = HOST_TO_LITTLE32(128);
    fs.m_pSuperblock->s_blocks_per_group = HOST_TO_LITTLE32(128);
    fs.m_pSuperblock->s_inodes_count = HOST_TO_LITTLE32(8);
    fs.m_pSuperblock->s_inodes_per_group = HOST_TO_LITTLE32(8);
    fs.m_pSuperblock->s_first_data_block = HOST_TO_LITTLE32(1);
    auto* group = disk.read(2048).as<GroupDesc>();
    group->bg_block_bitmap = HOST_TO_LITTLE32(3);
    group->bg_inode_bitmap = HOST_TO_LITTLE32(4);
    group->bg_inode_table = HOST_TO_LITTLE32(5);
    fs.m_pGroupDescriptors = new GroupDesc*[1]{group};
    fs.m_pBlockBitmaps = new Vector<size_t>[1];
    fs.m_pInodeBitmaps = new Vector<size_t>[1];
    fs.m_pInodeTables = new Vector<size_t>[1];
    fs.m_pBlockBitmaps[0].pushBack(disk.read(3072).address());
    fs.m_pInodeBitmaps[0].pushBack(disk.read(4096).address());
    fs.m_pInodeTables[0].pushBack(disk.read(5120).address());
    disk.metadataLocations = {1024, 2048, 3072, 4096, 5120};
    disk.metadataReferences = disk.pageReferences;
    std::fill_n(disk.storage.data() + 3072, 2048, 0xff);
    auto* inode = reinterpret_cast<Inode*>(fs.m_pInodeTables[0][0]) + 2;
    inode->i_mode = HOST_TO_LITTLE16(EXT2_S_IFREG | 0600);
    inode->i_links_count = HOST_TO_LITTLE16(1);
    return inode;
  }

  static void freeBlock(Ext2Filesystem& fs, FillCacheDisk& disk, uint32_t block) {
    const size_t bit = block - 1;
    disk.storage[3072 + bit / 8] &= ~(1U << (bit % 8));
    ++fs.m_pSuperblock->s_free_blocks_count;
    ++fs.m_pGroupDescriptors[0]->bg_free_blocks_count;
  }

  static size_t freeBlocks(Ext2Filesystem& fs) {
    return LITTLE_TO_HOST32(fs.m_pSuperblock->s_free_blocks_count);
  }

  static void unloadBitmap(Ext2Filesystem& fs, FillCacheDisk& disk) {
    fs.m_pBlockBitmaps[0].clear();
    disk.unpin(3072);
    disk.metadataReferences = disk.pageReferences;
  }

  static void readOnly(Ext2Filesystem& fs, bool value) {
    fs.m_bReadOnly = value;
  }
};

namespace {
constexpr size_t BlockSize = 1024;

class AllocationFile final : public Ext2File {
 public:
  using Ext2File::Ext2File;

  void poisonCachedTail(size_t offset) {
    LockGuard<Mutex> data(m_State->dataLock);
    const uintptr_t page = cacheState().fill.lookup(0);
    ASSERT_NE(page, 0U);
    std::fill_n(reinterpret_cast<uint8_t*>(page + offset), TargetInfo::getPageSize() - offset,
                0xe7);
    cacheState().fill.release(0);
  }

 protected:
  bool useFillCache() const override {
    return true;
  }
};

struct AllocationFixture {
  explicit AllocationFixture(bool withPrefix = false, bool freeSpace = true) {
    inode = Ext2AllocationTestPeer::configure(fs, disk);
    if (freeSpace) {
      for (uint32_t block = 16; block < 128; ++block)
        Ext2AllocationTestPeer::freeBlock(fs, disk, block);
    }
    if (withPrefix) {
      inode->i_size = HOST_TO_LITTLE32(13);
      inode->i_blocks = HOST_TO_LITTLE32(2);
      inode->i_block[0] = HOST_TO_LITTLE32(8);
      disk.fill(8 * BlockSize, BlockSize, 0x59);
    }
    file = reopen();
  }

  std::unique_ptr<AllocationFile> reopen() {
    return std::make_unique<AllocationFile>(String("allocation"), 3, inode, &fs);
  }

  FillCacheDisk disk;
  Ext2Filesystem fs;
  Inode* inode;
  std::unique_ptr<AllocationFile> file;
};
}  // namespace

TEST(FileAllocation, Ext2RetainsDirectAndIndirectReservationAcrossAliasClose) {
  AllocationFixture fixture(true);
  const size_t offset = 13 * BlockSize + 7, length = BlockSize + 5;
  ASSERT_TRUE(fixture.file->allocateRange(offset, length, true));
  EXPECT_EQ(fixture.file->getSize(), 13U);
  EXPECT_EQ(fixture.file->getAttributes().blocks, 8U);
  EXPECT_EQ(fixture.inode->i_block[1], 0U);
  const size_t available = Ext2AllocationTestPeer::freeBlocks(fixture.fs);
  fixture.file.reset();
  fixture.file = fixture.reopen();
  ASSERT_TRUE(fixture.file->allocateRange(offset, length, true));
  EXPECT_EQ(Ext2AllocationTestPeer::freeBlocks(fixture.fs), available);
  ASSERT_TRUE(fixture.file->allocateRange(offset, length));
  EXPECT_EQ(fixture.file->getSize(), offset + length);
  std::vector<uint8_t> bytes(offset + length, 0xcc);
  ASSERT_EQ(fixture.file->read(0, bytes.size(), reinterpret_cast<uintptr_t>(bytes.data())),
            bytes.size());
  EXPECT_TRUE(std::all_of(bytes.begin(), bytes.begin() + 13, [](uint8_t b) { return b == 0x59; }));
  EXPECT_TRUE(std::all_of(bytes.begin() + 13, bytes.end(), [](uint8_t b) { return b == 0; }));
  const uint8_t value = 0x83;
  ASSERT_EQ(fixture.file->write(offset, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  EXPECT_EQ(Ext2AllocationTestPeer::freeBlocks(fixture.fs), available);
  EXPECT_FALSE(fixture.disk.unbalancedUnpin);
}

TEST(FileAllocation, Ext2ZeroesCachedEofTailWithoutChangingVisiblePrefix) {
  AllocationFixture fixture(true);
  ASSERT_EQ(fixture.file->read(0, 13, 0), 13U);
  fixture.file->poisonCachedTail(13);
  ASSERT_TRUE(fixture.file->allocateRange(13, 2 * BlockSize));
  std::vector<uint8_t> bytes(13 + 2 * BlockSize, 0xcc);
  ASSERT_EQ(fixture.file->read(0, bytes.size(), reinterpret_cast<uintptr_t>(bytes.data())),
            bytes.size());
  EXPECT_TRUE(std::all_of(bytes.begin(), bytes.begin() + 13, [](uint8_t b) { return b == 0x59; }));
  EXPECT_TRUE(std::all_of(bytes.begin() + 13, bytes.end(), [](uint8_t b) { return b == 0; }));
}

TEST(FileAllocation, Ext2PartialNoSpaceRetainsAccountedBlocksAndRetriesWithoutGrowingEof) {
  AllocationFixture fixture(false, false);
  Ext2AllocationTestPeer::freeBlock(fixture.fs, fixture.disk, 16);
  EXPECT_FALSE(fixture.file->allocateRange(0, 2 * BlockSize));
  EXPECT_EQ(fixture.file->getSize(), 0U);
  EXPECT_EQ(fixture.file->getAttributes().blocks, 2U);
  EXPECT_EQ(Ext2AllocationTestPeer::freeBlocks(fixture.fs), 0U);
  const uint32_t firstBlock = fixture.inode->i_block[0];
  fixture.file.reset();
  fixture.file = fixture.reopen();
  Ext2AllocationTestPeer::freeBlock(fixture.fs, fixture.disk, 17);
  ASSERT_TRUE(fixture.file->allocateRange(0, 2 * BlockSize));
  EXPECT_EQ(fixture.inode->i_block[0], firstBlock);
  EXPECT_EQ(fixture.file->getSize(), 2 * BlockSize);
  EXPECT_EQ(fixture.file->getAttributes().blocks, 4U);
  EXPECT_EQ(Ext2AllocationTestPeer::freeBlocks(fixture.fs), 0U);
  ASSERT_TRUE(fixture.file->resize(0));
  EXPECT_EQ(fixture.file->getAttributes().blocks, 0U);
  EXPECT_EQ(Ext2AllocationTestPeer::freeBlocks(fixture.fs), 2U);
}

TEST(FileAllocation, Ext2FailedPayloadReadReturnsAllocationAndPreservesData) {
  AllocationFixture fixture(true);
  const size_t available = Ext2AllocationTestPeer::freeBlocks(fixture.fs);
  fixture.disk.failedReadLocation = 16 * BlockSize;
  EXPECT_FALSE(fixture.file->allocateRange(BlockSize, BlockSize));
  EXPECT_EQ(fixture.file->getSize(), 13U);
  EXPECT_EQ(fixture.file->getAttributes().blocks, 2U);
  EXPECT_EQ(Ext2AllocationTestPeer::freeBlocks(fixture.fs), available);
  EXPECT_TRUE(fixture.disk.hasOnlyMetadataPins());
  fixture.disk.failedReadLocation = ~uint64_t(0);
  ASSERT_TRUE(fixture.file->allocateRange(BlockSize, BlockSize));
  EXPECT_EQ(fixture.file->getAttributes().blocks, 4U);
  EXPECT_EQ(Ext2AllocationTestPeer::freeBlocks(fixture.fs), available - 1);
}

TEST(FileAllocation, Ext2FailedBitmapReadCannotChangeReservationOrEof) {
  AllocationFixture fixture(true);
  const size_t available = Ext2AllocationTestPeer::freeBlocks(fixture.fs);
  Ext2AllocationTestPeer::unloadBitmap(fixture.fs, fixture.disk);
  fixture.disk.failedReadLocation = 3072;
  EXPECT_FALSE(fixture.file->allocateRange(BlockSize, BlockSize));
  EXPECT_EQ(fixture.file->getSize(), 13U);
  EXPECT_EQ(fixture.file->getAttributes().blocks, 2U);
  EXPECT_EQ(Ext2AllocationTestPeer::freeBlocks(fixture.fs), available);
  EXPECT_TRUE(fixture.disk.hasOnlyMetadataPins());
  fixture.disk.failedReadLocation = ~uint64_t(0);
  ASSERT_TRUE(fixture.file->allocateRange(BlockSize, BlockSize));
  EXPECT_EQ(fixture.file->getAttributes().blocks, 4U);
}

TEST(FileAllocation, Ext2RejectsReadOnlyOverflowAndEmptyRangeWithoutReservation) {
  AllocationFixture fixture(true);
  const size_t available = Ext2AllocationTestPeer::freeBlocks(fixture.fs);
  EXPECT_FALSE(fixture.file->allocateRange(0, 0));
  EXPECT_FALSE(fixture.file->allocateRange(~size_t(0), 2));
  EXPECT_FALSE(fixture.file->allocateRange(fixture.file->maximumFileSize(), 1));
  Ext2AllocationTestPeer::readOnly(fixture.fs, true);
  EXPECT_FALSE(fixture.file->allocateRange(BlockSize, BlockSize));
  Ext2AllocationTestPeer::readOnly(fixture.fs, false);
  EXPECT_EQ(fixture.file->getSize(), 13U);
  EXPECT_EQ(Ext2AllocationTestPeer::freeBlocks(fixture.fs), available);
}

TEST(FileAllocation, TruncateAtUnchangedEofReleasesInvisibleReservations) {
  AllocationFixture fixture;
  const size_t available = Ext2AllocationTestPeer::freeBlocks(fixture.fs);
  ASSERT_TRUE(fixture.file->allocateRange(13 * BlockSize, BlockSize, true));
  EXPECT_EQ(fixture.file->getSize(), 0U);
  EXPECT_EQ(fixture.file->getAttributes().blocks, 4U);
  ASSERT_TRUE(fixture.file->resize(0));
  EXPECT_EQ(fixture.file->getAttributes().blocks, 0U);
  EXPECT_EQ(Ext2AllocationTestPeer::freeBlocks(fixture.fs), available);

  RamFs filesystem;
  filesystem.setProcessOwnership(false);
  RamFile ram(String("invisible-reservation"), 1, &filesystem, nullptr);
  ASSERT_TRUE(ram.allocateRange(2 * ram.getBlockSize(), ram.getBlockSize(), true));
  EXPECT_EQ(ram.getSize(), 0U);
  EXPECT_GT(ram.getAttributes().blocks, 0U);
  ASSERT_TRUE(ram.resize(0));
  EXPECT_EQ(ram.getAttributes().blocks, 0U);
}

TEST(FileAllocation, RamReservationOwnsPagesAndPreservesPrefixThroughGrowthAndShrink) {
  RamFs filesystem;
  filesystem.setProcessOwnership(false);
  RamFile file(String("ram-allocation"), 1, &filesystem, nullptr);
  const size_t pageSize = file.getBlockSize();
  const uint8_t prefix[] = {1, 3, 5, 7, 9};
  ASSERT_EQ(file.write(0, sizeof(prefix), reinterpret_cast<uintptr_t>(prefix)), sizeof(prefix));
  ASSERT_TRUE(file.allocateRange(3 * pageSize + 7, pageSize, true));
  EXPECT_EQ(file.getSize(), sizeof(prefix));
  EXPECT_EQ(file.getAttributes().blocks, 3 * pageSize / 512);
  ASSERT_TRUE(file.allocateRange(3 * pageSize + 7, pageSize));
  EXPECT_EQ(file.getSize(), 4 * pageSize + 7);
  EXPECT_EQ(file.getAttributes().blocks, 3 * pageSize / 512);
  std::vector<uint8_t> bytes(file.getSize(), 0xcc);
  ASSERT_EQ(file.read(0, bytes.size(), reinterpret_cast<uintptr_t>(bytes.data())), bytes.size());
  EXPECT_TRUE(std::equal(prefix, prefix + sizeof(prefix), bytes.begin()));
  EXPECT_TRUE(
      std::all_of(bytes.begin() + sizeof(prefix), bytes.end(), [](uint8_t b) { return b == 0; }));
  ASSERT_TRUE(file.resize(0));
  EXPECT_EQ(file.getAttributes().blocks, 0U);
}
