/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <vector>

#include "modules/system/ext2/Ext2File.h"
#include "modules/system/ext2/Ext2Filesystem.h"
#include "modules/system/ext2/Ext2Symlink.h"
#include "modules/system/ext2/Ext2Xattr.h"
#include "modules/system/ext2/ext2.h"
#include <gtest/gtest.h>

class Ext2XattrTestPeer {
 public:
  static void configure(Ext2Filesystem& fs, Disk& disk) {
    fs.m_pDisk = &disk;
    fs.m_pSuperblock = reinterpret_cast<Superblock*>(disk.read(1024).data());
    fs.m_BlockSize = 4096;
    fs.m_InodeSize = sizeof(Inode);
    fs.m_nGroupDescriptors = 2;
    fs.m_pGroupDescriptors = new GroupDesc*[2];
    fs.m_pBlockBitmaps = new Vector<size_t>[2];
    fs.m_pInodeBitmaps = new Vector<size_t>[2];
    fs.m_pInodeTables = new Vector<size_t>[2];
    for (size_t group = 0; group < 2; ++group)
      fs.m_pGroupDescriptors[group] = reinterpret_cast<GroupDesc*>(disk.read(4096).data()) + group;
  }
  static Inode* inode(Ext2Filesystem& fs, uint32_t number) {
    return fs.getInode(number);
  }
  static bool unlink(Ext2Filesystem& fs, uint32_t number) {
    return fs.releaseInode(number);
  }
};

namespace {
constexpr size_t BlockSize = 4096, BlockCount = 256;
using Status = XattrStatus;

class AttributeDisk final : public Disk {
 public:
  AttributeDisk() : bytes(BlockSize * BlockCount), persisted(bytes) {}
  BufferView read(uint64_t location) override {
    reads.push_back(location);
    if (location >= bytes.size() || location == failedRead)
      return {};
    ++pins[location];
    return BufferView(bytes.data() + location, BlockSize - location % BlockSize);
  }
  void write(uint64_t location) override {
    writes.push_back(location);
  }
  bool sync(uint64_t location, bool async) override {
    if (async) {
      write(location);
      return true;
    }
    syncs.push_back(location);
    if (location == failedSync || location >= bytes.size())
      return false;
    const size_t start = location - location % BlockSize;
    std::copy_n(bytes.data() + start, BlockSize, persisted.data() + start);
    return true;
  }
  bool pin(uint64_t location) override {
    if (location >= bytes.size())
      return false;
    ++pins[location];
    return true;
  }
  void unpin(uint64_t location) override {
    EXPECT_GT(pins[location], 0U) << location;
    if (pins[location])
      --pins[location];
  }
  size_t getSize() const override {
    return bytes.size();
  }
  size_t getBlockSize() const override {
    return BlockSize;
  }
  uint8_t* block(size_t number) {
    return bytes.data() + number * BlockSize;
  }
  std::vector<uint8_t> snapshot(size_t number) {
    return {block(number), block(number) + BlockSize};
  }
  std::vector<uint8_t> bytes, persisted;
  std::vector<uint64_t> reads, writes, syncs;
  std::map<uint64_t, size_t> pins;
  uint64_t failedRead = ~uint64_t(0), failedSync = ~uint64_t(0);
};

struct Fixture {
  Fixture() {
    auto* super = reinterpret_cast<Superblock*>(disk.bytes.data() + 1024);
    super->s_rev_level = HOST_TO_LITTLE32(1);
    super->s_blocks_count = HOST_TO_LITTLE32(BlockCount);
    super->s_blocks_per_group = HOST_TO_LITTLE32(128);
    super->s_inodes_per_group = HOST_TO_LITTLE32(8);
    super->s_free_blocks_count = HOST_TO_LITTLE32(32);
    super->s_inodes_count = HOST_TO_LITTLE32(16);
    auto* groups = reinterpret_cast<GroupDesc*>(disk.block(1));
    for (size_t group = 0; group < 2; ++group) {
      const size_t start = group * 128;
      groups[group].bg_block_bitmap = HOST_TO_LITTLE32(start + 2);
      groups[group].bg_inode_bitmap = HOST_TO_LITTLE32(start + 3);
      groups[group].bg_inode_table = HOST_TO_LITTLE32(start + 4);
      groups[group].bg_free_blocks_count = HOST_TO_LITTLE16(16);
      std::fill_n(disk.block(start + 2), BlockSize, 0xff);
      std::fill_n(disk.block(start + 3), BlockSize, 0xff);
      const size_t freeStart = group ? 160 : 16;
      for (size_t n = freeStart; n < freeStart + 16; ++n) {
        const size_t relative = n - start;
        disk.block(start + 2)[relative / 8] &= ~(1U << (relative % 8));
      }
      auto* inodes = reinterpret_cast<Inode*>(disk.block(start + 4));
      for (size_t n = 0; n < 8; ++n) {
        inodes[n].i_mode = HOST_TO_LITTLE16(EXT2_S_IFREG | 0600);
        inodes[n].i_links_count = HOST_TO_LITTLE16(1);
      }
    }
    Ext2XattrTestPeer::configure(fs, disk);
  }
  Inode* inode(uint32_t number = 1) {
    return Ext2XattrTestPeer::inode(fs, number);
  }
  void reserve(size_t block) {
    const size_t group = block / 128, relative = block % 128;
    auto& byte = disk.block(group * 128 + 2)[relative / 8];
    ASSERT_FALSE(byte & (1U << (relative % 8)));
    byte |= 1U << (relative % 8);
    auto* groups = reinterpret_cast<GroupDesc*>(disk.block(1));
    groups[group].bg_free_blocks_count =
        HOST_TO_LITTLE16(LITTLE_TO_HOST16(groups[group].bg_free_blocks_count) - 1);
    auto* super = reinterpret_cast<Superblock*>(disk.bytes.data() + 1024);
    super->s_free_blocks_count = HOST_TO_LITTLE32(LITTLE_TO_HOST32(super->s_free_blocks_count) - 1);
  }
  bool allocated(size_t block) const {
    const size_t group = block / 128, relative = block % 128;
    return disk.bytes[(group * 128 + 2) * BlockSize + relative / 8] & (1U << (relative % 8));
  }
  void attach(uint32_t number, uint32_t block) {
    inode(number)->i_file_acl = HOST_TO_LITTLE32(block);
    inode(number)->i_blocks = HOST_TO_LITTLE32(BlockSize / 512);
  }
  // Independent on-disk fixture, including an unknown namespace that must survive repacking.
  void attributes(uint32_t block, unsigned references = 1, bool opaque = false) {
    reserve(block);
    auto* header = reinterpret_cast<Ext2Ea::Header*>(disk.block(block));
    header->magic = HOST_TO_LITTLE32(0xea020000);
    header->references = HOST_TO_LITTLE32(references);
    header->blocks = HOST_TO_LITTLE32(1);
    auto* entry = reinterpret_cast<Ext2Ea::Entry*>(header + 1);
    entry->nameLength = 3;
    entry->nameIndex = 1;
    entry->valueOffset = HOST_TO_LITTLE16(BlockSize - 4);
    entry->valueLength = HOST_TO_LITTLE32(3);
    MemoryCopy(entry + 1, "key", 3);
    MemoryCopy(disk.block(block) + BlockSize - 4, "old", 3);
    if (opaque) {
      entry = reinterpret_cast<Ext2Ea::Entry*>(reinterpret_cast<uint8_t*>(entry) + 20);
      entry->nameLength = 5;
      entry->nameIndex = 6;
      entry->valueOffset = HOST_TO_LITTLE16(BlockSize - 8);
      entry->valueLength = HOST_TO_LITTLE32(4);
      MemoryCopy(entry + 1, "label", 5);
      const uint8_t value[] = {0, 0xa7, 0xff, 0};
      MemoryCopy(disk.block(block) + BlockSize - 8, value, sizeof(value));
    }
  }
  AttributeDisk disk;
  Ext2Filesystem fs;
};

std::string value(File& file, const char* name = "user.key") {
  char output[32] = {};
  size_t required = 0;
  EXPECT_EQ(file.getExtendedAttribute(StringView(name), output, sizeof(output), required),
            Status::Success);
  return std::string(output, std::min(required, sizeof(output)));
}

bool synced(const AttributeDisk& disk, uint32_t block) {
  return std::find(disk.syncs.begin(), disk.syncs.end(), block * BlockSize) != disk.syncs.end();
}
}  // namespace

TEST(Ext2Xattr, AliasesConditionalUpdatesAndCapacityPreserveState) {
  Fixture f;
  f.inode()->i_links_count = HOST_TO_LITTLE16(2);
  Ext2File first(String("first"), 1, f.inode(), &f.fs);
  Ext2File alias(String("alias"), 1, f.inode(), &f.fs);
  const StringView key("user.key");
  ASSERT_EQ(first.setExtendedAttribute(key, "one", 3, Xattr::Create), Status::Success);
  const uint32_t block = LITTLE_TO_HOST32(f.inode()->i_file_acl);
  EXPECT_EQ(value(alias), "one");
  EXPECT_EQ(LITTLE_TO_HOST32(f.inode()->i_blocks), BlockSize / 512);
  auto before = f.disk.snapshot(block);
  EXPECT_EQ(alias.setExtendedAttribute(key, "two", 3, Xattr::Create), Status::Exists);
  const std::vector<uint8_t> excessive(BlockSize, 0xc5);
  EXPECT_EQ(alias.setExtendedAttribute(key, excessive.data(), excessive.size(), 0),
            Status::NoSpace);
  EXPECT_EQ(f.disk.snapshot(block), before);
  EXPECT_EQ(alias.setExtendedAttribute(key, nullptr, 1, 0), Status::Invalid);
  ASSERT_EQ(alias.setExtendedAttribute(StringView("user.empty"), nullptr, 0, 0), Status::Success);
  size_t required = 0;
  std::array<char, 32> names;
  names.fill('x');
  EXPECT_EQ(first.listExtendedAttributes(names.data(), 3, required), Status::Range);
  EXPECT_EQ(names[0], 'x');
  ASSERT_EQ(first.listExtendedAttributes(names.data(), names.size(), required), Status::Success);
  EXPECT_EQ(std::string(names.data(), required), std::string("user.key\0user.empty\0", 20));
  ASSERT_EQ(alias.removeExtendedAttribute(key), Status::Success);
  ASSERT_EQ(first.removeExtendedAttribute(StringView("user.empty")), Status::Success);
  EXPECT_EQ(f.inode()->i_file_acl, 0U);
  EXPECT_EQ(f.inode()->i_blocks, 0U);
  EXPECT_FALSE(f.allocated(block));
}

TEST(Ext2Xattr, SharedBlockCopyOnWritePreservesOtherInodeAndOpaqueEntry) {
  Fixture f;
  f.attributes(160, 2, true);
  f.attach(1, 160);
  f.attach(9, 160);
  Ext2File first(String("first"), 1, f.inode(1), &f.fs);
  Ext2File other(String("other"), 9, f.inode(9), &f.fs);
  ASSERT_EQ(first.setExtendedAttribute(StringView("user.key"), "new", 3, 0), Status::Success);
  EXPECT_EQ(value(first), "new");
  EXPECT_EQ(value(other), "old");
  EXPECT_EQ(LITTLE_TO_HOST32(f.inode(9)->i_file_acl), 160U);
  const uint32_t replacement = LITTLE_TO_HOST32(f.inode()->i_file_acl);
  EXPECT_NE(replacement, 160U);
  EXPECT_EQ(LITTLE_TO_HOST32(reinterpret_cast<Ext2Ea::Header*>(f.disk.block(160))->references), 1U);
  auto* opaque = reinterpret_cast<Ext2Ea::Entry*>(f.disk.block(replacement) + 32 + 20);
  EXPECT_EQ(opaque->nameIndex, 6);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(opaque + 1), opaque->nameLength), "label");
  const uint8_t expected[] = {0, 0xa7, 0xff, 0};
  EXPECT_EQ(
      std::vector<uint8_t>(f.disk.block(replacement) + LITTLE_TO_HOST16(opaque->valueOffset),
                           f.disk.block(replacement) + LITTLE_TO_HOST16(opaque->valueOffset) + 4),
      std::vector<uint8_t>(expected, expected + 4));
  ASSERT_EQ(first.removeExtendedAttribute(StringView("user.key")), Status::Success);
  EXPECT_EQ(LITTLE_TO_HOST32(f.inode()->i_file_acl), replacement);
  size_t required = 99;
  EXPECT_EQ(first.listExtendedAttributes(nullptr, 0, required), Status::Success);
  EXPECT_EQ(required, 0U);
  ASSERT_TRUE(first.sync());
  auto* persistedOld = reinterpret_cast<Ext2Ea::Header*>(f.disk.persisted.data() + 160 * BlockSize);
  EXPECT_EQ(LITTLE_TO_HOST32(persistedOld->references), 1U);
  EXPECT_TRUE(synced(f.disk, replacement));
  EXPECT_TRUE(synced(f.disk, 160));
}

TEST(Ext2Xattr, ReadAndAllocationFailuresPreserveSharedPayloadAndAllocationBits) {
  Fixture f;
  f.attributes(160, 2);
  f.attach(1, 160);
  f.attach(9, 160);
  Ext2File file(String("file"), 1, f.inode(), &f.fs);
  const auto payload = f.disk.snapshot(160), bitmap = f.disk.snapshot(2);
  const auto inode = *f.inode();
  f.disk.failedRead = 160 * BlockSize;
  EXPECT_EQ(file.setExtendedAttribute(StringView("user.key"), "new", 3, 0), Status::IoError);
  f.disk.failedRead = 130 * BlockSize;
  EXPECT_EQ(file.setExtendedAttribute(StringView("user.key"), "new", 3, 0), Status::IoError);
  f.disk.failedRead = 2 * BlockSize;
  EXPECT_EQ(file.setExtendedAttribute(StringView("user.key"), "new", 3, 0), Status::IoError);
  f.disk.failedRead = 16 * BlockSize;
  EXPECT_EQ(file.setExtendedAttribute(StringView("user.key"), "new", 3, 0), Status::IoError);
  EXPECT_EQ(f.disk.snapshot(160), payload);
  EXPECT_EQ(f.disk.snapshot(2), bitmap);
  EXPECT_EQ(f.inode()->i_file_acl, inode.i_file_acl);
  EXPECT_EQ(f.inode()->i_blocks, inode.i_blocks);
  EXPECT_EQ(f.inode()->i_ctime, inode.i_ctime);
  f.disk.failedRead = ~uint64_t(0);
  EXPECT_EQ(value(file), "old");
  EXPECT_EQ(file.setExtendedAttribute(StringView("user.key"), "new", 3, 0), Status::Success);
}

TEST(Ext2Xattr, FastSymlinkWithAttributeSectorsKeepsInlineTargetAndRetiresOnce) {
  Fixture f;
  f.attributes(160);
  f.attach(1, 160);
  f.inode()->i_mode = HOST_TO_LITTLE16(EXT2_S_IFLNK | 0777);
  f.inode()->i_size = HOST_TO_LITTLE32(4);
  MemoryCopy(f.inode()->i_block, "dest", 4);
  {
    Ext2Symlink link(String("link"), 1, f.inode(), &f.fs);
    char target[4] = {};
    EXPECT_EQ(link.readBytewise(0, sizeof(target), reinterpret_cast<uintptr_t>(target), true), 4U);
    EXPECT_EQ(std::string(target, sizeof(target)), "dest");
    ASSERT_EQ(link.setExtendedAttribute(StringView("user.key"), "tag", 3, 0), Status::Success);
    EXPECT_EQ(value(link), "tag");
    EXPECT_EQ(LITTLE_TO_HOST32(f.inode()->i_blocks), BlockSize / 512);
    EXPECT_TRUE(Ext2XattrTestPeer::unlink(f.fs, 1));
    EXPECT_TRUE(f.allocated(160));
    EXPECT_EQ(value(link), "tag");
  }
  EXPECT_FALSE(f.allocated(160));
  EXPECT_EQ(f.inode()->i_file_acl, 0U);
  EXPECT_EQ(f.inode()->i_blocks, 0U);
  EXPECT_FALSE(f.disk.block(3)[0] & 1);
}

TEST(Ext2Xattr, FailedOrphanPreflightPreservesInodeDataAndAttributeOwnership) {
  Fixture f;
  f.attributes(160);
  f.attach(1, 160);
  f.reserve(20);
  f.inode()->i_blocks = HOST_TO_LITTLE32(2 * BlockSize / 512);
  f.inode()->i_block[0] = HOST_TO_LITTLE32(20);
  f.inode()->i_size = HOST_TO_LITTLE32(BlockSize);
  std::fill_n(f.disk.block(20), BlockSize, 0xa7);
  {
    Ext2File file(String("file"), 1, f.inode(), &f.fs);
    EXPECT_TRUE(Ext2XattrTestPeer::unlink(f.fs, 1));
    f.disk.failedRead = 160 * BlockSize;
  }
  EXPECT_TRUE(f.allocated(160));
  EXPECT_TRUE(f.allocated(20));
  EXPECT_TRUE(f.disk.block(3)[0] & 1);
  EXPECT_EQ(LITTLE_TO_HOST32(f.inode()->i_file_acl), 160U);
  EXPECT_EQ(LITTLE_TO_HOST32(f.inode()->i_block[0]), 20U);
  EXPECT_EQ(f.disk.block(20)[0], 0xa7);
  f.disk.failedRead = ~uint64_t(0);
}

TEST(Ext2Xattr, FileShrinkPreservesAttributeSectorsAndRemovesOnlyData) {
  Fixture f;
  f.attributes(160);
  f.attach(1, 160);
  f.reserve(20);
  f.inode()->i_blocks = HOST_TO_LITTLE32(2 * BlockSize / 512);
  f.inode()->i_block[0] = HOST_TO_LITTLE32(20);
  f.inode()->i_size = HOST_TO_LITTLE32(BlockSize);
  Ext2File file(String("file"), 1, f.inode(), &f.fs);
  ASSERT_TRUE(file.resize(0));
  EXPECT_EQ(value(file), "old");
  EXPECT_EQ(LITTLE_TO_HOST32(f.inode()->i_blocks), BlockSize / 512);
  EXPECT_TRUE(f.allocated(160));
  EXPECT_FALSE(f.allocated(20));
  ASSERT_EQ(file.removeExtendedAttribute(StringView("user.key")), Status::Success);
  EXPECT_EQ(f.inode()->i_blocks, 0U);
}

TEST(Ext2Xattr, RemovedAttributeAllocationDependencySurvivesFailedFsync) {
  Fixture f;
  f.attributes(160);
  f.attach(1, 160);
  Ext2File file(String("file"), 1, f.inode(), &f.fs);
  ASSERT_EQ(file.removeExtendedAttribute(StringView("user.key")), Status::Success);
  f.disk.failedSync = 130 * BlockSize;
  EXPECT_FALSE(file.sync());
  EXPECT_TRUE(synced(f.disk, 130));
  EXPECT_FALSE(synced(f.disk, 4));
  f.disk.syncs.clear();
  f.disk.failedSync = ~uint64_t(0);
  EXPECT_TRUE(file.sync());
  EXPECT_TRUE(synced(f.disk, 130));
  EXPECT_TRUE(synced(f.disk, 4));
  const auto oldGroupSync = std::find(f.disk.syncs.begin(), f.disk.syncs.end(), 130 * BlockSize);
  const auto inodeSync = std::find(f.disk.syncs.begin(), f.disk.syncs.end(), 4 * BlockSize);
  EXPECT_LT(oldGroupSync, inodeSync);
  EXPECT_FALSE(f.disk.persisted[130 * BlockSize + 4] & 1);
  EXPECT_EQ(reinterpret_cast<Inode*>(f.disk.persisted.data() + 4 * BlockSize)->i_file_acl, 0U);
}

TEST(Ext2Xattr, UnsupportedFormatAndCorruptSectorCountDoNotMutateBacking) {
  Fixture f;
  f.attributes(160);
  f.attach(1, 160);
  auto* entry = reinterpret_cast<Ext2Ea::Entry*>(f.disk.block(160) + 32);
  entry->valueBlock = HOST_TO_LITTLE32(50);
  const auto original = f.disk.snapshot(160);
  {
    Ext2File file(String("file"), 1, f.inode(), &f.fs);
    EXPECT_EQ(file.setExtendedAttribute(StringView("user.key"), "new", 3, 0), Status::Unsupported);
    EXPECT_EQ(f.disk.snapshot(160), original);
  }
  entry->valueBlock = 0;
  f.inode()->i_blocks = 0;
  Ext2File corrupt(String("corrupt"), 1, f.inode(), &f.fs);
  EXPECT_EQ(corrupt.setExtendedAttribute(StringView("user.key"), "new", 3, 0), Status::IoError);
  EXPECT_EQ(LITTLE_TO_HOST32(f.inode()->i_file_acl), 160U);
  EXPECT_TRUE(f.allocated(160));
}

TEST(Ext2Xattr, FailedPayloadSyncRetainsRetryPinAndDefersInodePublication) {
  Fixture f;
  Ext2File file(String("file"), 1, f.inode(), &f.fs);
  ASSERT_EQ(file.setExtendedAttribute(StringView("user.key"), "new", 3, 0), Status::Success);
  const uint32_t block = LITTLE_TO_HOST32(f.inode()->i_file_acl);
  f.disk.failedSync = block * BlockSize;
  EXPECT_FALSE(file.sync());
  EXPECT_EQ(f.disk.pins[block * BlockSize], 1U);
  EXPECT_FALSE(synced(f.disk, 4));
  EXPECT_EQ(reinterpret_cast<Inode*>(f.disk.persisted.data() + 4 * BlockSize)->i_file_acl, 0U);
  f.disk.failedSync = ~uint64_t(0);
  ASSERT_TRUE(file.sync());
  EXPECT_EQ(f.disk.pins[block * BlockSize], 0U);
  EXPECT_EQ(reinterpret_cast<Inode*>(f.disk.persisted.data() + 4 * BlockSize)->i_file_acl,
            HOST_TO_LITTLE32(block));
  EXPECT_EQ(value(file), "new");
}

TEST(Ext2Xattr, UnrepresentableUserNamesRejectMutationButAclAndBinaryNamesSurvive) {
  for (unsigned invalid = 0; invalid < 3; ++invalid) {
    Fixture f;
    f.attributes(160);
    f.attach(1, 160);
    auto* entry = reinterpret_cast<Ext2Ea::Entry*>(f.disk.block(160) + 32);
    auto* name = reinterpret_cast<uint8_t*>(entry + 1);
    if (!invalid)
      entry->nameLength = 0;
    else if (invalid == 1)
      name[1] = 0;
    else {
      entry->nameLength = 251;
      std::fill_n(name, 251, 'a');
    }
    const auto before = f.disk.snapshot(160);
    Ext2File file(String("file"), 1, f.inode(), &f.fs);
    size_t required = 0;
    EXPECT_EQ(file.listExtendedAttributes(nullptr, 0, required), Status::IoError);
    EXPECT_EQ(file.setExtendedAttribute(StringView("user.key"), "new", 3, 0), Status::IoError);
    EXPECT_EQ(f.disk.snapshot(160), before);
    EXPECT_EQ(LITTLE_TO_HOST32(f.inode()->i_file_acl), 160U);
  }
  {
    Fixture f;
    f.attributes(160);
    f.attach(1, 160);
    auto* entry = reinterpret_cast<Ext2Ea::Entry*>(f.disk.block(160) + 32);
    entry->nameIndex = 2;
    entry->nameLength = 0;
    ByteSet(entry + 1, 0, 4);
    Ext2File file(String("acl"), 1, f.inode(), &f.fs);
    ASSERT_EQ(file.setExtendedAttribute(StringView("user.key"), "new", 3, 0), Status::Success);
    auto* acl = reinterpret_cast<Ext2Ea::Entry*>(f.disk.block(160) + 32 + 20);
    EXPECT_EQ(acl->nameIndex, 2);
    EXPECT_EQ(acl->nameLength, 0);
    EXPECT_EQ(
        std::string(reinterpret_cast<char*>(f.disk.block(160)) + LITTLE_TO_HOST16(acl->valueOffset),
                    3),
        "old");
  }
  {
    Fixture f;
    f.attributes(160);
    f.attach(1, 160);
    const char key[] = {'u', 's', 'e', 'r', '.', char(0x80), char(0xff), 'x'};
    MemoryCopy(f.disk.block(160) + 48, key + 5, 3);
    Ext2File file(String("binary"), 1, f.inode(), &f.fs);
    char output[3];
    size_t required = 0;
    EXPECT_EQ(
        file.getExtendedAttribute(StringView(key, sizeof(key)), output, sizeof(output), required),
        Status::Success);
    EXPECT_EQ(std::string(output, sizeof(output)), "old");
  }
}
