/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/machine/Disk.h"

#include <algorithm>
#include <cerrno>
#include <future>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "modules/system/ext2/Ext2Directory.h"
#include "modules/system/ext2/Ext2File.h"
#include "modules/system/ext2/Ext2Filesystem.h"
#include "modules/system/ext2/Ext2Xattr.h"
#include "modules/system/ext2/ext2.h"
#include <gtest/gtest.h>

class Ext2QuotaTestPeer {
 public:
  static void attach(Ext2Filesystem& fs, Disk& disk, bool warm = true) {
    fs.m_pDisk = &disk;
    fs.m_BlockSize = 1024;
    fs.m_InodeSize = sizeof(Inode);
    fs.m_nGroupDescriptors = 1;
    fs.m_pSuperblock = disk.read(1024).as<Superblock>();
    fs.m_pGroupDescriptors = new GroupDesc*[1]{disk.read(2048).as<GroupDesc>()};
    fs.m_pBlockBitmaps = new Vector<size_t>[1];
    fs.m_pInodeBitmaps = new Vector<size_t>[1];
    fs.m_pInodeTables = new Vector<size_t>[1];
    if (warm) {
      fs.m_pBlockBitmaps[0].pushBack(disk.read(3072).address());
      fs.m_pInodeBitmaps[0].pushBack(disk.read(4096).address());
      fs.m_pInodeTables[0].pushBack(disk.read(5120).address());
    }
  }
  static Inode* inode(Ext2Filesystem& fs, uint32_t number) {
    return fs.getInode(number);
  }
  static size_t freeBlocks(Ext2Filesystem& fs) {
    return LITTLE_TO_HOST32(fs.m_pSuperblock->s_free_blocks_count);
  }
  static uint32_t firstFreeBlock(Ext2Filesystem& fs) {
    const auto* bitmap = reinterpret_cast<const uint8_t*>(fs.m_pBlockBitmaps[0][0]);
    for (uint32_t block = 1; block < 128; ++block) {
      if (!(bitmap[(block - 1) / 8] & (1U << ((block - 1) % 8))))
        return block;
    }
    return 0;
  }
  static uint32_t freeInodes(Ext2Filesystem& fs) {
    return LITTLE_TO_HOST32(fs.m_pSuperblock->s_free_inodes_count);
  }
  static bool create(Ext2Filesystem& fs, Ext2Directory& parent, const String& value) {
    return fs.createNode(&parent, String("new-entry"), 0600, value,
                         value.length() ? EXT2_S_IFLNK : EXT2_S_IFREG);
  }
  static uint32_t allocateInode(Ext2Filesystem& fs) {
    return fs.findFreeInode(1000, 42);
  }
  static bool allocateBlocks(Ext2Filesystem& fs, uint32_t inode, size_t count,
                             Vector<uint32_t>& blocks) {
    return fs.findFreeBlocks(inode, count, blocks);
  }
  static uint64_t chargedBytes(Ext2Filesystem& fs, uint32_t inode) {
    const auto* charge = fs.m_Quota.find(inode);
    return charge ? charge->bytes : 0;
  }
  static bool unlink(Ext2Filesystem& fs, Ext2File& file) {
    return fs.releaseInode(file.getInodeNumber(), &file);
  }
  static void offHook(Ext2Filesystem& fs, void (*hook)(void*), void* context) {
    fs.m_QuotaOffTestHook = hook;
    fs.m_QuotaOffTestContext = context;
  }
};

namespace {
constexpr size_t Block = 1024, Blocks = 128;

class QuotaDisk final : public Disk {
 public:
  QuotaDisk() : bytes(Block * Blocks), persisted(bytes), pins(Blocks, 0) {}
  explicit QuotaDisk(const std::vector<uint8_t>& image)
      : bytes(image), persisted(image), pins(Blocks, 0) {}
  BufferView read(uint64_t location) override {
    std::lock_guard<std::mutex> guard(lock);
    if (location >= bytes.size() || location == failedRead)
      return {};
    ++pins[location / Block];
    return BufferView(bytes.data() + location, Block - location % Block);
  }
  void write(uint64_t) override {}
  bool sync(uint64_t location, bool async) override {
    std::lock_guard<std::mutex> guard(lock);
    if (location >= bytes.size() || location == failedSync)
      return false;
    if (!async) {
      const size_t start = location - location % Block;
      std::copy_n(bytes.data() + start, Block, persisted.data() + start);
    }
    return true;
  }
  bool syncAll() override {
    std::lock_guard<std::mutex> guard(lock);
    ++allCalls;
    if (failedHardware)
      return false;
    persisted = bytes;
    return true;
  }
  bool pin(uint64_t location) override {
    std::lock_guard<std::mutex> guard(lock);
    if (location >= bytes.size())
      return false;
    ++pins[location / Block];
    return true;
  }
  void unpin(uint64_t location) override {
    std::lock_guard<std::mutex> guard(lock);
    ASSERT_LT(location, bytes.size());
    ASSERT_GT(pins[location / Block], 0U);
    --pins[location / Block];
  }
  size_t getSize() const override {
    return bytes.size();
  }
  size_t getBlockSize() const override {
    return Block;
  }
  std::vector<uint8_t> bytes, persisted;
  std::vector<size_t> pins;
  uint64_t failedRead = ~uint64_t(0), failedSync = ~uint64_t(0);
  bool failedHardware = false;
  size_t allCalls = 0;
  std::mutex lock;
};

void makeImage(QuotaDisk& disk) {
  auto* super = reinterpret_cast<Superblock*>(disk.bytes.data() + Block);
  super->s_rev_level = HOST_TO_LITTLE32(1);
  super->s_blocks_count = HOST_TO_LITTLE32(Blocks);
  super->s_blocks_per_group = HOST_TO_LITTLE32(Blocks);
  super->s_inodes_count = HOST_TO_LITTLE32(8);
  super->s_inodes_per_group = HOST_TO_LITTLE32(8);
  super->s_first_ino = HOST_TO_LITTLE32(2);
  super->s_first_data_block = HOST_TO_LITTLE32(1);
  super->s_free_inodes_count = HOST_TO_LITTLE32(2);
  auto* group = reinterpret_cast<GroupDesc*>(disk.bytes.data() + 2 * Block);
  group->bg_block_bitmap = HOST_TO_LITTLE32(3);
  group->bg_inode_bitmap = HOST_TO_LITTLE32(4);
  group->bg_inode_table = HOST_TO_LITTLE32(5);
  group->bg_free_inodes_count = HOST_TO_LITTLE16(2);
  auto* bitmap = disk.bytes.data() + 3 * Block;
  std::fill_n(bitmap, Block, 0xff);
  for (size_t block = 18; block < Blocks; ++block) {
    bitmap[(block - 1) / 8] &= ~(1U << ((block - 1) % 8));
    ++super->s_free_blocks_count;
    ++group->bg_free_blocks_count;
  }
  disk.bytes[4 * Block] = 0x3f;
  auto* inodes = reinterpret_cast<Inode*>(disk.bytes.data() + 5 * Block);
  for (uint32_t inode = 2; inode <= 6; ++inode) {
    auto& metadata = inodes[inode - 1];
    metadata.i_mode = HOST_TO_LITTLE16(EXT2_S_IFREG | 0600);
    metadata.i_links_count = HOST_TO_LITTLE16(1);
    metadata.i_generation = HOST_TO_LITTLE32(1);
    if (inode == 3 || inode == 6) {
      Ext2Owner::setUid(metadata, 1000);
      Ext2Owner::setGid(metadata, 42);
    }
    if (inode == 4 || inode == 5) {
      metadata.i_size = HOST_TO_LITTLE32(QuotaOld::RecordSize);
      metadata.i_blocks = HOST_TO_LITTLE32(2);
      metadata.i_block[0] = HOST_TO_LITTLE32(inode + 12);
    }
  }
  disk.persisted = disk.bytes;
}

struct Fixture {
  Fixture() {
    makeImage(disk);
    Ext2QuotaTestPeer::attach(fs, disk);
    first = file(3);
    second = file(6);
    users = file(4);
    groups = file(5);
  }
  std::unique_ptr<Ext2File> file(uint32_t number) {
    return std::unique_ptr<Ext2File>(
        new Ext2File(String("quota-test"), number, Ext2QuotaTestPeer::inode(fs, number), &fs));
  }
  QuotaStatus control(QuotaOperation operation, QuotaType type, uint32_t id = 0,
                      const QuotaRecord& record = {}) {
    QuotaRequest request;
    request.operation = operation;
    request.type = type;
    request.id = id;
    request.format = Quota::OldFormat;
    request.record = record;
    return fs.quotaControl(request, response,
                           operation == QuotaOperation::Enable
                               ? (type == QuotaType::User ? users.get() : groups.get())
                               : nullptr);
  }
  QuotaStatus limit(QuotaType type, uint32_t id, uint64_t blocks, uint64_t inodes) {
    QuotaRecord limits;
    limits.valid = Quota::Limits;
    limits.blockHardLimit = blocks;
    limits.inodeHardLimit = inodes;
    return control(QuotaOperation::Set, type, id, limits);
  }
  QuotaDisk disk;
  Ext2Filesystem fs;
  std::unique_ptr<Ext2File> first, second, users, groups;
  QuotaResponse response;
};
}  // namespace

TEST(Ext2Quota, SuccessfulWarmAllocationsDoNotSetErrno) {
  Fixture f;
  errno = 0;
  const auto inode = Ext2QuotaTestPeer::allocateInode(f.fs);
  ASSERT_NE(inode, 0U);
  EXPECT_EQ(errno, 0);
  Vector<uint32_t> blocks;
  errno = 0;
  ASSERT_TRUE(Ext2QuotaTestPeer::allocateBlocks(f.fs, inode, 0, blocks));
  EXPECT_EQ(errno, 0);
  EXPECT_EQ(blocks.count(), 0U);
  errno = 0;
  ASSERT_TRUE(Ext2QuotaTestPeer::allocateBlocks(f.fs, inode, 1, blocks));
  EXPECT_EQ(errno, 0);
  ASSERT_EQ(blocks.count(), 1U);
  EXPECT_EQ(Ext2QuotaTestPeer::chargedBytes(f.fs, inode), Block);
  errno = 0;
  ASSERT_TRUE(f.first->resize(Block));
  EXPECT_EQ(errno, 0);
  errno = 0;
  ASSERT_TRUE(f.first->resize(3 * 4096));
  EXPECT_EQ(errno, 0);
  EXPECT_EQ(f.first->getSize(), 3 * 4096U);
}

TEST(Ext2Quota, InodeExhaustionSetsNoSpaceAfterSuccessfulAllocations) {
  Fixture f;
  for (unsigned i = 0; i < 2; ++i) {
    errno = 0;
    ASSERT_NE(Ext2QuotaTestPeer::allocateInode(f.fs), 0U);
    EXPECT_EQ(errno, 0);
  }
  EXPECT_EQ(Ext2QuotaTestPeer::freeInodes(f.fs), 0U);
  errno = 0;
  EXPECT_EQ(Ext2QuotaTestPeer::allocateInode(f.fs), 0U);
  EXPECT_EQ(errno, ENOSPC);
}

TEST(Ext2Quota, BlockExhaustionRollsBackAllocationAndChargeBeforeReportingNoSpace) {
  Fixture f;
  const size_t free = Ext2QuotaTestPeer::freeBlocks(f.fs);
  Vector<uint32_t> blocks;
  errno = 0;
  EXPECT_FALSE(Ext2QuotaTestPeer::allocateBlocks(f.fs, 3, free + 1, blocks));
  EXPECT_EQ(errno, ENOSPC);
  EXPECT_EQ(blocks.count(), 0U);
  EXPECT_EQ(Ext2QuotaTestPeer::freeBlocks(f.fs), free);
  EXPECT_EQ(Ext2QuotaTestPeer::chargedBytes(f.fs, 3), 0U);
}

TEST(Ext2Quota, AllocationMetadataReadFailuresPreserveIoErrorAndDoNotCharge) {
  for (const auto failure :
       {std::pair<bool, uint32_t>{false, 3}, {true, 4}, {true, 5}, {false, 5}}) {
    QuotaDisk disk;
    makeImage(disk);
    Ext2Filesystem fs;
    Ext2QuotaTestPeer::attach(fs, disk, false);
    const size_t blocksBefore = Ext2QuotaTestPeer::freeBlocks(fs);
    const size_t inodesBefore = Ext2QuotaTestPeer::freeInodes(fs);
    disk.failedRead = failure.second * Block;
    Vector<uint32_t> blocks;
    errno = 0;
    if (failure.first)
      EXPECT_EQ(Ext2QuotaTestPeer::allocateInode(fs), 0U);
    else
      EXPECT_FALSE(Ext2QuotaTestPeer::allocateBlocks(fs, 3, 1, blocks));
    EXPECT_EQ(errno, EIO);
    EXPECT_EQ(blocks.count(), 0U);
    EXPECT_EQ(Ext2QuotaTestPeer::freeBlocks(fs), blocksBefore);
    EXPECT_EQ(Ext2QuotaTestPeer::freeInodes(fs), inodesBefore);
    EXPECT_EQ(Ext2QuotaTestPeer::chargedBytes(fs, 3), 0U);
  }
}

TEST(Ext2Quota, MissingInodeTableReportsIoError) {
  QuotaDisk disk;
  makeImage(disk);
  auto* group = reinterpret_cast<GroupDesc*>(disk.bytes.data() + 2 * Block);
  group->bg_inode_table = 0;
  Ext2Filesystem fs;
  Ext2QuotaTestPeer::attach(fs, disk, false);
  errno = 0;
  EXPECT_EQ(Ext2QuotaTestPeer::allocateInode(fs), 0U);
  EXPECT_EQ(errno, EIO);
  EXPECT_EQ(Ext2QuotaTestPeer::freeInodes(fs), 2U);
}

TEST(Ext2Quota, ChargesDirectAndIndirectReservationsAndRefundsTruncate) {
  Fixture f;
  ASSERT_EQ(f.control(QuotaOperation::Enable, QuotaType::User), QuotaStatus::Success);
  ASSERT_EQ(f.control(QuotaOperation::Enable, QuotaType::Group), QuotaStatus::Success);
  ASSERT_EQ(f.limit(QuotaType::User, 1000, 14, 2), QuotaStatus::Success);
  ASSERT_EQ(f.limit(QuotaType::Group, 42, 15, 2), QuotaStatus::Success);
  ASSERT_TRUE(f.first->allocateRange(0, 13 * Block, true));
  EXPECT_EQ(f.first->getSize(), 0U);
  EXPECT_EQ(f.first->getAttributes().blocks, 28U);
  ASSERT_EQ(f.control(QuotaOperation::Get, QuotaType::User, 1000), QuotaStatus::Success);
  EXPECT_EQ(f.response.record.currentSpace, 14 * Block);
  errno = 0;
  EXPECT_FALSE(f.first->allocateRange(13 * Block, Block, true));
  EXPECT_EQ(errno, 122);
  ASSERT_TRUE(f.first->resize(0));
  ASSERT_EQ(f.control(QuotaOperation::Get, QuotaType::Group, 42), QuotaStatus::Success);
  EXPECT_EQ(f.response.record.currentSpace, 0U);
  EXPECT_EQ(f.response.record.currentInodes, 2U);
}

TEST(Ext2Quota, FailedPayloadReadReturnsItsProvisionalCharge) {
  Fixture f;
  ASSERT_EQ(f.control(QuotaOperation::Enable, QuotaType::User), QuotaStatus::Success);
  const size_t free = Ext2QuotaTestPeer::freeBlocks(f.fs);
  const uint32_t next = Ext2QuotaTestPeer::firstFreeBlock(f.fs);
  ASSERT_NE(next, 0U);
  f.disk.failedRead = next * Block;
  errno = 0;
  EXPECT_FALSE(f.first->allocateRange(0, Block, true));
  EXPECT_EQ(errno, EIO);
  EXPECT_EQ(Ext2QuotaTestPeer::freeBlocks(f.fs), free);
  ASSERT_EQ(f.control(QuotaOperation::Get, QuotaType::User, 1000), QuotaStatus::Success);
  EXPECT_EQ(f.response.record.currentSpace, 0U);
  f.disk.failedRead = ~uint64_t(0);
  EXPECT_TRUE(f.first->allocateRange(0, Block, true));
}

TEST(Ext2Quota, InodeLimitsAndFailedOwnershipTransferPreserveOriginalIdentity) {
  Fixture f;
  ASSERT_EQ(f.control(QuotaOperation::Enable, QuotaType::User), QuotaStatus::Success);
  ASSERT_EQ(f.control(QuotaOperation::Enable, QuotaType::Group), QuotaStatus::Success);
  ASSERT_EQ(f.limit(QuotaType::User, 1000, 10, 2), QuotaStatus::Success);
  errno = 0;
  EXPECT_EQ(Ext2QuotaTestPeer::allocateInode(f.fs), 0U);
  EXPECT_EQ(errno, 122);
  ASSERT_TRUE(f.first->allocateRange(0, 2 * Block, true));
  ASSERT_EQ(f.limit(QuotaType::User, 1001, 10, 2), QuotaStatus::Success);
  ASSERT_EQ(f.limit(QuotaType::Group, 43, 1, 2), QuotaStatus::Success);
  errno = 0;
  EXPECT_FALSE(f.first->setOwnership(1001, 43, true, true));
  EXPECT_EQ(errno, 122);
  EXPECT_EQ(f.first->getUid(), 1000U);
  EXPECT_EQ(f.first->getGid(), 42U);
  ASSERT_EQ(f.control(QuotaOperation::Get, QuotaType::User, 1001), QuotaStatus::Success);
  EXPECT_EQ(f.response.record.currentSpace, 0U);
  ASSERT_EQ(f.limit(QuotaType::Group, 43, 2, 1), QuotaStatus::Success);
  EXPECT_TRUE(f.first->setOwnership(1001, 43, true, true));
  ASSERT_EQ(f.control(QuotaOperation::Get, QuotaType::Group, 43), QuotaStatus::Success);
  EXPECT_EQ(f.response.record.currentSpace, 2 * Block);
  EXPECT_EQ(f.response.record.currentInodes, 1U);
}

TEST(Ext2Quota, OpenUnlinkedInodeStaysChargedThroughFinalAliasRelease) {
  Fixture f;
  ASSERT_EQ(f.control(QuotaOperation::Enable, QuotaType::User), QuotaStatus::Success);
  ASSERT_TRUE(f.first->allocateRange(0, 2 * Block, true));
  auto alias = f.file(3);
  ASSERT_TRUE(Ext2QuotaTestPeer::unlink(f.fs, *f.first));
  f.first.reset();
  ASSERT_EQ(f.control(QuotaOperation::Get, QuotaType::User, 1000), QuotaStatus::Success);
  EXPECT_EQ(f.response.record.currentSpace, 2 * Block);
  EXPECT_EQ(f.response.record.currentInodes, 2U);
  alias.reset();
  ASSERT_EQ(f.control(QuotaOperation::Get, QuotaType::User, 1000), QuotaStatus::Success);
  EXPECT_EQ(f.response.record.currentSpace, 0U);
  EXPECT_EQ(f.response.record.currentInodes, 1U);
}

TEST(Ext2Quota, FailedDeviceBarrierRetainsEnforcementAndRetriesQuotaOff) {
  Fixture f;
  ASSERT_EQ(f.control(QuotaOperation::Enable, QuotaType::User), QuotaStatus::Success);
  f.disk.failedHardware = true;
  EXPECT_EQ(f.limit(QuotaType::User, 1000, 1, 2), QuotaStatus::IoError);
  EXPECT_EQ(f.control(QuotaOperation::Disable, QuotaType::User), QuotaStatus::IoError);
  const uint8_t byte = 7;
  errno = 0;
  EXPECT_EQ(f.users->write(0, 1, reinterpret_cast<uintptr_t>(&byte)), 0U);
  EXPECT_EQ(errno, EPERM);
  EXPECT_FALSE(f.first->allocateRange(0, 2 * Block, true));
  EXPECT_EQ(errno, 122);
  f.disk.failedHardware = false;
  EXPECT_EQ(f.control(QuotaOperation::Sync, QuotaType::User), QuotaStatus::Success);
  EXPECT_EQ(f.control(QuotaOperation::Disable, QuotaType::User), QuotaStatus::Success);
  EXPECT_EQ(f.control(QuotaOperation::Get, QuotaType::User, 1000), QuotaStatus::NotEnabled);
}

TEST(Ext2Quota, ConcurrentAllocatorsCannotBothConsumeLastBlockAllowance) {
  Fixture f;
  ASSERT_EQ(f.control(QuotaOperation::Enable, QuotaType::User), QuotaStatus::Success);
  ASSERT_EQ(f.limit(QuotaType::User, 1000, 1, 2), QuotaStatus::Success);
  std::promise<void> start;
  auto ready = start.get_future().share();
  auto first = std::async(std::launch::async, [&] {
    ready.wait();
    return f.first->allocateRange(0, Block, true);
  });
  auto second = std::async(std::launch::async, [&] {
    ready.wait();
    return f.second->allocateRange(0, Block, true);
  });
  start.set_value();
  EXPECT_NE(first.get(), second.get());
  ASSERT_EQ(f.control(QuotaOperation::Get, QuotaType::User, 1000), QuotaStatus::Success);
  EXPECT_EQ(f.response.record.currentSpace, Block);
}

TEST(Ext2Quota, BusyQuotaOffIsBoundedAndQuietRetrySucceeds) {
  Fixture f;
  ASSERT_EQ(f.control(QuotaOperation::Enable, QuotaType::User), QuotaStatus::Success);
  struct ConcurrentCharges {
    Ext2File* file;
    size_t calls = 0;
  } charges{f.first.get()};
  Ext2QuotaTestPeer::offHook(
      f.fs,
      [](void* context) {
        auto& charges = *static_cast<ConcurrentCharges*>(context);
        auto allocation = std::async(std::launch::async, [&] {
          return charges.file->allocateRange(charges.calls++ * Block, Block, true);
        });
        EXPECT_TRUE(allocation.get());
      },
      &charges);
  EXPECT_EQ(f.control(QuotaOperation::Disable, QuotaType::User), QuotaStatus::Busy);
  EXPECT_EQ(charges.calls, 8U);
  ASSERT_EQ(f.control(QuotaOperation::Get, QuotaType::User, 1000), QuotaStatus::Success);
  EXPECT_EQ(f.response.record.currentSpace, 8 * Block);
  Ext2QuotaTestPeer::offHook(f.fs, nullptr, nullptr);
  EXPECT_EQ(f.control(QuotaOperation::Disable, QuotaType::User), QuotaStatus::Success);
}

TEST(Ext2Quota, ColdEnableLoadsLimitsAndRecountsAllocatedInodes) {
  std::vector<uint8_t> image;
  {
    Fixture f;
    ASSERT_EQ(f.control(QuotaOperation::Enable, QuotaType::User), QuotaStatus::Success);
    ASSERT_EQ(f.limit(QuotaType::User, 1000, 4, 2), QuotaStatus::Success);
    ASSERT_TRUE(f.first->allocateRange(0, 2 * Block, true));
    ASSERT_EQ(f.control(QuotaOperation::Disable, QuotaType::User), QuotaStatus::Success);
    image = f.disk.persisted;
  }
  QuotaDisk disk(image);
  Ext2Filesystem filesystem;
  Ext2QuotaTestPeer::attach(filesystem, disk);
  Ext2File users(String("quota.user"), 4, Ext2QuotaTestPeer::inode(filesystem, 4), &filesystem);
  QuotaRequest request;
  QuotaResponse response;
  request.operation = QuotaOperation::Enable;
  request.format = Quota::OldFormat;
  ASSERT_EQ(filesystem.quotaControl(request, response, &users), QuotaStatus::Success);
  request.operation = QuotaOperation::Get;
  request.id = 1000;
  ASSERT_EQ(filesystem.quotaControl(request, response), QuotaStatus::Success);
  EXPECT_EQ(response.record.blockHardLimit, 4U);
  EXPECT_EQ(response.record.currentSpace, 2 * Block);
  EXPECT_EQ(response.record.currentInodes, 2U);
}

TEST(Ext2Quota, AttributeBlocksAreChargedOncePerInodeAndRefundedAfterRemoval) {
  Fixture f;
  ASSERT_EQ(f.control(QuotaOperation::Enable, QuotaType::User), QuotaStatus::Success);
  ASSERT_EQ(f.control(QuotaOperation::Enable, QuotaType::Group), QuotaStatus::Success);
  ASSERT_EQ(f.limit(QuotaType::User, 1000, 1, 2), QuotaStatus::Success);
  const StringView key("user.quota");
  ASSERT_EQ(f.first->setExtendedAttribute(key, "value", 5, 0), XattrStatus::Success);
  ASSERT_EQ(f.control(QuotaOperation::Get, QuotaType::Group, 42), QuotaStatus::Success);
  EXPECT_EQ(f.response.record.currentSpace, Block);
  const size_t free = Ext2QuotaTestPeer::freeBlocks(f.fs);
  EXPECT_EQ(f.second->setExtendedAttribute(key, "other", 5, 0), XattrStatus::Quota);
  EXPECT_EQ(Ext2QuotaTestPeer::freeBlocks(f.fs), free);
  EXPECT_EQ(Ext2QuotaTestPeer::inode(f.fs, 6)->i_file_acl, 0U);
  ASSERT_EQ(f.first->setExtendedAttribute(key, "replacement", 11, 0), XattrStatus::Success);
  ASSERT_EQ(f.first->removeExtendedAttribute(key), XattrStatus::Success);
  ASSERT_EQ(f.control(QuotaOperation::Get, QuotaType::User, 1000), QuotaStatus::Success);
  EXPECT_EQ(f.response.record.currentSpace, 0U);
  EXPECT_EQ(f.second->setExtendedAttribute(key, "retry", 5, 0), XattrStatus::Success);
}

TEST(Ext2Quota, SharedAttributeCopyOnWriteKeepsBothLogicalCharges) {
  Fixture f;
  const StringView key("user.quota");
  ASSERT_EQ(f.first->setExtendedAttribute(key, "old", 3, 0), XattrStatus::Success);
  const uint32_t block = LITTLE_TO_HOST32(Ext2QuotaTestPeer::inode(f.fs, 3)->i_file_acl);
  auto* header = reinterpret_cast<Ext2Ea::Header*>(f.disk.bytes.data() + block * Block);
  header->references = HOST_TO_LITTLE32(2);
  auto* second = Ext2QuotaTestPeer::inode(f.fs, 6);
  second->i_file_acl = HOST_TO_LITTLE32(block);
  second->i_blocks = HOST_TO_LITTLE32(2);
  ASSERT_EQ(f.control(QuotaOperation::Enable, QuotaType::User), QuotaStatus::Success);
  ASSERT_EQ(f.limit(QuotaType::User, 1000, 2, 2), QuotaStatus::Success);
  ASSERT_EQ(f.first->setExtendedAttribute(key, "new", 3, 0), XattrStatus::Success);
  EXPECT_NE(Ext2QuotaTestPeer::inode(f.fs, 3)->i_file_acl, second->i_file_acl);
  ASSERT_EQ(f.control(QuotaOperation::Get, QuotaType::User, 1000), QuotaStatus::Success);
  EXPECT_EQ(f.response.record.currentSpace, 2 * Block);
  ASSERT_EQ(f.first->removeExtendedAttribute(key), XattrStatus::Success);
  ASSERT_EQ(f.control(QuotaOperation::Get, QuotaType::User, 1000), QuotaStatus::Success);
  EXPECT_EQ(f.response.record.currentSpace, Block);
  char contents[3] = {};
  size_t required = 0;
  EXPECT_EQ(f.second->getExtendedAttribute(key, contents, sizeof(contents), required),
            XattrStatus::Success);
  EXPECT_EQ(std::string(contents, sizeof(contents)), "old");
}

TEST(Ext2Quota, FailedDirectoryPayloadReadRollsBackBlockAndUnpublishedInode) {
  Fixture f;
  auto* metadata = Ext2QuotaTestPeer::inode(f.fs, 2);
  metadata->i_mode = HOST_TO_LITTLE16(EXT2_S_IFDIR | 0700);
  Ext2Directory directory(String("root"), 2, metadata, &f.fs, nullptr);
  ASSERT_EQ(f.control(QuotaOperation::Enable, QuotaType::User), QuotaStatus::Success);
  ASSERT_EQ(f.control(QuotaOperation::Get, QuotaType::User, 0), QuotaStatus::Success);
  const auto before = f.response.record;
  const size_t blocks = Ext2QuotaTestPeer::freeBlocks(f.fs);
  const size_t inodes = Ext2QuotaTestPeer::freeInodes(f.fs);
  f.disk.failedRead = Ext2QuotaTestPeer::firstFreeBlock(f.fs) * Block;
  errno = 0;
  EXPECT_FALSE(Ext2QuotaTestPeer::create(f.fs, directory, String("")));
  EXPECT_EQ(errno, EIO);
  f.disk.failedRead = ~uint64_t(0);
  EXPECT_EQ(Ext2QuotaTestPeer::freeBlocks(f.fs), blocks);
  EXPECT_EQ(Ext2QuotaTestPeer::freeInodes(f.fs), inodes);
  ASSERT_EQ(f.control(QuotaOperation::Get, QuotaType::User, 0), QuotaStatus::Success);
  EXPECT_EQ(f.response.record.currentSpace, before.currentSpace);
  EXPECT_EQ(f.response.record.currentInodes, before.currentInodes);
}

TEST(Ext2Quota, FailedLongSymlinkChargeDoesNotPublishOrLeakAnInode) {
  Fixture f;
  auto* metadata = Ext2QuotaTestPeer::inode(f.fs, 2);
  metadata->i_mode = HOST_TO_LITTLE16(EXT2_S_IFDIR | 0700);
  Ext2Directory directory(String("root"), 2, metadata, &f.fs, nullptr);
  ASSERT_TRUE(f.first->setOwnership(0, 0, true, false));
  ASSERT_TRUE(f.first->allocateRange(0, Block, true));
  ASSERT_EQ(f.control(QuotaOperation::Enable, QuotaType::User), QuotaStatus::Success);
  ASSERT_EQ(f.limit(QuotaType::User, 0, 1, 0), QuotaStatus::Success);
  ASSERT_EQ(f.control(QuotaOperation::Get, QuotaType::User, 0), QuotaStatus::Success);
  const auto before = f.response.record;
  const size_t inodes = Ext2QuotaTestPeer::freeInodes(f.fs);
  const String target("a-long-symlink-target-that-needs-a-data-block-instead-of-the-inline-inode");
  errno = 0;
  EXPECT_FALSE(Ext2QuotaTestPeer::create(f.fs, directory, target));
  EXPECT_EQ(errno, 122);
  EXPECT_EQ(Ext2QuotaTestPeer::freeInodes(f.fs), inodes);
  ASSERT_EQ(f.control(QuotaOperation::Get, QuotaType::User, 0), QuotaStatus::Success);
  EXPECT_EQ(f.response.record.currentSpace, before.currentSpace);
  EXPECT_EQ(f.response.record.currentInodes, before.currentInodes);
}

TEST(Ext2Quota, FailedSecondTypeEnableCannotUnprotectTheActiveQuotaFile) {
  Fixture f;
  ASSERT_EQ(f.control(QuotaOperation::Enable, QuotaType::User), QuotaStatus::Success);
  QuotaRequest request;
  request.operation = QuotaOperation::Enable;
  request.type = QuotaType::Group;
  request.format = Quota::OldFormat;
  EXPECT_EQ(f.fs.quotaControl(request, f.response, f.users.get()), QuotaStatus::Busy);
  EXPECT_EQ(f.control(QuotaOperation::GetFormat, QuotaType::User), QuotaStatus::Success);
  const uint8_t byte = 1;
  errno = 0;
  EXPECT_EQ(f.users->write(0, 1, reinterpret_cast<uintptr_t>(&byte)), 0U);
  EXPECT_EQ(errno, EPERM);
  EXPECT_EQ(f.control(QuotaOperation::Disable, QuotaType::User), QuotaStatus::Success);
}
