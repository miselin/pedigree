/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "modules/drivers/common/partition/Partition.h"
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

  std::vector<uint64_t> writes;
  std::vector<uint64_t> flushes;
  std::vector<uint64_t> retirements;
  std::vector<uint64_t> alignments;
  std::vector<uint64_t> reads;
  bool retirementResult = true;
};

class OrderedSyncFile final : public File {
 public:
  void sync(size_t offset, bool async) override {
    syncOffset = offset;
    syncAsync = async;
    syncSawPin = pinned;
    order.push_back('S');
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
  EXPECT_EQ(std::count(disk.writes.begin(), disk.writes.end(), targetTableLocation), 2);
  EXPECT_EQ(std::count(disk.writes.begin(), disk.writes.end(), previousTableLocation), 0);
  ASSERT_FALSE(disk.writes.empty());
  EXPECT_EQ(disk.writes.back(), targetTableLocation);

  disk.writes.clear();
  ASSERT_TRUE(Ext2WritebackTestPeer::releaseInode(filesystem, kTargetInode));
  EXPECT_EQ(LITTLE_TO_HOST16(inodeTableBlock1[0].i_links_count), 0);
  EXPECT_EQ(inodeBitmap[targetIndex / 8] & targetMask, 0);
  EXPECT_EQ(LITTLE_TO_HOST32(superblock.s_free_inodes_count), kFreeInodes + 1);
  EXPECT_EQ(LITTLE_TO_HOST16(groupDescriptor.bg_free_inodes_count), kGroupFreeInodes + 1);

  EXPECT_EQ(std::count(disk.writes.begin(), disk.writes.end(), targetTableLocation), 2);
  EXPECT_EQ(std::count(disk.writes.begin(), disk.writes.end(), previousTableLocation), 0);
  ASSERT_FALSE(disk.writes.empty());
  EXPECT_NE(disk.writes.back(), previousTableLocation);
  EXPECT_EQ(disk.writes.back(), targetTableLocation);
}

TEST(PartitionWriteback, AlignsAndTranslatesFlush) {
  constexpr uint64_t kStart = 1536;
  constexpr uint64_t kCachePageSize = TargetInfo::getPageSize();
  constexpr uint64_t kLength = 4 * kCachePageSize;
  constexpr uint64_t kLocation = 1024;
  constexpr uint64_t kInteriorLocation = (3 * kCachePageSize) + 512;

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

  partition.flush(kLength);
  partition.flush(UINT64_MAX);
  EXPECT_EQ(parent.alignments.size(), 1U);
  EXPECT_EQ(parent.flushes.size(), 2U);
}

TEST(PartitionWriteback, AlignsAndTranslatesRetirement) {
  constexpr uint64_t kStart = 1536;
  constexpr uint64_t kCachePageSize = TargetInfo::getPageSize();
  constexpr uint64_t kLength = 4 * kCachePageSize;
  constexpr uint64_t kLocation = 1024;
  constexpr uint64_t kInteriorLocation = (3 * kCachePageSize) + 512;

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
