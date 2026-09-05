/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/machine/Disk.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "modules/system/fat/FatFilesystem.h"
#include "modules/system/vfs/Symlink.h"
#include <gtest/gtest.h>

class FatWritebackTestPeer {
 public:
  static bool evict(FatFile& file, size_t offset) {
    return file.m_FileBlockCache.evict(offset);
  }

  static bool exists(FatFile& file, size_t offset) {
    return file.m_FileBlockCache.exists(offset, TargetInfo::getPageSize());
  }
};

namespace {
constexpr size_t SectorSize = 512;
constexpr size_t FatLocation = SectorSize;
constexpr size_t DirectoryLocation = 8 * SectorSize;
constexpr size_t FileLocation = DirectoryLocation + SectorSize;

class FatDisk final : public Disk {
 public:
  static constexpr size_t DiskSize = 80 * SectorSize;
  FatDisk() : bytes(DiskSize, 0), stored(DiskSize, 0) {}

  BufferView read(uint64_t location) override {
    if (location >= DiskSize || location == failedRead)
      return BufferView();
    ++references;
    return BufferView(bytes.data() + location, SectorSize - location % SectorSize);
  }

  void write(uint64_t location) override {
    writes.push_back(location);
  }

  bool sync(uint64_t location, bool async) override {
    syncs.push_back(location);
    if (location == failedSync)
      return false;
    const size_t start = location - location % SectorSize;
    std::copy_n(bytes.data() + start, SectorSize, stored.data() + start);
    return true;
  }

  bool pin(uint64_t location) override {
    if (location >= DiskSize)
      return false;
    ++references;
    return true;
  }

  void unpin(uint64_t) override {
    --references;
  }

  size_t getSize() const override {
    return DiskSize;
  }

  size_t getBlockSize() const override {
    return SectorSize;
  }

  uint32_t storedSize() const {
    const Dir* entry = reinterpret_cast<const Dir*>(stored.data() + DirectoryLocation);
    return LITTLE_TO_HOST32(entry->DIR_FileSize);
  }

  std::vector<uint8_t> bytes;
  std::vector<uint8_t> stored;
  std::vector<uint64_t> writes;
  std::vector<uint64_t> syncs;
  uint64_t failedRead = UINT64_MAX;
  uint64_t failedSync = UINT64_MAX;
  int references = 0;
};

class FatHarness final : public FatFilesystem {
 public:
  void configure(FatDisk& disk, FatType type = FAT16) {
    m_pDisk = &disk;
    m_Type = type;
    m_BlockSize = SectorSize;
    m_ClusterCount = 64;
    m_DataAreaStart = 8;
    m_FatSector = 1;
    m_RootDir.sector = 4;
    m_RootDirCount = 1;
    m_Superblock.BPB_BytsPerSec = SectorSize;
    m_Superblock.BPB_SecPerClus = 1;
    m_Superblock.BPB_NumFATs = 1;
    m_Superblock.BPB_FATSz16 = 1;
    m_Superblock32.BPB_FATSz32 = 1;
    m_FreeClusterHint = 3;
    m_FatCache.insert(0, reinterpret_cast<uintptr_t>(disk.bytes.data() + FatLocation));
    seed(disk, 2, eofValue());
  }

  void seed(FatDisk& disk, uint32_t cluster, uint32_t next) {
    const size_t width = m_Type == FAT32 ? 4 : 2;
    for (size_t byte = 0; byte < width; ++byte)
      disk.bytes[FatLocation + cluster * width + byte] = static_cast<uint8_t>(next >> (8 * byte));
  }

  void chain(FatDisk& disk, const std::vector<uint32_t>& clusters, size_t size) {
    for (size_t i = 0; i < clusters.size(); ++i)
      seed(disk, clusters[i], i + 1 == clusters.size() ? eofValue() : clusters[i + 1]);
    Dir* entry = reinterpret_cast<Dir*>(disk.bytes.data() + DirectoryLocation);
    entry->DIR_FstClusLO = HOST_TO_LITTLE16(clusters.empty() ? 0 : clusters[0]);
    entry->DIR_FileSize = HOST_TO_LITTLE32(size);
    disk.stored = disk.bytes;
  }

  uint32_t entry(uint32_t cluster) {
    return getClusterEntry(cluster);
  }

  bool setEntry(uint32_t cluster, uint32_t value) {
    return setClusterEntry(cluster, value);
  }

  void mirrored() {
    m_Superblock.BPB_NumFATs = 2;
  }

  bool writeSectors(uint32_t sector, size_t length, uintptr_t source) {
    return writeSectorBlock(sector, length, source);
  }

  FatDirectory* createRoot() {
    FatFileInfo info = {};
    m_pRoot = new FatDirectory(String("root"), 0, this, nullptr, info);
    return static_cast<FatDirectory*>(m_pRoot);
  }
};

struct FatFixture {
  explicit FatFixture(size_t size, std::vector<uint32_t> clusters = {3}, FatType type = FAT16)
      : file(String("checked"), 0, 0, 0, clusters.empty() ? 0 : clusters[0], &filesystem, size, 2) {
    filesystem.configure(disk, type);
    filesystem.chain(disk, clusters, size);
  }

  FatDisk disk;
  FatHarness filesystem;
  FatFile file;
};
}  // namespace

TEST(FatWriteback, FailedCacheWritebackRetainsPageAndRetries) {
  FatFixture fixture(512);
  const uint8_t source = 0x6B;
  ASSERT_EQ(fixture.file.write(17, 1, reinterpret_cast<uintptr_t>(&source)), 1U);
  fixture.disk.failedSync = FileLocation;
  EXPECT_FALSE(fixture.file.sync());
  EXPECT_FALSE(fixture.file.sync(17, false));
  EXPECT_FALSE(FatWritebackTestPeer::evict(fixture.file, 0));
  EXPECT_TRUE(FatWritebackTestPeer::exists(fixture.file, 0));
  uint8_t value = 0;
  ASSERT_EQ(fixture.file.read(17, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  EXPECT_EQ(value, source);
  EXPECT_EQ(fixture.disk.stored[FileLocation + 17], 0U);

  fixture.disk.failedSync = UINT64_MAX;
  EXPECT_TRUE(fixture.file.sync());
  EXPECT_EQ(fixture.disk.stored[FileLocation + 17], source);
  EXPECT_TRUE(FatWritebackTestPeer::evict(fixture.file, 0));
  EXPECT_FALSE(FatWritebackTestPeer::exists(fixture.file, 0));
  EXPECT_EQ(fixture.disk.references, 0);
}

TEST(FatWriteback, CountsOnlyCompletedClustersAndPreservesUnalignedBytes) {
  FatFixture fixture(3 * SectorSize, {3, 4, 5});
  std::array<uint8_t, 3 * SectorSize> source;
  source.fill(0x5A);
  fixture.disk.failedRead = FileLocation + SectorSize;
  EXPECT_EQ(fixture.filesystem.write(&fixture.file, 0, source.size(),
                                     reinterpret_cast<uintptr_t>(source.data())),
            SectorSize);
  fixture.disk.failedRead = UINT64_MAX;
  fixture.disk.failedSync = FileLocation + SectorSize;
  EXPECT_EQ(fixture.filesystem.write(&fixture.file, 0, source.size(),
                                     reinterpret_cast<uintptr_t>(source.data())),
            SectorSize);
  fixture.disk.failedSync = UINT64_MAX;
  ASSERT_EQ(fixture.filesystem.write(&fixture.file, 0, source.size(),
                                     reinterpret_cast<uintptr_t>(source.data())),
            source.size());
  source.fill(0xC7);
  ASSERT_EQ(fixture.filesystem.write(&fixture.file, SectorSize - 1, 2 * SectorSize,
                                     reinterpret_cast<uintptr_t>(source.data())),
            2 * SectorSize);
  EXPECT_EQ(fixture.disk.stored[FileLocation + SectorSize - 2], 0x5A);
  EXPECT_EQ(fixture.disk.stored[FileLocation + SectorSize - 1], 0xC7);
  EXPECT_EQ(fixture.disk.stored[FileLocation + 3 * SectorSize - 2], 0xC7);
  EXPECT_EQ(fixture.disk.stored[FileLocation + 3 * SectorSize - 1], 0x5A);
  EXPECT_EQ(fixture.disk.references, 0);
}

TEST(FatWriteback, MetadataFailureDoesNotPublishSizeAndRetryIsAbsolute) {
  FatFixture fixture(0);
  const uint8_t source = 0x92;
  fixture.disk.failedSync = DirectoryLocation;
  EXPECT_EQ(fixture.file.write(0, 1, reinterpret_cast<uintptr_t>(&source)), 0U);
  EXPECT_EQ(fixture.file.getSize(), 0U);
  EXPECT_EQ(fixture.file.getAttributes().blocks, 1U);
  EXPECT_FALSE(fixture.file.sync());
  EXPECT_EQ(fixture.disk.storedSize(), 0U);
  fixture.disk.failedSync = UINT64_MAX;
  EXPECT_TRUE(fixture.file.sync());
  EXPECT_EQ(fixture.disk.storedSize(), 0U);
  ASSERT_EQ(fixture.file.write(0, 1, reinterpret_cast<uintptr_t>(&source)), 1U);
  EXPECT_TRUE(fixture.file.sync());
  EXPECT_EQ(fixture.disk.storedSize(), 1U);
  EXPECT_EQ(fixture.disk.stored[FileLocation], source);
  EXPECT_EQ(fixture.file.getAttributes().blocks, 1U);
  EXPECT_EQ(fixture.disk.references, 0);
}

TEST(FatWriteback, FailedAllocationRetainsLinkedReservationAcrossRetries) {
  FatFixture fixture(SectorSize);
  const uint8_t source = 0x81;
  fixture.disk.failedSync = FatLocation;
  EXPECT_EQ(fixture.file.write(SectorSize, 1, reinterpret_cast<uintptr_t>(&source)), 0U);
  EXPECT_EQ(fixture.file.getSize(), SectorSize);
  EXPECT_EQ(fixture.file.getAttributes().blocks, 2U);
  EXPECT_EQ(fixture.filesystem.entry(3), 4U);
  EXPECT_EQ(fixture.file.write(SectorSize, 1, reinterpret_cast<uintptr_t>(&source)), 0U);
  EXPECT_EQ(fixture.file.getAttributes().blocks, 2U);
  EXPECT_FALSE(fixture.file.sync());

  fixture.disk.failedSync = UINT64_MAX;
  ASSERT_EQ(fixture.file.write(SectorSize, 1, reinterpret_cast<uintptr_t>(&source)), 1U);
  EXPECT_TRUE(fixture.file.sync());
  EXPECT_EQ(fixture.file.getSize(), SectorSize + 1);
  EXPECT_EQ(fixture.file.getAttributes().blocks, 2U);
  EXPECT_EQ(fixture.filesystem.entry(5), 0U);
  EXPECT_EQ(fixture.disk.storedSize(), SectorSize + 1);
  EXPECT_EQ(fixture.disk.stored[FileLocation + SectorSize], source);
}

TEST(FatWriteback, MirrorFailureRemainsPendingUntilBothCopiesPersist) {
  FatFixture fixture(SectorSize);
  fixture.filesystem.mirrored();
  fixture.disk.failedSync = FatLocation + SectorSize;
  fixture.file.extend(SectorSize + 1);
  EXPECT_EQ(fixture.file.getSize(), SectorSize);
  EXPECT_EQ(fixture.file.getAttributes().blocks, 2U);
  EXPECT_FALSE(fixture.file.sync());
  fixture.disk.failedSync = UINT64_MAX;
  fixture.file.extend(SectorSize + 1);
  EXPECT_EQ(fixture.file.getSize(), SectorSize + 1);
  EXPECT_TRUE(fixture.file.sync());
  EXPECT_TRUE(std::equal(fixture.disk.stored.begin() + FatLocation,
                         fixture.disk.stored.begin() + FatLocation + SectorSize,
                         fixture.disk.stored.begin() + FatLocation + SectorSize));
}

TEST(FatWriteback, GrowthZeroesNewlyVisibleBytesBeforePublishingMetadata) {
  FatFixture fixture(1);
  std::fill(fixture.disk.bytes.begin() + FileLocation, fixture.disk.bytes.end(), 0xF3);
  fixture.disk.bytes[FileLocation] = 0x4A;
  fixture.disk.stored = fixture.disk.bytes;
  const uint8_t source = 0x71;
  const size_t offset = 2 * SectorSize + 13;
  ASSERT_EQ(fixture.file.write(offset, 1, reinterpret_cast<uintptr_t>(&source)), 1U);
  ASSERT_TRUE(fixture.file.sync());
  EXPECT_EQ(fixture.disk.stored[FileLocation], 0x4A);
  EXPECT_TRUE(std::all_of(fixture.disk.stored.begin() + FileLocation + 1,
                          fixture.disk.stored.begin() + FileLocation + offset,
                          [](uint8_t byte) { return byte == 0; }));
  EXPECT_EQ(fixture.disk.stored[FileLocation + offset], source);
  EXPECT_EQ(fixture.file.getAttributes().blocks, 3U);
}

TEST(FatMetadata, CountsAllocatedChainsIncludingEmptyFilesAndOddFat16Entries) {
  FatFixture fixture(0, {3, 4});
  EXPECT_EQ(fixture.file.getAttributes().size, 0U);
  EXPECT_EQ(fixture.file.getAttributes().blocks, 2U);
  EXPECT_TRUE(fixture.filesystem.setEntry(3, 0xFFF8));
  EXPECT_EQ(fixture.filesystem.entry(4), 0xFFF8U);
  EXPECT_EQ(fixture.file.getAttributes().blocks, 1U);
  FatFile empty(String("empty"), 0, 0, 0, 0, &fixture.filesystem, 0, 2);
  EXPECT_EQ(empty.getAttributes().blocks, 0U);
  FatFileInfo info = {};
  FatDirectory root(String("root"), 0, &fixture.filesystem, nullptr, info);
  EXPECT_EQ(root.getAttributes().blocks, 1U);
  FatDirectory directory(String("directory"), 2, &fixture.filesystem, nullptr, info);
  EXPECT_EQ(directory.getAttributes().blocks, 1U);
}

TEST(FatMetadata, CountsFat32EntriesWithoutReservedHighBits) {
  FatFixture fixture(0, {3, 4}, FAT32);
  fixture.filesystem.seed(fixture.disk, 3, 0xA0000004);
  EXPECT_EQ(fixture.file.getAttributes().blocks, 2U);
  ASSERT_TRUE(fixture.filesystem.setEntry(3, 0x0FFFFFF8));
  EXPECT_EQ(fixture.disk.bytes[FatLocation + 3 * 4 + 3], 0xAF);
  EXPECT_EQ(fixture.file.getAttributes().blocks, 1U);
}

TEST(FatMetadata, SnapshotSerializesWithGrowth) {
  FatFixture fixture(1);
  std::atomic<bool> done{false};
  std::atomic<bool> valid{true};
  std::thread reader([&] {
    while (!done.load()) {
      const File::Attributes attributes = fixture.file.getAttributes();
      if (attributes.blocks != (attributes.size + SectorSize - 1) / SectorSize)
        valid.store(false);
    }
  });
  const uint8_t source = 0x42;
  for (size_t cluster = 1; cluster < 12; ++cluster)
    EXPECT_EQ(fixture.file.write(cluster * SectorSize, 1, reinterpret_cast<uintptr_t>(&source)),
              1U);
  done.store(true);
  reader.join();
  EXPECT_TRUE(valid.load());
  EXPECT_TRUE(fixture.file.sync());
  EXPECT_EQ(fixture.file.getAttributes().blocks, 12U);
}

TEST(FatWriteback, SectorFailureReleasesEveryBorrowedBuffer) {
  FatFixture fixture(0);
  std::array<uint8_t, 3 * SectorSize> source;
  source.fill(0xA6);
  fixture.disk.failedSync = 11 * SectorSize;
  EXPECT_FALSE(fixture.filesystem.writeSectors(10, source.size(),
                                               reinterpret_cast<uintptr_t>(source.data())));
  EXPECT_EQ(fixture.disk.references, 0);
  EXPECT_EQ(fixture.disk.stored[10 * SectorSize], 0xA6);
  EXPECT_EQ(fixture.disk.stored[11 * SectorSize], 0U);
  EXPECT_EQ(fixture.disk.stored[12 * SectorSize], 0U);
  fixture.disk.failedSync = UINT64_MAX;
  EXPECT_TRUE(fixture.filesystem.writeSectors(10, source.size(),
                                              reinterpret_cast<uintptr_t>(source.data())));
  EXPECT_EQ(fixture.disk.references, 0);
}

TEST(FatWriteback, SymlinkCreationPublishesCompletedTargetAndCleansFailedWrites) {
  FatDisk disk;
  FatHarness filesystem;
  filesystem.configure(disk);
  FatDirectory* root = filesystem.createRoot();
  Filesystem& publicFilesystem = filesystem;
  disk.failedSync = FileLocation;
  EXPECT_FALSE(publicFilesystem.createSymlink(StringView("failed"), String("target"), root));
  Directory::ChildLease absent;
  EXPECT_EQ(root->lookupChild(HashedStringView(StringView("failed")), absent),
            Directory::LookupStatus::NotFound);
  EXPECT_EQ(filesystem.entry(3), 0U);

  disk.failedSync = UINT64_MAX;
  ASSERT_TRUE(publicFilesystem.createSymlink(StringView("link"), String("target"), root));
  Directory::ChildLease child;
  ASSERT_EQ(root->lookupChild(HashedStringView(StringView("link")), child),
            Directory::LookupStatus::Found);
  ASSERT_TRUE(child.get()->isSymlink());
  char target[16] = {};
  EXPECT_EQ(static_cast<Symlink*>(child.get())->followLink(target, sizeof(target)), 6);
  EXPECT_STREQ(target, "target");
  EXPECT_EQ(child.get()->getAttributes().size, 6U);
  EXPECT_EQ(child.get()->getAttributes().blocks, 1U);
  EXPECT_EQ(disk.references, 0);
}

TEST(FatWriteback, FailedUnownedReservationsRollbackBeforeAnotherCreate) {
  FatDisk disk;
  FatHarness filesystem;
  filesystem.configure(disk);
  FatDirectory* root = filesystem.createRoot();
  Filesystem& publicFilesystem = filesystem;
  disk.failedSync = FatLocation;
  for (size_t attempt = 0; attempt < 3; ++attempt) {
    EXPECT_FALSE(publicFilesystem.createSymlink(StringView("link"), String("target"), root));
    EXPECT_EQ(filesystem.entry(3), 0U);
    EXPECT_EQ(filesystem.entry(4), 0U);
  }
  disk.failedSync = UINT64_MAX;
  ASSERT_TRUE(publicFilesystem.createSymlink(StringView("link"), String("target"), root));
  Directory::ChildLease child;
  ASSERT_EQ(root->lookupChild(HashedStringView(StringView("link")), child),
            Directory::LookupStatus::Found);
  EXPECT_EQ(child.get()->getInode(), 3U);
  EXPECT_EQ(filesystem.entry(4), 0U);
}

TEST(FatWriteback, ReclamationRetainsEveryFreeAfterFlushFailure) {
  FatDisk disk;
  FatHarness filesystem;
  filesystem.configure(disk);
  FatDirectory* root = filesystem.createRoot();
  Filesystem& publicFilesystem = filesystem;
  std::array<char, SectorSize + 20> target;
  target.fill('t');
  ASSERT_TRUE(publicFilesystem.createSymlink(StringView("link"),
                                             String(target.data(), target.size(), true), root));
  ASSERT_EQ(filesystem.entry(3), 4U);
  disk.failedSync = FatLocation;
  EXPECT_TRUE(publicFilesystem.remove(StringView("link"), root));
  EXPECT_EQ(filesystem.entry(3), 0U);
  EXPECT_EQ(filesystem.entry(4), 0U);
  FatFile retry(String("retry"), 0, 0, 0, 0, &filesystem, 0, 2);
  EXPECT_FALSE(retry.sync());
  disk.failedSync = UINT64_MAX;
  EXPECT_TRUE(retry.sync());
  EXPECT_EQ(disk.stored[FatLocation + 3 * 2], 0U);
  EXPECT_EQ(disk.stored[FatLocation + 4 * 2], 0U);
  EXPECT_EQ(disk.references, 0);
}

TEST(FatWriteback, FailedTruncateDetachmentRetainsItsWholeChainForRetry) {
  FatFixture fixture(3 * SectorSize, {3, 4, 5});
  fixture.disk.failedSync = FatLocation;
  fixture.filesystem.truncate(&fixture.file);
  EXPECT_EQ(fixture.file.getSize(), 0U);
  EXPECT_EQ(fixture.filesystem.entry(3), 4U);
  EXPECT_EQ(fixture.filesystem.entry(4), 5U);
  EXPECT_EQ(fixture.file.getAttributes().blocks, 3U);
  EXPECT_FALSE(fixture.file.sync());
  fixture.disk.failedSync = UINT64_MAX;
  fixture.filesystem.truncate(&fixture.file);
  EXPECT_TRUE(fixture.file.sync());
  EXPECT_EQ(fixture.file.getAttributes().blocks, 1U);
  EXPECT_EQ(fixture.filesystem.entry(4), 0U);
  EXPECT_EQ(fixture.filesystem.entry(5), 0U);
}
