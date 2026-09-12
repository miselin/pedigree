/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/utilities/Vector.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <vector>

#include "modules/system/ext2/Ext2File.h"
#include "modules/system/ext2/Ext2Filesystem.h"
#include "modules/system/ext2/ext2.h"
#include <condition_variable>
#include <gtest/gtest.h>

class Ext2FilesystemSyncTestPeer {
 public:
  static void configure(Ext2Filesystem& fs, Disk& disk) {
    fs.m_pDisk = &disk;
    fs.m_BlockSize = 1024;
    fs.m_InodeSize = sizeof(Inode);
    fs.m_nGroupDescriptors = 1;
    fs.m_pSuperblock = disk.read(1024).as<Superblock>();
    fs.m_pGroupDescriptors = new GroupDesc*[1]{disk.read(2048).as<GroupDesc>()};
    fs.m_pBlockBitmaps = new Vector<size_t>[1];
    fs.m_pInodeBitmaps = new Vector<size_t>[1];
    fs.m_pInodeTables = new Vector<size_t>[1];
    fs.m_pBlockBitmaps[0].pushBack(disk.read(3072).address());
    fs.m_pInodeBitmaps[0].pushBack(disk.read(4096).address());
    fs.m_pInodeTables[0].pushBack(disk.read(5120).address());
  }

  static void dirty(Ext2Filesystem& fs, uint32_t inode, uint8_t value) {
    Ext2InodeState* state = fs.m_InodeStates.lookup(inode);
    ASSERT_NE(state, nullptr);
    LockGuard<Mutex> guard(state->dataLock);
    ASSERT_NE(state->cache, nullptr);
    const uintptr_t page = state->cache->fill.lookup(0);
    ASSERT_NE(page, 0U);
    reinterpret_cast<uint8_t*>(page)[0] = value;
    state->cache->fill.markDirty(0);
    state->cache->fill.release(0);
  }

  static size_t references(Ext2Filesystem& fs, uint32_t inode) {
    LockGuard<Mutex> registry(fs.m_InodeStateLock);
    Ext2InodeState* state = fs.m_InodeStates.lookup(inode);
    return state ? state->references : 0;
  }

  static bool retainedCache(Ext2Filesystem& fs, uint32_t inode) {
    LockGuard<Mutex> registry(fs.m_InodeStateLock);
    Ext2InodeState* state = fs.m_InodeStates.lookup(inode);
    return state && state->cache;
  }

  static bool unlink(Ext2Filesystem& fs, Ext2File& file) {
    return fs.releaseInode(file.getInodeNumber(), &file);
  }

  static void xattrDependencies(Ext2Filesystem& fs) {
    using Kind = Ext2Filesystem::AttributeWriteKind;
    fs.recordAttributeWriteLocked(5 * 1024, Kind::Inode);
    fs.recordAttributeWriteLocked(3 * 1024, Kind::Allocation);
    fs.recordAttributeWriteLocked(24 * 1024, Kind::Payload);
  }

  static size_t pendingAttributes(Ext2Filesystem& fs) {
    return fs.m_AttributeWriteCount;
  }

  static bool beginWritableMount(Ext2Filesystem& fs, uint16_t state = EXT2_STATE_CLEAN) {
    fs.m_pSuperblock->s_state = HOST_TO_LITTLE16(state);
    return fs.beginWritableMount();
  }
};

namespace {
using Status = Filesystem::SyncStatus;
using namespace std::chrono_literals;
constexpr size_t BlockSize = 1024, BlockCount = 128;

class SyncDisk final : public Disk {
 public:
  SyncDisk() : bytes(BlockSize * BlockCount), persisted(bytes), pins(BlockCount, 0) {}

  BufferView read(uint64_t location) override {
    std::lock_guard<std::mutex> guard(lock);
    if (location >= bytes.size())
      return {};
    ++pins[location / BlockSize];
    return BufferView(bytes.data() + location, BlockSize - location % BlockSize);
  }

  void write(uint64_t location) override {
    std::lock_guard<std::mutex> guard(lock);
    writes.push_back(location);
  }

  bool sync(uint64_t location, bool async) override {
    std::unique_lock<std::mutex> guard(lock);
    syncs.push_back(location);
    if (location == pausedSync) {
      enteredSync = true;
      changed.notify_all();
      if (!changed.wait_for(guard, 5s, [&] { return releasedSync; }))
        return false;
    }
    if (async || location == failedSync || location >= bytes.size())
      return false;
    const size_t start = location - location % BlockSize;
    std::copy_n(bytes.data() + start, BlockSize, persisted.data() + start);
    return true;
  }

  bool syncAll() override {
    std::unique_lock<std::mutex> guard(lock);
    ++allCalls;
    enteredAll = true;
    changed.notify_all();
    if (pauseAll && !changed.wait_for(guard, 5s, [&] { return releasedAll; }))
      return false;
    bool success = true;
    for (size_t block = 0; block < BlockCount; ++block) {
      if (block == failedAllBlock) {
        success = false;
        continue;
      }
      std::copy_n(bytes.data() + block * BlockSize, BlockSize,
                  persisted.data() + block * BlockSize);
    }
    ++hardwareFlushes;
    return success && !failedHardwareFlush;
  }

  bool pin(uint64_t location) override {
    std::lock_guard<std::mutex> guard(lock);
    if (location >= bytes.size())
      return false;
    ++pins[location / BlockSize];
    return true;
  }

  void unpin(uint64_t location) override {
    std::lock_guard<std::mutex> guard(lock);
    EXPECT_LT(location, bytes.size());
    if (location >= bytes.size())
      return;
    EXPECT_GT(pins[location / BlockSize], 0U);
    if (pins[location / BlockSize])
      --pins[location / BlockSize];
  }

  size_t getSize() const override {
    return bytes.size();
  }

  size_t getBlockSize() const override {
    return BlockSize;
  }

  bool waitForAll() {
    std::unique_lock<std::mutex> guard(lock);
    return changed.wait_for(guard, 5s, [&] { return enteredAll; });
  }

  bool waitForSync() {
    std::unique_lock<std::mutex> guard(lock);
    return changed.wait_for(guard, 5s, [&] { return enteredSync; });
  }

  void releaseSync() {
    std::lock_guard<std::mutex> guard(lock);
    releasedSync = true;
    changed.notify_all();
  }

  void releaseAll() {
    std::lock_guard<std::mutex> guard(lock);
    releasedAll = true;
    changed.notify_all();
  }

  std::vector<uint8_t> bytes, persisted;
  std::vector<size_t> pins;
  std::vector<uint64_t> writes, syncs;
  std::mutex lock;
  std::condition_variable changed;
  uint64_t failedSync = ~uint64_t(0);
  uint64_t pausedSync = ~uint64_t(0);
  size_t failedAllBlock = ~size_t(0), allCalls = 0, hardwareFlushes = 0;
  bool failedHardwareFlush = false;
  bool pauseAll = false, enteredAll = false, releasedAll = false;
  bool enteredSync = false, releasedSync = false;
};

class SyncFile final : public Ext2File {
 public:
  using Ext2File::Ext2File;

 protected:
  bool useFillCache() const override {
    // Exercise the guest cache path even in the native utility's NOMMU build.
    return true;
  }
};

struct Fixture {
  Fixture() {
    auto* super = reinterpret_cast<Superblock*>(disk.bytes.data() + 1024);
    super->s_rev_level = HOST_TO_LITTLE32(1);
    super->s_blocks_count = HOST_TO_LITTLE32(BlockCount);
    super->s_blocks_per_group = HOST_TO_LITTLE32(BlockCount);
    super->s_inodes_count = HOST_TO_LITTLE32(8);
    super->s_inodes_per_group = HOST_TO_LITTLE32(8);
    super->s_first_data_block = HOST_TO_LITTLE32(1);
    auto* group = reinterpret_cast<GroupDesc*>(disk.bytes.data() + 2 * BlockSize);
    group->bg_block_bitmap = HOST_TO_LITTLE32(3);
    group->bg_inode_bitmap = HOST_TO_LITTLE32(4);
    group->bg_inode_table = HOST_TO_LITTLE32(5);
    std::fill_n(disk.bytes.data() + 3 * BlockSize, BlockSize, 0xff);
    std::fill_n(disk.bytes.data() + 4 * BlockSize, BlockSize, 0xff);
    for (uint32_t number = 3; number <= 4; ++number) {
      Inode* metadata = inode(number);
      metadata->i_mode = HOST_TO_LITTLE16(EXT2_S_IFREG | 0600);
      metadata->i_links_count = HOST_TO_LITTLE16(1);
      metadata->i_size = HOST_TO_LITTLE32(2 * BlockSize);
      metadata->i_blocks = HOST_TO_LITTLE32(4);
      metadata->i_block[0] = HOST_TO_LITTLE32(16 + 2 * (number - 3));
      metadata->i_block[1] = HOST_TO_LITTLE32(17 + 2 * (number - 3));
    }
    Ext2FilesystemSyncTestPeer::configure(fs, disk);
    disk.persisted = disk.bytes;
  }

  Inode* inode(uint32_t number) {
    return reinterpret_cast<Inode*>(disk.bytes.data() + 5 * BlockSize) + number - 1;
  }

  std::unique_ptr<Ext2File> file(uint32_t number = 3) {
    auto result = std::make_unique<SyncFile>(String("cached"), number, inode(number), &fs);
    EXPECT_EQ(result->read(0, 2 * BlockSize, 0), 2 * BlockSize);
    return result;
  }

  SyncDisk disk;
  Ext2Filesystem fs;
};

TEST(Ext2FilesystemShutdown, WritableMountClearsCleanStateAndShutdownCommitsItLast) {
  Fixture fixture;
  ASSERT_TRUE(Ext2FilesystemSyncTestPeer::beginWritableMount(fixture.fs));
  auto& stored = *reinterpret_cast<Superblock*>(fixture.disk.persisted.data() + 1024);
  EXPECT_EQ(LITTLE_TO_HOST16(stored.s_state), 0U);
  auto file = fixture.file();
  Ext2FilesystemSyncTestPeer::dirty(fixture.fs, 3, 0x6d);
  fixture.disk.failedSync = 16 * BlockSize;
  file.reset();
  fixture.disk.failedSync = ~uint64_t(0);
  EXPECT_EQ(fixture.fs.shutdown(), Status::Success);
  EXPECT_EQ(fixture.disk.persisted[16 * BlockSize], 0x6d);
  EXPECT_EQ(LITTLE_TO_HOST16(stored.s_state), EXT2_STATE_CLEAN);
  const size_t completed = fixture.disk.allCalls;
  EXPECT_EQ(fixture.fs.shutdown(), Status::Success);
  EXPECT_EQ(fixture.disk.allCalls, completed);
}

TEST(Ext2FilesystemShutdown, FailedDataFlushNeverCommitsCleanState) {
  Fixture fixture;
  ASSERT_TRUE(Ext2FilesystemSyncTestPeer::beginWritableMount(fixture.fs));
  auto file = fixture.file();
  Ext2FilesystemSyncTestPeer::dirty(fixture.fs, 3, 0x71);
  fixture.disk.failedSync = 16 * BlockSize;
  file.reset();
  EXPECT_EQ(fixture.fs.shutdown(), Status::IoError);
  const auto* stored = reinterpret_cast<const Superblock*>(fixture.disk.persisted.data() + 1024);
  EXPECT_EQ(LITTLE_TO_HOST16(stored->s_state), 0U);
  fixture.disk.failedSync = ~uint64_t(0);
  EXPECT_EQ(fixture.fs.shutdown(), Status::Success);
}

TEST(Ext2FilesystemShutdown, FailedHardwareFlushNeverCommitsCleanState) {
  Fixture fixture;
  ASSERT_TRUE(Ext2FilesystemSyncTestPeer::beginWritableMount(fixture.fs));
  fixture.disk.failedHardwareFlush = true;
  EXPECT_EQ(fixture.fs.shutdown(), Status::IoError);
  const auto* stored = reinterpret_cast<const Superblock*>(fixture.disk.persisted.data() + 1024);
  EXPECT_EQ(LITTLE_TO_HOST16(stored->s_state), 0U);
}

TEST(Ext2FilesystemShutdown, PreviouslyUncheckedAndErrorMarkedVolumesStayUnchecked) {
  for (uint16_t initial : {uint16_t(0), uint16_t(EXT2_STATE_CLEAN | EXT2_STATE_UNCLEAN)}) {
    Fixture fixture;
    ASSERT_TRUE(Ext2FilesystemSyncTestPeer::beginWritableMount(fixture.fs, initial));
    EXPECT_EQ(fixture.fs.shutdown(), Status::IoError);
    const auto* stored = reinterpret_cast<const Superblock*>(fixture.disk.persisted.data() + 1024);
    EXPECT_EQ(LITTLE_TO_HOST16(stored->s_state), initial & ~EXT2_STATE_CLEAN);
  }
}

TEST(Ext2FilesystemShutdown, FailedCleanMarkerWriteReturnsFailure) {
  Fixture fixture;
  ASSERT_TRUE(Ext2FilesystemSyncTestPeer::beginWritableMount(fixture.fs));
  fixture.disk.failedSync = 1024;
  EXPECT_EQ(fixture.fs.shutdown(), Status::IoError);
  const auto* stored = reinterpret_cast<const Superblock*>(fixture.disk.persisted.data() + 1024);
  EXPECT_EQ(LITTLE_TO_HOST16(stored->s_state), 0U);
}

TEST(Ext2FilesystemShutdown, UnsupportedFeaturesDoNotChangeTheMountState) {
  for (size_t feature = 0; feature < 3; ++feature) {
    Fixture fixture;
    auto* super = reinterpret_cast<Superblock*>(fixture.disk.bytes.data() + 1024);
    super->s_state = HOST_TO_LITTLE16(EXT2_STATE_CLEAN);
    if (feature == 0)
      super->s_feature_compat = HOST_TO_LITTLE32(0x4);
    else if (feature == 1)
      super->s_feature_incompat = HOST_TO_LITTLE32(0x40);
    else
      super->s_feature_ro_compat = HOST_TO_LITTLE32(0x8);
    fixture.disk.persisted = fixture.disk.bytes;
    EXPECT_FALSE(Ext2FilesystemSyncTestPeer::beginWritableMount(fixture.fs));
    EXPECT_EQ(LITTLE_TO_HOST16(super->s_state), EXT2_STATE_CLEAN);
    EXPECT_TRUE(fixture.disk.writes.empty());
    EXPECT_TRUE(fixture.disk.syncs.empty());
  }
}

TEST(Ext2FilesystemSync, FlushesSharedPagesAfterLastFileAliasCloses) {
  Fixture fixture;
  auto file = fixture.file();
  Ext2FilesystemSyncTestPeer::dirty(fixture.fs, 3, 71);
  // Closing the final alias tries writeback itself; fail that attempt to leave
  // authoritative dirty data owned only by the shared inode state.
  fixture.disk.failedSync = 16 * BlockSize;
  file.reset();
  ASSERT_EQ(Ext2FilesystemSyncTestPeer::references(fixture.fs, 3), 0U);
  ASSERT_TRUE(Ext2FilesystemSyncTestPeer::retainedCache(fixture.fs, 3));
  EXPECT_EQ(fixture.disk.persisted[16 * BlockSize], 0);
  fixture.disk.failedSync = ~uint64_t(0);
  EXPECT_EQ(fixture.fs.sync(), Status::Success);
  EXPECT_EQ(fixture.disk.persisted[16 * BlockSize], 71);
  EXPECT_EQ(fixture.disk.hardwareFlushes, 1U);
  EXPECT_EQ(Ext2FilesystemSyncTestPeer::references(fixture.fs, 3), 0U);
}

TEST(Ext2FilesystemSync, RetainsFailedCacheForRetryAndContinuesOtherInodes) {
  Fixture fixture;
  auto first = fixture.file(3), second = fixture.file(4);
  Ext2FilesystemSyncTestPeer::dirty(fixture.fs, 3, 79);
  Ext2FilesystemSyncTestPeer::dirty(fixture.fs, 4, 83);
  fixture.disk.failedSync = 16 * BlockSize;
  first.reset();
  ASSERT_EQ(Ext2FilesystemSyncTestPeer::references(fixture.fs, 3), 0U);
  ASSERT_TRUE(Ext2FilesystemSyncTestPeer::retainedCache(fixture.fs, 3));
  EXPECT_EQ(fixture.fs.sync(), Status::IoError);
  EXPECT_EQ(fixture.disk.persisted[18 * BlockSize], 83);
  EXPECT_EQ(fixture.disk.hardwareFlushes, 1U);
  ASSERT_TRUE(Ext2FilesystemSyncTestPeer::retainedCache(fixture.fs, 3));
  fixture.disk.failedSync = ~uint64_t(0);
  EXPECT_EQ(fixture.fs.sync(), Status::Success);
  EXPECT_EQ(fixture.disk.persisted[16 * BlockSize], 79);
  EXPECT_GE(std::count(fixture.disk.syncs.begin(), fixture.disk.syncs.end(), 16 * BlockSize), 2);
}

TEST(Ext2FilesystemSync, PreservesXattrDependencyOrderAndNeverBypassesFailure) {
  const std::vector<uint64_t> order{24 * BlockSize, 3 * BlockSize, 5 * BlockSize};
  for (size_t failure = 0; failure < order.size(); ++failure) {
    Fixture fixture;
    Ext2FilesystemSyncTestPeer::xattrDependencies(fixture.fs);
    fixture.disk.failedSync = order[failure];
    EXPECT_EQ(fixture.fs.sync(), Status::IoError);
    EXPECT_EQ(fixture.disk.syncs,
              (std::vector<uint64_t>(order.begin(), order.begin() + failure + 1)));
    EXPECT_EQ(fixture.disk.allCalls, 0U);
    EXPECT_EQ(Ext2FilesystemSyncTestPeer::pendingAttributes(fixture.fs), 3U);
    fixture.disk.failedSync = ~uint64_t(0);
    fixture.disk.syncs.clear();
    EXPECT_EQ(fixture.fs.sync(), Status::Success);
    EXPECT_EQ(fixture.disk.syncs, order);
    EXPECT_EQ(Ext2FilesystemSyncTestPeer::pendingAttributes(fixture.fs), 0U);
    EXPECT_EQ(fixture.disk.allCalls, 1U);
  }
}

TEST(Ext2FilesystemSync, DormantIdentityStillReloadsMappingsAfterSync) {
  Fixture fixture;
  {
    Ext2File dormant(String("dormant"), 3, fixture.inode(3), &fixture.fs);
  }
  EXPECT_EQ(Ext2FilesystemSyncTestPeer::references(fixture.fs, 3), 0U);
  EXPECT_FALSE(Ext2FilesystemSyncTestPeer::retainedCache(fixture.fs, 3));
  EXPECT_EQ(fixture.fs.sync(), Status::Success);
  auto reopened = fixture.file();
  EXPECT_EQ(reopened->getSize(), 2 * BlockSize);
}

TEST(Ext2FilesystemSync, EmptyDataStillFlushesDiskMetadataAndReportsHardwareFailure) {
  Fixture fixture;
  fixture.disk.bytes[3 * BlockSize + 9] = 97;
  fixture.disk.bytes[5 * BlockSize + 13] = 101;
  fixture.disk.failedAllBlock = 3;
  EXPECT_EQ(fixture.fs.sync(), Status::IoError);
  EXPECT_EQ(fixture.disk.persisted[5 * BlockSize + 13], 101);
  fixture.disk.failedAllBlock = ~size_t(0);
  fixture.disk.failedHardwareFlush = true;
  EXPECT_EQ(fixture.fs.sync(), Status::IoError);
  fixture.disk.failedHardwareFlush = false;
  EXPECT_EQ(fixture.fs.sync(), Status::Success);
  EXPECT_EQ(fixture.disk.persisted[3 * BlockSize + 9], 97);
  EXPECT_EQ(fixture.disk.hardwareFlushes, 3U);
}

TEST(Ext2FilesystemSync, FinalAliasCloseCannotRetireOrphanBeforeDiskDrain) {
  Fixture fixture;
  auto file = fixture.file();
  Ext2FilesystemSyncTestPeer::dirty(fixture.fs, 3, 107);
  ASSERT_TRUE(Ext2FilesystemSyncTestPeer::unlink(fixture.fs, *file));
  fixture.disk.pauseAll = true;
  auto result = std::async(std::launch::async, [&] { return fixture.fs.sync(); });
  const bool admitted = fixture.disk.waitForAll();
  if (admitted) {
    file.reset();
    EXPECT_EQ(Ext2FilesystemSyncTestPeer::references(fixture.fs, 3), 1U);
    EXPECT_EQ(LITTLE_TO_HOST32(fixture.inode(3)->i_block[0]), 16U);
    EXPECT_NE(fixture.disk.bytes[4 * BlockSize] & (1U << 2), 0U);
  }
  fixture.disk.releaseAll();
  EXPECT_TRUE(admitted);
  EXPECT_EQ(result.get(), Status::Success);
  EXPECT_EQ(Ext2FilesystemSyncTestPeer::references(fixture.fs, 3), 0U);
  EXPECT_EQ(LITTLE_TO_HOST32(fixture.inode(3)->i_block[0]), 0U);
  EXPECT_EQ(fixture.disk.bytes[4 * BlockSize] & (1U << 2), 0U);
}

TEST(Ext2FilesystemSync, OrphanFillStillWritesAfterFinalAliasClosesDuringAnotherInode) {
  Fixture fixture;
  auto first = fixture.file(3), second = fixture.file(4);
  Ext2FilesystemSyncTestPeer::dirty(fixture.fs, 3, 109);
  Ext2FilesystemSyncTestPeer::dirty(fixture.fs, 4, 113);
  ASSERT_TRUE(Ext2FilesystemSyncTestPeer::unlink(fixture.fs, *second));
  fixture.disk.pausedSync = 16 * BlockSize;
  auto result = std::async(std::launch::async, [&] { return fixture.fs.sync(); });
  const bool admitted = fixture.disk.waitForSync();
  if (admitted) {
    second.reset();
    EXPECT_EQ(Ext2FilesystemSyncTestPeer::references(fixture.fs, 4), 1U);
    EXPECT_EQ(LITTLE_TO_HOST32(fixture.inode(4)->i_block[0]), 18U);
  }
  fixture.disk.releaseSync();
  EXPECT_TRUE(admitted);
  EXPECT_EQ(result.get(), Status::Success);
  EXPECT_EQ(fixture.disk.persisted[18 * BlockSize], 113);
  EXPECT_EQ(LITTLE_TO_HOST32(fixture.inode(4)->i_block[0]), 0U);
}
}  // namespace
