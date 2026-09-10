/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/errors.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "modules/drivers/common/partition/Partition.h"
#include "modules/system/ext2/Ext2Directory.h"
#include "modules/system/ext2/Ext2File.h"
#include "modules/system/ext2/Ext2Filesystem.h"
#include "modules/system/ext2/ext2.h"
#include <gtest/gtest.h>

class Ext2WritebackTestPeer {
 public:
  static void configure(Ext2Filesystem& filesystem, Disk* disk, uint32_t blockSize) {
    filesystem.m_pDisk = disk;
    filesystem.m_BlockSize = blockSize;
  }

  static void configureDirectory(Ext2Filesystem& filesystem, Disk* disk, Superblock* superblock,
                                 uint32_t blockSize) {
    filesystem.m_pDisk = disk;
    filesystem.m_pSuperblock = superblock;
    filesystem.m_BlockSize = blockSize;
  }

  static void configureInodeRelease(Ext2Filesystem& filesystem, Disk* disk, Superblock* superblock,
                                    GroupDesc* groupDescriptor, uintptr_t inodeBitmap,
                                    uintptr_t inodeTableBlock0, uintptr_t inodeTableBlock1) {
    filesystem.m_pDisk = disk;
    filesystem.m_pSuperblock = superblock;
    filesystem.m_BlockSize = 4096;
    filesystem.m_InodeSize = sizeof(Inode);
    filesystem.m_nGroupDescriptors = 1;

    filesystem.m_pGroupDescriptors = new GroupDesc*[1];
    filesystem.m_pGroupDescriptors[0] = groupDescriptor;
    filesystem.m_pBlockBitmaps = new Vector<size_t>[1];
    filesystem.m_pInodeBitmaps = new Vector<size_t>[1];
    filesystem.m_pInodeBitmaps[0].pushBack(inodeBitmap);
    filesystem.m_pInodeTables = new Vector<size_t>[1];
    filesystem.m_pInodeTables[0].pushBack(inodeTableBlock0);
    filesystem.m_pInodeTables[0].pushBack(inodeTableBlock1);
  }

  static bool releaseInode(Ext2Filesystem& filesystem, uint32_t inode) {
    return filesystem.releaseInode(inode);
  }

  static void linkInode(Ext2Filesystem& filesystem, uint32_t inode) {
    filesystem.increaseInodeRefcount(inode);
  }

  static uint32_t findFreeBlock(Ext2Filesystem& filesystem, uint32_t inode) {
    return filesystem.findFreeBlock(inode);
  }

  static uint32_t findFreeInode(Ext2Filesystem& filesystem) {
    return filesystem.findFreeInode();
  }

  static Inode* getInode(Ext2Filesystem& filesystem, uint32_t inode) {
    return filesystem.getInode(inode);
  }

  static Ext2InodeState* retainedState(Ext2Filesystem& filesystem, uint32_t inode) {
    return filesystem.m_InodeStates.lookup(inode);
  }

  static bool createFile(Ext2Filesystem& filesystem, File* parent, const String& name) {
    return filesystem.createFile(parent, name, 0644);
  }

  static bool createDirectory(Ext2Filesystem& filesystem, File* parent, const String& name) {
    return filesystem.createDirectory(parent, name, 0755);
  }

  static void configureGrowth(Ext2Filesystem& filesystem, Disk* disk, Superblock* superblock,
                              GroupDesc* groupDescriptor) {
    filesystem.m_pDisk = disk;
    filesystem.m_pSuperblock = superblock;
    filesystem.m_BlockSize = 4096;
    filesystem.m_InodeSize = sizeof(Inode);
    filesystem.m_nGroupDescriptors = 1;

    filesystem.m_pGroupDescriptors = new GroupDesc*[1];
    filesystem.m_pGroupDescriptors[0] = groupDescriptor;
    filesystem.m_pBlockBitmaps = new Vector<size_t>[1];
    filesystem.m_pInodeBitmaps = new Vector<size_t>[1];
    filesystem.m_pInodeTables = new Vector<size_t>[1];
  }
};

uint32_t getUnixTimestamp() {
  return 0;
}

namespace {
constexpr uint32_t kBlockSize = 4096;

class TrackingDisk final : public Disk {
 public:
  BufferView read(uint64_t location) override {
    reads.push_back(location);
    return BufferView();
  }

  void write(uint64_t location) override {
    writes.push_back(location);
  }

  void align(uint64_t location) override {
    alignments.push_back(location);
  }

  void flush(uint64_t location) override {
    flushes.push_back(location);
  }

  bool sync(uint64_t location, bool async) override {
    if (async) {
      write(location);
    } else {
      flush(location);
    }
    return syncResult;
  }

  bool syncPages(const uint64_t* locations, size_t count) override {
    batchSizes.push_back(count);
    return Disk::syncPages(locations, count);
  }

  bool retireCachePage(uint64_t location) override {
    retirements.push_back(location);
    return retirementResult;
  }

  size_t getSize() const override {
    return static_cast<size_t>(-1);
  }

  size_t getBlockSize() const override {
    return kBlockSize;
  }

  bool pin(uint64_t) override {
    return true;
  }

  void unpin(uint64_t) override {}

  std::vector<size_t> batchSizes;
  std::vector<uint64_t> writes;
  std::vector<uint64_t> flushes;
  std::vector<uint64_t> retirements;
  std::vector<uint64_t> alignments;
  std::vector<uint64_t> reads;
  bool retirementResult = true;
  bool syncResult = true;
};

class GrowthDisk final : public Disk {
 public:
  static constexpr size_t kBlockCount = 32768;

  GrowthDisk() : blocks(kBlockCount) {}

  BufferView read(uint64_t location) override {
    if (location >= getSize()) {
      return BufferView();
    }
    reads.push_back(location);
    if (location == failedRead)
      return BufferView();
    const size_t block = static_cast<size_t>(location / kBlockSize);
    const size_t offset = static_cast<size_t>(location % kBlockSize);
    uint8_t* data = getBlock(block);
    return BufferView(data + offset, kBlockSize - offset);
  }

  void write(uint64_t location) override {
    writes.push_back(location);
  }

  size_t getSize() const override {
    return kBlockCount * kBlockSize;
  }

  size_t getBlockSize() const override {
    return kBlockSize;
  }

  bool pin(uint64_t location) override {
    return location < getSize();
  }

  void unpin(uint64_t location) override {
    unpins.push_back(location);
  }

  uint8_t* getBlock(size_t block) {
    if (!blocks[block]) {
      blocks[block] = std::unique_ptr<uint8_t[]>(new uint8_t[kBlockSize]());
    }
    return blocks[block].get();
  }

  std::vector<std::unique_ptr<uint8_t[]>> blocks;
  std::vector<uint64_t> reads;
  std::vector<uint64_t> unpins;
  uint64_t failedRead = UINT64_MAX;
  std::vector<uint64_t> writes;
};

struct MutableInodeFixture {
  MutableInodeFixture() {
    superblock.s_blocks_per_group = HOST_TO_LITTLE32(GrowthDisk::kBlockCount);
    superblock.s_inodes_per_group = HOST_TO_LITTLE32(32);
    superblock.s_free_blocks_count = HOST_TO_LITTLE32(GrowthDisk::kBlockCount - 16);
    group.bg_block_bitmap = HOST_TO_LITTLE32(2);
    group.bg_inode_bitmap = HOST_TO_LITTLE32(3);
    group.bg_inode_table = HOST_TO_LITTLE32(4);
    group.bg_free_blocks_count = HOST_TO_LITTLE16(GrowthDisk::kBlockCount - 16);
    std::fill(disk.getBlock(2), disk.getBlock(2) + 2, 0xff);
    Ext2WritebackTestPeer::configureGrowth(filesystem, &disk, &superblock, &group);
  }
  GrowthDisk disk;
  Superblock superblock = {};
  GroupDesc group = {};
  Ext2Filesystem filesystem;
};

class OrderedSyncFile final : public File {
 public:
  using File::sync;

  bool sync(size_t offset, bool async) override {
    syncOffset = offset;
    syncAsync = async;
    syncSawPin = pinned;
    order.push_back('S');
    return true;
  }

  void returnPhysicalPage(size_t offset) override {
    returnOffset = offset;
    pinned = false;
    order.push_back('R');
  }

  bool pinned = true;
  bool syncSawPin = false;
  bool syncAsync = false;
  size_t syncOffset = 0;
  size_t returnOffset = 0;
  std::vector<char> order;
};

Inode makeInode(uint32_t synchronousBlock, uint32_t asynchronousBlock) {
  Inode inode = {};
  inode.i_mode = HOST_TO_LITTLE16(EXT2_S_IFREG);
  inode.i_size = HOST_TO_LITTLE32(3 * kBlockSize);
  inode.i_blocks = HOST_TO_LITTLE32((3 * kBlockSize) / 512);
  inode.i_block[0] = HOST_TO_LITTLE32(synchronousBlock);
  inode.i_block[1] = HOST_TO_LITTLE32(asynchronousBlock);
  inode.i_block[2] = 0;
  return inode;
}
}  // namespace

TEST(Ext2Writeback, UsesPhysicalBlockNumberExactlyOnce) {
  constexpr uint32_t kSynchronousBlock = (UINT32_C(1) << 20) + 3;
  constexpr uint32_t kAsynchronousBlock = kSynchronousBlock + 5;

  TrackingDisk disk;
  Ext2Filesystem filesystem;
  Ext2WritebackTestPeer::configure(filesystem, &disk, kBlockSize);
  Inode inode = makeInode(kSynchronousBlock, kAsynchronousBlock);
  Ext2File file(String("mapped"), 3, &inode, &filesystem);

  file.sync(137, false);
  ASSERT_EQ(disk.flushes.size(), 1U);
  EXPECT_EQ(disk.flushes[0], static_cast<uint64_t>(kSynchronousBlock) * kBlockSize);
  EXPECT_TRUE(disk.writes.empty());

  file.sync(kBlockSize + 511, true);
  ASSERT_EQ(disk.writes.size(), 1U);
  EXPECT_EQ(disk.writes[0], static_cast<uint64_t>(kAsynchronousBlock) * kBlockSize);
  EXPECT_EQ(disk.flushes.size(), 1U);

  file.sync((2 * kBlockSize) + 29, false);
  file.sync((2 * kBlockSize) + 31, true);
  file.sync(3 * kBlockSize, false);
  EXPECT_EQ(disk.flushes.size(), 1U);
  EXPECT_EQ(disk.writes.size(), 1U);
}

TEST(Ext2Writeback, ReportsBackendSyncFailureOnEveryRetry) {
  TrackingDisk disk;
  Ext2Filesystem filesystem;
  Ext2WritebackTestPeer::configure(filesystem, &disk, kBlockSize);
  Inode inode = makeInode(127, 128);
  Ext2File file(String("sync-failure"), 3, &inode, &filesystem);
  disk.syncResult = false;
  EXPECT_FALSE(file.sync(0, false));
  EXPECT_FALSE(file.sync(0, false));
  EXPECT_FALSE(file.sync(kBlockSize, true));
  EXPECT_EQ(disk.flushes.size(), 2U);
  disk.syncResult = true;
  EXPECT_TRUE(file.sync(0, false));
  EXPECT_EQ(disk.flushes.size(), 3U);
}

TEST(Ext2Writeback, KeepsNativeBlockWritePath) {
  constexpr uint32_t kPhysicalBlock = 127;
  uint8_t source[kBlockSize] = {};
  TrackingDisk disk;
  Ext2Filesystem filesystem;
  Ext2WritebackTestPeer::configure(filesystem, &disk, kBlockSize);
  Inode inode = makeInode(kPhysicalBlock, kPhysicalBlock + 1);
  Ext2File file(String("native-write"), 3, &inode, &filesystem);

  file.writeBlock(0, reinterpret_cast<uintptr_t>(source));

  EXPECT_TRUE(disk.reads.empty());
  EXPECT_EQ(disk.writes,
            std::vector<uint64_t>({static_cast<uint64_t>(kPhysicalBlock) * kBlockSize}));
}

TEST(Ext2Writeback, ReleasesClosedBlockMapsWithoutRecyclingIdentity) {
  constexpr uint32_t kPhysicalBlock = 127;
  GrowthDisk disk;
  disk.getBlock(kPhysicalBlock)[0] = 0xA7;
  Ext2Filesystem filesystem;
  Ext2WritebackTestPeer::configure(filesystem, &disk, kBlockSize);
  Inode inode = makeInode(kPhysicalBlock, kPhysicalBlock + 1);
  inode.i_links_count = HOST_TO_LITTLE16(1);
  uintptr_t identity = 0;
  {
    Ext2File file(String("first-alias"), 3, &inode, &filesystem);
    identity = file.futexIdentity();
    uint8_t value = 0;
    ASSERT_EQ(file.read(0, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
    EXPECT_EQ(value, 0xA7);
  }

  Ext2InodeState* state = Ext2WritebackTestPeer::retainedState(filesystem, 3);
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->references, 0U);
  EXPECT_EQ(state->blocks.size(), 0U);
  EXPECT_EQ(state->files.size(), 0U);
  EXPECT_NE(identity, 0U);
  EXPECT_EQ(state->futexIdentity, identity);

  {
    Ext2File reopened(String("second-alias"), 3, &inode, &filesystem);
    EXPECT_EQ(reopened.futexIdentity(), identity);
    EXPECT_EQ(reopened.getSize(), 3 * kBlockSize);
    EXPECT_EQ(state->blocks.count(), 3U);
    uint8_t value = 0;
    ASSERT_EQ(reopened.read(0, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
    EXPECT_EQ(value, 0xA7);
  }
  EXPECT_EQ(state->references, 0U);
  EXPECT_EQ(state->blocks.size(), 0U);
}

TEST(Ext2Metadata, HardlinkAliasesObserveMaskedUpdatesAndLinkCounts) {
  MutableInodeFixture fixture;
  Inode* inode = Ext2WritebackTestPeer::getInode(fixture.filesystem, 3);
  ASSERT_NE(inode, nullptr);
  inode->i_mode = HOST_TO_LITTLE16(EXT2_S_IFREG | 0600);
  inode->i_links_count = HOST_TO_LITTLE16(2);
  inode->i_uid = HOST_TO_LITTLE16(111);
  inode->i_gid = HOST_TO_LITTLE16(222);
  Ext2File first(String("first"), 3, inode, &fixture.filesystem);
  Ext2File second(String("second"), 3, inode, &fixture.filesystem);
  constexpr uint32_t permissions = FILE_UR | FILE_UW | FILE_GR | FILE_STICKY;
  first.setPermissions(permissions);
  second.setUid(333);
  first.setModifiedTime(77);
  second.setGid(444);
  first.setAccessedTime(66);
  second.setCreationTime(55);
  for (File* alias : {static_cast<File*>(&first), static_cast<File*>(&second)}) {
    const File::Attributes attributes = alias->getAttributes();
    EXPECT_EQ(attributes.permissions, permissions);
    EXPECT_EQ(attributes.uid, 333U);
    EXPECT_EQ(attributes.gid, 444U);
    EXPECT_EQ(attributes.modified, 77U);
    EXPECT_EQ(attributes.accessed, 66U);
    EXPECT_EQ(attributes.changed, 55U);
    EXPECT_EQ(attributes.links, 2U);
  }
  Ext2WritebackTestPeer::linkInode(fixture.filesystem, 3);
  EXPECT_EQ(first.getAttributes().links, 3U);
  EXPECT_FALSE(Ext2WritebackTestPeer::releaseInode(fixture.filesystem, 3));
  EXPECT_EQ(second.getAttributes().links, 2U);
  EXPECT_EQ(LITTLE_TO_HOST16(inode->i_mode), EXT2_S_IFREG | 01640);
}

TEST(Ext2Growth, SparseWritesMaterializeDirectAndMissingIndirectPaths) {
  MutableInodeFixture fixture;
  Inode* inode = Ext2WritebackTestPeer::getInode(fixture.filesystem, 3);
  ASSERT_NE(inode, nullptr);
  const size_t indices[] = {3, 14, 12 + kBlockSize / sizeof(uint32_t) + 1};
  const size_t extent = (indices[2] + 1) * kBlockSize;
  inode->i_mode = HOST_TO_LITTLE16(EXT2_S_IFREG | 0600);
  inode->i_links_count = HOST_TO_LITTLE16(1);
  inode->i_size = HOST_TO_LITTLE32(extent);
  {
    Ext2File file(String("sparse"), 3, inode, &fixture.filesystem);
    for (size_t index : indices) {
      uint8_t value = 0xff;
      ASSERT_EQ(file.read(index * kBlockSize + 17, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
      EXPECT_EQ(value, 0U);
    }
    EXPECT_EQ(file.getAttributes().blocks, 0U);
    for (size_t index : indices) {
      const uint8_t value = 0x6b;
      ASSERT_EQ(file.write(index * kBlockSize + 17, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
    }
    EXPECT_EQ(file.getAttributes().blocks, 6U * (kBlockSize / 512));
    EXPECT_EQ(file.getSize(), extent);
  }
  Ext2File reopened(String("sparse-alias"), 3, inode, &fixture.filesystem);
  for (size_t index : indices) {
    uint8_t values[3] = {};
    ASSERT_EQ(reopened.read(index * kBlockSize + 16, 3, reinterpret_cast<uintptr_t>(values)), 3U);
    EXPECT_EQ(values[0], 0U);
    EXPECT_EQ(values[1], 0x6b);
    EXPECT_EQ(values[2], 0U);
  }
  EXPECT_EQ(reopened.getAttributes().blocks, 6U * (kBlockSize / 512));
  ASSERT_TRUE(reopened.prepareSharedMapping(7 * kBlockSize, kBlockSize));
  EXPECT_EQ(reopened.getAttributes().blocks, 7U * (kBlockSize / 512));
}

TEST(Ext2Metadata, OrdinaryWritesShareTimesAndAllocationAcrossHardlinks) {
  MutableInodeFixture fixture;
  Inode* inode = Ext2WritebackTestPeer::getInode(fixture.filesystem, 3);
  ASSERT_NE(inode, nullptr);
  inode->i_mode = HOST_TO_LITTLE16(EXT2_S_IFREG | 0600);
  inode->i_links_count = HOST_TO_LITTLE16(2);
  inode->i_size = HOST_TO_LITTLE32(2 * kBlockSize);
  inode->i_atime = HOST_TO_LITTLE32(11);
  inode->i_mtime = HOST_TO_LITTLE32(22);
  inode->i_ctime = HOST_TO_LITTLE32(33);
  Ext2File first(String("first"), 3, inode, &fixture.filesystem);
  Ext2File second(String("second"), 3, inode, &fixture.filesystem);
  const uint8_t value = 0x6b;
  EXPECT_EQ(second.write(17, 0, reinterpret_cast<uintptr_t>(&value)), 0U);
  EXPECT_EQ(first.getAttributes().modified, 22U);
  EXPECT_EQ(first.getAttributes().changed, 33U);
  EXPECT_EQ(first.getAttributes().blocks, 0U);

  const Time::Timestamp before = Time::getTime();
  ASSERT_EQ(second.write(17, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  const File::Attributes firstAttributes = first.getAttributes();
  const File::Attributes secondAttributes = second.getAttributes();
  EXPECT_EQ(firstAttributes.accessed, 11U);
  EXPECT_GE(firstAttributes.modified, before);
  EXPECT_LE(firstAttributes.modified, Time::getTime());
  EXPECT_EQ(firstAttributes.changed, firstAttributes.modified);
  EXPECT_EQ(firstAttributes.modified, secondAttributes.modified);
  EXPECT_EQ(firstAttributes.changed, secondAttributes.changed);
  EXPECT_EQ(firstAttributes.blocks, kBlockSize / 512);
  EXPECT_EQ(firstAttributes.blocks, secondAttributes.blocks);
  EXPECT_EQ(firstAttributes.size, 2 * kBlockSize);
  EXPECT_EQ(firstAttributes.links, 2U);
  EXPECT_EQ(LITTLE_TO_HOST32(inode->i_mtime), firstAttributes.modified);
  EXPECT_EQ(LITTLE_TO_HOST32(inode->i_ctime), firstAttributes.changed);
}

TEST(Ext2Growth, SparseAllocationFailurePreservesHoleAndSize) {
  MutableInodeFixture fixture;
  fixture.superblock.s_free_blocks_count = 0;
  fixture.group.bg_free_blocks_count = 0;
  Inode* inode = Ext2WritebackTestPeer::getInode(fixture.filesystem, 3);
  ASSERT_NE(inode, nullptr);
  inode->i_mode = HOST_TO_LITTLE16(EXT2_S_IFREG | 0600);
  inode->i_links_count = HOST_TO_LITTLE16(1);
  inode->i_size = HOST_TO_LITTLE32(2 * kBlockSize);
  inode->i_mtime = HOST_TO_LITTLE32(22);
  inode->i_ctime = HOST_TO_LITTLE32(33);
  Ext2File file(String("full-device"), 3, inode, &fixture.filesystem);
  const uint8_t source = 0x6b;
  EXPECT_EQ(file.write(17, 1, reinterpret_cast<uintptr_t>(&source)), 0U);
  EXPECT_FALSE(file.prepareSharedMapping(kBlockSize, kBlockSize));
  uint8_t value = 0xff;
  ASSERT_EQ(file.read(17, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  EXPECT_EQ(value, 0U);
  EXPECT_EQ(file.getSize(), 2 * kBlockSize);
  EXPECT_EQ(file.getAttributes().blocks, 0U);
  EXPECT_EQ(file.getAttributes().modified, 22U);
  EXPECT_EQ(file.getAttributes().changed, 33U);
}

TEST(Ext2Growth, BatchesIndirectMappingAndInodeWrites) {
  GrowthDisk disk;
  Superblock superblock = {};
  superblock.s_first_data_block = HOST_TO_LITTLE32(0);
  superblock.s_blocks_per_group = HOST_TO_LITTLE32(GrowthDisk::kBlockCount);
  superblock.s_inodes_per_group = HOST_TO_LITTLE32(32);
  superblock.s_free_blocks_count = HOST_TO_LITTLE32(GrowthDisk::kBlockCount - 16);
  superblock.s_free_inodes_count = HOST_TO_LITTLE32(31);

  GroupDesc groupDescriptor = {};
  groupDescriptor.bg_block_bitmap = HOST_TO_LITTLE32(2);
  groupDescriptor.bg_inode_bitmap = HOST_TO_LITTLE32(3);
  groupDescriptor.bg_inode_table = HOST_TO_LITTLE32(4);
  groupDescriptor.bg_free_blocks_count = HOST_TO_LITTLE16(GrowthDisk::kBlockCount - 16);
  groupDescriptor.bg_free_inodes_count = HOST_TO_LITTLE16(31);

  // Reserve the filesystem metadata blocks so allocation starts at block 16.
  std::fill(disk.getBlock(2), disk.getBlock(2) + 2, 0xFF);

  Ext2Filesystem filesystem;
  Ext2WritebackTestPeer::configureGrowth(filesystem, &disk, &superblock, &groupDescriptor);

  Inode inode = {};
  inode.i_mode = HOST_TO_LITTLE16(EXT2_S_IFREG);
  inode.i_links_count = HOST_TO_LITTLE16(1);
  {
    Ext2File file(String("growth"), 3, &inode, &filesystem);
    file.preallocate(16 * kBlockSize, false);
  }

  EXPECT_EQ(LITTLE_TO_HOST32(inode.i_block[12]), 32U);
  EXPECT_EQ(LITTLE_TO_HOST32(inode.i_blocks), 17U * (kBlockSize / 512));
  uint32_t* indirect = reinterpret_cast<uint32_t*>(disk.getBlock(32));
  for (size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(LITTLE_TO_HOST32(indirect[i]), 28U + i);
  }
  for (uint32_t block = 16; block < 32; ++block) {
    EXPECT_EQ(static_cast<size_t>(std::count(disk.reads.begin(), disk.reads.end(),
                                             static_cast<uint64_t>(block) * kBlockSize)),
              0U);
  }

  const uint64_t inodeTableLocation = 4ULL * kBlockSize;
  const uint64_t indirectLocation = 32ULL * kBlockSize;
  EXPECT_EQ(
      static_cast<size_t>(std::count(disk.writes.begin(), disk.writes.end(), inodeTableLocation)),
      1U);
  // The prepared block publishes its zeroed tail and complete entries together.
  EXPECT_EQ(
      static_cast<size_t>(std::count(disk.writes.begin(), disk.writes.end(), indirectLocation)),
      1U);
}

TEST(Ext2InodeTable, ReadsOnlyRequestedBlocksAndPreservesPointersAcrossGrowth) {
  constexpr uint32_t inodesPerGroup = 4096;
  constexpr uint32_t tableStart = 100;
  constexpr uint32_t lastBlock = tableStart + inodesPerGroup * sizeof(Inode) / kBlockSize - 1;
  GrowthDisk disk;
  Superblock superblock = {};
  superblock.s_inodes_per_group = HOST_TO_LITTLE32(inodesPerGroup);
  GroupDesc descriptor = {};
  descriptor.bg_inode_table = HOST_TO_LITTLE32(tableStart);
  {
    Ext2Filesystem filesystem;
    Ext2WritebackTestPeer::configureGrowth(filesystem, &disk, &superblock, &descriptor);
    Inode* first = Ext2WritebackTestPeer::getInode(filesystem, 1);
    ASSERT_NE(first, nullptr);
    first->i_uid = HOST_TO_LITTLE16(1234);
    EXPECT_EQ(disk.reads, std::vector<uint64_t>({uint64_t(tableStart) * kBlockSize}));
    EXPECT_EQ(Ext2WritebackTestPeer::getInode(filesystem, 2), first + 1);
    ASSERT_NE(Ext2WritebackTestPeer::getInode(filesystem, inodesPerGroup), nullptr);
    EXPECT_EQ(disk.reads, std::vector<uint64_t>({uint64_t(tableStart) * kBlockSize,
                                                 uint64_t(lastBlock) * kBlockSize}));
    EXPECT_EQ(Ext2WritebackTestPeer::getInode(filesystem, 1), first);
    EXPECT_EQ(LITTLE_TO_HOST16(first->i_uid), 1234);
  }
  EXPECT_EQ(std::count(disk.unpins.begin(), disk.unpins.end(), uint64_t(tableStart) * kBlockSize),
            1);
  EXPECT_EQ(std::count(disk.unpins.begin(), disk.unpins.end(), uint64_t(lastBlock) * kBlockSize),
            1);
  for (uint32_t block = tableStart + 1; block < lastBlock; ++block)
    EXPECT_EQ(std::count(disk.unpins.begin(), disk.unpins.end(), uint64_t(block) * kBlockSize), 0);
}

TEST(Ext2InodeTable, FailedBlockCanRetryWithoutDiscardingEarlierPointers) {
  constexpr uint32_t tableStart = 100;
  constexpr uint32_t targetInode = 2 * kBlockSize / sizeof(Inode) + 1;
  const uint64_t targetLocation = uint64_t(tableStart + 2) * kBlockSize;
  GrowthDisk disk;
  Superblock superblock = {};
  superblock.s_inodes_per_group = HOST_TO_LITTLE32(256);
  GroupDesc descriptor = {};
  descriptor.bg_inode_table = HOST_TO_LITTLE32(tableStart);
  {
    Ext2Filesystem filesystem;
    Ext2WritebackTestPeer::configureGrowth(filesystem, &disk, &superblock, &descriptor);
    Inode* first = Ext2WritebackTestPeer::getInode(filesystem, 1);
    ASSERT_NE(first, nullptr);
    disk.failedRead = targetLocation;
    EXPECT_EQ(Ext2WritebackTestPeer::getInode(filesystem, targetInode), nullptr);
    EXPECT_EQ(Ext2WritebackTestPeer::getInode(filesystem, 1), first);
    EXPECT_TRUE(disk.unpins.empty());
    disk.failedRead = UINT64_MAX;
    ASSERT_NE(Ext2WritebackTestPeer::getInode(filesystem, targetInode), nullptr);
    EXPECT_EQ(disk.reads, std::vector<uint64_t>(
                              {uint64_t(tableStart) * kBlockSize, targetLocation, targetLocation}));
  }
  EXPECT_EQ(std::count(disk.unpins.begin(), disk.unpins.end(), uint64_t(tableStart) * kBlockSize),
            1);
  EXPECT_EQ(std::count(disk.unpins.begin(), disk.unpins.end(), targetLocation), 1);
  EXPECT_EQ(
      std::count(disk.unpins.begin(), disk.unpins.end(), uint64_t(tableStart + 1) * kBlockSize), 0);
}

TEST(Ext2Growth, SerializesGlobalBitmapAllocation) {
  constexpr size_t kWorkerCount = 8;

  GrowthDisk disk;
  Superblock superblock = {};
  superblock.s_first_data_block = HOST_TO_LITTLE32(0);
  superblock.s_blocks_per_group = HOST_TO_LITTLE32(GrowthDisk::kBlockCount);
  superblock.s_inodes_per_group = HOST_TO_LITTLE32(64);
  superblock.s_free_blocks_count = HOST_TO_LITTLE32(GrowthDisk::kBlockCount - 16);
  superblock.s_free_inodes_count = HOST_TO_LITTLE32(53);

  GroupDesc groupDescriptor = {};
  groupDescriptor.bg_block_bitmap = HOST_TO_LITTLE32(2);
  groupDescriptor.bg_inode_bitmap = HOST_TO_LITTLE32(3);
  groupDescriptor.bg_inode_table = HOST_TO_LITTLE32(4);
  groupDescriptor.bg_free_blocks_count = HOST_TO_LITTLE16(GrowthDisk::kBlockCount - 16);
  groupDescriptor.bg_free_inodes_count = HOST_TO_LITTLE16(53);

  std::fill(disk.getBlock(2), disk.getBlock(2) + 2, 0xFF);
  std::fill(disk.getBlock(3), disk.getBlock(3) + 1, 0xFF);

  Ext2Filesystem filesystem;
  Ext2WritebackTestPeer::configureGrowth(filesystem, &disk, &superblock, &groupDescriptor);

  std::array<uint32_t, kWorkerCount> blocks = {};
  std::array<std::thread, kWorkerCount> workers;
  for (size_t i = 0; i < kWorkerCount; ++i) {
    workers[i] =
        std::thread([&, i] { blocks[i] = Ext2WritebackTestPeer::findFreeBlock(filesystem, 1); });
  }
  for (auto& worker : workers)
    worker.join();

  std::sort(blocks.begin(), blocks.end());
  EXPECT_EQ(blocks.front(), 16U);
  EXPECT_EQ(blocks.back(), 16U + kWorkerCount - 1);
  EXPECT_EQ(std::unique(blocks.begin(), blocks.end()), blocks.end());

  std::array<uint32_t, kWorkerCount> inodes = {};
  for (size_t i = 0; i < kWorkerCount; ++i) {
    workers[i] =
        std::thread([&, i] { inodes[i] = Ext2WritebackTestPeer::findFreeInode(filesystem); });
  }
  for (auto& worker : workers)
    worker.join();

  std::sort(inodes.begin(), inodes.end());
  EXPECT_EQ(inodes.front(), 9U);
  EXPECT_EQ(inodes.back(), 9U + kWorkerCount - 1);
  EXPECT_EQ(std::unique(inodes.begin(), inodes.end()), inodes.end());

  std::array<Inode*, kWorkerCount> inodePointers = {};
  for (size_t i = 0; i < kWorkerCount; ++i) {
    workers[i] =
        std::thread([&, i] { inodePointers[i] = Ext2WritebackTestPeer::getInode(filesystem, 1); });
  }
  for (auto& worker : workers)
    worker.join();

  EXPECT_NE(inodePointers.front(), nullptr);
  EXPECT_TRUE(std::all_of(inodePointers.begin(), inodePointers.end(),
                          [&](Inode* inode) { return inode == inodePointers.front(); }));
}

TEST(Ext2Directory, RejectsRecordsCrossingFilesystemBlocks) {
  constexpr uint32_t kFirstDirectoryBlock = 16;

  GrowthDisk disk;
  Superblock superblock = {};
  superblock.s_inodes_count = HOST_TO_LITTLE32(32);

  Ext2Filesystem filesystem;
  Ext2WritebackTestPeer::configureDirectory(filesystem, &disk, &superblock, kBlockSize);

  Inode inode = {};
  inode.i_mode = HOST_TO_LITTLE16(EXT2_S_IFDIR);
  inode.i_size = HOST_TO_LITTLE32(2 * kBlockSize);
  inode.i_blocks = HOST_TO_LITTLE32((2 * kBlockSize) / 512);
  inode.i_block[0] = HOST_TO_LITTLE32(kFirstDirectoryBlock);
  inode.i_block[1] = HOST_TO_LITTLE32(kFirstDirectoryBlock + 1);

  Dir* entry = reinterpret_cast<Dir*>(disk.getBlock(kFirstDirectoryBlock));
  entry->d_inode = HOST_TO_LITTLE32(2);
  entry->d_reclen = HOST_TO_LITTLE16(kBlockSize + 4);
  entry->d_namelen = 1;
  entry->d_file_type = EXT2_FILE;
  entry->d_name[0] = 'x';

  Ext2Directory directory(String("malformed"), 2, &inode, &filesystem, nullptr);
  uint64_t cookie = 2;
  auto accept = [](void*, const Directory::DirectoryEntryView&) -> bool { return true; };
  EXPECT_EQ(directory.enumerate(cookie, accept, nullptr), Directory::ReadStatus::IoError);
}

TEST(Ext2Directory, DuplicateCreateReleasesReservedInode) {
  constexpr uint32_t kInodeBitmapBlock = 3;
  constexpr uint32_t kInodeTableBlock = 4;
  constexpr uint32_t kDirectoryBlock = 16;
  constexpr uint32_t kInitialFreeInodes = 29;
  constexpr char kDuplicateName[] = "duplicate";

  GrowthDisk disk;
  Superblock superblock = {};
  superblock.s_rev_level = HOST_TO_LITTLE32(1);
  superblock.s_feature_incompat = HOST_TO_LITTLE32(2);
  superblock.s_blocks_per_group = HOST_TO_LITTLE32(GrowthDisk::kBlockCount);
  superblock.s_inodes_count = HOST_TO_LITTLE32(32);
  superblock.s_inodes_per_group = HOST_TO_LITTLE32(32);
  superblock.s_free_inodes_count = HOST_TO_LITTLE32(kInitialFreeInodes);

  GroupDesc groupDescriptor = {};
  groupDescriptor.bg_block_bitmap = HOST_TO_LITTLE32(2);
  groupDescriptor.bg_inode_bitmap = HOST_TO_LITTLE32(kInodeBitmapBlock);
  groupDescriptor.bg_inode_table = HOST_TO_LITTLE32(kInodeTableBlock);
  groupDescriptor.bg_free_inodes_count = HOST_TO_LITTLE16(kInitialFreeInodes);

  uint8_t* inodeBitmap = disk.getBlock(kInodeBitmapBlock);
  inodeBitmap[0] = 0x07;

  Ext2Filesystem filesystem;
  Ext2WritebackTestPeer::configureGrowth(filesystem, &disk, &superblock, &groupDescriptor);

  Inode* parentInode = Ext2WritebackTestPeer::getInode(filesystem, 2);
  ASSERT_NE(parentInode, nullptr);
  parentInode->i_mode = HOST_TO_LITTLE16(EXT2_S_IFDIR | 0755);
  parentInode->i_size = HOST_TO_LITTLE32(kBlockSize);
  parentInode->i_blocks = HOST_TO_LITTLE32(kBlockSize / 512);
  parentInode->i_block[0] = HOST_TO_LITTLE32(kDirectoryBlock);

  Dir* entry = reinterpret_cast<Dir*>(disk.getBlock(kDirectoryBlock));
  entry->d_inode = HOST_TO_LITTLE32(3);
  entry->d_reclen = HOST_TO_LITTLE16(kBlockSize);
  entry->d_namelen = sizeof(kDuplicateName) - 1;
  entry->d_file_type = EXT2_FILE;
  MemoryCopy(entry->d_name, kDuplicateName, sizeof(kDuplicateName) - 1);

  Ext2Directory parent(String("parent"), 2, parentInode, &filesystem, nullptr);
  EXPECT_FALSE(Ext2WritebackTestPeer::createFile(filesystem, &parent, String(kDuplicateName)));
  EXPECT_EQ(inodeBitmap[0] & 0x08, 0);
  EXPECT_EQ(LITTLE_TO_HOST32(superblock.s_free_inodes_count), kInitialFreeInodes);
  EXPECT_EQ(LITTLE_TO_HOST16(groupDescriptor.bg_free_inodes_count), kInitialFreeInodes);
}

TEST(Ext2Directory, DuplicateDirectoryCreateReleasesReservedStorage) {
  constexpr uint32_t kBlockBitmapBlock = 2;
  constexpr uint32_t kInodeBitmapBlock = 3;
  constexpr uint32_t kInodeTableBlock = 4;
  constexpr uint32_t kParentDirectoryBlock = 15;
  constexpr uint32_t kFirstFreeBlock = 16;
  constexpr uint32_t kInitialFreeBlocks = GrowthDisk::kBlockCount - kFirstFreeBlock;
  constexpr uint32_t kInitialFreeInodes = 29;
  constexpr uint16_t kParentLinks = 2;
  constexpr char kDuplicateName[] = "duplicate-directory";

  GrowthDisk disk;
  Superblock superblock = {};
  superblock.s_rev_level = HOST_TO_LITTLE32(1);
  superblock.s_feature_incompat = HOST_TO_LITTLE32(2);
  superblock.s_first_data_block = HOST_TO_LITTLE32(0);
  superblock.s_blocks_per_group = HOST_TO_LITTLE32(GrowthDisk::kBlockCount);
  superblock.s_inodes_count = HOST_TO_LITTLE32(32);
  superblock.s_inodes_per_group = HOST_TO_LITTLE32(32);
  superblock.s_free_blocks_count = HOST_TO_LITTLE32(kInitialFreeBlocks);
  superblock.s_free_inodes_count = HOST_TO_LITTLE32(kInitialFreeInodes);

  GroupDesc groupDescriptor = {};
  groupDescriptor.bg_block_bitmap = HOST_TO_LITTLE32(kBlockBitmapBlock);
  groupDescriptor.bg_inode_bitmap = HOST_TO_LITTLE32(kInodeBitmapBlock);
  groupDescriptor.bg_inode_table = HOST_TO_LITTLE32(kInodeTableBlock);
  groupDescriptor.bg_free_blocks_count = HOST_TO_LITTLE16(kInitialFreeBlocks);
  groupDescriptor.bg_free_inodes_count = HOST_TO_LITTLE16(kInitialFreeInodes);

  uint8_t* blockBitmap = disk.getBlock(kBlockBitmapBlock);
  std::fill(blockBitmap, blockBitmap + (kFirstFreeBlock / 8), 0xFF);
  uint8_t* inodeBitmap = disk.getBlock(kInodeBitmapBlock);
  inodeBitmap[0] = 0x07;

  Ext2Filesystem filesystem;
  Ext2WritebackTestPeer::configureGrowth(filesystem, &disk, &superblock, &groupDescriptor);

  Inode* parentInode = Ext2WritebackTestPeer::getInode(filesystem, 2);
  ASSERT_NE(parentInode, nullptr);
  parentInode->i_mode = HOST_TO_LITTLE16(EXT2_S_IFDIR | 0755);
  parentInode->i_links_count = HOST_TO_LITTLE16(kParentLinks);
  parentInode->i_size = HOST_TO_LITTLE32(kBlockSize);
  parentInode->i_blocks = HOST_TO_LITTLE32(kBlockSize / 512);
  parentInode->i_block[0] = HOST_TO_LITTLE32(kParentDirectoryBlock);

  Dir* entry = reinterpret_cast<Dir*>(disk.getBlock(kParentDirectoryBlock));
  entry->d_inode = HOST_TO_LITTLE32(3);
  entry->d_reclen = HOST_TO_LITTLE16(kBlockSize);
  entry->d_namelen = sizeof(kDuplicateName) - 1;
  entry->d_file_type = EXT2_DIRECTORY;
  MemoryCopy(entry->d_name, kDuplicateName, sizeof(kDuplicateName) - 1);

  Ext2Directory parent(String("parent"), 2, parentInode, &filesystem, nullptr);
  EXPECT_FALSE(Ext2WritebackTestPeer::createDirectory(filesystem, &parent, String(kDuplicateName)));
  EXPECT_EQ(inodeBitmap[0] & 0x08, 0);
  EXPECT_EQ(blockBitmap[kFirstFreeBlock / 8] & (1U << (kFirstFreeBlock % 8)), 0U);
  EXPECT_EQ(LITTLE_TO_HOST16(parentInode->i_links_count), kParentLinks);
  EXPECT_EQ(LITTLE_TO_HOST32(superblock.s_free_blocks_count), kInitialFreeBlocks);
  EXPECT_EQ(LITTLE_TO_HOST16(groupDescriptor.bg_free_blocks_count), kInitialFreeBlocks);
  EXPECT_EQ(LITTLE_TO_HOST32(superblock.s_free_inodes_count), kInitialFreeInodes);
  EXPECT_EQ(LITTLE_TO_HOST16(groupDescriptor.bg_free_inodes_count), kInitialFreeInodes);
}

TEST(Ext2Writeback, ReleaseInodeFinishesOnTargetTableBlock) {
  constexpr uint32_t kTargetInode = 33;
  constexpr uint32_t kInodesPerGroup = 64;
  constexpr uint32_t kInodeTableBlock = 100;
  constexpr uint32_t kInodeBitmapBlock = 200;
  constexpr uint32_t kFreeInodes = 7;
  constexpr uint16_t kGroupFreeInodes = 3;
  constexpr size_t kInodesPerTableBlock = kBlockSize / sizeof(Inode);
  static_assert(kInodesPerTableBlock == 32, "fixture requires 128-byte ext2 inodes");

  TrackingDisk disk;
  Superblock superblock = {};
  superblock.s_first_data_block = HOST_TO_LITTLE32(0);
  superblock.s_inodes_per_group = HOST_TO_LITTLE32(kInodesPerGroup);
  superblock.s_free_inodes_count = HOST_TO_LITTLE32(kFreeInodes);

  GroupDesc groupDescriptor = {};
  groupDescriptor.bg_inode_bitmap = HOST_TO_LITTLE32(kInodeBitmapBlock);
  groupDescriptor.bg_inode_table = HOST_TO_LITTLE32(kInodeTableBlock);
  groupDescriptor.bg_free_inodes_count = HOST_TO_LITTLE16(kGroupFreeInodes);

  std::array<Inode, kInodesPerTableBlock> inodeTableBlock0 = {};
  std::array<Inode, kInodesPerTableBlock> inodeTableBlock1 = {};
  inodeTableBlock1[0].i_links_count = HOST_TO_LITTLE16(2);

  std::array<uint8_t, kBlockSize> inodeBitmap = {};
  const size_t targetIndex = kTargetInode - 1;
  const uint8_t targetMask = 1U << (targetIndex % 8);
  inodeBitmap[targetIndex / 8] |= targetMask;

  Ext2Filesystem filesystem;
  Ext2WritebackTestPeer::configureInodeRelease(
      filesystem, &disk, &superblock, &groupDescriptor,
      reinterpret_cast<uintptr_t>(inodeBitmap.data()),
      reinterpret_cast<uintptr_t>(inodeTableBlock0.data()),
      reinterpret_cast<uintptr_t>(inodeTableBlock1.data()));

  const uint64_t previousTableLocation = static_cast<uint64_t>(kInodeTableBlock) * kBlockSize;
  const uint64_t targetTableLocation = static_cast<uint64_t>(kInodeTableBlock + 1) * kBlockSize;

  ASSERT_FALSE(Ext2WritebackTestPeer::releaseInode(filesystem, kTargetInode));
  EXPECT_EQ(LITTLE_TO_HOST16(inodeTableBlock1[0].i_links_count), 1);
  EXPECT_NE(inodeBitmap[targetIndex / 8] & targetMask, 0);
  EXPECT_EQ(LITTLE_TO_HOST32(superblock.s_free_inodes_count), kFreeInodes);
  EXPECT_EQ(LITTLE_TO_HOST16(groupDescriptor.bg_free_inodes_count), kGroupFreeInodes);
  EXPECT_GE(std::count(disk.writes.begin(), disk.writes.end(), targetTableLocation), 1);
  EXPECT_EQ(std::count(disk.writes.begin(), disk.writes.end(),
                       static_cast<uint64_t>(kInodeBitmapBlock) * kBlockSize),
            0);
  EXPECT_EQ(std::count(disk.writes.begin(), disk.writes.end(), previousTableLocation), 0);
  ASSERT_FALSE(disk.writes.empty());
  EXPECT_EQ(disk.writes.back(), targetTableLocation);

  disk.writes.clear();
  ASSERT_TRUE(Ext2WritebackTestPeer::releaseInode(filesystem, kTargetInode));
  EXPECT_EQ(LITTLE_TO_HOST16(inodeTableBlock1[0].i_links_count), 0);
  EXPECT_EQ(inodeBitmap[targetIndex / 8] & targetMask, 0);
  EXPECT_EQ(LITTLE_TO_HOST32(superblock.s_free_inodes_count), kFreeInodes + 1);
  EXPECT_EQ(LITTLE_TO_HOST16(groupDescriptor.bg_free_inodes_count), kGroupFreeInodes + 1);

  EXPECT_GE(std::count(disk.writes.begin(), disk.writes.end(), targetTableLocation), 1);
  EXPECT_GE(std::count(disk.writes.begin(), disk.writes.end(),
                       static_cast<uint64_t>(kInodeBitmapBlock) * kBlockSize),
            1);
  EXPECT_EQ(std::count(disk.writes.begin(), disk.writes.end(), previousTableLocation), 0);
  ASSERT_FALSE(disk.writes.empty());
  EXPECT_NE(disk.writes.back(), previousTableLocation);
  EXPECT_EQ(disk.writes.back(), targetTableLocation);
}

TEST(PartitionWriteback, AlignsAndTranslatesFlush) {
  constexpr uint64_t kStart = 1536;
  constexpr uint64_t kCachePageSize = TargetInfo::getPageSize();
  constexpr uint64_t kLength = (4 * kCachePageSize) + 512;
  constexpr uint64_t kLocation = 1024;
  constexpr uint64_t kInteriorLocation = (3 * kCachePageSize) + 512;
  constexpr uint64_t kPartialTailLocation = 4 * kCachePageSize;

  TrackingDisk parent;
  Partition partition(String("test"), kStart, kLength);
  partition.setParent(&parent);

  partition.flush(kLocation);
  ASSERT_EQ(parent.alignments.size(), 1U);
  EXPECT_EQ(parent.alignments[0], kStart);
  ASSERT_EQ(parent.flushes.size(), 1U);
  EXPECT_EQ(parent.flushes[0], kStart + kLocation);

  partition.flush(kInteriorLocation);
  ASSERT_EQ(parent.flushes.size(), 2U);
  EXPECT_EQ(parent.flushes[1], kStart + kInteriorLocation);

  partition.flush(kPartialTailLocation);
  partition.flush(kLength);
  partition.flush(UINT64_MAX);
  EXPECT_EQ(parent.alignments.size(), 1U);
  EXPECT_EQ(parent.flushes.size(), 2U);
}

TEST(PartitionWriteback, AlignsAndTranslatesRetirement) {
  constexpr uint64_t kStart = 1536;
  constexpr uint64_t kCachePageSize = TargetInfo::getPageSize();
  constexpr uint64_t kLength = (4 * kCachePageSize) + 512;
  constexpr uint64_t kLocation = 1024;
  constexpr uint64_t kInteriorLocation = (3 * kCachePageSize) + 512;
  constexpr uint64_t kPartialTailLocation = 4 * kCachePageSize;

  TrackingDisk parent;
  Partition partition(String("test"), kStart, kLength);
  partition.setParent(&parent);

  ASSERT_TRUE(partition.retireCachePage(kLocation));
  ASSERT_EQ(parent.alignments.size(), 1U);
  EXPECT_EQ(parent.alignments[0], kStart);
  ASSERT_EQ(parent.retirements.size(), 1U);
  EXPECT_EQ(parent.retirements[0], kStart + kLocation);

  ASSERT_TRUE(partition.retireCachePage(kInteriorLocation));
  ASSERT_EQ(parent.retirements.size(), 2U);
  EXPECT_EQ(parent.retirements[1], kStart + kInteriorLocation);

  EXPECT_FALSE(partition.retireCachePage(kPartialTailLocation));
  EXPECT_FALSE(partition.retireCachePage(kLength));
  EXPECT_FALSE(partition.retireCachePage(UINT64_MAX));
  EXPECT_EQ(parent.alignments.size(), 1U);
  EXPECT_EQ(parent.retirements.size(), 2U);

  parent.retirementResult = false;
  EXPECT_FALSE(partition.retireCachePage(kLocation));
  ASSERT_EQ(parent.retirements.size(), 3U);
  EXPECT_EQ(parent.retirements[2], kStart + kLocation);
}

TEST(DiskWriteback, RetirementIsUnsupportedByDefault) {
  TrackingDisk disk;
  EXPECT_FALSE(disk.Disk::retireCachePage(0));
  EXPECT_TRUE(disk.retirements.empty());
}

TEST(FileWriteback, SyncsMappedPageBeforeReturningIt) {
  OrderedSyncFile file;
  file.syncAndReturnPhysicalPage(8192, true);

  ASSERT_EQ(file.order.size(), 2U);
  EXPECT_EQ(file.order[0], 'S');
  EXPECT_EQ(file.order[1], 'R');
  EXPECT_TRUE(file.syncSawPin);
  EXPECT_TRUE(file.syncAsync);
  EXPECT_EQ(file.syncOffset, 8192U);
  EXPECT_EQ(file.returnOffset, 8192U);
  EXPECT_FALSE(file.pinned);
}

TEST(Ext2Writeback, BatchMapsNativeBlocksAndSkipsSparseMembers) {
  TrackingDisk disk;
  Ext2Filesystem filesystem;
  Ext2WritebackTestPeer::configure(filesystem, &disk, kBlockSize);
  Inode inode = makeInode(127, 139);
  Ext2File file(String("batch"), 3, &inode, &filesystem);
  const uint64_t offsets[] = {0, kBlockSize, 2 * kBlockSize};
  EXPECT_TRUE(file.syncPages(offsets, 3));
  EXPECT_EQ(disk.batchSizes, (std::vector<size_t>{2}));
  EXPECT_EQ(disk.flushes, (std::vector<uint64_t>{127ULL * kBlockSize, 139ULL * kBlockSize}));
  disk.syncResult = false;
  EXPECT_FALSE(file.syncPages(offsets, 3));
  EXPECT_EQ(disk.flushes.size(), 4U);
}

TEST(Ext2Writeback, InvalidBatchDoesNotPartiallyFlushValidPrefix) {
  TrackingDisk disk;
  Ext2Filesystem filesystem;
  Ext2WritebackTestPeer::configure(filesystem, &disk, kBlockSize);
  Inode inode = makeInode(127, 139);
  Ext2File file(String("invalid-batch"), 3, &inode, &filesystem);
  const uint64_t pastEnd[] = {0, 3 * kBlockSize};
  const uint64_t unaligned[] = {0, 1};
  EXPECT_FALSE(file.syncPages(pastEnd, 2));
  EXPECT_FALSE(file.syncPages(unaligned, 2));
  EXPECT_FALSE(file.syncPages(nullptr, 1));
  EXPECT_FALSE(file.syncPages(pastEnd, Disk::MaxSyncPages + 1));
  EXPECT_TRUE(file.syncPages(nullptr, 0));
  EXPECT_TRUE(disk.batchSizes.empty());
  EXPECT_TRUE(disk.flushes.empty());
}

TEST(PartitionWriteback, BatchTranslatesEveryLocationAndValidatesBeforeForwarding) {
  constexpr uint64_t start = 1536;
  constexpr uint64_t page = TargetInfo::getPageSize();
  TrackingDisk parent;
  Partition partition(String("batch"), start, 4 * page + 512);
  partition.setParent(&parent);
  const uint64_t valid[] = {0, page + 512};
  EXPECT_TRUE(partition.syncPages(valid, 2));
  EXPECT_EQ(parent.batchSizes, (std::vector<size_t>{2}));
  EXPECT_EQ(parent.flushes, (std::vector<uint64_t>{start, start + page + 512}));
  EXPECT_EQ(parent.alignments, (std::vector<uint64_t>{start}));
  const uint64_t invalid[] = {0, 4 * page};
  EXPECT_FALSE(partition.syncPages(invalid, 2));
  EXPECT_FALSE(partition.syncPages(nullptr, 1));
  EXPECT_FALSE(partition.syncPages(valid, Disk::MaxSyncPages + 1));
  EXPECT_EQ(parent.batchSizes.size(), 1U);
  parent.syncResult = false;
  EXPECT_FALSE(partition.syncPages(valid, 2));
  EXPECT_EQ(parent.flushes.size(), 4U);
  Partition overflow(String("overflow"), ~uint64_t(0) - 511, 2 * page);
  overflow.setParent(&parent);
  const uint64_t overflowing[] = {page};
  EXPECT_FALSE(overflow.syncPages(overflowing, 1));
  EXPECT_EQ(parent.batchSizes.size(), 2U);
}

TEST(PartitionWriteback, DefaultBatchRejectsInvalidPrefixAndAttemptsEveryValidMember) {
  TrackingDisk disk;
  const uint64_t invalid[] = {0, ~uint64_t(0)};
  EXPECT_FALSE(disk.Disk::syncPages(invalid, 2));
  EXPECT_TRUE(disk.flushes.empty());
  EXPECT_FALSE(disk.Disk::syncPages(nullptr, 1));
  EXPECT_TRUE(disk.Disk::syncPages(nullptr, 0));
  const uint64_t valid[] = {0, kBlockSize};
  disk.syncResult = false;
  EXPECT_FALSE(disk.Disk::syncPages(valid, 2));
  EXPECT_EQ(disk.flushes, (std::vector<uint64_t>{0, kBlockSize}));
}

namespace {
class FillBatchDisk final : public Disk {
 public:
  static constexpr size_t Page = TargetInfo::getPageSize();
  struct Transfer {
    uint64_t location;
    size_t length;
  };
  FillBatchDisk() : bytes(2 * 1024 * 1024, 0x11), persisted(bytes), pins(bytes.size() / Page, 0) {}

  BufferView read(uint64_t location) override {
    if (location >= bytes.size())
      return {};
    ++pins[location / Page];
    ++reads;
    return BufferView(bytes.data() + location, Page - location % Page);
  }
  bool readInto(uint64_t location, void* buffer, size_t length) override {
    if (!buffer || location > bytes.size() || length > bytes.size() - location)
      return false;
    dataReads.push_back({location, length});
    if (location == failedReadLocation)
      return false;
    MemoryCopy(buffer, bytes.data() + location, length);
    return true;
  }
  bool readIntoBatch(ReadBuffer* buffers, size_t count) override {
    readBatchSizes.push_back(count);
    if (readBatchObserver)
      readBatchObserver(readBatchContext);
    return Disk::readIntoBatch(buffers, count);
  }
  bool writeFrom(uint64_t location, const void* buffer, size_t length) override {
    if (!buffer || location > bytes.size() || length > bytes.size() - location)
      return false;
    dataWrites.push_back({location, length});
    if (!allowTransfers || dataWrites.size() == failedTransfer)
      return false;
    MemoryCopy(bytes.data() + location, buffer, length);
    pendingWrites.push_back({location, length});
    ++completedSinceBarrier;
    return true;
  }
  bool syncData() override {
    batchSizes.push_back(completedSinceBarrier);
    completedSinceBarrier = 0;
    if (failBarrier)
      return false;
    for (const Transfer& transfer : pendingWrites)
      std::copy_n(bytes.begin() + transfer.location, transfer.length,
                  persisted.begin() + transfer.location);
    pendingWrites.clear();
    return true;
  }
  bool pin(uint64_t location) override {
    if (location >= bytes.size())
      return false;
    ++pins[location / Page];
    return true;
  }
  void unpin(uint64_t location) override {
    if (location >= bytes.size() || !pins[location / Page]) {
      unbalanced = true;
      return;
    }
    --pins[location / Page];
    ++unpins;
  }
  size_t getSize() const override {
    return bytes.size();
  }
  size_t getBlockSize() const override {
    return Page;
  }
  void write(uint64_t) override {}
  void align(uint64_t) override {}
  bool sync(uint64_t, bool) override {
    ++singleSyncs;
    return false;
  }
  bool syncPages(const uint64_t*, size_t) override {
    ++legacyBatches;
    return false;
  }
  bool balanced() const {
    return !unbalanced && std::all_of(pins.begin(), pins.end(), [](size_t n) { return n == 0; });
  }
  void resetActivity() {
    batchSizes.clear();
    dataReads.clear();
    readBatchSizes.clear();
    dataWrites.clear();
    singleSyncs = legacyBatches = reads = unpins = completedSinceBarrier = 0;
  }

  std::vector<uint8_t> bytes;
  std::vector<uint8_t> persisted;
  std::vector<size_t> pins;
  std::vector<size_t> batchSizes;
  std::vector<Transfer> dataReads;
  std::vector<size_t> readBatchSizes;
  void (*readBatchObserver)(void*) = nullptr;
  void* readBatchContext = nullptr;
  uint64_t failedReadLocation = UINT64_MAX;
  std::vector<Transfer> dataWrites;
  std::vector<Transfer> pendingWrites;
  size_t failedTransfer = ~size_t(0);
  size_t completedSinceBarrier = 0;
  size_t singleSyncs = 0;
  size_t legacyBatches = 0;
  size_t reads = 0;
  size_t unpins = 0;
  bool allowTransfers = true;
  bool failBarrier = false;
  bool unbalanced = false;
};

class ForcedFillBatchFile final : public Ext2File {
 public:
  ForcedFillBatchFile(Inode* inode, Ext2Filesystem* filesystem)
      : Ext2File(String("forced-fill-batch"), 3, inode, filesystem) {}
  bool syncPages(const uint64_t* offsets, size_t count) override {
    upperBatches.push_back(count);
    return Ext2File::syncPages(offsets, count);
  }
  bool changeAll(uint8_t value) {
    for (size_t offset = 0; offset < getSize(); offset += FillBatchDisk::Page) {
      const uintptr_t page = cacheState().fill.lookup(offset);
      if (!page)
        return false;
      cacheState().fill.markExternallyWritable(offset);
      std::fill_n(reinterpret_cast<uint8_t*>(page), FillBatchDisk::Page, value);
      cacheState().fill.release(offset);
    }
    return true;
  }
  bool evictUpper(size_t offset) {
    return cacheState().fill.evict(offset);
  }
  bool hasUpper(size_t offset) {
    return cacheState().fill.exists(offset, FillBatchDisk::Page);
  }
  uintptr_t upperAddress(size_t offset) {
    const uintptr_t page = cacheState().fill.lookup(offset);
    if (page)
      cacheState().fill.release(offset);
    return page;
  }
  std::vector<uint8_t> upperContents(size_t offset) {
    const uintptr_t page = cacheState().fill.lookup(offset);
    if (!page)
      return {};
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(page);
    std::vector<uint8_t> result(bytes, bytes + FillBatchDisk::Page);
    cacheState().fill.release(offset);
    return result;
  }
  std::vector<size_t> upperBatches;
  std::vector<size_t> upperReadBatches;

 protected:
  bool readPages(ReadPage* pages, size_t count) override {
    upperReadBatches.push_back(count);
    return Ext2File::readPages(pages, count);
  }
  bool useFillCache() const override {
    // MMU Ext2 uses its fill cache even when filesystem blocks are native pages.
    return true;
  }
};

struct FillBatchFixture {
  FillBatchFixture(size_t blockSize, size_t fileSize, size_t sparseBlock = ~size_t(0))
      : blockSize(blockSize), fileSize(fileSize) {
    Ext2WritebackTestPeer::configure(filesystem, &disk, blockSize);
    const size_t count = (fileSize + blockSize - 1) / blockSize;
    inode.i_mode = HOST_TO_LITTLE16(EXT2_S_IFREG);
    inode.i_size = HOST_TO_LITTLE32(fileSize);
    const size_t indirect = count > 12 ? 1 : 0;
    inode.i_blocks = HOST_TO_LITTLE32((count + indirect) * blockSize / 512);
    if (indirect) {
      inode.i_block[12] = HOST_TO_LITTLE32(8);
      std::fill_n(disk.bytes.begin() + 8 * blockSize, blockSize, 0);
    }
    for (size_t i = 0; i < count; ++i) {
      const uint32_t physical = i == sparseBlock ? 0 : 64 + i * (FillBatchDisk::Page / blockSize);
      blocks.push_back(physical);
      const uint32_t encoded = HOST_TO_LITTLE32(physical);
      if (i < 12) {
        inode.i_block[i] = encoded;
      } else {
        MemoryCopy(disk.bytes.data() + 8 * blockSize + (i - 12) * sizeof(encoded), &encoded,
                   sizeof(encoded));
      }
    }
    disk.persisted = disk.bytes;
    file.reset(new ForcedFillBatchFile(&inode, &filesystem));
  }
  bool prime() {
    const bool read = file->read(0, fileSize, 0) == fileSize;
    disk.resetActivity();
    return read && disk.balanced();
  }
  void expectPersisted(uint8_t value) {
    for (size_t i = 0; i < blocks.size(); ++i) {
      if (!blocks[i])
        continue;
      const size_t location = blocks[i] * blockSize;
      const size_t valid = std::min(blockSize, fileSize - i * blockSize);
      EXPECT_TRUE(std::all_of(disk.persisted.begin() + location,
                              disk.persisted.begin() + location + valid,
                              [value](uint8_t byte) { return byte == value; }));
      EXPECT_TRUE(std::all_of(disk.persisted.begin() + location + valid,
                              disk.persisted.begin() + location + blockSize,
                              [](uint8_t byte) { return byte == 0x11; }));
    }
  }
  void expectAllTransfers() const {
    size_t transfer = 0;
    for (size_t i = 0; i < blocks.size(); ++i) {
      if (!blocks[i])
        continue;
      ASSERT_LT(transfer, disk.dataWrites.size());
      EXPECT_EQ(disk.dataWrites[transfer].location, blocks[i] * blockSize);
      EXPECT_EQ(disk.dataWrites[transfer].length, std::min(blockSize, fileSize - i * blockSize));
      ++transfer;
    }
    EXPECT_EQ(disk.dataWrites.size(), transfer);
    EXPECT_TRUE(disk.dataReads.empty());
    EXPECT_EQ(disk.singleSyncs, 0U);
    EXPECT_EQ(disk.legacyBatches, 0U);
  }
  const size_t blockSize;
  const size_t fileSize;
  FillBatchDisk disk;
  Ext2Filesystem filesystem;
  Inode inode = {};
  std::vector<uint32_t> blocks;
  std::unique_ptr<ForcedFillBatchFile> file;
};
}  // namespace

TEST(Ext2Writeback, ForcedNativeFillCombinesSeveralUpperPagesInOneDiskBatch) {
  FillBatchFixture fixture(FillBatchDisk::Page, 3 * FillBatchDisk::Page);
  ASSERT_TRUE(fixture.prime());
  ASSERT_TRUE(fixture.file->changeAll(0x67));
  ASSERT_TRUE(fixture.file->syncRange(0, 0));
  EXPECT_EQ(fixture.file->upperBatches, (std::vector<size_t>{3}));
  EXPECT_EQ(fixture.disk.batchSizes, (std::vector<size_t>{3}));
  fixture.expectAllTransfers();
  fixture.expectPersisted(0x67);
  EXPECT_TRUE(fixture.disk.balanced());
  EXPECT_EQ(fixture.disk.reads, fixture.disk.unpins);
}

TEST(Ext2Writeback, ForcedSubpageFillBatchSkipsSparseBlocksAndPreservesPartialTail) {
  FillBatchFixture fixture(1024, 2 * FillBatchDisk::Page + 333, 1);
  ASSERT_TRUE(fixture.prime());
  ASSERT_TRUE(fixture.file->changeAll(0x79));
  ASSERT_TRUE(fixture.file->syncRange(0, 0));
  EXPECT_EQ(fixture.file->upperBatches, (std::vector<size_t>{3}));
  EXPECT_EQ(fixture.disk.batchSizes, (std::vector<size_t>{fixture.blocks.size() - 1}));
  fixture.expectAllTransfers();
  fixture.expectPersisted(0x79);
  EXPECT_TRUE(fixture.disk.balanced());
  EXPECT_EQ(fixture.disk.reads, fixture.disk.unpins);
}

TEST(Ext2Writeback, ForcedFillFinalChunkFailureRetainsWholeUpperBatchForRetry) {
  constexpr size_t blockSize = 1024;
  const size_t pages = (Disk::MaxSyncPages * blockSize) / FillBatchDisk::Page + 1;
  FillBatchFixture fixture(blockSize, pages * FillBatchDisk::Page);
  ASSERT_TRUE(fixture.prime());
  ASSERT_TRUE(fixture.file->changeAll(0x8B));
  fixture.disk.failedTransfer = fixture.blocks.size();
  EXPECT_FALSE(fixture.file->syncRange(0, 0));
  EXPECT_EQ(fixture.file->upperBatches, (std::vector<size_t>{pages}));
  EXPECT_EQ(fixture.disk.batchSizes, (std::vector<size_t>{fixture.blocks.size() - 1}));
  fixture.expectAllTransfers();
  EXPECT_EQ(fixture.disk.persisted[fixture.blocks.front() * blockSize], 0x8B);
  EXPECT_EQ(fixture.disk.persisted[fixture.blocks.back() * blockSize], 0x11);
  EXPECT_TRUE(fixture.disk.balanced());
  EXPECT_EQ(fixture.disk.reads, fixture.disk.unpins);

  // The failed shared result retains even pages whose writes became durable.
  fixture.disk.allowTransfers = false;
  for (size_t i = 0; i < pages; ++i) {
    EXPECT_FALSE(fixture.file->evictUpper(i * FillBatchDisk::Page));
    EXPECT_TRUE(fixture.file->hasUpper(i * FillBatchDisk::Page));
  }
  fixture.disk.allowTransfers = true;
  fixture.disk.failedTransfer = ~size_t(0);
  fixture.disk.resetActivity();
  ASSERT_TRUE(fixture.file->syncRange(0, 0));
  EXPECT_EQ(fixture.disk.batchSizes, (std::vector<size_t>{fixture.blocks.size()}));
  fixture.expectAllTransfers();
  fixture.expectPersisted(0x8B);
  EXPECT_TRUE(fixture.disk.balanced());
  EXPECT_EQ(fixture.disk.reads, fixture.disk.unpins);
  for (size_t i = 0; i < pages; ++i)
    EXPECT_TRUE(fixture.file->evictUpper(i * FillBatchDisk::Page));
}

TEST(Ext2Writeback, ForcedFillBarrierFailureRetainsEveryUpperPageForRetry) {
  constexpr size_t pages = 3;
  FillBatchFixture fixture(FillBatchDisk::Page, pages * FillBatchDisk::Page);
  ASSERT_TRUE(fixture.prime());
  ASSERT_TRUE(fixture.file->changeAll(0x95));
  const auto persisted = fixture.disk.persisted;
  fixture.disk.failBarrier = true;
  EXPECT_FALSE(fixture.file->syncRange(0, 0));
  EXPECT_EQ(fixture.disk.batchSizes, (std::vector<size_t>{pages}));
  fixture.expectAllTransfers();
  EXPECT_EQ(fixture.disk.persisted, persisted);
  for (size_t i = 0; i < pages; ++i) {
    EXPECT_FALSE(fixture.file->evictUpper(i * FillBatchDisk::Page));
    EXPECT_TRUE(fixture.file->hasUpper(i * FillBatchDisk::Page));
  }

  fixture.disk.failBarrier = false;
  fixture.disk.resetActivity();
  ASSERT_TRUE(fixture.file->syncRange(0, 0));
  EXPECT_EQ(fixture.disk.batchSizes, (std::vector<size_t>{pages}));
  fixture.expectAllTransfers();
  fixture.expectPersisted(0x95);
  EXPECT_TRUE(fixture.disk.balanced());
  for (size_t i = 0; i < pages; ++i)
    EXPECT_TRUE(fixture.file->evictUpper(i * FillBatchDisk::Page));
}

TEST(Ext2Writeback, MultiPageReadBoundsBatchesAndPreservesExactEofCopy) {
  constexpr size_t Page = FillBatchDisk::Page;
  const size_t fileSize = (File::MaxReadPages + 2) * Page + 333;
  FillBatchFixture fixture(Page, fileSize);
  for (size_t i = 0; i < fixture.blocks.size(); ++i)
    std::fill_n(fixture.disk.bytes.begin() + fixture.blocks[i] * Page, Page,
                static_cast<uint8_t>(0x20 + i));
  std::vector<uint8_t> result(fileSize + Page, 0xE3);
  ASSERT_EQ(fixture.file->read(7, result.size(), reinterpret_cast<uintptr_t>(result.data())),
            fileSize - 7);
  EXPECT_EQ(fixture.file->upperReadBatches, (std::vector<size_t>{File::MaxReadPages, 3}));
  EXPECT_EQ(fixture.disk.readBatchSizes, (std::vector<size_t>{Disk::MaxReadBuffers, 3}));
  for (size_t i = 0; i < fileSize - 7; ++i)
    ASSERT_EQ(result[i], static_cast<uint8_t>(0x20 + (i + 7) / Page));
  EXPECT_TRUE(std::all_of(result.begin() + fileSize - 7, result.end(),
                          [](uint8_t value) { return value == 0xE3; }));
  const auto tail = fixture.file->upperContents((fileSize / Page) * Page);
  ASSERT_EQ(tail.size(), Page);
  EXPECT_TRUE(
      std::all_of(tail.begin() + 333, tail.end(), [](uint8_t value) { return value == 0; }));
  fixture.disk.resetActivity();
  fixture.file->upperReadBatches.clear();
  EXPECT_EQ(fixture.file->read(0, fileSize, 0), fileSize);
  EXPECT_TRUE(fixture.disk.dataReads.empty());
  EXPECT_TRUE(fixture.disk.readBatchSizes.empty());
  EXPECT_TRUE(fixture.file->upperReadBatches.empty());
  for (size_t i = 0; i < fixture.blocks.size(); ++i)
    EXPECT_TRUE(fixture.file->evictUpper(i * Page));
  EXPECT_TRUE(fixture.disk.dataWrites.empty());
  EXPECT_TRUE(fixture.disk.balanced());
}

TEST(Ext2Writeback, MultiPagePopulationRetainsCanonicalCachedGapsAndTransportPins) {
  constexpr size_t Page = FillBatchDisk::Page;
  FillBatchFixture fixture(Page, 4 * Page);
  ASSERT_EQ(fixture.file->populateRange(Page, Page), Page);
  const uintptr_t existing = fixture.file->upperAddress(Page);
  ASSERT_NE(existing, 0U);
  fixture.disk.resetActivity();
  fixture.file->upperReadBatches.clear();
  fixture.disk.readBatchContext = fixture.file.get();
  fixture.disk.readBatchObserver = [](void* context) {
    auto* file = static_cast<ForcedFillBatchFile*>(context);
    for (size_t i = 0; i < 4; ++i)
      EXPECT_FALSE(file->evictUpper(i * Page));
  };
  EXPECT_EQ(fixture.file->populateRange(0, 4 * Page), 4 * Page);
  EXPECT_EQ(fixture.file->upperAddress(Page), existing);
  EXPECT_EQ(fixture.file->upperReadBatches, (std::vector<size_t>{3}));
  EXPECT_EQ(fixture.disk.readBatchSizes, (std::vector<size_t>{3}));
  fixture.disk.readBatchObserver = nullptr;
  fixture.disk.resetActivity();
  EXPECT_EQ(fixture.file->populateRange(0, 4 * Page), Page);
  EXPECT_TRUE(fixture.disk.readBatchSizes.empty());
  EXPECT_EQ(fixture.file->populateRange(4 * Page, Page), 0U);
  EXPECT_EQ(fixture.file->populateRange(~size_t{0}, Page), 0U);
  EXPECT_EQ(fixture.file->populateRange(0, 0), 0U);
  for (size_t i = 0; i < 4; ++i)
    EXPECT_TRUE(fixture.file->evictUpper(i * Page));
  EXPECT_TRUE(fixture.disk.dataWrites.empty());
  EXPECT_TRUE(fixture.disk.balanced());
}

TEST(Ext2Writeback, FailedMiddlePrefetchPublishesOnlyCompletePagesAndPreservesReadPrefix) {
  constexpr size_t Page = FillBatchDisk::Page;
  FillBatchFixture fixture(Page, 3 * Page);
  fixture.disk.failedReadLocation = fixture.blocks[1] * Page;
  EXPECT_EQ(fixture.file->populateRange(0, 3 * Page), Page);
  EXPECT_TRUE(fixture.file->hasUpper(0));
  EXPECT_FALSE(fixture.file->hasUpper(Page));
  EXPECT_TRUE(fixture.file->hasUpper(2 * Page));
  EXPECT_EQ(fixture.disk.readBatchSizes, (std::vector<size_t>{3}));
  std::vector<uint8_t> result(3 * Page, 0xE3);
  errno = Error::OutOfMemory;
  EXPECT_EQ(fixture.file->read(0, result.size(), reinterpret_cast<uintptr_t>(result.data())), Page);
  EXPECT_EQ(errno, Error::IoError);
  EXPECT_TRUE(std::all_of(result.begin(), result.begin() + Page,
                          [](uint8_t value) { return value == 0x11; }));
  EXPECT_TRUE(std::all_of(result.begin() + Page, result.end(),
                          [](uint8_t value) { return value == 0xE3; }));
  EXPECT_FALSE(fixture.file->hasUpper(Page));
  fixture.disk.failedReadLocation = UINT64_MAX;
  fixture.disk.resetActivity();
  EXPECT_EQ(fixture.file->read(Page, 2 * Page, 0), 2 * Page);
  EXPECT_EQ(fixture.disk.readBatchSizes, (std::vector<size_t>{1}));
  for (size_t i = 0; i < 3; ++i)
    EXPECT_TRUE(fixture.file->evictUpper(i * Page));
  EXPECT_TRUE(fixture.disk.dataWrites.empty());
  EXPECT_TRUE(fixture.disk.balanced());
}

TEST(Ext2Writeback, MultiPageSubpageReadsHandleSparseTailsAndConstituentFailure) {
  constexpr size_t Page = FillBatchDisk::Page;
  constexpr size_t Block = 1024;
  FillBatchFixture fixture(Block, 9 * Page + 333, 1);
  EXPECT_EQ(fixture.file->populateRange(0, fixture.fileSize), fixture.fileSize);
  EXPECT_EQ(fixture.file->upperReadBatches, (std::vector<size_t>{10}));
  EXPECT_EQ(fixture.disk.readBatchSizes, (std::vector<size_t>{Disk::MaxReadBuffers, 4}));
  const auto first = fixture.file->upperContents(0);
  ASSERT_EQ(first.size(), Page);
  for (size_t i = 0; i < first.size(); ++i)
    ASSERT_EQ(first[i], i >= Block && i < 2 * Block ? 0 : 0x11);
  const auto tail = fixture.file->upperContents(9 * Page);
  ASSERT_EQ(tail.size(), Page);
  EXPECT_TRUE(
      std::all_of(tail.begin(), tail.begin() + 333, [](uint8_t value) { return value == 0x11; }));
  EXPECT_TRUE(
      std::all_of(tail.begin() + 333, tail.end(), [](uint8_t value) { return value == 0; }));

  ASSERT_TRUE(fixture.file->evictUpper(8 * Page));
  fixture.disk.failedReadLocation = fixture.blocks[34] * Block;
  EXPECT_EQ(fixture.file->populateRange(8 * Page, Page), 0U);
  EXPECT_FALSE(fixture.file->hasUpper(8 * Page));
  EXPECT_TRUE(fixture.file->hasUpper(9 * Page));
  fixture.disk.failedReadLocation = UINT64_MAX;
  EXPECT_EQ(fixture.file->populateRange(8 * Page, Page), Page);
  for (size_t i = 0; i < 10; ++i)
    EXPECT_TRUE(fixture.file->evictUpper(i * Page));
  EXPECT_TRUE(fixture.disk.dataWrites.empty());
  EXPECT_TRUE(fixture.disk.balanced());
}
