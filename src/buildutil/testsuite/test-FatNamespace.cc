/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/machine/Disk.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <vector>

#include "modules/system/fat/FatFilesystem.h"
#include "modules/system/fat/FatSymlink.h"
#include <condition_variable>
#include <gtest/gtest.h>

namespace {
using namespace std::chrono_literals;
constexpr size_t Sector = 512;
class NamespaceDisk final : public Disk {
 public:
  NamespaceDisk() : bytes(256 * Sector, 0), stored(bytes) {}
  BufferView read(uint64_t location) override {
    if (location >= bytes.size() || location == failedRead)
      return BufferView();
    ++references;
    return BufferView(bytes.data() + location, Sector - location % Sector);
  }
  void write(uint64_t) override {}
  bool sync(uint64_t location, bool) override {
    const bool failure = location == failedSync;
    if (failure && !persistentFailure)
      failedSync = UINT64_MAX;
    if (failure)
      return false;
    const size_t start = location - location % Sector;
    std::copy_n(bytes.data() + start, Sector, stored.data() + start);
    return true;
  }
  bool syncAll() override {
    return true;
  }
  bool pin(uint64_t location) override {
    if (location >= bytes.size())
      return false;
    ++references;
    return true;
  }
  void unpin(uint64_t location) override {
    --references;
    std::unique_lock<std::mutex> guard(pauseLock);
    if (location == pausedRead && !readPaused) {
      readPaused = true;
      changed.notify_all();
      EXPECT_TRUE(changed.wait_for(guard, 5s, [&] { return readReleased; }));
    }
  }
  size_t getSize() const override {
    return bytes.size();
  }
  size_t getBlockSize() const override {
    return Sector;
  }
  void pauseAfterRead(uint64_t location) {
    std::lock_guard<std::mutex> guard(pauseLock);
    pausedRead = location;
    readPaused = readReleased = false;
  }
  bool waitForRead() {
    std::unique_lock<std::mutex> guard(pauseLock);
    return changed.wait_for(guard, 5s, [&] { return readPaused; });
  }
  void releaseRead() {
    std::lock_guard<std::mutex> guard(pauseLock);
    readReleased = true;
    changed.notify_all();
  }
  std::vector<uint8_t> bytes, stored;
  uint64_t failedRead = UINT64_MAX, failedSync = UINT64_MAX;
  bool persistentFailure = false;
  int references = 0;

 private:
  std::mutex pauseLock;
  std::condition_variable changed;
  uint64_t pausedRead = UINT64_MAX;
  bool readPaused = false, readReleased = false;
};

class NamespaceFilesystem final : public FatFilesystem {
 public:
  explicit NamespaceFilesystem(NamespaceDisk& disk, FatType type = FAT16) {
    m_pDisk = &disk;
    m_Type = type;
    m_BlockSize = Sector;
    m_ClusterCount = 200;
    m_DataAreaStart = 8;
    m_FatSector = 1;
    m_RootDir.sector = 4;
    m_RootDirCount = 4;
    m_Superblock.BPB_BytsPerSec = Sector;
    m_Superblock.BPB_SecPerClus = 1;
    m_Superblock.BPB_NumFATs = 1;
    m_Superblock.BPB_FATSz16 = 2;
    m_Superblock32.BPB_FATSz32 = 2;
    m_Superblock32.BPB_RootClus = 2;
    m_FreeClusterHint = 3;
    if (type == FAT32) {
      uint32_t eof = HOST_TO_LITTLE32(eofValue());
      std::memcpy(disk.bytes.data() + Sector + 8, &eof, sizeof(eof));
    }
    FatFileInfo info = {};
    m_pRoot = new FatDirectory(String("root"), type == FAT32 ? 2 : 0, this, nullptr, info);
    disk.stored = disk.bytes;
  }
  using FatFilesystem::getUnixTimestamp;
  using Filesystem::createDirectory;
  using Filesystem::createFile;
  using Filesystem::createSymlink;
  Dir record(FatDirectory* directory, size_t offset) {
    Dir* entry = getDirectoryEntry(directory->getInode(), offset);
    if (!entry)
      return Dir{};
    const Dir copy = *entry;
    delete entry;
    return copy;
  }
  uint32_t clusterEntry(uint32_t cluster) {
    return getClusterEntry(cluster);
  }
  bool mutationAvailable() {
    if (!m_FileMutationLock.tryAcquire())
      return false;
    m_FileMutationLock.release();
    return true;
  }
  uint64_t directoryLocation(FatDirectory* directory) {
    return directory->getInode() ? uint64_t(getSectorNumber(directory->getInode())) * Sector
                                 : uint64_t(m_RootDir.sector) * Sector;
  }
  uint64_t entryLocation(File* file) {
    uint32_t cluster, offset;
    if (file->isDirectory()) {
      auto* directory = static_cast<FatDirectory*>(file);
      cluster = directory->getDirCluster();
      offset = directory->getDirOffset();
    } else if (file->isSymlink()) {
      auto* symlink = static_cast<FatSymlink*>(file);
      cluster = symlink->getDirCluster();
      offset = symlink->getDirOffset();
    } else {
      auto* regular = static_cast<FatFile*>(file);
      cluster = regular->getDirCluster();
      offset = regular->getDirOffset();
    }
    return uint64_t(cluster ? getSectorNumber(cluster) : m_RootDir.sector) * Sector + offset;
  }
  Dir storedEntry(const NamespaceDisk& disk, File* file) {
    Dir entry;
    std::memcpy(&entry, disk.stored.data() + entryLocation(file), sizeof(entry));
    return entry;
  }
};

Directory::LookupStatus lookup(Directory* directory, const char* name,
                               Directory::ChildLease& lease) {
  return directory->lookupChild(HashedStringView(name), lease);
}
}  // namespace

TEST(FatNamespace, AliasLookupSerializesMaterializationWithSlotReuse) {
  for (bool withCookie : {false, true}) {
    SCOPED_TRACE(withCookie);
    NamespaceDisk disk;
    NamespaceFilesystem fs(disk);
    auto* root = static_cast<FatDirectory*>(fs.getRoot());
    ASSERT_TRUE(fs.createDirectory(StringView("parent"), 0700, root));
    Directory::ChildLease first, second, original;
    ASSERT_EQ(lookup(root, "parent", first), Directory::LookupStatus::Found);
    auto* firstDirectory = static_cast<FatDirectory*>(first.get());
    ASSERT_TRUE(fs.createFile(StringView("held"), 0600, firstDirectory));
    ASSERT_EQ(lookup(firstDirectory, "held", original), Directory::LookupStatus::Found);
    const char oldData[] = "original";
    ASSERT_EQ(original.get()->write(0, sizeof(oldData), reinterpret_cast<uintptr_t>(oldData)),
              sizeof(oldData));
    ASSERT_TRUE(original.get()->sync());
    const uint64_t oldSlot = fs.entryLocation(original.get());
    uint64_t cookie = 0, fileCookie = UINT64_MAX;
    auto captureCookie = [](void* context, const Directory::DirectoryEntryView& entry) {
      if (entry.name != StringView("held"))
        return true;
      *static_cast<uint64_t*>(context) = entry.currentCookie;
      return false;
    };
    firstDirectory->enumerate(cookie, captureCookie, &fileCookie);
    ASSERT_NE(fileCookie, UINT64_MAX);
    root->emptyCache();
    ASSERT_EQ(lookup(root, "parent", second), Directory::LookupStatus::Found);
    auto* secondDirectory = static_cast<FatDirectory*>(second.get());
    ASSERT_NE(firstDirectory, secondDirectory);
    firstDirectory->emptyCache();

    // unpin runs after the directory bytes were copied, before materialization.
    disk.pauseAfterRead(fs.directoryLocation(firstDirectory));
    Directory::ChildLease resolved;
    auto resolution = std::async(std::launch::async, [&] {
      return withCookie
                 ? firstDirectory->lookupChildAt(fileCookie, HashedStringView("held"), resolved)
                 : lookup(firstDirectory, "held", resolved);
    });
    const bool paused = disk.waitForRead();
    EXPECT_TRUE(paused);
    if (paused)
      EXPECT_FALSE(fs.mutationAvailable());
    std::promise<void> mutationStarted;
    auto started = mutationStarted.get_future();
    auto mutation = std::async(std::launch::async, [&] {
      mutationStarted.set_value();
      return fs.remove(StringView("held"), secondDirectory) &&
             fs.createFile(StringView("next"), 0600, secondDirectory);
    });
    started.wait();
    if (paused)
      EXPECT_EQ(mutation.wait_for(25ms), std::future_status::timeout);
    disk.releaseRead();
    EXPECT_EQ(resolution.get(), Directory::LookupStatus::Found);
    EXPECT_TRUE(mutation.get());
    ASSERT_NE(resolved.get(), nullptr);

    Directory::ChildLease replacement;
    ASSERT_EQ(lookup(secondDirectory, "next", replacement), Directory::LookupStatus::Found);
    EXPECT_EQ(fs.entryLocation(replacement.get()), oldSlot);
    const char newData[] = "replacement";
    ASSERT_EQ(replacement.get()->write(0, sizeof(newData), reinterpret_cast<uintptr_t>(newData)),
              sizeof(newData));
    ASSERT_TRUE(replacement.get()->sync());
    const char changed = '!';
    ASSERT_EQ(resolved.get()->write(0, 1, reinterpret_cast<uintptr_t>(&changed)), 1U);
    ASSERT_TRUE(resolved.get()->sync());
    std::array<char, sizeof(newData)> read{};
    ASSERT_EQ(replacement.get()->read(0, read.size(), reinterpret_cast<uintptr_t>(read.data())),
              read.size());
    EXPECT_EQ(std::memcmp(read.data(), newData, read.size()), 0);
    EXPECT_EQ(resolved.get()->getSize(), sizeof(oldData));
    firstDirectory->emptyCache();
    Directory::ChildLease absent;
    EXPECT_EQ(lookup(firstDirectory, "held", absent), Directory::LookupStatus::NotFound);
    EXPECT_EQ(disk.references, 0);
  }
}

TEST(FatNamespace, RenamesAcrossLfnLengthsAndRetainsWritableIdentity) {
  NamespaceDisk disk;
  NamespaceFilesystem fs(disk);
  auto* root = static_cast<FatDirectory*>(fs.getRoot());
  ASSERT_TRUE(fs.createFile(StringView("short"), 0600, root));
  Directory::ChildLease source;
  ASSERT_EQ(lookup(root, "short", source), Directory::LookupStatus::Found);
  const char payload[] = "new EFI payload";
  ASSERT_EQ(source.get()->write(0, sizeof(payload), reinterpret_cast<uintptr_t>(payload)),
            sizeof(payload));
  ASSERT_TRUE(source.get()->sync());
  ASSERT_TRUE(
      fs.rename(StringView("short"), root, StringView("much-longer-boot-loader-name.efi"), root));
  EXPECT_EQ(source.get()->getName(), String("much-longer-boot-loader-name.efi"));
  const char extra[] = "!";
  ASSERT_EQ(source.get()->write(sizeof(payload), sizeof(extra), reinterpret_cast<uintptr_t>(extra)),
            sizeof(extra));
  ASSERT_TRUE(source.get()->sync());
  root->emptyCache();
  Directory::ChildLease renamed, absent;
  EXPECT_EQ(lookup(root, "short", absent), Directory::LookupStatus::NotFound);
  ASSERT_EQ(lookup(root, "much-longer-boot-loader-name.efi", renamed),
            Directory::LookupStatus::Found);
  EXPECT_EQ(renamed.get()->getSize(), sizeof(payload) + sizeof(extra));
  std::array<char, sizeof(payload)> read{};
  ASSERT_EQ(renamed.get()->read(0, read.size(), reinterpret_cast<uintptr_t>(read.data())),
            read.size());
  EXPECT_EQ(std::memcmp(read.data(), payload, read.size()), 0);
  EXPECT_EQ(disk.references, 0);
}

TEST(FatNamespace, ReplacementRetainsOpenVictimAndMovesSourceAcrossDirectories) {
  NamespaceDisk disk;
  NamespaceFilesystem fs(disk);
  auto* root = static_cast<FatDirectory*>(fs.getRoot());
  ASSERT_TRUE(fs.createDirectory(StringView("EFI"), 0700, root));
  ASSERT_TRUE(fs.createFile(StringView("staged"), 0600, root));
  ASSERT_TRUE(fs.createFile(StringView("EFI/loader.efi"), 0600, root));
  Directory::ChildLease source, parent, victim;
  ASSERT_EQ(lookup(root, "staged", source), Directory::LookupStatus::Found);
  ASSERT_EQ(lookup(root, "EFI", parent), Directory::LookupStatus::Found);
  auto* directory = static_cast<FatDirectory*>(parent.get());
  ASSERT_EQ(lookup(directory, "loader.efi", victim), Directory::LookupStatus::Found);
  const char oldData[] = "old", newData[] = "replacement";
  ASSERT_EQ(victim.get()->write(0, sizeof(oldData), reinterpret_cast<uintptr_t>(oldData)),
            sizeof(oldData));
  ASSERT_TRUE(victim.get()->sync());
  ASSERT_EQ(source.get()->write(0, sizeof(newData), reinterpret_cast<uintptr_t>(newData)),
            sizeof(newData));
  ASSERT_TRUE(source.get()->sync());
  const uint32_t victimCluster = victim.get()->getInode();
  ASSERT_TRUE(fs.rename(StringView("staged"), root, StringView("EFI/loader.efi"), root));
  EXPECT_NE(fs.clusterEntry(victimCluster), 0U);
  EXPECT_EQ(source.get()->getParent(), directory);
  std::array<char, sizeof(oldData)> read{};
  ASSERT_EQ(victim.get()->read(0, read.size(), reinterpret_cast<uintptr_t>(read.data())),
            read.size());
  EXPECT_EQ(std::memcmp(read.data(), oldData, read.size()), 0);
  ASSERT_EQ(victim.get()->write(0, sizeof(oldData), reinterpret_cast<uintptr_t>(oldData)),
            sizeof(oldData));
  ASSERT_TRUE(victim.get()->sync());
  directory->emptyCache();
  Directory::ChildLease replacement;
  ASSERT_EQ(lookup(directory, "loader.efi", replacement), Directory::LookupStatus::Found);
  EXPECT_EQ(replacement.get()->getInode(), source.get()->getInode());
  EXPECT_EQ(replacement.get()->getSize(), sizeof(newData));
  victim.reset();
  EXPECT_EQ(fs.clusterEntry(victimCluster), 0U);
}

TEST(FatNamespace, DirectoryMoveReplacesEmptyDirectoryAndUpdatesDotDot) {
  for (FatType type : {FAT16, FAT32}) {
    NamespaceDisk disk;
    NamespaceFilesystem fs(disk, type);
    auto* root = static_cast<FatDirectory*>(fs.getRoot());
    ASSERT_TRUE(fs.createDirectory(StringView("from"), 0700, root));
    ASSERT_TRUE(fs.createDirectory(StringView("to"), 0700, root));
    ASSERT_TRUE(fs.createDirectory(StringView("to/current"), 0700, root));
    ASSERT_TRUE(fs.createFile(StringView("from/boot.efi"), 0600, root));
    Directory::ChildLease source, destination, victim;
    ASSERT_EQ(lookup(root, "from", source), Directory::LookupStatus::Found);
    ASSERT_EQ(lookup(root, "to", destination), Directory::LookupStatus::Found);
    auto* parent = static_cast<FatDirectory*>(destination.get());
    ASSERT_EQ(lookup(parent, "current", victim), Directory::LookupStatus::Found);
    ASSERT_TRUE(fs.rename(StringView("from"), root, StringView("to/current"), root));
    EXPECT_TRUE(static_cast<FatDirectory*>(victim.get())->isDetached());
    const Dir dotdot = fs.record(static_cast<FatDirectory*>(source.get()), sizeof(Dir));
    EXPECT_EQ(LITTLE_TO_HOST16(dotdot.DIR_FstClusLO), parent->getInode());
    ASSERT_NE(fs.find(StringView("to/current/boot.efi"), root), nullptr);
    ASSERT_TRUE(fs.rename(StringView("to/current"), root, StringView("back"), root));
    const Dir rootParent = fs.record(static_cast<FatDirectory*>(source.get()), sizeof(Dir));
    EXPECT_EQ(LITTLE_TO_HOST16(rootParent.DIR_FstClusLO), 0U);
  }
}

TEST(FatNamespace, NonregularAliasSurvivesUnlinkUntilItsLastReferenceCloses) {
  for (bool symlink : {false, true}) {
    NamespaceDisk disk;
    NamespaceFilesystem fs(disk);
    auto* root = static_cast<FatDirectory*>(fs.getRoot());
    const char target[] = "destination";
    if (symlink)
      ASSERT_TRUE(fs.createSymlink(StringView("held"), String(target), root));
    else
      ASSERT_TRUE(fs.createDirectory(StringView("held"), 0700, root));
    Directory::ChildLease survivor, current;
    ASSERT_EQ(lookup(root, "held", survivor), Directory::LookupStatus::Found);
    const uint32_t cluster = survivor.get()->getInode();
    root->emptyCache();
    ASSERT_EQ(lookup(root, "held", current), Directory::LookupStatus::Found);
    ASSERT_NE(current.get(), survivor.get());
    ASSERT_TRUE(fs.remove(StringView("held"), root));
    current.reset();
    EXPECT_NE(fs.clusterEntry(cluster), 0U);
    if (symlink) {
      std::array<char, sizeof(target) - 1> read{};
      ASSERT_EQ(survivor.get()->read(0, read.size(), reinterpret_cast<uintptr_t>(read.data())),
                read.size());
      EXPECT_EQ(std::memcmp(read.data(), target, read.size()), 0);
    } else {
      EXPECT_TRUE(static_cast<FatDirectory*>(survivor.get())->isDetached());
      EXPECT_FALSE(fs.createFile(StringView("after-unlink"), 0600, survivor.get()));
    }
    survivor.reset();
    EXPECT_EQ(fs.clusterEntry(cluster), 0U);
  }
}

TEST(FatShutdown, FailedNonregularOrphanReclamationPreventsCleanCompletion) {
  for (bool symlink : {false, true}) {
    NamespaceDisk disk;
    NamespaceFilesystem fs(disk);
    auto* root = static_cast<FatDirectory*>(fs.getRoot());
    if (symlink)
      ASSERT_TRUE(fs.createSymlink(StringView("held"), String("destination"), root));
    else
      ASSERT_TRUE(fs.createDirectory(StringView("held"), 0700, root));
    Directory::ChildLease survivor;
    ASSERT_EQ(lookup(root, "held", survivor), Directory::LookupStatus::Found);
    ASSERT_TRUE(fs.remove(StringView("held"), root));
    disk.failedSync = Sector;
    disk.persistentFailure = true;
    survivor.reset();
    disk.failedSync = UINT64_MAX;
    EXPECT_EQ(fs.shutdown(), Filesystem::SyncStatus::IoError);
  }
}

TEST(FatNamespace, RejectsNonemptyReplacementAndDescendantMove) {
  NamespaceDisk disk;
  NamespaceFilesystem fs(disk);
  auto* root = static_cast<FatDirectory*>(fs.getRoot());
  ASSERT_TRUE(fs.createDirectory(StringView("source"), 0700, root));
  ASSERT_TRUE(fs.createDirectory(StringView("target"), 0700, root));
  ASSERT_TRUE(fs.createDirectory(StringView("source/child"), 0700, root));
  ASSERT_TRUE(fs.createFile(StringView("target/keep"), 0600, root));
  const auto before = disk.stored;
  EXPECT_FALSE(fs.rename(StringView("source"), root, StringView("target"), root));
  EXPECT_FALSE(fs.rename(StringView("source"), root, StringView("source/child/loop"), root));
  EXPECT_EQ(disk.stored, before);
}

TEST(FatNamespace, LongNamesSpanClustersAndShortAliasesRemainUnique) {
  NamespaceDisk disk;
  NamespaceFilesystem fs(disk, FAT32);
  auto* root = static_cast<FatDirectory*>(fs.getRoot());
  const std::string longName(255, 'x');
  ASSERT_TRUE(fs.createFile(StringView(longName.c_str()), 0600, root));
  ASSERT_TRUE(fs.createFile(StringView("collision-number-one.efi"), 0600, root));
  ASSERT_TRUE(fs.createFile(StringView("collision-number-two.efi"), 0600, root));
  Directory::ChildLease longFile, first, second;
  ASSERT_EQ(lookup(root, longName.c_str(), longFile), Directory::LookupStatus::Found);
  ASSERT_EQ(lookup(root, "collision-number-one.efi", first), Directory::LookupStatus::Found);
  ASSERT_EQ(lookup(root, "collision-number-two.efi", second), Directory::LookupStatus::Found);
  auto* one = static_cast<FatFile*>(first.get());
  auto* two = static_cast<FatFile*>(second.get());
  const size_t oneLocation = (8 + one->getDirCluster() - 2) * Sector + one->getDirOffset();
  const size_t twoLocation = (8 + two->getDirCluster() - 2) * Sector + two->getDirOffset();
  EXPECT_NE(std::memcmp(disk.stored.data() + oneLocation, disk.stored.data() + twoLocation, 11), 0);
  ASSERT_TRUE(fs.rename(StringView(longName.c_str()), root, StringView("renamed"), root));
  ASSERT_TRUE(fs.remove(StringView("renamed"), root));
  root->emptyCache();
  Directory::ChildLease absent;
  EXPECT_EQ(lookup(root, "renamed", absent), Directory::LookupStatus::NotFound);
  EXPECT_EQ(lookup(root, longName.c_str(), absent), Directory::LookupStatus::NotFound);
}

TEST(FatNamespace, RenameWriteFailureRestoresDiskAndCachedNames) {
  NamespaceDisk disk;
  NamespaceFilesystem fs(disk);
  auto* root = static_cast<FatDirectory*>(fs.getRoot());
  ASSERT_TRUE(fs.createDirectory(StringView("target"), 0700, root));
  ASSERT_TRUE(fs.createFile(StringView("source"), 0600, root));
  Directory::ChildLease parent, source;
  ASSERT_EQ(lookup(root, "target", parent), Directory::LookupStatus::Found);
  ASSERT_EQ(lookup(root, "source", source), Directory::LookupStatus::Found);
  const auto before = disk.stored;
  disk.failedSync = fs.directoryLocation(root);
  EXPECT_FALSE(fs.rename(StringView("source"), root, StringView("target/renamed"), root));
  EXPECT_EQ(disk.stored, before);
  EXPECT_FALSE(fs.isReadOnly());
  EXPECT_EQ(source.get()->getName(), String("source"));
  Directory::ChildLease absent;
  EXPECT_EQ(lookup(static_cast<FatDirectory*>(parent.get()), "renamed", absent),
            Directory::LookupStatus::NotFound);
  ASSERT_TRUE(fs.rename(StringView("source"), root, StringView("target/renamed"), root));
}

TEST(FatNamespace, FailedRollbackFencesFurtherNamespaceWrites) {
  NamespaceDisk disk;
  NamespaceFilesystem fs(disk);
  auto* root = static_cast<FatDirectory*>(fs.getRoot());
  ASSERT_TRUE(fs.createFile(StringView("source"), 0600, root));
  disk.failedSync = fs.directoryLocation(root);
  disk.persistentFailure = true;
  EXPECT_FALSE(fs.rename(StringView("source"), root, StringView("renamed"), root));
  EXPECT_TRUE(fs.isReadOnly());
  disk.failedSync = UINT64_MAX;
  EXPECT_FALSE(fs.createFile(StringView("another"), 0600, root));
  EXPECT_EQ(fs.sync(), Filesystem::SyncStatus::IoError);
}

TEST(FatNamespace, DirectoryBirthtimeRetainsPost2106SecondsAndValidOddSecond) {
  for (uint8_t tenths : {uint8_t(100), uint8_t(199), uint8_t(200)}) {
    NamespaceDisk disk;
    NamespaceFilesystem fs(disk);
    auto* root = static_cast<FatDirectory*>(fs.getRoot());
    ASSERT_TRUE(fs.createDirectory(StringView("dated"), 0700, root));
    Directory::ChildLease initial;
    ASSERT_EQ(lookup(root, "dated", initial), Directory::LookupStatus::Found);
    auto* directory = static_cast<FatDirectory*>(initial.get());
    const size_t location = 4 * Sector + directory->getDirOffset();
    Dir* entry = reinterpret_cast<Dir*>(disk.bytes.data() + location);
    entry->DIR_CrtDate = HOST_TO_LITTLE16((127U << 9) | (12U << 5) | 31U);
    entry->DIR_CrtTime = HOST_TO_LITTLE16((23U << 11) | (59U << 5) | 29U);
    entry->DIR_CrtTimeTenth = tenths;
    disk.stored = disk.bytes;
    initial.reset();
    root->emptyCache();
    Directory::ChildLease reloaded;
    ASSERT_EQ(lookup(root, "dated", reloaded), Directory::LookupStatus::Found);
    EXPECT_EQ(reloaded.get()->getAttributes().changed, 4354819198ULL + (tenths < 200));
  }
}

TEST(FatNamespace, FailedNodeTimesRetryAfterCloseAndFollowMoveButNotReusedSlots) {
  constexpr Time::Timestamp day = 1789171200;
  for (bool symlink : {false, true}) {
    NamespaceDisk disk;
    NamespaceFilesystem fs(disk);
    auto* root = static_cast<FatDirectory*>(fs.getRoot());
    if (symlink)
      ASSERT_TRUE(fs.createSymlink(StringView("source"), String("target"), root));
    else
      ASSERT_TRUE(fs.createDirectory(StringView("source"), 0700, root));
    ASSERT_TRUE(fs.createDirectory(StringView("dest"), 0700, root));
    Directory::ChildLease source, destination;
    ASSERT_EQ(lookup(root, "source", source), Directory::LookupStatus::Found);
    ASSERT_EQ(lookup(root, "dest", destination), Directory::LookupStatus::Found);
    source.get()->setTimes(day, day + 200, true, true);
    const Dir original = fs.storedEntry(disk, source.get());
    disk.failedSync = fs.directoryLocation(root);
    source.get()->setTimes(day + 86400, day + 444, true, true);
    EXPECT_EQ(fs.storedEntry(disk, source.get()).DIR_WrtTime, original.DIR_WrtTime);
    source.reset();
    root->emptyCache();
    ASSERT_EQ(fs.sync(), Filesystem::SyncStatus::Success);
    ASSERT_EQ(lookup(root, "source", source), Directory::LookupStatus::Found);
    Dir persisted = fs.storedEntry(disk, source.get());
    EXPECT_EQ(fs.getUnixTimestamp(persisted.DIR_WrtTime, persisted.DIR_WrtDate), day + 444);
    EXPECT_EQ(fs.getUnixTimestamp(0, persisted.DIR_LstAccDate), day + 86400);
    EXPECT_EQ(persisted.DIR_FileSize, original.DIR_FileSize);
    EXPECT_EQ(persisted.DIR_FstClusLO, original.DIR_FstClusLO);
    EXPECT_EQ(persisted.DIR_FstClusHI, original.DIR_FstClusHI);
    EXPECT_EQ(persisted.DIR_CrtDate, original.DIR_CrtDate);
    EXPECT_EQ(persisted.DIR_CrtTime, original.DIR_CrtTime);
    EXPECT_EQ(persisted.DIR_CrtTimeTenth, original.DIR_CrtTimeTenth);

    const uint64_t oldSlot = fs.entryLocation(source.get());
    disk.failedRead = fs.directoryLocation(root);
    source.get()->setTimes(day + 2 * 86400, day + 888, true, true);
    disk.failedRead = UINT64_MAX;
    ASSERT_TRUE(fs.rename(StringView("source"), root, StringView("dest/moved"), root));
    ASSERT_TRUE(fs.createFile(StringView("reused"), 0600, root));
    Directory::ChildLease replacement;
    ASSERT_EQ(lookup(root, "reused", replacement), Directory::LookupStatus::Found);
    ASSERT_EQ(fs.entryLocation(replacement.get()), oldSlot);
    replacement.get()->setTimes(day, day + 3000, true, true);
    ASSERT_TRUE(replacement.get()->sync());
    const Dir reused = fs.storedEntry(disk, replacement.get());
    ASSERT_EQ(fs.sync(), Filesystem::SyncStatus::Success);
    persisted = fs.storedEntry(disk, source.get());
    EXPECT_EQ(fs.getUnixTimestamp(persisted.DIR_WrtTime, persisted.DIR_WrtDate), day + 888);
    EXPECT_EQ(fs.getUnixTimestamp(0, persisted.DIR_LstAccDate), day + 2 * 86400);
    const Dir reusedAfterSync = fs.storedEntry(disk, replacement.get());
    EXPECT_EQ(std::memcmp(&reused, &reusedAfterSync, sizeof(Dir)), 0);

    auto* parent = static_cast<FatDirectory*>(source.get()->getParent());
    const uint64_t movedSlot = fs.entryLocation(source.get());
    disk.failedRead = fs.directoryLocation(parent);
    source.get()->setTimes(day + 3 * 86400, day + 1000, true, true);
    disk.failedRead = UINT64_MAX;
    ASSERT_TRUE(fs.remove(StringView("dest/moved"), root));
    ASSERT_TRUE(fs.createFile(StringView("dest/reused"), 0600, root));
    Directory::ChildLease afterUnlink;
    ASSERT_EQ(lookup(parent, "reused", afterUnlink), Directory::LookupStatus::Found);
    ASSERT_EQ(fs.entryLocation(afterUnlink.get()), movedSlot);
    afterUnlink.get()->setTimes(day, day + 4000, true, true);
    ASSERT_TRUE(afterUnlink.get()->sync());
    const Dir unlinkReplacement = fs.storedEntry(disk, afterUnlink.get());
    ASSERT_EQ(fs.sync(), Filesystem::SyncStatus::Success);
    const Dir unlinkAfterSync = fs.storedEntry(disk, afterUnlink.get());
    EXPECT_EQ(std::memcmp(&unlinkReplacement, &unlinkAfterSync, sizeof(Dir)), 0);
  }
}

TEST(FatNamespace, RegularTimesOnlyWriteWhenFatTimestampPrecisionChanges) {
  constexpr Time::Timestamp day = 1789171200;
  NamespaceDisk disk;
  NamespaceFilesystem fs(disk);
  auto* root = static_cast<FatDirectory*>(fs.getRoot());
  ASSERT_TRUE(fs.createFile(StringView("times"), 0600, root));
  Directory::ChildLease source, alias;
  ASSERT_EQ(lookup(root, "times", source), Directory::LookupStatus::Found);
  source.get()->setTimes(day + 60, day + 100, true, true);
  ASSERT_TRUE(source.get()->sync());
  root->emptyCache();
  ASSERT_EQ(lookup(root, "times", alias), Directory::LookupStatus::Found);
  disk.failedSync = fs.directoryLocation(root);
  source.get()->setTimes(day + 1000, day + 101, true, true);
  EXPECT_EQ(alias.get()->getAccessedTime(), day + 1000);
  EXPECT_EQ(alias.get()->getModifiedTime(), day + 101);
  EXPECT_TRUE(source.get()->sync());
  EXPECT_EQ(disk.failedSync, fs.directoryLocation(root));

  source.get()->setAccessedTime(day + 86400);
  EXPECT_FALSE(source.get()->sync());
  EXPECT_EQ(disk.failedSync, UINT64_MAX);
  source.get()->setTimes(day + 86400 + 60, day + 101, true, true);
  ASSERT_TRUE(source.get()->sync());
  const Dir accessChanged = fs.storedEntry(disk, source.get());
  EXPECT_EQ(fs.getUnixTimestamp(0, accessChanged.DIR_LstAccDate), day + 86400);

  disk.failedSync = fs.directoryLocation(root);
  source.get()->setModifiedTime(day + 102);
  EXPECT_FALSE(source.get()->sync());
  ASSERT_TRUE(source.get()->sync());
  const Dir modifyChanged = fs.storedEntry(disk, source.get());
  EXPECT_EQ(fs.getUnixTimestamp(modifyChanged.DIR_WrtTime, modifyChanged.DIR_WrtDate), day + 102);
}
