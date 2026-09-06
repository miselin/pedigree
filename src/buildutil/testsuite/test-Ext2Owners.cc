#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/utilities/utility.h"

#include <algorithm>
#include <array>

#include "modules/system/ext2/Ext2Filesystem.h"
#include "modules/system/ext2/ext2.h"
#include <gtest/gtest.h>

TEST(Ext2Owners, HighOwnerIdsPreserveOtherOsDependentFields) {
  Inode inode = {};
  std::fill_n(inode.i_osd2, 12, 0xa5);
  inode.i_generation = HOST_TO_LITTLE32(123);
  Ext2Owner::setUid(inode, 0xffee9876U);
  Ext2Owner::setGid(inode, 0x87654321U);
  EXPECT_EQ(Ext2Owner::uid(inode), 0xffee9876U);
  EXPECT_EQ(Ext2Owner::gid(inode), 0x87654321U);
  EXPECT_EQ(inode.i_osd2[4], 0xee);
  EXPECT_EQ(inode.i_osd2[5], 0xff);
  EXPECT_EQ(inode.i_osd2[6], 0x65);
  EXPECT_EQ(inode.i_osd2[7], 0x87);
  for (size_t i : {0U, 1U, 2U, 3U, 8U, 9U, 10U, 11U})
    EXPECT_EQ(inode.i_osd2[i], 0xa5);
  EXPECT_EQ(LITTLE_TO_HOST32(inode.i_generation), 123U);
}
TEST(Ext2Owners, SmallOwnerReplacementClearsPreviousHighBits) {
  Inode inode = {};
  Ext2Owner::setUid(inode, 0xfffffffeU);
  Ext2Owner::setGid(inode, 0xfffffffeU);
  Ext2Owner::setUid(inode, 1000);
  Ext2Owner::setGid(inode, 1001);
  EXPECT_EQ(Ext2Owner::uid(inode), 1000U);
  EXPECT_EQ(Ext2Owner::gid(inode), 1001U);
  for (size_t i = 4; i < 8; ++i)
    EXPECT_EQ(inode.i_osd2[i], 0);
}
namespace {
class UnsupportedOwnerDisk final : public Disk {
 public:
  BufferView read(uint64_t address) override {
    ++reads;
    if (address != 1024)
      return {};
    ++references;
    return BufferView(bytes.data(), bytes.size());
  }
  bool pin(uint64_t) override {
    ++references;
    return true;
  }
  void unpin(uint64_t address) override {
    EXPECT_EQ(address, 1024U);
    --references;
  }
  void write(uint64_t) override {
    ++writes;
  }
  std::array<uint8_t, 4096> bytes = {};
  size_t references = 0, reads = 0, writes = 0;
};
}  // namespace
TEST(Ext2Owners, UnsupportedCreatorIsRejectedBeforeMetadataAccessOrMutation) {
  for (uint32_t creator : {2U, 3U, 4U, UINT32_MAX}) {
    UnsupportedOwnerDisk disk;
    auto* super = reinterpret_cast<Superblock*>(disk.bytes.data());
    super->s_magic = HOST_TO_LITTLE16(0xef53);
    super->s_creator_os = HOST_TO_LITTLE32(creator);
    const auto before = disk.bytes;
    {
      Ext2Filesystem filesystem;
      EXPECT_FALSE(filesystem.initialise(&disk));
      EXPECT_EQ(disk.reads, 1U);
      EXPECT_EQ(disk.writes, 0U);
      EXPECT_EQ(disk.bytes, before);
    }
    EXPECT_EQ(disk.references, 0U);
  }
}
