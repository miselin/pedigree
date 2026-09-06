/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "modules/system/ext2/Ext2Directory.h"
#include "modules/system/ext2/Ext2File.h"
#include "modules/system/ext2/Ext2Filesystem.h"
#include "modules/system/ext2/ext2.h"
#include <condition_variable>
#include <gtest/gtest.h>

class Ext2HandlesTestPeer {
 public:
  static void configure(Ext2Filesystem& fs, Disk& disk) {
    fs.m_pDisk = &disk;
    fs.m_pSuperblock = reinterpret_cast<Superblock*>(disk.read(1024).data());
    fs.m_BlockSize = 4096;
    fs.m_InodeSize = sizeof(Inode);
    fs.m_nGroupDescriptors = 1;
    fs.m_pGroupDescriptors = new GroupDesc*[1];
    fs.m_pGroupDescriptors[0] = reinterpret_cast<GroupDesc*>(disk.read(4096).data());
    fs.m_pBlockBitmaps = new Vector<size_t>[1];
    fs.m_pInodeBitmaps = new Vector<size_t>[1];
    fs.m_pInodeTables = new Vector<size_t>[1];
  }
  static Inode* inode(Ext2Filesystem& fs, uint32_t number) {
    return fs.getInode(number);
  }
  static uint32_t allocate(Ext2Filesystem& fs) {
    return fs.findFreeInode();
  }
  static bool unlink(Ext2Filesystem& fs, Ext2File& file) {
    return fs.releaseInode(file.getInodeNumber(), &file);
  }
};

namespace {
using Status = FileHandleStatus;
using namespace std::chrono_literals;
constexpr size_t BlockSize = 4096, BlockCount = 64;

class HandleDisk final : public Disk {
 public:
  HandleDisk() : bytes(BlockSize * BlockCount), persisted(bytes) {}
  BufferView read(uint64_t location) override {
    std::lock_guard<std::mutex> guard(lock);
    if (location >= bytes.size() || location == failedRead)
      return {};
    ++pins[location];
    return BufferView(bytes.data() + location, BlockSize - location % BlockSize);
  }
  void write(uint64_t location) override {
    std::unique_lock<std::mutex> guard(lock);
    if (pauseInodeWrite && location == 4 * BlockSize) {
      pauseInodeWrite = false;
      entered = true;
      changed.notify_all();
      EXPECT_TRUE(changed.wait_for(guard, 5s, [&] { return released; }));
    }
  }
  bool sync(uint64_t location, bool async) override {
    if (async) {
      write(location);
      return true;
    }
    std::lock_guard<std::mutex> guard(lock);
    if (location >= bytes.size())
      return false;
    const size_t start = location - location % BlockSize;
    std::copy_n(bytes.data() + start, BlockSize, persisted.data() + start);
    return true;
  }
  bool pin(uint64_t location) override {
    std::lock_guard<std::mutex> guard(lock);
    if (location >= bytes.size())
      return false;
    ++pins[location];
    return true;
  }
  void unpin(uint64_t location) override {
    std::lock_guard<std::mutex> guard(lock);
    EXPECT_GT(pins[location], 0U);
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
  bool waitForWrite() {
    std::unique_lock<std::mutex> guard(lock);
    return changed.wait_for(guard, 5s, [&] { return entered; });
  }
  void releaseWrite() {
    std::lock_guard<std::mutex> guard(lock);
    released = true;
    changed.notify_all();
  }
  std::vector<uint8_t> bytes, persisted;
  std::map<uint64_t, size_t> pins;
  std::mutex lock;
  std::condition_variable changed;
  uint64_t failedRead = ~uint64_t(0);
  bool pauseInodeWrite = false, entered = false, released = false;
};

struct Fixture {
  Fixture() {
    super()->s_rev_level = HOST_TO_LITTLE32(1);
    super()->s_blocks_count = HOST_TO_LITTLE32(BlockCount);
    super()->s_blocks_per_group = HOST_TO_LITTLE32(BlockCount);
    super()->s_inodes_count = HOST_TO_LITTLE32(16);
    super()->s_inodes_per_group = HOST_TO_LITTLE32(16);
    for (size_t i = 0; i < 16; ++i)
      super()->s_uuid[i] = i + 1;
    group()->bg_block_bitmap = HOST_TO_LITTLE32(2);
    group()->bg_inode_bitmap = HOST_TO_LITTLE32(3);
    group()->bg_inode_table = HOST_TO_LITTLE32(4);
    std::fill_n(disk.block(2), BlockSize, 0xff);
    std::fill_n(disk.block(3), BlockSize, 0xff);
    for (size_t i = 0; i < 16; ++i) {
      rawInode(i + 1)->i_mode = HOST_TO_LITTLE16(EXT2_S_IFREG | 0600);
      rawInode(i + 1)->i_links_count = HOST_TO_LITTLE16(1);
    }
    Ext2HandlesTestPeer::configure(fs, disk);
  }
  Superblock* super() {
    return reinterpret_cast<Superblock*>(disk.bytes.data() + 1024);
  }
  GroupDesc* group() {
    return reinterpret_cast<GroupDesc*>(disk.block(1));
  }
  Inode* rawInode(size_t number = 1) {
    return reinterpret_cast<Inode*>(disk.block(4)) + number - 1;
  }
  Inode* inode(size_t number = 1) {
    return Ext2HandlesTestPeer::inode(fs, number);
  }
  void freeCandidate(size_t number, uint32_t generation) {
    disk.block(3)[(number - 1) / 8] &= ~(1U << ((number - 1) % 8));
    rawInode(number)->i_generation = HOST_TO_LITTLE32(generation);
    rawInode(number)->i_links_count = 0;
    group()->bg_free_inodes_count =
        HOST_TO_LITTLE16(LITTLE_TO_HOST16(group()->bg_free_inodes_count) + 1);
    super()->s_free_inodes_count =
        HOST_TO_LITTLE32(LITTLE_TO_HOST32(super()->s_free_inodes_count) + 1);
  }
  bool unlink(Ext2File& file) {
    const bool final = Ext2HandlesTestPeer::unlink(fs, file);
    file.finishInodeRetirement();
    return final;
  }
  HandleDisk disk;
  Ext2Filesystem fs;
};

class Observer final : public FileEventObserver {
 public:
  void fileEvent(const FileEvent& event) override {
    if (event.mask & FileEvents::SourceRetired)
      ++retired;
    if (!(event.mask & FileEvents::Modify))
      return;
    ++modified;
    std::unique_lock<std::mutex> guard(lock);
    if (pause) {
      entered = true;
      changed.notify_all();
      EXPECT_TRUE(changed.wait_for(guard, 5s, [&] { return released; }));
    }
  }
  bool waitForCallback() {
    std::unique_lock<std::mutex> guard(lock);
    return changed.wait_for(guard, 5s, [&] { return entered; });
  }
  void release() {
    std::lock_guard<std::mutex> guard(lock);
    released = true;
    changed.notify_all();
  }
  std::atomic<unsigned> modified{0}, retired{0};
  std::mutex lock;
  std::condition_variable changed;
  bool pause = false, entered = false, released = false;
};
}  // namespace

TEST(Ext2Handles, AliasIdentityCheckedDecodeAndFinalLinkStale) {
  Fixture f;
  f.inode()->i_links_count = HOST_TO_LITTLE16(2);
  f.inode()->i_size = HOST_TO_LITTLE32(5);
  f.inode()->i_blocks = HOST_TO_LITTLE32(BlockSize / 512);
  f.inode()->i_block[0] = HOST_TO_LITTLE32(10);
  MemoryCopy(f.disk.block(10), "alive", 5);
  Ext2File first(String("first"), 1, f.inode(), &f.fs);
  Ext2File alias(String("second"), 1, f.inode(), &f.fs);
  FileHandle handle, other;
  ASSERT_EQ(f.fs.encodeFileHandle(first, handle), Status::Success);
  ASSERT_EQ(f.fs.encodeFileHandle(alias, other), Status::Success);
  EXPECT_EQ(handle.length, 24U);
  EXPECT_EQ(handle.type, 0x50444731);
  EXPECT_EQ(MemoryCompare(handle.bytes, other.bytes, handle.length), 0);
  RetainedFile decoded, second;
  ASSERT_EQ(f.fs.decodeFileHandle(handle, decoded), Status::Success);
  ASSERT_EQ(f.fs.decodeFileHandle(handle, second), Status::Success);
  EXPECT_NE(decoded.get(), second.get());
  EXPECT_EQ(decoded.get()->futexIdentity(), alias.futexIdentity());
  FileSystemId id;
  ASSERT_EQ(f.fs.fileHandleFsid(id), Status::Success);
  EXPECT_NE(id.words[0] | id.words[1], 0U);

  for (unsigned mutation = 0; mutation < 5; ++mutation) {
    FileHandle invalid = handle;
    if (mutation == 0)
      ++invalid.type;
    if (mutation == 1)
      --invalid.length;
    if (mutation == 2)
      invalid.bytes[0] ^= 1;
    if (mutation == 3)
      invalid.bytes[20] ^= 1;
    if (mutation == 4)
      std::fill_n(invalid.bytes + 16, 4, 0);
    RetainedFile rejected;
    EXPECT_EQ(f.fs.decodeFileHandle(invalid, rejected), Status::Stale);
    EXPECT_FALSE(rejected);
  }
  EXPECT_FALSE(f.unlink(first));
  RetainedFile survivingAlias;
  EXPECT_EQ(f.fs.decodeFileHandle(handle, survivingAlias), Status::Success);
  EXPECT_TRUE(f.unlink(alias));
  RetainedFile rejected;
  EXPECT_EQ(f.fs.decodeFileHandle(handle, rejected), Status::Stale);
  EXPECT_EQ(f.fs.encodeFileHandle(first, other), Status::Stale);
  auto* retained = static_cast<Ext2File*>(decoded.get());
  const uintptr_t data = retained->readBlock(0);
  ASSERT_NE(data, 0U);
  EXPECT_EQ(MemoryCompare(reinterpret_cast<void*>(data), "alive", 5), 0);
  retained->unpinBlock(0);
  EXPECT_EQ(decoded.get()->getInode(), 1U);
}

TEST(Ext2Handles, ReuseAdvancesGenerationAndFsyncPersistsIt) {
  Fixture f;
  FileHandle oldHandle;
  {
    Ext2File old(String("old"), 1, f.inode(), &f.fs);
    ASSERT_EQ(f.fs.encodeFileHandle(old, oldHandle), Status::Success);
    ASSERT_TRUE(f.unlink(old));
  }
  ASSERT_EQ(Ext2HandlesTestPeer::allocate(f.fs), 1U);
  EXPECT_EQ(LITTLE_TO_HOST32(f.inode()->i_generation), 1U);
  EXPECT_EQ(f.inode()->i_links_count, 0U);
  f.inode()->i_mode = HOST_TO_LITTLE16(EXT2_S_IFREG | 0600);
  f.inode()->i_links_count = HOST_TO_LITTLE16(1);
  Ext2File replacement(String("new"), 1, f.inode(), &f.fs);
  RetainedFile rejected;
  EXPECT_EQ(f.fs.decodeFileHandle(oldHandle, rejected), Status::Stale);
  FileHandle newHandle;
  ASSERT_EQ(f.fs.encodeFileHandle(replacement, newHandle), Status::Success);
  ASSERT_TRUE(replacement.sync());
  Fixture reboot;
  MemoryCopy(reboot.disk.bytes.data(), f.disk.persisted.data(), f.disk.persisted.size());
  RetainedFile decoded;
  EXPECT_EQ(reboot.fs.decodeFileHandle(newHandle, decoded), Status::Success);
  EXPECT_EQ(reboot.fs.decodeFileHandle(oldHandle, rejected), Status::Stale);
}

TEST(Ext2Handles, GenerationExhaustionSkipsSlotAndTableFailurePreservesAllocation) {
  Fixture f;
  f.freeCandidate(1, 0xffffffffU);
  f.freeCandidate(2, 41);
  const auto bitmap = std::vector<uint8_t>(f.disk.block(3), f.disk.block(3) + BlockSize);
  f.disk.failedRead = 4 * BlockSize;
  EXPECT_EQ(Ext2HandlesTestPeer::allocate(f.fs), 0U);
  EXPECT_EQ(std::vector<uint8_t>(f.disk.block(3), f.disk.block(3) + BlockSize), bitmap);
  EXPECT_EQ(LITTLE_TO_HOST16(f.group()->bg_free_inodes_count), 2U);
  EXPECT_EQ(LITTLE_TO_HOST32(f.rawInode(2)->i_generation), 41U);
  f.disk.failedRead = ~uint64_t(0);
  EXPECT_EQ(Ext2HandlesTestPeer::allocate(f.fs), 2U);
  EXPECT_EQ(LITTLE_TO_HOST32(f.inode(1)->i_generation), 0xffffffffU);
  EXPECT_EQ(LITTLE_TO_HOST32(f.inode(2)->i_generation), 42U);
  EXPECT_EQ(LITTLE_TO_HOST16(f.group()->bg_free_inodes_count), 1U);
}

TEST(Ext2Handles, AllocationDoesNotExposeOldGenerationDuringPublication) {
  Fixture f;
  FileHandle oldHandle;
  {
    Ext2File old(String("old"), 1, f.inode(), &f.fs);
    ASSERT_EQ(f.fs.encodeFileHandle(old, oldHandle), Status::Success);
    ASSERT_TRUE(f.unlink(old));
  }
  f.disk.pauseInodeWrite = true;
  auto allocation =
      std::async(std::launch::async, [&] { return Ext2HandlesTestPeer::allocate(f.fs); });
  const bool entered = f.disk.waitForWrite();
  EXPECT_TRUE(entered);
  auto decode = std::async(std::launch::async, [&] {
    RetainedFile output;
    return f.fs.decodeFileHandle(oldHandle, output);
  });
  if (entered)
    EXPECT_EQ(decode.wait_for(25ms), std::future_status::timeout);
  f.disk.releaseWrite();
  EXPECT_EQ(allocation.get(), 1U);
  EXPECT_EQ(decode.get(), Status::Stale);
}

TEST(Ext2Handles, InodeMarksSurviveAllNamespaceWrapperGaps) {
  Fixture f;
  SharedPointer<FileEventObserver> observer(new Observer);
  auto* events = static_cast<Observer*>(observer.get());
  FileEventSubscription subscription;
  FileHandle handle;
  {
    Ext2File first(String("first"), 1, f.inode(), &f.fs);
    ASSERT_EQ(first.subscribeInodeEvents(FileEvents::Modify, observer, subscription),
              Status::Success);
    ASSERT_EQ(f.fs.encodeFileHandle(first, handle), Status::Success);
    Ext2File alias(String("alias"), 1, f.inode(), &f.fs);
    alias.publishEvent(FileEvents::Modify);
  }
  EXPECT_EQ(events->modified.load(), 1U);
  EXPECT_EQ(events->retired.load(), 0U);
  {
    RetainedFile reopened;
    ASSERT_EQ(f.fs.decodeFileHandle(handle, reopened), Status::Success);
    reopened.get()->publishEvent(FileEvents::Modify);
  }
  EXPECT_EQ(events->modified.load(), 2U);
  Ext2File final(String("reopened"), 1, f.inode(), &f.fs);
  ASSERT_TRUE(f.unlink(final));
  EXPECT_EQ(events->retired.load(), 1U);
  final.publishEvent(FileEvents::Modify);
  EXPECT_EQ(events->modified.load(), 2U);
}

TEST(Ext2Handles, FinalLinkClosesAdmissionBeforeCallbackDrain) {
  Fixture f;
  Ext2File file(String("file"), 1, f.inode(), &f.fs);
  SharedPointer<FileEventObserver> observer(new Observer);
  auto* events = static_cast<Observer*>(observer.get());
  events->pause = true;
  FileEventSubscription subscription;
  ASSERT_EQ(file.subscribeInodeEvents(FileEvents::Modify, observer, subscription), Status::Success);
  FileHandle handle;
  ASSERT_EQ(f.fs.encodeFileHandle(file, handle), Status::Success);
  auto publisher = std::async(std::launch::async, [&] { file.publishEvent(FileEvents::Modify); });
  const bool entered = events->waitForCallback();
  EXPECT_TRUE(entered);
  EXPECT_TRUE(Ext2HandlesTestPeer::unlink(f.fs, file));
  EXPECT_EQ(events->retired.load(), 1U);
  FileEventSubscription late;
  EXPECT_EQ(file.subscribeInodeEvents(FileEvents::Modify, observer, late), Status::Stale);
  RetainedFile rejected;
  EXPECT_EQ(f.fs.decodeFileHandle(handle, rejected), Status::Stale);
  auto drain = std::async(std::launch::async, [&] { file.finishInodeRetirement(); });
  if (entered)
    EXPECT_EQ(drain.wait_for(25ms), std::future_status::timeout);
  events->release();
  publisher.get();
  drain.get();
  EXPECT_EQ(events->modified.load(), 1U);
}

TEST(Ext2Handles, ReadFailuresAndUnsupportedKindsLeaveNoRetainedOutput) {
  Fixture f;
  Ext2File file(String("file"), 1, f.inode(), &f.fs);
  FileHandle handle;
  ASSERT_EQ(f.fs.encodeFileHandle(file, handle), Status::Success);
  Fixture imported;
  imported.disk.failedRead = 3 * BlockSize;
  RetainedFile output;
  EXPECT_EQ(imported.fs.decodeFileHandle(handle, output), Status::IoError);
  EXPECT_FALSE(output);
  imported.disk.failedRead = ~uint64_t(0);
  EXPECT_EQ(imported.fs.decodeFileHandle(handle, output), Status::Success);
  output.reset();
  imported.inode()->i_mode = HOST_TO_LITTLE16(EXT2_S_IFDIR | 0700);
  EXPECT_EQ(imported.fs.decodeFileHandle(handle, output), Status::Stale);
  EXPECT_FALSE(output);
  Ext2Directory directory(String("directory"), 1, imported.inode(), &imported.fs, nullptr);
  FileHandle unsupported;
  EXPECT_EQ(imported.fs.encodeFileHandle(directory, unsupported), Status::Unsupported);
  EXPECT_EQ(unsupported.length, 0U);
  std::fill_n(f.super()->s_uuid, 16, 0);
  EXPECT_EQ(f.fs.encodeFileHandle(file, unsupported), Status::Unsupported);
  EXPECT_EQ(f.fs.decodeFileHandle(handle, output), Status::Unsupported);
  FileSystemId id;
  EXPECT_EQ(f.fs.fileHandleFsid(id), Status::Unsupported);
  EXPECT_EQ(id.words[0] | id.words[1], 0U);
}
