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

#include "modules/system/fat/FatFilesystem.h"
#include <gtest/gtest.h>

namespace {
constexpr size_t SectorSize = 512;
constexpr size_t FatLocation = SectorSize;
constexpr size_t DirectoryLocation = 8 * SectorSize;
constexpr size_t FileLocation = 9 * SectorSize;
constexpr size_t InitialSize = 2 * TargetInfo::getPageSize() + 13;

class StateDisk final : public Disk {
 public:
  StateDisk() : bytes(256 * SectorSize, 0), stored(bytes) {}

  BufferView read(uint64_t location) override {
    if (location >= bytes.size())
      return BufferView();
    ++references;
    return BufferView(bytes.data() + location, SectorSize - location % SectorSize);
  }
  void write(uint64_t) override {}
  bool sync(uint64_t location, bool) override {
    if (location == failedSync)
      return false;
    const size_t start = location - location % SectorSize;
    std::copy_n(bytes.data() + start, SectorSize, stored.data() + start);
    return true;
  }
  bool syncAll() override {
    // A device flush cannot substitute for submitting the dirty file pages.
    ++deviceFlushes;
    return !failDeviceFlush;
  }
  bool pin(uint64_t location) override {
    if (location >= bytes.size())
      return false;
    ++references;
    return true;
  }
  void unpin(uint64_t) override {
    --references;
  }
  size_t getSize() const override {
    return bytes.size();
  }
  size_t getBlockSize() const override {
    return SectorSize;
  }
  uint16_t storedCluster(size_t cluster) const {
    const auto* fat = reinterpret_cast<const uint16_t*>(stored.data() + FatLocation);
    return LITTLE_TO_HOST16(fat[cluster]);
  }

  std::vector<uint8_t> bytes;
  std::vector<uint8_t> stored;
  uint64_t failedSync = UINT64_MAX;
  bool failDeviceFlush = false;
  size_t deviceFlushes = 0;
  int references = 0;
};

class StateFilesystem final : public FatFilesystem {
 public:
  void configure(StateDisk& disk) {
    m_pDisk = &disk;
    m_Type = FAT16;
    m_BlockSize = SectorSize;
    m_ClusterCount = 220;
    m_DataAreaStart = 8;
    m_FatSector = 1;
    m_Superblock.BPB_BytsPerSec = SectorSize;
    m_Superblock.BPB_SecPerClus = 1;
    m_Superblock.BPB_NumFATs = 1;
    m_Superblock.BPB_FATSz16 = 1;
    m_FreeClusterHint = 3;
    const size_t count = (InitialSize + SectorSize - 1) / SectorSize;
    auto* fat = reinterpret_cast<uint16_t*>(disk.bytes.data() + FatLocation);
    fat[2] = HOST_TO_LITTLE16(0xFFF8);
    for (size_t i = 0; i < count; ++i)
      fat[3 + i] = HOST_TO_LITTLE16(i + 1 == count ? 0xFFF8 : 4 + i);
    auto* entry = reinterpret_cast<Dir*>(disk.bytes.data() + DirectoryLocation);
    std::copy_n("STATE   BIN", 11, entry->DIR_Name);
    entry->DIR_FstClusLO = HOST_TO_LITTLE16(3);
    entry->DIR_FileSize = HOST_TO_LITTLE32(InitialSize);
    std::fill_n(disk.bytes.data() + FileLocation, count * SectorSize, 0xA7);
    disk.stored = disk.bytes;
    FatFileInfo times = {};
    m_pRoot = new FatDirectory(String("root"), 2, this, nullptr, times);
  }

  FatDirectory& root() {
    return *static_cast<FatDirectory*>(m_pRoot);
  }
};

struct StateFixture {
  StateFixture() {
    filesystem.configure(disk);
  }
  StateDisk disk;
  StateFilesystem filesystem;
};

uintptr_t directoryIdentifier(Directory& directory, const char* name) {
  struct Context {
    StringView name;
    uintptr_t identifier = 0;
  } context{StringView(name)};
  auto match = [](void* opaque, const Directory::DirectoryEntryView& entry) {
    auto& context = *static_cast<Context*>(opaque);
    if (entry.name == context.name)
      context.identifier = entry.inode;
    return true;
  };
  uint64_t cookie = 0;
  EXPECT_EQ(directory.enumerate(cookie, match, &context), Directory::ReadStatus::Complete);
  return context.identifier;
}
}  // namespace

TEST(FatState, PublicInodesMatchReaddirAndSurviveStorageAndNamespaceChanges) {
  StateFixture fixture;
  Filesystem& filesystem = fixture.filesystem;
  auto* root = &fixture.filesystem.root();

  const uintptr_t coldIdentifier = directoryIdentifier(*root, "state.bin");
  ASSERT_GE(coldIdentifier, 0x10000000U);
  Directory::ChildLease existing;
  ASSERT_NE(filesystem.findRetained(StringView("state.bin"), existing), nullptr);
  EXPECT_EQ(existing.get()->getAttributes().inode, coldIdentifier);
  EXPECT_EQ(existing.get()->getInode(), 3U);

  ASSERT_TRUE(filesystem.createFile(StringView("one"), 0600, root));
  ASSERT_TRUE(filesystem.createFile(StringView("two"), 0600, root));
  Directory::ChildLease first, second;
  ASSERT_NE(filesystem.findRetained(StringView("one"), first), nullptr);
  ASSERT_NE(filesystem.findRetained(StringView("two"), second), nullptr);
  const uint64_t firstIdentifier = first.get()->getAttributes().inode;
  const uint64_t secondIdentifier = second.get()->getAttributes().inode;
  EXPECT_NE(firstIdentifier, secondIdentifier);
  EXPECT_NE(firstIdentifier, coldIdentifier);
  EXPECT_NE(secondIdentifier, coldIdentifier);
  EXPECT_EQ(first.get()->getInode(), 0U);
  EXPECT_EQ(second.get()->getInode(), 0U);
  EXPECT_EQ(directoryIdentifier(*root, "one"), firstIdentifier);
  EXPECT_EQ(directoryIdentifier(*root, "two"), secondIdentifier);

  ASSERT_TRUE(first.get()->resize(SectorSize + 7));
  ASSERT_TRUE(first.get()->sync());
  EXPECT_NE(first.get()->getInode(), 0U);
  EXPECT_EQ(first.get()->getAttributes().inode, firstIdentifier);
  ASSERT_TRUE(first.get()->resize(0));
  ASSERT_TRUE(first.get()->sync());
  EXPECT_EQ(first.get()->getInode(), 0U);
  EXPECT_EQ(first.get()->getAttributes().inode, firstIdentifier);

  ASSERT_TRUE(filesystem.createDirectory(StringView("folder"), 0700, root));
  ASSERT_TRUE(filesystem.rename(StringView("one"), root, StringView("folder/renamed"), root));
  EXPECT_EQ(first.get()->getAttributes().inode, firstIdentifier);
  Directory::ChildLease folder, moved;
  ASSERT_NE(filesystem.findRetained(StringView("folder"), folder), nullptr);
  auto* destination = static_cast<Directory*>(folder.get());
  EXPECT_EQ(directoryIdentifier(*destination, "renamed"), firstIdentifier);
  destination->emptyCache();
  ASSERT_NE(filesystem.findRetained(StringView("folder/renamed"), moved), nullptr);
  ASSERT_NE(first.get(), moved.get());
  EXPECT_EQ(moved.get()->getAttributes().inode, firstIdentifier);

  const uint32_t oldCluster = static_cast<FatFile*>(moved.get())->getDirCluster();
  const uint32_t oldOffset = static_cast<FatFile*>(moved.get())->getDirOffset();
  ASSERT_TRUE(filesystem.remove(StringView("folder/renamed")));
  ASSERT_TRUE(filesystem.createFile(StringView("folder/renamed"), 0600, root));
  Directory::ChildLease replacement;
  ASSERT_NE(filesystem.findRetained(StringView("folder/renamed"), replacement), nullptr);
  EXPECT_EQ(static_cast<FatFile*>(replacement.get())->getDirCluster(), oldCluster);
  EXPECT_EQ(static_cast<FatFile*>(replacement.get())->getDirOffset(), oldOffset);
  const uint64_t replacementIdentifier = replacement.get()->getAttributes().inode;
  EXPECT_NE(replacementIdentifier, firstIdentifier);
  EXPECT_NE(replacementIdentifier, secondIdentifier);
  EXPECT_EQ(first.get()->getAttributes().inode, firstIdentifier);
  EXPECT_EQ(moved.get()->getAttributes().inode, firstIdentifier);
  EXPECT_EQ(directoryIdentifier(*destination, "renamed"), replacementIdentifier);
  EXPECT_EQ(fixture.disk.references, 0);
}

TEST(FatState, DirectoryCacheEvictionPreservesAliasDataAndSize) {
  StateFixture fixture;
  Directory::ChildLease first;
  ASSERT_NE(fixture.filesystem.findRetained(StringView("state.bin"), first), nullptr);
  uint8_t value = 0x5C;
  ASSERT_EQ(first.get()->write(7, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  fixture.filesystem.root().remove(HashedStringView("state.bin"));

  Directory::ChildLease second;
  ASSERT_NE(fixture.filesystem.findRetained(StringView("state.bin"), second), nullptr);
  ASSERT_NE(first.get(), second.get());
  value = 0;
  ASSERT_EQ(second.get()->read(7, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  EXPECT_EQ(value, 0x5C);
  ASSERT_TRUE(first.get()->resize(InitialSize + SectorSize + 11));
  EXPECT_EQ(second.get()->getSize(), InitialSize + SectorSize + 11);
  value = 0xD3;
  ASSERT_EQ(second.get()->write(InitialSize + 3, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  value = 0;
  ASSERT_EQ(first.get()->read(InitialSize + 3, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  EXPECT_EQ(value, 0xD3);
  EXPECT_EQ(fixture.disk.references, 0);
}

TEST(FatState, FilesystemSyncFlushesStillOpenCachedFiles) {
  StateFixture fixture;
  Directory::ChildLease file;
  ASSERT_NE(fixture.filesystem.findRetained(StringView("state.bin"), file), nullptr);
  const uint8_t value = 0x39;
  const size_t offset = TargetInfo::getPageSize() + 17;
  ASSERT_EQ(file.get()->write(offset, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  EXPECT_EQ(fixture.disk.stored[FileLocation + offset], 0xA7);
  EXPECT_EQ(fixture.filesystem.sync(), Filesystem::SyncStatus::Success);
  EXPECT_EQ(fixture.disk.stored[FileLocation + offset], value);
  EXPECT_GT(fixture.disk.deviceFlushes, 0U);
  EXPECT_EQ(fixture.disk.references, 0);
}

TEST(FatState, LinkedStateSurvivesLossOfEveryMaterializedAlias) {
  StateFixture fixture;
  Directory::ChildLease original;
  ASSERT_NE(fixture.filesystem.findRetained(StringView("state.bin"), original), nullptr);
  const uint8_t value = 0xE2;
  ASSERT_EQ(original.get()->write(21, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  fixture.filesystem.root().remove(HashedStringView("state.bin"));
  original.reset();
  ASSERT_EQ(fixture.filesystem.sync(), Filesystem::SyncStatus::Success);
  EXPECT_EQ(fixture.disk.stored[FileLocation + 21], value);

  Directory::ChildLease reopened;
  ASSERT_NE(fixture.filesystem.findRetained(StringView("state.bin"), reopened), nullptr);
  uint8_t actual = 0;
  ASSERT_EQ(reopened.get()->read(21, 1, reinterpret_cast<uintptr_t>(&actual)), 1U);
  EXPECT_EQ(actual, value);
  EXPECT_EQ(fixture.disk.references, 0);
}

TEST(FatState, UnlinkKeepsAllocationUntilTheFinalOpenAliasIsReleased) {
  StateFixture fixture;
  Directory::ChildLease first;
  ASSERT_NE(fixture.filesystem.findRetained(StringView("state.bin"), first), nullptr);
  fixture.filesystem.root().remove(HashedStringView("state.bin"));
  Directory::ChildLease second;
  ASSERT_NE(fixture.filesystem.findRetained(StringView("state.bin"), second), nullptr);
  ASSERT_NE(first.get(), second.get());
  ASSERT_TRUE(fixture.filesystem.remove(StringView("state.bin")));
  Directory::ChildLease missing;
  EXPECT_EQ(fixture.filesystem.findRetained(StringView("state.bin"), missing), nullptr);
  EXPECT_EQ(fixture.disk.stored[DirectoryLocation], 0xE5);
  EXPECT_NE(fixture.disk.storedCluster(3), 0U);

  const uint8_t value = 0x73;
  ASSERT_EQ(first.get()->write(19, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  first.reset();
  EXPECT_NE(fixture.disk.storedCluster(3), 0U);
  uint8_t actual = 0;
  ASSERT_EQ(second.get()->read(19, 1, reinterpret_cast<uintptr_t>(&actual)), 1U);
  EXPECT_EQ(actual, value);
  ASSERT_TRUE(second.get()->sync());
  EXPECT_EQ(fixture.disk.stored[DirectoryLocation], 0xE5);
  second.reset();
  ASSERT_EQ(fixture.filesystem.sync(), Filesystem::SyncStatus::Success);
  const size_t count = (InitialSize + SectorSize - 1) / SectorSize;
  for (size_t i = 0; i < count; ++i)
    EXPECT_EQ(fixture.disk.storedCluster(3 + i), 0U);
  EXPECT_EQ(fixture.disk.references, 0);
}

TEST(FatState, FilesystemSyncReportsDataAndDeviceFailuresAndRetries) {
  StateFixture fixture;
  Directory::ChildLease file;
  ASSERT_NE(fixture.filesystem.findRetained(StringView("state.bin"), file), nullptr);
  const uint8_t value = 0x49;
  ASSERT_EQ(file.get()->write(11, 1, reinterpret_cast<uintptr_t>(&value)), 1U);
  fixture.disk.failedSync = FileLocation;
  EXPECT_EQ(fixture.filesystem.sync(), Filesystem::SyncStatus::IoError);
  EXPECT_EQ(fixture.disk.stored[FileLocation + 11], 0xA7);
  fixture.disk.failedSync = UINT64_MAX;
  fixture.disk.failDeviceFlush = true;
  EXPECT_EQ(fixture.filesystem.sync(), Filesystem::SyncStatus::IoError);
  fixture.disk.failDeviceFlush = false;
  const size_t priorDeviceFlushes = fixture.disk.deviceFlushes;
  EXPECT_EQ(fixture.filesystem.sync(), Filesystem::SyncStatus::Success);
  EXPECT_GT(fixture.disk.deviceFlushes, priorDeviceFlushes);
  EXPECT_EQ(fixture.disk.stored[FileLocation + 11], value);
  EXPECT_EQ(fixture.disk.references, 0);
}
