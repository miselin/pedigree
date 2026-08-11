/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/utilities/utility.h"

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
};

uint32_t getUnixTimestamp() {
  return 0;
}

namespace {
constexpr uint32_t kBlockSize = 4096;

class TrackingDisk final : public Disk {
 public:
  uintptr_t read(uint64_t location) override {
    reads.push_back(location);
    return 0;
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
  std::vector<uint64_t> alignments;
  std::vector<uint64_t> reads;
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

TEST(PartitionWriteback, AlignsAndTranslatesFlush) {
  constexpr uint64_t kStart = 1536;
  constexpr uint64_t kLength = 4 * kBlockSize;
  constexpr uint64_t kLocation = 1024;

  TrackingDisk parent;
  Partition partition(String("test"), kStart, kLength);
  partition.setParent(&parent);

  partition.flush(kLocation);
  ASSERT_EQ(parent.alignments.size(), 1U);
  EXPECT_EQ(parent.alignments[0], kStart);
  ASSERT_EQ(parent.flushes.size(), 1U);
  EXPECT_EQ(parent.flushes[0], kStart + kLocation);

  partition.flush(kLength - kBlockSize);
  ASSERT_EQ(parent.flushes.size(), 2U);
  EXPECT_EQ(parent.flushes[1], kStart + kLength - kBlockSize);

  partition.flush(kLength - kBlockSize + 1);
  partition.flush(kLength);
  partition.flush(UINT64_MAX);
  EXPECT_EQ(parent.alignments.size(), 1U);
  EXPECT_EQ(parent.flushes.size(), 2U);
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
