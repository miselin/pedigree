/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/utilities/utility.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "modules/system/ext2/Ext2Directory.h"
#include "modules/system/ext2/Ext2Filesystem.h"
#include "modules/system/ext2/ext2.h"
#include <gtest/gtest.h>

namespace {
constexpr size_t kBlockSize = 4096;
constexpr size_t kBlocksPerGroup = 128;
constexpr size_t kInodesPerGroup = 64;
constexpr size_t kRootBlock = 16;
constexpr size_t kChildTable = 132;

class DirectorySyncDisk final : public Disk {
 public:
  explicit DirectorySyncDisk(bool indirectDirectory = false,
                             size_t inodesPerGroup = kInodesPerGroup)
      : cache(2 * kBlocksPerGroup * kBlockSize), durable(cache.size()), pins(2 * kBlocksPerGroup) {
    const size_t tableBlocks = (inodesPerGroup * sizeof(Inode) + kBlockSize - 1) / kBlockSize;
    const size_t rootBlock = std::max(kRootBlock, 4 + tableBlocks);
    Superblock* superblock = reinterpret_cast<Superblock*>(cache.data() + 1024);
    superblock->s_magic = HOST_TO_LITTLE16(0xef53);
    superblock->s_state = HOST_TO_LITTLE16(EXT2_STATE_CLEAN);
    superblock->s_rev_level = HOST_TO_LITTLE32(1);
    superblock->s_inode_size = HOST_TO_LITTLE16(sizeof(Inode));
    superblock->s_feature_incompat = HOST_TO_LITTLE32(2);
    superblock->s_log_block_size = HOST_TO_LITTLE32(2);
    superblock->s_blocks_count = HOST_TO_LITTLE32(2 * kBlocksPerGroup);
    superblock->s_blocks_per_group = HOST_TO_LITTLE32(kBlocksPerGroup);
    superblock->s_inodes_count = HOST_TO_LITTLE32(2 * inodesPerGroup);
    superblock->s_inodes_per_group = HOST_TO_LITTLE32(inodesPerGroup);
    superblock->s_free_inodes_count = HOST_TO_LITTLE32(inodesPerGroup);

    GroupDesc* groups = reinterpret_cast<GroupDesc*>(cache.data() + kBlockSize);
    for (size_t group = 0; group < 2; ++group) {
      const size_t base = group * kBlocksPerGroup;
      const size_t reserved =
          !group ? rootBlock + (indirectDirectory ? 14 : 1) : std::max(size_t{16}, 4 + tableBlocks);
      groups[group].bg_block_bitmap = HOST_TO_LITTLE32(base + 2);
      groups[group].bg_inode_bitmap = HOST_TO_LITTLE32(base + 3);
      groups[group].bg_inode_table = HOST_TO_LITTLE32(base + 4);
      groups[group].bg_free_blocks_count = HOST_TO_LITTLE16(kBlocksPerGroup - reserved);
      groups[group].bg_free_inodes_count = HOST_TO_LITTLE16(group ? inodesPerGroup : 0);
      superblock->s_free_blocks_count = HOST_TO_LITTLE32(
          LITTLE_TO_HOST32(superblock->s_free_blocks_count) + kBlocksPerGroup - reserved);
      for (size_t block = 0; block < reserved; ++block) {
        cache[(base + 2) * kBlockSize + block / 8] |= 1U << (block % 8);
      }
    }
    // Force newly created children into a different group and inode-table page.
    std::fill_n(cache.data() + 3 * kBlockSize, inodesPerGroup / 8, 0xff);

    Inode& root = inode(cache, 2);
    root.i_mode = HOST_TO_LITTLE16(EXT2_S_IFDIR | 0755);
    root.i_links_count = HOST_TO_LITTLE16(2);
    const size_t dataBlocks = indirectDirectory ? 13 : 1;
    root.i_size = HOST_TO_LITTLE32(dataBlocks * kBlockSize);
    root.i_blocks =
        HOST_TO_LITTLE32((dataBlocks + (indirectDirectory ? 1 : 0)) * (kBlockSize / 512));
    for (size_t block = 0; block < dataBlocks; ++block) {
      const size_t physical = rootBlock + block + (block >= 12 ? 1 : 0);
      if (block < 12) {
        root.i_block[block] = HOST_TO_LITTLE32(physical);
      } else {
        root.i_block[12] = HOST_TO_LITTLE32(rootBlock + 12);
        *reinterpret_cast<uint32_t*>(cache.data() + (rootBlock + 12) * kBlockSize) =
            HOST_TO_LITTLE32(physical);
      }
      Dir* empty = reinterpret_cast<Dir*>(cache.data() + physical * kBlockSize);
      empty->d_reclen = HOST_TO_LITTLE16(kBlockSize);
    }
    makeEntry(cache.data() + rootBlock * kBlockSize, 2, ".", 12);
    makeEntry(cache.data() + rootBlock * kBlockSize + 12, 2, "..", kBlockSize - 12);
    durable = cache;
  }

  BufferView read(uint64_t location) override {
    if (location >= cache.size() || location == failedRead) {
      return BufferView();
    }
    ++pins[location / kBlockSize];
    return BufferView(cache.data() + location, kBlockSize - location % kBlockSize);
  }

  void write(uint64_t) override {}

  bool sync(uint64_t location, bool async) override {
    syncs.push_back(location);
    if (async || location >= cache.size() || location == failedSync ||
        !pins[location / kBlockSize]) {
      return false;
    }
    const size_t start = (location / kBlockSize) * kBlockSize;
    std::copy_n(cache.data() + start, kBlockSize, durable.data() + start);
    return true;
  }

  bool syncPages(const uint64_t* locations, size_t count) override {
    if (!locations || !count || count > MaxSyncPages)
      return false;
    batches.emplace_back(locations, locations + count);
    const bool succeeded = Disk::syncPages(locations, count);
    // A failed shared barrier leaves the whole batch unconfirmed, even if
    // individual writes reached the backing store before the failure.
    return succeeded &&
           std::find(locations, locations + count, failedBatchLocation) == locations + count;
  }

  bool pin(uint64_t location) override {
    if (location >= cache.size()) {
      return false;
    }
    ++pins[location / kBlockSize];
    return true;
  }

  void unpin(uint64_t location) override {
    ASSERT_LT(location, cache.size());
    ASSERT_GT(pins[location / kBlockSize], 0U);
    --pins[location / kBlockSize];
  }

  bool retireCachePage(uint64_t location) override {
    return location < cache.size() && !pins[location / kBlockSize];
  }

  size_t getSize() const override {
    return cache.size();
  }

  size_t getBlockSize() const override {
    return kBlockSize;
  }

  static Inode& inode(std::vector<uint8_t>& bytes, uint32_t number) {
    const Superblock* superblock = reinterpret_cast<const Superblock*>(bytes.data() + 1024);
    const size_t inodesPerGroup = LITTLE_TO_HOST32(superblock->s_inodes_per_group);
    const size_t group = (number - 1) / inodesPerGroup;
    const size_t index = (number - 1) % inodesPerGroup;
    return *reinterpret_cast<Inode*>(bytes.data() + (group * kBlocksPerGroup + 4) * kBlockSize +
                                     index * sizeof(Inode));
  }

  uint32_t durableEntry(uint32_t directory, const char* name) {
    const Inode& metadata = inode(durable, directory);
    for (size_t i = 0; i < LITTLE_TO_HOST32(metadata.i_size) / kBlockSize; ++i) {
      const uint32_t block =
          i < 12
              ? LITTLE_TO_HOST32(metadata.i_block[i])
              : LITTLE_TO_HOST32(reinterpret_cast<const uint32_t*>(
                    durable.data() + LITTLE_TO_HOST32(metadata.i_block[12]) * kBlockSize)[i - 12]);
      for (size_t offset = 0; offset < kBlockSize;) {
        const Dir* entry =
            reinterpret_cast<const Dir*>(durable.data() + block * kBlockSize + offset);
        const size_t length = LITTLE_TO_HOST16(entry->d_reclen);
        if (length < offsetof(Dir, d_name) || length > kBlockSize - offset) {
          return 0;
        }
        if (entry->d_inode && entry->d_namelen == std::strlen(name) &&
            !std::memcmp(entry->d_name, name, entry->d_namelen)) {
          return LITTLE_TO_HOST32(entry->d_inode);
        }
        offset += length;
      }
    }
    return 0;
  }

  bool durableInodeAllocated(uint32_t number) {
    const Superblock* superblock = reinterpret_cast<const Superblock*>(durable.data() + 1024);
    const size_t inodesPerGroup = LITTLE_TO_HOST32(superblock->s_inodes_per_group);
    const size_t group = (number - 1) / inodesPerGroup;
    const size_t index = (number - 1) % inodesPerGroup;
    return durable[(group * kBlocksPerGroup + 3) * kBlockSize + index / 8] & (1U << (index % 8));
  }

  std::vector<uint8_t> cache;
  std::vector<uint8_t> durable;
  std::vector<size_t> pins;
  std::vector<uint64_t> syncs;
  std::vector<std::vector<uint64_t>> batches;
  uint64_t failedSync = UINT64_MAX;
  uint64_t failedBatchLocation = UINT64_MAX;
  uint64_t failedRead = UINT64_MAX;

 private:
  static void makeEntry(uint8_t* destination, uint32_t inode, const char* name, size_t length) {
    Dir* entry = reinterpret_cast<Dir*>(destination);
    entry->d_inode = HOST_TO_LITTLE32(inode);
    entry->d_reclen = HOST_TO_LITTLE16(length);
    entry->d_namelen = std::strlen(name);
    entry->d_file_type = EXT2_DIRECTORY;
    std::memcpy(entry->d_name, name, entry->d_namelen);
  }
};

class Ext2DirectorySyncFailure : public testing::TestWithParam<uint64_t> {};

TEST_P(Ext2DirectorySyncFailure, CreateReportsBackendFailureAndRetriesThroughFileInterface) {
  DirectorySyncDisk disk;
  Ext2Filesystem filesystem;
  ASSERT_TRUE(filesystem.initialise(&disk));
  File* parent = filesystem.getRoot();
  ASSERT_TRUE(filesystem.Filesystem::createFile(String("created").view(), 0644, parent));
  EXPECT_EQ(disk.durableEntry(2, "created"), 0U);

  disk.failedSync = GetParam();
  EXPECT_FALSE(parent->sync());
  EXPECT_FALSE(parent->sync());
  EXPECT_GE(std::count(disk.syncs.begin(), disk.syncs.end(), GetParam()), 2);
  disk.failedSync = UINT64_MAX;
  ASSERT_TRUE(parent->sync());
  EXPECT_EQ(disk.durableEntry(2, "created"), 65U);
  EXPECT_EQ(LITTLE_TO_HOST16(DirectorySyncDisk::inode(disk.durable, 65).i_links_count), 1U);
  EXPECT_TRUE(disk.durableInodeAllocated(65));
  EXPECT_EQ(disk.pins[kRootBlock], 0U);
}

INSTANTIATE_TEST_CASE_P(DataAndMetadata, Ext2DirectorySyncFailure,
                         testing::Values(kRootBlock* kBlockSize, kChildTable* kBlockSize,
                                         131 * kBlockSize, 4 * kBlockSize, 3 * kBlockSize,
                                         2 * kBlockSize, kBlockSize, 1024ULL));

TEST(Ext2DirectorySync, ReplacementRenameAndUnlinkPersistChildRetirementAcrossGroups) {
  DirectorySyncDisk disk;
  Ext2Filesystem filesystem;
  ASSERT_TRUE(filesystem.initialise(&disk));
  File* parent = filesystem.getRoot();
  ASSERT_TRUE(filesystem.Filesystem::createFile(String("temporary").view(), 0644, parent));
  ASSERT_TRUE(filesystem.Filesystem::createFile(String("saved").view(), 0644, parent));
  ASSERT_TRUE(parent->sync());
  ASSERT_EQ(disk.durableEntry(2, "temporary"), 65U);
  ASSERT_EQ(disk.durableEntry(2, "saved"), 66U);

  ASSERT_TRUE(
      filesystem.rename(String("temporary").view(), parent, String("saved").view(), parent));
  disk.failedSync = kChildTable * kBlockSize;
  EXPECT_FALSE(parent->sync());
  disk.failedSync = UINT64_MAX;
  ASSERT_TRUE(parent->sync());
  EXPECT_EQ(disk.durableEntry(2, "temporary"), 0U);
  EXPECT_EQ(disk.durableEntry(2, "saved"), 65U);
  EXPECT_FALSE(disk.durableInodeAllocated(66));
  EXPECT_EQ(LITTLE_TO_HOST16(DirectorySyncDisk::inode(disk.durable, 66).i_links_count), 0U);

  ASSERT_TRUE(filesystem.remove(String("saved").view(), parent));
  disk.failedSync = 131 * kBlockSize;
  EXPECT_FALSE(parent->sync());
  disk.failedSync = UINT64_MAX;
  ASSERT_TRUE(parent->sync());
  EXPECT_EQ(disk.durableEntry(2, "saved"), 0U);
  EXPECT_FALSE(disk.durableInodeAllocated(65));
}

TEST(Ext2DirectorySync, IndirectDataAndMappingFailuresAreRetried) {
  DirectorySyncDisk disk(true);
  Ext2Filesystem filesystem;
  ASSERT_TRUE(filesystem.initialise(&disk));
  File* directory = filesystem.getRoot();
  disk.failedSync = 29 * kBlockSize;
  EXPECT_FALSE(directory->sync());
  disk.failedSync = 28 * kBlockSize;
  EXPECT_FALSE(directory->sync());
  disk.failedSync = UINT64_MAX;
  disk.failedRead = 29 * kBlockSize;
  EXPECT_FALSE(directory->sync());
  disk.failedRead = UINT64_MAX;
  EXPECT_TRUE(directory->sync());
  EXPECT_EQ(disk.pins[28], 0U);
  EXPECT_EQ(disk.pins[29], 0U);
}

TEST(Ext2DirectorySync, ParentSyncPersistsNewDirectoryAndMovedParentThroughAlias) {
  DirectorySyncDisk disk;
  Ext2Filesystem filesystem;
  ASSERT_TRUE(filesystem.initialise(&disk));
  File* root = filesystem.getRoot();
  ASSERT_TRUE(filesystem.Filesystem::createDirectory(String("from").view(), 0755, root));
  ASSERT_TRUE(filesystem.Filesystem::createDirectory(String("to").view(), 0755, root));
  ASSERT_TRUE(root->sync());
  const uint32_t fromInode = disk.durableEntry(2, "from");
  const uint32_t toInode = disk.durableEntry(2, "to");
  ASSERT_NE(fromInode, 0U);
  ASSERT_NE(toInode, 0U);
  EXPECT_EQ(disk.durableEntry(fromInode, ".."), 2U);
  EXPECT_EQ(disk.durableEntry(toInode, ".."), 2U);

  File* from = filesystem.find(String("from"), root);
  File* to = filesystem.find(String("to"), root);
  ASSERT_NE(from, nullptr);
  ASSERT_NE(to, nullptr);
  ASSERT_TRUE(filesystem.Filesystem::createDirectory(String("moved").view(), 0755, from));
  ASSERT_TRUE(from->sync());
  const uint32_t movedInode = disk.durableEntry(fromInode, "moved");
  ASSERT_NE(movedInode, 0U);
  const uint32_t movedBlock =
      LITTLE_TO_HOST32(DirectorySyncDisk::inode(disk.cache, movedInode).i_block[0]);
  ASSERT_TRUE(filesystem.rename(String("moved").view(), from, String("moved").view(), to));

  disk.failedSync = movedBlock * kBlockSize;
  EXPECT_FALSE(from->sync());
  EXPECT_FALSE(to->sync());
  EXPECT_EQ(disk.durableEntry(movedInode, ".."), fromInode);
  {
    Ext2Directory alias(String("to-alias"), toInode, &DirectorySyncDisk::inode(disk.cache, toInode),
                        &filesystem, root);
    disk.failedSync = UINT64_MAX;
    disk.failedRead = movedBlock * kBlockSize;
    EXPECT_FALSE(static_cast<File*>(&alias)->sync());
    disk.failedRead = UINT64_MAX;
    ASSERT_TRUE(static_cast<File*>(&alias)->sync());
  }
  ASSERT_TRUE(from->sync());
  EXPECT_EQ(disk.durableEntry(fromInode, "moved"), 0U);
  EXPECT_EQ(disk.durableEntry(toInode, "moved"), movedInode);
  EXPECT_EQ(disk.durableEntry(movedInode, ".."), toInode);
  EXPECT_EQ(disk.pins[movedBlock], 0U);
}

TEST(Ext2DirectorySync, LoadedMetadataUsesBoundedBatchesWithoutDroppingDependencies) {
  constexpr size_t inodesPerGroup = 2048;
  constexpr size_t tableBlocks = inodesPerGroup * sizeof(Inode) / kBlockSize;
  DirectorySyncDisk disk(false, inodesPerGroup);
  Ext2Filesystem filesystem;
  ASSERT_TRUE(filesystem.initialise(&disk));
  File* parent = filesystem.getRoot();
  ASSERT_TRUE(parent->sync());
  ASSERT_TRUE(filesystem.Filesystem::createFile(String("created").view(), 0644, parent));
  const auto pins = disk.pins;
  disk.syncs.clear();
  disk.batches.clear();

  ASSERT_TRUE(parent->sync());
  ASSERT_EQ(disk.batches.size(), 3U);
  std::vector<uint64_t> submitted;
  for (const auto& batch : disk.batches) {
    EXPECT_FALSE(batch.empty());
    EXPECT_LE(batch.size(), Disk::MaxSyncPages);
    submitted.insert(submitted.end(), batch.begin(), batch.end());
  }
  for (size_t group = 0; group < 2; ++group) {
    const size_t base = group * kBlocksPerGroup;
    for (size_t block = 0; block < tableBlocks; ++block)
      EXPECT_EQ(std::count(submitted.begin(), submitted.end(), (base + 4 + block) * kBlockSize), 1);
    for (size_t block : {base + 2, base + 3, size_t{1}}) {
      const bool submittedBlock =
          std::find(submitted.begin(), submitted.end(), block * kBlockSize) != submitted.end();
      EXPECT_EQ(submittedBlock, pins[block] != 0);
    }
  }
  // Directory data and the inode's existing ordered metadata path remain
  // separate; loaded table size must not add individual durability barriers.
  EXPECT_EQ(disk.syncs.size() - submitted.size(), 6U);
  EXPECT_EQ(disk.durableEntry(2, "created"), inodesPerGroup + 1);
  EXPECT_TRUE(disk.durableInodeAllocated(inodesPerGroup + 1));
  EXPECT_EQ(disk.pins, pins);
}

class Ext2DirectoryBatchFailure : public testing::TestWithParam<uint64_t> {};

TEST_P(Ext2DirectoryBatchFailure, SharedBarrierFailureRetriesCompleteMetadataAndNewChanges) {
  constexpr size_t inodesPerGroup = 2048;
  DirectorySyncDisk disk(false, inodesPerGroup);
  Ext2Filesystem filesystem;
  ASSERT_TRUE(filesystem.initialise(&disk));
  File* parent = filesystem.getRoot();
  ASSERT_TRUE(parent->sync());
  ASSERT_TRUE(filesystem.Filesystem::createFile(String("created").view(), 0644, parent));
  const auto pins = disk.pins;
  disk.batches.clear();
  disk.failedBatchLocation = GetParam();

  EXPECT_FALSE(parent->sync());
  const auto firstBatches = disk.batches;
  ASSERT_EQ(firstBatches.size(), 3U);
  disk.batches.clear();
  EXPECT_FALSE(parent->sync());
  EXPECT_EQ(disk.batches, firstBatches);

  ASSERT_TRUE(filesystem.Filesystem::createFile(String("later").view(), 0600, parent));
  disk.failedBatchLocation = UINT64_MAX;
  disk.batches.clear();
  ASSERT_TRUE(parent->sync());
  EXPECT_EQ(disk.batches, firstBatches);
  EXPECT_EQ(disk.durableEntry(2, "created"), inodesPerGroup + 1);
  EXPECT_EQ(disk.durableEntry(2, "later"), inodesPerGroup + 2);
  EXPECT_TRUE(disk.durableInodeAllocated(inodesPerGroup + 1));
  EXPECT_TRUE(disk.durableInodeAllocated(inodesPerGroup + 2));
  EXPECT_EQ(LITTLE_TO_HOST16(DirectorySyncDisk::inode(disk.durable, inodesPerGroup + 2).i_mode),
            EXT2_S_IFREG | 0600);
  EXPECT_EQ(disk.pins, pins);
}

INSTANTIATE_TEST_CASE_P(FirstMiddleAndLastBatch, Ext2DirectoryBatchFailure,
                        testing::Values(4 * kBlockSize, 132 * kBlockSize, 195 * kBlockSize));
}  // namespace
