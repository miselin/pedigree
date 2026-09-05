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

class GrowthDisk final : public Disk {
 public:
  static constexpr size_t kBlockCount = 32768;

  GrowthDisk() : blocks(kBlockCount) {}

  BufferView read(uint64_t location) override {
    if (location >= getSize()) {
      return BufferView();
    }
    reads.push_back(location);
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

  void unpin(uint64_t) override {}

  uint8_t* getBlock(size_t block) {
    if (!blocks[block]) {
      blocks[block] = std::unique_ptr<uint8_t[]>(new uint8_t[kBlockSize]());
    }
    return blocks[block].get();
  }

  std::vector<std::unique_ptr<uint8_t[]>> blocks;
  std::vector<uint64_t> reads;
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

TEST(Ext2Growth, SparseAllocationFailurePreservesHoleAndSize) {
  MutableInodeFixture fixture;
  fixture.superblock.s_free_blocks_count = 0;
  fixture.group.bg_free_blocks_count = 0;
  Inode* inode = Ext2WritebackTestPeer::getInode(fixture.filesystem, 3);
  ASSERT_NE(inode, nullptr);
  inode->i_mode = HOST_TO_LITTLE16(EXT2_S_IFREG | 0600);
  inode->i_links_count = HOST_TO_LITTLE16(1);
  inode->i_size = HOST_TO_LITTLE32(2 * kBlockSize);
  Ext2File file(String("full-device"), 3, inode, &fixture.filesystem);
  const uint8_t source = 0x6b;
  EXPECT_EQ(file.write(17, 1, reinterpret_cast<uintptr_t>(&source)), 0U);
  EXPECT_FALSE(file.prepareSharedMapping(kBlockSize, kBlockSize));
  uint8_t value = 0xff;
  ASSERT_EQ(file.read(17, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  EXPECT_EQ(value, 0U);
  EXPECT_EQ(file.getSize(), 2 * kBlockSize);
  EXPECT_EQ(file.getAttributes().blocks, 0U);
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
