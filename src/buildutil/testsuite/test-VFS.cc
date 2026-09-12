/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "pedigree/kernel/machine/Disk.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "modules/system/ramfs/RamFs.h"
#include "modules/system/vfs/Symlink.h"
#include "modules/system/vfs/VFS.h"
#include <condition_variable>
#include <gtest/gtest.h>

namespace {
class StartGate {
 public:
  explicit StartGate(size_t participants) : m_Participants(participants) {}

  void arriveAndWait() {
    std::unique_lock<std::mutex> lock(m_Mutex);
    ++m_Arrived;
    if (m_Arrived == m_Participants) {
      m_Open = true;
      m_Condition.notify_all();
    } else {
      m_Condition.wait(lock, [this] { return m_Open; });
    }
  }

 private:
  const size_t m_Participants;
  size_t m_Arrived = 0;
  bool m_Open = false;
  std::mutex m_Mutex;
  std::condition_variable m_Condition;
};

class MountTestDisk final : public Disk {
 public:
  void getName(String& name) override {
    name.assign("test-disk");
  }

  BufferView read(uint64_t) override {
    return BufferView();
  }

  size_t getSize() const override {
    return 0;
  }

  size_t getBlockSize() const override {
    return 512;
  }

  bool pin(uint64_t) override {
    return false;
  }

  void unpin(uint64_t) override {}
};

class MountTestFilesystem final : public Filesystem {
 public:
  std::function<SyncStatus()> syncAction;

  SyncStatus sync() override {
    return syncAction ? syncAction() : Filesystem::sync();
  }

  explicit MountTestFilesystem(const String& label, VFS* registry = nullptr,
                               std::atomic<size_t>* destructions = nullptr,
                               std::atomic<size_t>* reentries = nullptr,
                               std::atomic<size_t>* reentryMounts = nullptr)
      : m_Label(label),
        m_Registry(registry),
        m_Destructions(destructions),
        m_Reentries(reentries),
        m_ReentryMounts(reentryMounts) {}

  ~MountTestFilesystem() override {
    if (m_Registry) {
      Vector<VFS::MountSnapshot> mounts;
      m_Registry->getMounts(mounts);
      m_Reentries->fetch_add(1);
      m_ReentryMounts->fetch_add(mounts.count());
    }
    if (m_Destructions) {
      m_Destructions->fetch_add(1);
    }
  }

  bool initialise(Disk* disk) override {
    m_pDisk = disk;
    return true;
  }

  File* getRoot() const override {
    return nullptr;
  }

  const String& getVolumeLabel() const override {
    return m_Label;
  }

 protected:
  bool createFile(File*, const String&, uint32_t) override {
    return false;
  }

  bool createDirectory(File*, const String&, uint32_t) override {
    return false;
  }

  bool createSymlink(File*, const String&, const String&) override {
    return false;
  }

  bool removeNode(File*, const String&, File*) override {
    return false;
  }

 private:
  String m_Label;
  VFS* m_Registry;
  std::atomic<size_t>* m_Destructions;
  std::atomic<size_t>* m_Reentries;
  std::atomic<size_t>* m_ReentryMounts;
};

class PausedRenameFilesystem final : public RamFs {
 public:
  bool waitForBackend() {
    std::unique_lock<std::mutex> lock(m_Mutex);
    return m_Condition.wait_for(lock, std::chrono::seconds(1), [this] { return m_Entered; });
  }

  void releaseBackend(bool commit) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Commit = commit;
    m_Released = true;
    m_Condition.notify_all();
  }

 protected:
  bool renameNode(Directory*, const String&, File*, Directory*, const String&, File*) override {
    std::unique_lock<std::mutex> lock(m_Mutex);
    m_Entered = true;
    m_Condition.notify_all();
    m_Condition.wait(lock, [this] { return m_Released; });
    return m_Commit;
  }

 private:
  std::mutex m_Mutex;
  std::condition_variable m_Condition;
  bool m_Entered = false;
  bool m_Released = false;
  bool m_Commit = false;
};

class SparseTestFile final : public File {
 public:
  SparseTestFile(const String& name, File* parent, std::atomic<size_t>& destructions)
      : File(name, 0, 0, 0, 1, nullptr, 0, parent), m_Destructions(destructions) {}

  ~SparseTestFile() override {
    m_Destructions.fetch_add(1);
  }

 private:
  std::atomic<size_t>& m_Destructions;
};

class SparseTestDirectory final : public Directory {
 public:
  enum class ResolveMode { Immediate, Blocked, Concurrent };

  explicit SparseTestDirectory(ResolveMode mode = ResolveMode::Immediate)
      : Directory(String("sparse"), 0, 0, 0, 1, nullptr, 0, nullptr),
        m_Mode(mode),
        m_Release(false),
        m_ConcurrentResolvers(0),
        m_ConcurrentArrivals(0),
        m_MaxConcurrentResolvers(0),
        m_ResolveCalls(0),
        m_Constructions(0),
        m_Destructions(0) {}

  ~SparseTestDirectory() override {
    emptyCache();
  }

  bool waitForResolver() {
    std::unique_lock<std::mutex> lock(m_Mutex);
    return m_Condition.wait_for(lock, std::chrono::seconds(1),
                                [this] { return m_ResolveCalls.load() != 0; });
  }

  void releaseResolver() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Release = true;
    m_Condition.notify_all();
  }

  bool waitForLookupUsers(size_t users) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
      if (inFlightUsersForHostedTest(HashedStringView("shared")) >= users ||
          inFlightUsersForHostedTest(HashedStringView("io-error")) >= users) {
        return true;
      }
      std::this_thread::yield();
    }
    return false;
  }

  bool publishResident(File* file) {
    return addDirectoryEntry(file->getName(), file);
  }

  bool removeResident(File* file) {
    return removeDirectoryEntry(HashedStringView(file->getName()), file);
  }

  void detach() {
    markDetached();
  }

  size_t resolveCalls() const {
    return m_ResolveCalls.load();
  }

  size_t constructions() const {
    return m_Constructions.load();
  }

  size_t destructions() const {
    return m_Destructions.load();
  }

  size_t maxConcurrentResolvers() const {
    return m_MaxConcurrentResolvers.load();
  }

 protected:
  LookupStatus resolveChild(const StringView& name, File*& child) override {
    child = nullptr;
    m_ResolveCalls.fetch_add(1);

    if (m_Mode == ResolveMode::Blocked) {
      std::unique_lock<std::mutex> lock(m_Mutex);
      m_Condition.notify_all();
      m_Condition.wait(lock, [this] { return m_Release; });
    } else if (m_Mode == ResolveMode::Concurrent) {
      const size_t concurrent = m_ConcurrentResolvers.fetch_add(1) + 1;
      m_ConcurrentArrivals.fetch_add(1);
      size_t maximum = m_MaxConcurrentResolvers.load();
      while (maximum < concurrent &&
             !m_MaxConcurrentResolvers.compare_exchange_weak(maximum, concurrent)) {
      }

      std::unique_lock<std::mutex> lock(m_Mutex);
      m_Condition.notify_all();
      const bool met = m_Condition.wait_for(lock, std::chrono::seconds(1),
                                            [this] { return m_ConcurrentArrivals.load() >= 2; });
      m_ConcurrentResolvers.fetch_sub(1);
      m_Condition.notify_all();
      if (!met) {
        return LookupStatus::IoError;
      }
    }

    if (name == "missing") {
      return LookupStatus::NotFound;
    }
    if (name == "io-error") {
      return LookupStatus::IoError;
    }
    child = new SparseTestFile(name.toString(), this, m_Destructions);
    m_Constructions.fetch_add(1);
    return LookupStatus::Found;
  }

  ReadStatus readDirectory(uint64_t& cookie, DirectoryEntryEmitter emitter,
                           void* context) override {
    static const char* names[] = {"alpha", "overlay", "beta"};
    while (cookie < 3) {
      const uint64_t current = cookie;
      const uint64_t next = current + 1;
      DirectoryEntryView entry = {StringView(names[current]), current + 10, EntryType::Regular,
                                  current, next};
      if (!emitter(context, entry)) {
        return ReadStatus::Stopped;
      }
      cookie = next;
    }
    return ReadStatus::Complete;
  }

 private:
  ResolveMode m_Mode;
  std::mutex m_Mutex;
  std::condition_variable m_Condition;
  bool m_Release;
  std::atomic<size_t> m_ConcurrentResolvers;
  std::atomic<size_t> m_ConcurrentArrivals;
  std::atomic<size_t> m_MaxConcurrentResolvers;
  std::atomic<size_t> m_ResolveCalls;
  std::atomic<size_t> m_Constructions;
  std::atomic<size_t> m_Destructions;
};

class SparseMutationDirectory final : public Directory {
 public:
  explicit SparseMutationDirectory(Filesystem* filesystem)
      : Directory(String(""), 0, 0, 0, 1, filesystem, 0, nullptr),
        m_ResolveCalls(0),
        m_Constructions(0),
        m_Destructions(0) {}

  ~SparseMutationDirectory() override {
    emptyCache();
  }

  void addBacking(const char* name) {
    m_Backing.emplace_back(name);
  }

  bool removeBacking(const String& name) {
    for (auto it = m_Backing.begin(); it != m_Backing.end(); ++it) {
      if (name == it->c_str()) {
        m_Removed.emplace_back(*it);
        m_Backing.erase(it);
        invalidateDirectoryEntry(HashedStringView(name));
        return true;
      }
    }
    return false;
  }

  size_t backingCount() const {
    return m_Backing.size();
  }

  size_t removedCount() const {
    return m_Removed.size();
  }

  size_t resolveCalls() const {
    return m_ResolveCalls.load();
  }

  size_t constructions() const {
    return m_Constructions.load();
  }

  size_t destructions() const {
    return m_Destructions.load();
  }

 protected:
  LookupStatus resolveChild(const StringView& name, File*& child) override {
    child = nullptr;
    m_ResolveCalls.fetch_add(1);
    if (name == "io-error") {
      return LookupStatus::IoError;
    }

    for (const auto& backingName : m_Backing) {
      if (name == backingName.c_str()) {
        const String vnodeName = name == "alias" ? String("shared-inode-name") : name.toString();
        child = new SparseTestFile(vnodeName, this, m_Destructions);
        m_Constructions.fetch_add(1);
        return LookupStatus::Found;
      }
    }
    return LookupStatus::NotFound;
  }

  ReadStatus readDirectory(uint64_t& cookie, DirectoryEntryEmitter emitter,
                           void* context) override {
    while (cookie < m_Backing.size()) {
      const uint64_t current = cookie;
      const uint64_t next = current + 1;
      DirectoryEntryView entry = {StringView(m_Backing[current].c_str()), current + 100,
                                  EntryType::Regular, current, next};
      if (!emitter(context, entry)) {
        return ReadStatus::Stopped;
      }
      cookie = next;
    }
    return ReadStatus::Complete;
  }

 private:
  std::vector<std::string> m_Backing;
  std::vector<std::string> m_Removed;
  std::atomic<size_t> m_ResolveCalls;
  std::atomic<size_t> m_Constructions;
  std::atomic<size_t> m_Destructions;
};

class SparseMutationFilesystem final : public Filesystem {
 public:
  SparseMutationFilesystem()
      : m_Root(new SparseMutationDirectory(this)), m_CreateCalls(0), m_RemoveCalls(0) {}

  ~SparseMutationFilesystem() override {
    delete m_Root;
  }

  bool initialise(Disk*) override {
    return true;
  }

  File* getRoot() const override {
    return m_Root;
  }

  const String& getVolumeLabel() const override {
    return m_Label;
  }

  SparseMutationDirectory* root() const {
    return m_Root;
  }

  size_t createCalls() const {
    return m_CreateCalls;
  }

  size_t removeCalls() const {
    return m_RemoveCalls;
  }

 protected:
  bool createFile(File*, const String&, uint32_t) override {
    ++m_CreateCalls;
    return true;
  }

  bool createDirectory(File*, const String&, uint32_t) override {
    return false;
  }

  bool createSymlink(File*, const String&, const String&) override {
    return false;
  }

  bool removeNode(File* parent, const String& filename, File* file) override {
    ++m_RemoveCalls;
    return parent == m_Root && m_Root->removeBacking(filename);
  }

 private:
  SparseMutationDirectory* m_Root;
  String m_Label = String("sparse-mutation");
  size_t m_CreateCalls;
  size_t m_RemoveCalls;
};

class UnmaterializableDirectory final : public Directory {
 public:
  UnmaterializableDirectory()
      : Directory(String("unmaterializable"), 0, 0, 0, 1, nullptr, 0, nullptr) {}

 protected:
  LookupStatus resolveChild(const StringView&, File*& child) override {
    child = nullptr;
    return LookupStatus::NotFound;
  }

  ReadStatus readDirectory(uint64_t& cookie, DirectoryEntryEmitter emitter,
                           void* context) override {
    if (cookie) {
      return ReadStatus::Complete;
    }
    DirectoryEntryView entry = {StringView("device"), 42, EntryType::CharacterDevice, 0, 1};
    if (!emitter(context, entry)) {
      return ReadStatus::Stopped;
    }
    cookie = 1;
    return ReadStatus::Complete;
  }
};

class LifetimeTestDirectory final : public Directory {
 public:
  LifetimeTestDirectory(const String& name, Filesystem* filesystem, File* parent,
                        std::atomic<size_t>& destructions)
      : Directory(name, 0, 0, 0, 20, filesystem, 0, parent), m_Destructions(destructions) {}

  ~LifetimeTestDirectory() override {
    m_Destructions.fetch_add(1);
  }

 protected:
  LookupStatus resolveChild(const StringView&, File*& child) override {
    child = nullptr;
    return LookupStatus::NotFound;
  }

  ReadStatus readDirectory(uint64_t&, DirectoryEntryEmitter, void*) override {
    return ReadStatus::Complete;
  }

 private:
  std::atomic<size_t>& m_Destructions;
};

class LifetimeTestFile final : public File {
 public:
  LifetimeTestFile(const String& name, Filesystem* filesystem, File* parent,
                   std::atomic<size_t>& destructions)
      : File(name, 0, 0, 0, 21, filesystem, 0, parent), m_Destructions(destructions) {}

  ~LifetimeTestFile() override {
    m_Destructions.fetch_add(1);
  }

  void detachFromParentForTest() {
    retainDetachedParent();
  }

 private:
  std::atomic<size_t>& m_Destructions;
};

class LifetimeTestSymlink final : public Symlink {
 public:
  LifetimeTestSymlink(const String& name, const String& target, Filesystem* filesystem,
                      File* parent, std::atomic<size_t>& destructions)
      : Symlink(name, 0, 0, 0, 22, filesystem, target.length(), parent),
        m_Destructions(destructions) {
    m_sTarget = target;
  }

  ~LifetimeTestSymlink() override {
    m_Destructions.fetch_add(1);
  }

 private:
  std::atomic<size_t>& m_Destructions;
};

bool validSnapshot(const Vector<VFS::MountSnapshot>& mounts, size_t minimum, size_t maximum) {
  if (mounts.count() < minimum || mounts.count() > maximum) {
    return false;
  }

  for (size_t i = 0; i < mounts.count(); ++i) {
    String expectedPath("/media/");
    expectedPath += mounts[i].stableName;
    if (!mounts[i].stableName.length() || mounts[i].path != expectedPath) {
      return false;
    }
    for (size_t j = i + 1; j < mounts.count(); ++j) {
      if (mounts[i].stableName == mounts[j].stableName || mounts[i].path == mounts[j].path) {
        return false;
      }
    }
  }
  return true;
}
}  // namespace

TEST(VFS, AbsolutePathsUseRootFilesystem) {
  VFS vfs;
  RamFs root;
  ASSERT_TRUE(root.initialise(nullptr));
  vfs.registerFilesystem(&root, String("root"));
  ASSERT_TRUE(vfs.setRootFilesystem(&root));

  EXPECT_TRUE(vfs.createDirectory(String("/usr"), 0755));
  EXPECT_TRUE(vfs.createDirectory(String("/usr/bin"), 0755));
  EXPECT_NE(vfs.find(String("/usr/bin")), nullptr);

  vfs.unregisterFilesystem(&root, false);
}

TEST(VFS, RelativePathsStillRequireAStartingNode) {
  VFS vfs;
  RamFs root;
  ASSERT_TRUE(root.initialise(nullptr));
  vfs.registerFilesystem(&root, String("root"));
  ASSERT_TRUE(vfs.setRootFilesystem(&root));

  EXPECT_FALSE(vfs.createDirectory(String("usr"), 0755));
  EXPECT_EQ(vfs.find(String("usr")), nullptr);

  vfs.unregisterFilesystem(&root, false);
}

TEST(VFS, NonRootFilesystemsMountUnderMedia) {
  VFS vfs;
  RamFs root;
  RamFs data;
  ASSERT_TRUE(root.initialise(nullptr));
  ASSERT_TRUE(data.initialise(nullptr));

  vfs.registerFilesystem(&data, String("Data Disk"));
  vfs.registerFilesystem(&root, String("root"));
  ASSERT_TRUE(vfs.setRootFilesystem(&root));

  String mountPath;
  ASSERT_TRUE(vfs.getMountPath(&data, mountPath));
  EXPECT_EQ(mountPath, String("/media/Data-Disk"));
  EXPECT_TRUE(vfs.createFile(String("/media/Data-Disk/example"), 0644));
  EXPECT_TRUE(vfs.createFile(String("/root-only"), 0644));

  File* example = vfs.find(String("/media/Data-Disk/example"));
  ASSERT_NE(example, nullptr);
  EXPECT_EQ(example->getFilesystem(), &data);
  EXPECT_EQ(vfs.find(String("/root-only"), data.getRoot()), vfs.find(String("/root-only")));

  vfs.unregisterFilesystem(&data, false);
  vfs.unregisterFilesystem(&root, false);
}

TEST(VFS, FilesystemWithoutRootDoesNotCrashRootAttachment) {
  VFS vfs;
  RamFs root;
  RamFs uninitialised;
  ASSERT_TRUE(root.initialise(nullptr));
  ASSERT_EQ(uninitialised.getRoot(), nullptr);

  vfs.registerFilesystem(&uninitialised, String("incomplete"));
  vfs.registerFilesystem(&root, String("root"));
  EXPECT_TRUE(vfs.setRootFilesystem(&root));
  EXPECT_EQ(vfs.find(String("/media/incomplete")), nullptr);
  EXPECT_TRUE(vfs.createFile(String("/root-still-works"), 0644));

  vfs.unregisterFilesystem(&uninitialised, false);
  vfs.unregisterFilesystem(&root, false);
}

TEST(VFS, ConcurrentSameNameRegistrationPublishesUniqueNames) {
  VFS vfs;
  MountTestFilesystem first(String("first"));
  MountTestFilesystem second(String("second"));
  StartGate gate(2);
  String firstName;
  String secondName;

  std::thread firstWriter([&] {
    gate.arriveAndWait();
    firstName = vfs.registerFilesystem(&first, String("tmpfs"));
  });
  std::thread secondWriter([&] {
    gate.arriveAndWait();
    secondName = vfs.registerFilesystem(&second, String("tmpfs"));
  });
  firstWriter.join();
  secondWriter.join();

  EXPECT_NE(firstName, secondName);
  EXPECT_TRUE((firstName == String("tmpfs") && secondName == String("tmpfs-2")) ||
              (firstName == String("tmpfs-2") && secondName == String("tmpfs")));

  Vector<VFS::MountSnapshot> mounts;
  vfs.getMounts(mounts);
  EXPECT_TRUE(validSnapshot(mounts, 2, 2));
  EXPECT_TRUE(vfs.unregisterFilesystem(&first, false));
  EXPECT_TRUE(vfs.unregisterFilesystem(&second, false));
}

TEST(VFS, ConcurrentReadersReceiveIndependentSnapshots) {
  VFS vfs;
  std::vector<std::unique_ptr<MountTestFilesystem>> filesystems;
  for (size_t i = 0; i < 16; ++i) {
    filesystems.emplace_back(new MountTestFilesystem(String("snapshot")));
    vfs.registerFilesystem(filesystems.back().get(), String("snapshot"));
  }

  StartGate gate(2);
  std::atomic<size_t> failures(0);
  auto reader = [&] {
    gate.arriveAndWait();
    for (size_t i = 0; i < 500; ++i) {
      Vector<VFS::MountSnapshot> mounts;
      vfs.getMounts(mounts);
      if (!validSnapshot(mounts, filesystems.size(), filesystems.size())) {
        failures.fetch_add(1);
      }
    }
  };

  std::thread firstReader(reader);
  std::thread secondReader(reader);
  firstReader.join();
  secondReader.join();
  EXPECT_EQ(failures.load(), 0U);

  for (const auto& filesystem : filesystems) {
    EXPECT_TRUE(vfs.unregisterFilesystem(filesystem.get(), false));
  }
}

TEST(VFS, SnapshotAndUnregisterSerializeDestructionAndAllowReentry) {
  VFS vfs;
  MountTestFilesystem persistent(String("persistent"));
  ASSERT_TRUE(vfs.registerFilesystem(&persistent, String("persistent")).length());

  std::atomic<size_t> destructions(0);
  std::atomic<size_t> reentries(0);
  std::atomic<size_t> reentryMounts(0);
  std::atomic<size_t> failures(0);
  std::atomic<bool> writerDone(false);

  std::thread reader([&] {
    while (!writerDone.load()) {
      Vector<VFS::MountSnapshot> mounts;
      vfs.getMounts(mounts);
      if (!validSnapshot(mounts, 1, 2)) {
        failures.fetch_add(1);
      }
    }
  });
  std::thread writer([&] {
    for (size_t i = 0; i < 200; ++i) {
      MountTestFilesystem* filesystem = new MountTestFilesystem(
          String("volatile"), &vfs, &destructions, &reentries, &reentryMounts);
      vfs.registerFilesystem(filesystem, String("volatile"));
      if (!vfs.unregisterFilesystem(filesystem)) {
        failures.fetch_add(1);
      }
    }
    writerDone = true;
  });
  writer.join();
  reader.join();

  EXPECT_EQ(failures.load(), 0U);
  EXPECT_EQ(destructions.load(), 200U);
  EXPECT_EQ(reentries.load(), 200U);
  EXPECT_EQ(reentryMounts.load(), 200U);
  EXPECT_TRUE(vfs.unregisterFilesystem(&persistent, false));
}

TEST(VFS, MissingUnregisterDoesNotTakeOwnership) {
  VFS vfs;
  MountTestFilesystem filesystem(String("absent"));
  EXPECT_FALSE(vfs.unregisterFilesystem(&filesystem));
}

TEST(VFS, DestructorUnpublishesBeforeOwnedFilesystemDestruction) {
  std::atomic<size_t> destructions(0);
  std::atomic<size_t> reentries(0);
  std::atomic<size_t> reentryMounts(0);
  {
    VFS vfs;
    MountTestFilesystem* filesystem =
        new MountTestFilesystem(String("owned"), &vfs, &destructions, &reentries, &reentryMounts);
    vfs.registerFilesystem(filesystem, String("owned"));
  }

  EXPECT_EQ(destructions.load(), 1U);
  EXPECT_EQ(reentries.load(), 1U);
  EXPECT_EQ(reentryMounts.load(), 0U);
}

TEST(VFS, SnapshotOwnsParentlessDiskNames) {
  VFS vfs;
  MountTestDisk disk;
  MountTestFilesystem filesystem(String("disk"));
  ASSERT_TRUE(filesystem.initialise(&disk));
  vfs.registerFilesystem(&filesystem, String("disk"));

  Vector<VFS::MountSnapshot> mounts;
  vfs.getMounts(mounts);
  ASSERT_EQ(mounts.count(), 1U);
  EXPECT_TRUE(mounts[0].hasDisk);
  EXPECT_EQ(mounts[0].diskName, String("test-disk"));
  EXPECT_EQ(mounts[0].diskParentName, String());

  EXPECT_TRUE(vfs.unregisterFilesystem(&filesystem, false));
}

TEST(VFS, SparseLookupCoalescesSameNameResolvers) {
  SparseTestDirectory directory(SparseTestDirectory::ResolveMode::Blocked);
  StartGate gate(2);
  Directory::LookupStatus firstStatus = Directory::LookupStatus::IoError;
  Directory::LookupStatus secondStatus = Directory::LookupStatus::IoError;
  File* firstFile = nullptr;
  File* secondFile = nullptr;

  std::thread first([&] {
    gate.arriveAndWait();
    Directory::ChildLease child;
    firstStatus = directory.lookupChild(HashedStringView("shared"), child);
    firstFile = child.get();
  });
  std::thread second([&] {
    gate.arriveAndWait();
    Directory::ChildLease child;
    secondStatus = directory.lookupChild(HashedStringView("shared"), child);
    secondFile = child.get();
  });

  const bool resolverEntered = directory.waitForResolver();
  const bool waiterEntered = directory.waitForLookupUsers(2);
  directory.releaseResolver();
  first.join();
  second.join();

  ASSERT_TRUE(resolverEntered);
  ASSERT_TRUE(waiterEntered);
  EXPECT_EQ(firstStatus, Directory::LookupStatus::Found);
  EXPECT_EQ(secondStatus, Directory::LookupStatus::Found);
  EXPECT_EQ(firstFile, secondFile);
  EXPECT_EQ(directory.resolveCalls(), 1U);
  EXPECT_EQ(directory.constructions(), 1U);
}

TEST(VFS, SparseLookupAllowsDifferentNamesToResolveConcurrently) {
  SparseTestDirectory directory(SparseTestDirectory::ResolveMode::Concurrent);
  StartGate gate(2);
  Directory::LookupStatus firstStatus = Directory::LookupStatus::IoError;
  Directory::LookupStatus secondStatus = Directory::LookupStatus::IoError;

  std::thread first([&] {
    gate.arriveAndWait();
    Directory::ChildLease child;
    firstStatus = directory.lookupChild(HashedStringView("first"), child);
  });
  std::thread second([&] {
    gate.arriveAndWait();
    Directory::ChildLease child;
    secondStatus = directory.lookupChild(HashedStringView("second"), child);
  });
  first.join();
  second.join();

  EXPECT_EQ(firstStatus, Directory::LookupStatus::Found);
  EXPECT_EQ(secondStatus, Directory::LookupStatus::Found);
  EXPECT_EQ(directory.maxConcurrentResolvers(), 2U);
}

TEST(VFS, SparseLookupWakesAllWaitersAfterResolverFailure) {
  SparseTestDirectory directory(SparseTestDirectory::ResolveMode::Blocked);
  StartGate gate(2);
  Directory::LookupStatus firstStatus = Directory::LookupStatus::Found;
  Directory::LookupStatus secondStatus = Directory::LookupStatus::Found;

  std::thread first([&] {
    gate.arriveAndWait();
    Directory::ChildLease child;
    firstStatus = directory.lookupChild(HashedStringView("io-error"), child);
  });
  std::thread second([&] {
    gate.arriveAndWait();
    Directory::ChildLease child;
    secondStatus = directory.lookupChild(HashedStringView("io-error"), child);
  });

  const bool resolverEntered = directory.waitForResolver();
  const bool waiterEntered = directory.waitForLookupUsers(2);
  directory.releaseResolver();
  first.join();
  second.join();

  ASSERT_TRUE(resolverEntered);
  ASSERT_TRUE(waiterEntered);
  EXPECT_EQ(firstStatus, Directory::LookupStatus::IoError);
  EXPECT_EQ(secondStatus, Directory::LookupStatus::IoError);
  EXPECT_EQ(directory.resolveCalls(), 1U);
  EXPECT_EQ(directory.constructions(), 0U);
}

TEST(VFS, ResidentPublicationWaitsForActiveResolver) {
  std::atomic<size_t> residentDestructions(0);
  SparseTestDirectory directory(SparseTestDirectory::ResolveMode::Blocked);
  Directory::LookupStatus status = Directory::LookupStatus::Found;
  File* resolved = nullptr;
  std::thread lookup([&] {
    Directory::ChildLease child;
    status = directory.lookupChild(HashedStringView("missing"), child);
    resolved = child.get();
  });

  const bool resolverEntered = directory.waitForResolver();
  File* resident = new SparseTestFile(String("missing"), &directory, residentDestructions);
  std::atomic<bool> publisherStarted(false);
  bool published = false;
  std::thread publisher([&] {
    publisherStarted.store(true);
    published = directory.publishResident(resident);
  });
  while (!publisherStarted.load())
    std::this_thread::yield();
  directory.releaseResolver();
  lookup.join();
  publisher.join();

  ASSERT_TRUE(resolverEntered);
  ASSERT_TRUE(published);
  EXPECT_EQ(status, Directory::LookupStatus::NotFound);
  EXPECT_EQ(resolved, nullptr);
  EXPECT_EQ(directory.constructions(), 0U);
  EXPECT_EQ(directory.destructions(), 0U);
}

TEST(VFS, ConcurrentRenameLookupsObserveCommittedOrUnchangedIdentities) {
  for (bool commit : {false, true}) {
    SCOPED_TRACE(commit ? "committed rename" : "failed rename");
    PausedRenameFilesystem filesystem;
    ASSERT_TRUE(filesystem.initialise(nullptr));
    Filesystem& interface = filesystem;
    Directory* root = Directory::fromFile(filesystem.getRoot());
    ASSERT_TRUE(interface.createFile(StringView("source"), 0600, root));
    ASSERT_TRUE(interface.createFile(StringView("destination"), 0600, root));
    Directory::ChildLease originalSource;
    Directory::ChildLease originalDestination;
    ASSERT_EQ(root->lookupChild(HashedStringView("source"), originalSource),
              Directory::LookupStatus::Found);
    ASSERT_EQ(root->lookupChild(HashedStringView("destination"), originalDestination),
              Directory::LookupStatus::Found);

    bool renamed = false;
    std::thread writer([&] {
      renamed = interface.rename(StringView("source"), root, StringView("destination"), root);
    });
    const bool backendEntered = filesystem.waitForBackend();
    std::atomic<bool> sourceDone(false);
    std::atomic<bool> destinationDone(false);
    Directory::LookupStatus sourceStatus = Directory::LookupStatus::Retry;
    Directory::LookupStatus destinationStatus = Directory::LookupStatus::Retry;
    File* observedSource = nullptr;
    File* observedDestination = nullptr;
    std::thread sourceReader([&] {
      Directory::ChildLease source;
      sourceStatus = root->lookupChild(HashedStringView("source"), source);
      observedSource = source.get();
      sourceDone.store(true);
    });
    std::thread destinationReader([&] {
      Directory::ChildLease destination;
      destinationStatus = root->lookupChild(HashedStringView("destination"), destination);
      observedDestination = destination.get();
      destinationDone.store(true);
    });

    bool readersWaiting = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
      if (root->inFlightUsersForHostedTest(HashedStringView("source")) >= 2 &&
          root->inFlightUsersForHostedTest(HashedStringView("destination")) >= 2) {
        readersWaiting = true;
        break;
      }
      std::this_thread::yield();
    }
    const bool completedBeforePublication = sourceDone.load() || destinationDone.load();
    filesystem.releaseBackend(commit);
    writer.join();
    sourceReader.join();
    destinationReader.join();

    EXPECT_TRUE(backendEntered);
    EXPECT_TRUE(readersWaiting);
    EXPECT_FALSE(completedBeforePublication);
    EXPECT_EQ(renamed, commit);
    EXPECT_EQ(sourceStatus,
              commit ? Directory::LookupStatus::NotFound : Directory::LookupStatus::Found);
    EXPECT_EQ(observedSource, commit ? nullptr : originalSource.get());
    EXPECT_EQ(destinationStatus, Directory::LookupStatus::Found);
    EXPECT_EQ(observedDestination, commit ? originalSource.get() : originalDestination.get());
    EXPECT_EQ(originalSource.get()->getName(), String(commit ? "destination" : "source"));
  }
}

TEST(VFS, EnumerationStreamsDotsBackingAndResidentOverlayWithStableResumeCookies) {
  std::atomic<size_t> residentDestructions(0);
  SparseTestDirectory directory;
  ASSERT_TRUE(directory.publishResident(
      new SparseTestFile(String("overlay"), &directory, residentDestructions)));

  struct Record {
    String name;
    uint64_t current;
    uint64_t next;
  };
  struct Batch {
    std::vector<Record> records;
    size_t acceptedBeforeStop;
  };
  auto collectUntilFull = [](void* opaque, const Directory::DirectoryEntryView& entry) -> bool {
    Batch* batch = reinterpret_cast<Batch*>(opaque);
    batch->records.push_back({entry.name.toString(), entry.currentCookie, entry.nextCookie});
    return batch->records.size() <= batch->acceptedBeforeStop;
  };

  uint64_t cookie = 0;
  Batch rejectedDot = {{}, 0};
  EXPECT_EQ(directory.enumerate(cookie, collectUntilFull, &rejectedDot),
            Directory::ReadStatus::Stopped);
  ASSERT_EQ(rejectedDot.records.size(), 1U);
  EXPECT_EQ(rejectedDot.records[0].name, String("."));
  EXPECT_EQ(rejectedDot.records[0].current, 0U);
  EXPECT_EQ(rejectedDot.records[0].next, 1U);
  EXPECT_EQ(cookie, rejectedDot.records[0].current);

  Batch rejectedDotDot = {{}, 1};
  EXPECT_EQ(directory.enumerate(cookie, collectUntilFull, &rejectedDotDot),
            Directory::ReadStatus::Stopped);
  ASSERT_EQ(rejectedDotDot.records.size(), 2U);
  EXPECT_EQ(rejectedDotDot.records[0].name, String("."));
  EXPECT_EQ(rejectedDotDot.records[1].name, String(".."));
  EXPECT_EQ(rejectedDotDot.records[1].current, 1U);
  EXPECT_EQ(rejectedDotDot.records[1].next, 2U);
  EXPECT_EQ(cookie, rejectedDotDot.records[1].current);

  Batch rejectedBacking = {{}, 1};
  EXPECT_EQ(directory.enumerate(cookie, collectUntilFull, &rejectedBacking),
            Directory::ReadStatus::Stopped);
  ASSERT_EQ(rejectedBacking.records.size(), 2U);
  EXPECT_EQ(rejectedBacking.records[0].name, String(".."));
  EXPECT_EQ(rejectedBacking.records[1].name, String("alpha"));
  EXPECT_EQ(rejectedBacking.records[1].current, 2U);
  EXPECT_EQ(rejectedBacking.records[1].next, 3U);
  EXPECT_EQ(cookie, rejectedBacking.records[1].current);

  std::vector<std::string> names;
  auto accept = [](void* opaque, const Directory::DirectoryEntryView& entry) -> bool {
    reinterpret_cast<std::vector<std::string>*>(opaque)->emplace_back(entry.name.str(),
                                                                      entry.name.length());
    return true;
  };
  EXPECT_EQ(directory.enumerate(cookie, accept, &names), Directory::ReadStatus::Complete);
  ASSERT_EQ(names.size(), 3U);
  EXPECT_EQ(names[0], "alpha");
  EXPECT_EQ(names[1], "beta");
  EXPECT_EQ(names[2], "overlay");
  EXPECT_EQ(directory.resolveCalls(), 0U);
  EXPECT_EQ(directory.constructions(), 0U);
}

TEST(VFS, LookupChildAtFallsBackByNameAfterResidentCookieBecomesStale) {
  std::atomic<size_t> residentDestructions(0);
  SparseTestDirectory directory;
  File* resident = new SparseTestFile(String("fallback"), &directory, residentDestructions);
  ASSERT_TRUE(directory.publishResident(resident));

  struct Match {
    uint64_t cookie;
    bool found;
  } match = {0, false};
  auto findFallback = [](void* opaque, const Directory::DirectoryEntryView& entry) -> bool {
    Match* match = reinterpret_cast<Match*>(opaque);
    if (entry.name == "fallback") {
      match->cookie = entry.currentCookie;
      match->found = true;
      return false;
    }
    return true;
  };

  uint64_t enumerationCookie = 2;
  EXPECT_EQ(directory.enumerate(enumerationCookie, findFallback, &match),
            Directory::ReadStatus::Stopped);
  ASSERT_TRUE(match.found);
  ASSERT_TRUE(directory.removeResident(resident));
  EXPECT_EQ(residentDestructions.load(), 1U);

  Directory::ChildLease resolved;
  EXPECT_EQ(directory.lookupChildAt(match.cookie, HashedStringView("fallback"), resolved),
            Directory::LookupStatus::Found);
  ASSERT_NE(resolved.get(), nullptr);
  EXPECT_EQ(resolved.get()->getName(), String("fallback"));
  EXPECT_EQ(directory.resolveCalls(), 1U);
  EXPECT_EQ(directory.constructions(), 1U);
}

TEST(VFS, EmptyResolvesAndRemovesBackingOnlyEntries) {
  SparseMutationFilesystem filesystem;
  SparseMutationDirectory* directory = filesystem.root();
  directory->addBacking("first");
  directory->addBacking("alias");
  directory->addBacking("second");

  EXPECT_EQ(directory->backingCount(), 3U);
  EXPECT_EQ(directory->constructions(), 0U);
  ASSERT_TRUE(directory->empty());
  EXPECT_EQ(directory->backingCount(), 0U);
  EXPECT_EQ(directory->removedCount(), 3U);
  EXPECT_EQ(filesystem.removeCalls(), 3U);
  EXPECT_EQ(directory->resolveCalls(), 3U);
  EXPECT_EQ(directory->constructions(), 3U);
  EXPECT_EQ(directory->destructions(), 3U);
}

TEST(VFS, EmptyFailsWhenAStableEntryCannotBeMaterialized) {
  UnmaterializableDirectory directory;
  EXPECT_FALSE(directory.empty());
}

TEST(VFS, DetachedDirectoryRejectsNewResidentAndEphemeralEntries) {
  SparseTestDirectory directory;
  directory.detach();

  std::atomic<size_t> rejectedDestructions(0);
  File* resident = new SparseTestFile(String("resident"), &directory, rejectedDestructions);
  EXPECT_FALSE(directory.publishResident(resident));
  delete resident;

  File* ephemeral = new SparseTestFile(String("missing"), &directory, rejectedDestructions);
  EXPECT_EQ(directory.addEphemeralFile(ephemeral), Directory::AddStatus::Detached);
  delete ephemeral;

  Directory::ChildLease child;
  EXPECT_EQ(directory.lookupChild(HashedStringView("resident"), child),
            Directory::LookupStatus::NotFound);
  EXPECT_EQ(directory.resolveCalls(), 0U);

  std::vector<std::string> names;
  auto accept = [](void* opaque, const Directory::DirectoryEntryView& entry) -> bool {
    reinterpret_cast<std::vector<std::string>*>(opaque)->emplace_back(entry.name.str(),
                                                                      entry.name.length());
    return true;
  };
  uint64_t cookie = 0;
  EXPECT_EQ(directory.enumerate(cookie, accept, &names), Directory::ReadStatus::Complete);
  ASSERT_EQ(names.size(), 2U);
  EXPECT_EQ(names[0], ".");
  EXPECT_EQ(names[1], "..");
  EXPECT_EQ(rejectedDestructions.load(), 2U);
}

TEST(VFS, CreateDoesNotCallDriverWhenFinalNameResolutionFails) {
  SparseMutationFilesystem filesystem;
  Filesystem& interface = filesystem;

  EXPECT_FALSE(interface.createFile(StringView("io-error"), 0644, filesystem.root()));
  EXPECT_EQ(filesystem.root()->resolveCalls(), 1U);
  EXPECT_EQ(filesystem.createCalls(), 0U);
}

TEST(VFS, ExpectedRemovalCannotDeleteReplacementAndEphemeralSkipsDriverRemoval) {
  SparseMutationFilesystem filesystem;
  SparseMutationDirectory* directory = filesystem.root();
  std::atomic<size_t> staleDestructions(0);
  auto staleOwner =
      std::make_unique<SparseTestFile>(String("ephemeral"), directory, staleDestructions);
  File* stale = staleOwner.get();
  auto replacementOwner =
      std::make_unique<SparseTestFile>(String("ephemeral"), directory, staleDestructions);
  File* replacement = replacementOwner.get();
  ASSERT_EQ(directory->addEphemeralFile(replacement), Directory::AddStatus::Added);
  replacementOwner.release();

  EXPECT_FALSE(filesystem.remove(directory, stale));
  EXPECT_EQ(filesystem.removeCalls(), 0U);

  Directory::ChildLease current;
  ASSERT_EQ(directory->lookupChild(HashedStringView("ephemeral"), current),
            Directory::LookupStatus::Found);
  EXPECT_EQ(current.get(), replacement);

  EXPECT_TRUE(filesystem.remove(directory, replacement));
  EXPECT_EQ(filesystem.removeCalls(), 0U);
  Directory::ChildLease absent;
  EXPECT_EQ(directory->lookupChild(HashedStringView("ephemeral"), absent),
            Directory::LookupStatus::NotFound);

  staleOwner.reset();
  EXPECT_EQ(staleDestructions.load(), 1U);
}

TEST(VFS, NonEmptyEphemeralDirectoryCannotBeRemoved) {
  SparseMutationFilesystem filesystem;
  SparseMutationDirectory* root = filesystem.root();
  std::atomic<size_t> parentDestructions(0);
  std::atomic<size_t> fileDestructions(0);

  LifetimeTestDirectory* parent =
      new LifetimeTestDirectory(String("parent"), &filesystem, root, parentDestructions);
  ASSERT_EQ(root->addEphemeralFile(parent), Directory::AddStatus::Added);
  LifetimeTestFile* file =
      new LifetimeTestFile(String("open"), &filesystem, parent, fileDestructions);
  ASSERT_EQ(parent->addEphemeralFile(file), Directory::AddStatus::Added);

  EXPECT_FALSE(filesystem.remove(root, parent));
  EXPECT_EQ(parentDestructions.load(), 0U);
  EXPECT_EQ(fileDestructions.load(), 0U);

  ASSERT_TRUE(filesystem.remove(parent, file));
  EXPECT_TRUE(filesystem.remove(root, parent));
  EXPECT_EQ(fileDestructions.load(), 1U);
  EXPECT_EQ(parentDestructions.load(), 1U);
}

TEST(VFS, RetainedUnlinkedFilePinsItsFormerParent) {
  SparseMutationFilesystem filesystem;
  SparseMutationDirectory* root = filesystem.root();
  std::atomic<size_t> parentDestructions(0);
  std::atomic<size_t> fileDestructions(0);

  LifetimeTestDirectory* parent =
      new LifetimeTestDirectory(String("parent"), &filesystem, root, parentDestructions);
  ASSERT_EQ(root->addEphemeralFile(parent), Directory::AddStatus::Added);
  LifetimeTestFile* file =
      new LifetimeTestFile(String("open"), &filesystem, parent, fileDestructions);
  ASSERT_EQ(parent->addEphemeralFile(file), Directory::AddStatus::Added);

  Directory::ChildLease parentLease;
  ASSERT_EQ(root->lookupChild(HashedStringView("parent"), parentLease),
            Directory::LookupStatus::Found);
  Directory::ChildLease fileLease;
  ASSERT_EQ(parent->lookupChild(HashedStringView("open"), fileLease),
            Directory::LookupStatus::Found);

  ASSERT_TRUE(filesystem.remove(parent, file));
  ASSERT_TRUE(filesystem.remove(root, parent));
  parentLease.reset();

  EXPECT_EQ(parentDestructions.load(), 0U);
  EXPECT_EQ(fileLease.get()->getFullPath(false), String("/parent/open"));

  fileLease.reset();
  EXPECT_EQ(fileDestructions.load(), 1U);
  EXPECT_EQ(parentDestructions.load(), 1U);
}

TEST(VFS, RetainedUnlinkedRelativeSymlinkPinsItsLookupParent) {
  SparseMutationFilesystem filesystem;
  SparseMutationDirectory* root = filesystem.root();
  std::atomic<size_t> parentDestructions(0);
  std::atomic<size_t> linkDestructions(0);

  LifetimeTestDirectory* parent =
      new LifetimeTestDirectory(String("parent"), &filesystem, root, parentDestructions);
  ASSERT_EQ(root->addEphemeralFile(parent), Directory::AddStatus::Added);
  LifetimeTestSymlink* link =
      new LifetimeTestSymlink(String("link"), String("."), &filesystem, parent, linkDestructions);
  ASSERT_EQ(parent->addEphemeralFile(link), Directory::AddStatus::Added);

  Directory::ChildLease parentLease;
  ASSERT_EQ(root->lookupChild(HashedStringView("parent"), parentLease),
            Directory::LookupStatus::Found);
  Directory::ChildLease linkLease;
  ASSERT_EQ(parent->lookupChild(HashedStringView("link"), linkLease),
            Directory::LookupStatus::Found);

  ASSERT_TRUE(filesystem.remove(parent, link));
  ASSERT_TRUE(filesystem.remove(root, parent));
  parentLease.reset();
  EXPECT_EQ(parentDestructions.load(), 0U);

  Directory::ChildLease targetLease;
  EXPECT_EQ(static_cast<Symlink*>(linkLease.get())->followLinkRetained(targetLease), parent);
  targetLease.reset();
  linkLease.reset();

  EXPECT_EQ(linkDestructions.load(), 1U);
  EXPECT_EQ(parentDestructions.load(), 1U);
}

TEST(VFS, ReadlinkDoesNotTerminatePastAFullBuffer) {
  std::atomic<size_t> destructions(0);
  LifetimeTestSymlink link(String("link"), String("target"), nullptr, nullptr, destructions);
  char buffer[] = {'?', '?', '?', '?', '!'};

  EXPECT_EQ(link.followLink(buffer, 4), 4);
  EXPECT_EQ(buffer[0], 't');
  EXPECT_EQ(buffer[1], 'a');
  EXPECT_EQ(buffer[2], 'r');
  EXPECT_EQ(buffer[3], 'g');
  EXPECT_EQ(buffer[4], '!');
}

TEST(VFS, DetachedParentFallbackKeepsOnlyStableFilesystemRootBorrowed) {
  SparseMutationFilesystem filesystem;
  SparseMutationDirectory* root = filesystem.root();
  std::atomic<size_t> parentDestructions(0);
  std::atomic<size_t> fileDestructions(0);

  LifetimeTestDirectory* untrackedParent =
      new LifetimeTestDirectory(String("retiring"), &filesystem, root, parentDestructions);
  LifetimeTestFile detached(String("detached"), &filesystem, untrackedParent, fileDestructions);
  detached.detachFromParentForTest();
  EXPECT_EQ(detached.getParent(), nullptr);
  delete untrackedParent;

  LifetimeTestFile rooted(String("rooted"), &filesystem, root, fileDestructions);
  rooted.detachFromParentForTest();
  EXPECT_EQ(rooted.getParent(), root);
  EXPECT_EQ(parentDestructions.load(), 1U);
}

namespace {
class InodeTestObserver final : public FileEventObserver {
 public:
  explicit InodeTestObserver(FileEventMask pausedMask = 0) : pausedMask(pausedMask) {}
  void fileEvent(const FileEvent& event) override {
    calls.fetch_add(1);
    masks.fetch_or(event.mask);
    pid.store(event.producerPid);
    if (event.mask == pausedMask) {
      std::unique_lock<std::mutex> guard(lock);
      entered = true;
      changed.notify_all();
      changed.wait(guard, [&] { return released; });
    }
  }
  bool waitUntilEntered() {
    std::unique_lock<std::mutex> guard(lock);
    return changed.wait_for(guard, std::chrono::seconds(2), [&] { return entered; });
  }
  void release() {
    std::lock_guard<std::mutex> guard(lock);
    released = true;
    changed.notify_all();
  }
  std::atomic<size_t> calls{0};
  std::atomic<FileEventMask> masks{0};
  std::atomic<uint32_t> pid{0};

 private:
  const FileEventMask pausedMask;
  std::mutex lock;
  std::condition_variable changed;
  bool entered = false, released = false;
};

class InodeAliasFile final : public File {
 public:
  explicit InodeAliasFile(InodeEventSource& source) : source(source) {}
  FileHandleStatus subscribeInodeEvents(FileEventMask mask,
                                        const SharedPointer<FileEventObserver>& observer,
                                        FileEventSubscription& subscription) override {
    return source.subscribeFileEvents(mask, observer, subscription) ? FileHandleStatus::Success
                                                                    : FileHandleStatus::Stale;
  }
  void finishInodeRetirement() override {
    source.finishRetirement();
  }

 protected:
  void publishInodeEvent(const FileEvent& event) override {
    source.publish(event);
  }

 private:
  InodeEventSource& source;
};
}  // namespace

TEST(VFS, RetainedHandleFileOwnsExactlyOneTrackedReference) {
  std::atomic<size_t> destroyed{0};
  auto* file = new SparseTestFile(String("decoded"), nullptr, destroyed);
  ASSERT_TRUE(VFS::instance().tryTrackFile(file));
  ASSERT_TRUE(VFS::instance().tryTrackFile(file));
  RetainedFile first;
  first.adopt(file);
  RetainedFile second(pedigree_std::move(first));
  EXPECT_FALSE(first);
  EXPECT_EQ(second.get(), file);
  second.reset();
  EXPECT_EQ(destroyed.load(), 0U);
  first.adopt(file);
  MountTestFilesystem unsupported(String("unsupported"));
  FileHandle handle;
  handle.length = 7;
  handle.type = 9;
  EXPECT_EQ(unsupported.decodeFileHandle(handle, first), FileHandleStatus::Unsupported);
  EXPECT_FALSE(first);
  EXPECT_EQ(destroyed.load(), 1U);
  File plain;
  EXPECT_EQ(unsupported.encodeFileHandle(plain, handle), FileHandleStatus::Unsupported);
  EXPECT_EQ(handle.length, 0U);
  EXPECT_EQ(handle.type, 0);
  FileSystemId id = {{1, 2}};
  EXPECT_EQ(unsupported.fileHandleFsid(id), FileHandleStatus::Unsupported);
  EXPECT_EQ(id.words[0] | id.words[1], 0U);
}

TEST(VFS, InodeEventsFollowAliasesWithoutNamespaceRetirement) {
  InodeEventSource source;
  auto* observed = new InodeTestObserver;
  SharedPointer<FileEventObserver> observer(observed);
  FileEventSubscription inode, namespaceEvents;
  auto* namespaceObserved = new InodeTestObserver;
  SharedPointer<FileEventObserver> namespaceObserver(namespaceObserved);
  InodeAliasFile second(source);
  {
    InodeAliasFile first(source);
    ASSERT_EQ(first.subscribeInodeEvents(
                  FileEvents::Attributes | FileEvents::Modify | FileEvents::SourceRetired, observer,
                  inode),
              FileHandleStatus::Success);
    ASSERT_TRUE(first.subscribeFileEvents(FileEvents::Attributes | FileEvents::DeletedSelf,
                                          namespaceObserver, namespaceEvents));
    first.publishEvent(FileEvents::Attributes);
    EXPECT_EQ(observed->calls.load(), 1U);
    EXPECT_EQ(namespaceObserved->calls.load(), 1U);
    first.publishEvent(FileEvents::Modify, StringView("child"));
    EXPECT_EQ(observed->calls.load(), 1U);
    first.publishEvent(FileEvents::DeletedSelf);
    EXPECT_EQ(observed->calls.load(), 1U);
    EXPECT_EQ(namespaceObserved->calls.load(), 2U);
  }
  second.publishEvent(FileEvents::Modify);
  EXPECT_EQ(observed->calls.load(), 2U);
  EXPECT_EQ(observed->pid.load(), 0U);
  source.beginRetirement();
  second.finishInodeRetirement();
  EXPECT_EQ(observed->calls.load(), 3U);
  EXPECT_TRUE(observed->masks.load() & FileEvents::SourceRetired);
  second.publishEvent(FileEvents::Modify);
  FileEventSubscription late;
  EXPECT_EQ(second.subscribeInodeEvents(FileEvents::Modify, observer, late),
            FileHandleStatus::Stale);
  EXPECT_EQ(observed->calls.load(), 3U);
  namespaceEvents.reset();
  inode.reset();
}

TEST(VFS, InodeRetirementClosesAdmissionBeforeCallbackDrain) {
  InodeEventSource source;
  auto* observed = new InodeTestObserver(FileEvents::Modify);
  SharedPointer<FileEventObserver> observer(observed);
  FileEventSubscription subscription;
  ASSERT_TRUE(source.subscribeFileEvents(FileEvents::Modify | FileEvents::SourceRetired, observer,
                                         subscription));
  std::thread publisher(
      [&] { source.publish(FileEvent(FileEvents::Modify, StringView(), false, 123)); });
  const bool entered = observed->waitUntilEntered();
  EXPECT_TRUE(entered);
  if (!entered) {
    observed->release();
    publisher.join();
    return;
  }
  EXPECT_EQ(observed->pid.load(), 123U);
  source.beginRetirement();
  EXPECT_EQ(observed->calls.load(), 2U);
  FileEventSubscription late;
  EXPECT_FALSE(source.subscribeFileEvents(FileEvents::Modify, observer, late));
  source.publish(FileEvent(FileEvents::Modify, StringView(), false));
  EXPECT_EQ(observed->calls.load(), 2U);
  std::atomic<bool> drainStarted{false}, drained{false};
  std::thread drainer([&] {
    drainStarted.store(true);
    source.finishRetirement();
    drained.store(true);
  });
  const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!drainStarted.load() && std::chrono::steady_clock::now() < drainDeadline)
    std::this_thread::yield();
  EXPECT_TRUE(drainStarted.load());
  EXPECT_FALSE(drained.load());
  observed->release();
  publisher.join();
  drainer.join();
  EXPECT_TRUE(drained.load());
  subscription.reset();
}

TEST(VFS, MountRetirementWaitsForOperationsButNotIdentityTokens) {
  VFS vfs;
  std::atomic<size_t> destroyed{0};
  auto* filesystem = new MountTestFilesystem(String("leased"), nullptr, &destroyed);
  ASSERT_TRUE(vfs.registerFilesystem(filesystem, String("leased")).length());
  VFS::MountOperation operation;
  ASSERT_TRUE(vfs.acquireMount(filesystem, operation));
  EXPECT_EQ(operation.filesystem(), filesystem);
  VFS::MountIdentity identity = operation.identity();
  const uint32_t id = identity.id();
  EXPECT_GT(id, 0U);
  auto* observed = new InodeTestObserver;
  SharedPointer<FileEventObserver> observer(observed);
  FileEventSubscription retirement;
  ASSERT_TRUE(identity.subscribeRetirement(observer, retirement));
  std::atomic<bool> removed{false};
  std::thread remover([&] { removed.store(vfs.unregisterFilesystem(filesystem)); });
  bool closed = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    VFS::MountOperation probe;
    if (!identity.acquire(probe)) {
      closed = true;
      break;
    }
    std::this_thread::yield();
  }
  EXPECT_TRUE(closed);
  EXPECT_FALSE(removed.load());
  EXPECT_EQ(destroyed.load(), 0U);
  EXPECT_EQ(observed->calls.load(), 0U);
  VFS::MountOperation missing;
  EXPECT_FALSE(vfs.acquireMount(filesystem, missing));
  operation.reset();
  remover.join();
  EXPECT_TRUE(removed.load());
  EXPECT_EQ(destroyed.load(), 1U);
  EXPECT_EQ(observed->calls.load(), 1U);
  EXPECT_EQ(observed->masks.load(), FileEvents::SourceRetired);
  EXPECT_EQ(identity.id(), id);
  EXPECT_FALSE(identity.acquire(missing));
  retirement.reset();
}

TEST(VFS, FilesystemPinsRejectRemovalWithoutClosingAdmission) {
  VFS vfs;
  std::atomic<size_t> destroyed{0};
  auto* filesystem = new MountTestFilesystem(String("pinned"), nullptr, &destroyed);
  ASSERT_TRUE(vfs.registerFilesystem(filesystem, String("pinned")).length());
  VFS::FilesystemPin pin;
  ASSERT_TRUE(vfs.pinFilesystem(filesystem, pin));
  auto identity = pin.identity();
  VFS::FilesystemPin copied(pin);
  VFS::FilesystemPin moved(pedigree_std::move(copied));
  EXPECT_FALSE(copied);
  pin.reset();
  std::thread remover([&] { EXPECT_FALSE(vfs.unregisterFilesystem(filesystem)); });
  remover.join();
  EXPECT_EQ(destroyed.load(), 0U);
  EXPECT_EQ(moved.filesystem(), filesystem);
  EXPECT_EQ(vfs.getFilesystemAt(String("/media/pinned")), filesystem);
  VFS::MountOperation operation;
  EXPECT_TRUE(identity.acquire(operation));
  operation.reset();
  EXPECT_TRUE(identity.pin(pin));
  moved.reset();
  EXPECT_FALSE(vfs.unregisterFilesystem(filesystem, false));
  pin.reset();
  EXPECT_TRUE(vfs.unregisterFilesystem(filesystem));
  EXPECT_EQ(destroyed.load(), 1U);
  EXPECT_FALSE(identity.pin(pin));
  EXPECT_FALSE(vfs.pinFilesystem(filesystem, pin));
}

TEST(VFS, FilesystemSyncPinsBackingAndRunsWithoutPublicationLocks) {
  VFS vfs;
  auto* filesystem = new MountTestFilesystem(String("sync"));
  ASSERT_TRUE(vfs.registerFilesystem(filesystem, String("sync")).length());
  size_t calls = 0;
  filesystem->syncAction = [&] {
    ++calls;
    Vector<VFS::MountSnapshot> mounts;
    vfs.getMounts(mounts);
    EXPECT_EQ(mounts.count(), 1U);
    EXPECT_FALSE(vfs.unregisterFilesystem(filesystem));
    return Filesystem::SyncStatus::IoError;
  };
  EXPECT_EQ(vfs.syncFilesystem(nullptr), Filesystem::SyncStatus::Unsupported);
  EXPECT_EQ(vfs.syncFilesystem(filesystem), Filesystem::SyncStatus::IoError);
  EXPECT_EQ(calls, 1U);
  EXPECT_TRUE(vfs.unregisterFilesystem(filesystem));
  EXPECT_EQ(vfs.syncFilesystem(filesystem), Filesystem::SyncStatus::Unsupported);
}

TEST(VFS, GlobalSyncAdmitsAllBackendsBeforeIoAndContinuesAfterErrors) {
  VFS vfs;
  auto firstOwner = std::make_unique<MountTestFilesystem>(String("first-sync"));
  auto* first = firstOwner.get();
  auto secondOwner = std::make_unique<MountTestFilesystem>(String("second-sync"));
  auto* second = secondOwner.get();
  ASSERT_TRUE(vfs.registerFilesystem(first, String("first-sync")).length());
  firstOwner.release();
  ASSERT_TRUE(vfs.registerFilesystem(second, String("second-sync")).length());
  secondOwner.release();
  std::vector<int> calls;
  first->syncAction = [&] {
    calls.push_back(1);
    EXPECT_FALSE(vfs.unregisterFilesystem(second));
    return Filesystem::SyncStatus::IoError;
  };
  second->syncAction = [&] {
    calls.push_back(2);
    EXPECT_FALSE(vfs.unregisterFilesystem(first));
    return Filesystem::SyncStatus::Unsupported;
  };
  const auto result = vfs.syncAll();
  ASSERT_EQ(calls.size(), 2U);
  EXPECT_NE(calls[0], calls[1]);
  EXPECT_EQ(result,
            calls[0] == 1 ? Filesystem::SyncStatus::IoError : Filesystem::SyncStatus::Unsupported);
  EXPECT_TRUE(vfs.unregisterFilesystem(first));
  EXPECT_TRUE(vfs.unregisterFilesystem(second));
  EXPECT_EQ(vfs.syncAll(), Filesystem::SyncStatus::Success);
}

TEST(VFS, FilesystemPinsRemainCopyableWhileRegistryShutdownDrains) {
  auto* vfs = new VFS;
  std::atomic<size_t> destroyed{0};
  std::atomic<bool> finished{false};
  auto* filesystem = new MountTestFilesystem(String("shutdown"), nullptr, &destroyed);
  ASSERT_TRUE(vfs->registerFilesystem(filesystem, String("shutdown")).length());
  VFS::FilesystemPin pin;
  ASSERT_TRUE(vfs->pinFilesystem(filesystem, pin));
  auto identity = pin.identity();
  std::thread remover([&] {
    delete vfs;
    finished.store(true);
  });
  bool closed = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    VFS::MountOperation probe;
    if (!identity.acquire(probe)) {
      closed = true;
      break;
    }
    std::this_thread::yield();
  }
  EXPECT_TRUE(closed);
  EXPECT_FALSE(finished.load());
  EXPECT_EQ(destroyed.load(), 0U);
  VFS::FilesystemPin late;
  EXPECT_FALSE(identity.pin(late));
  VFS::FilesystemPin copied = pin;
  pin.reset();
  EXPECT_EQ(copied.filesystem()->getVolumeLabel(), String("shutdown"));
  EXPECT_FALSE(finished.load());
  copied.reset();
  remover.join();
  EXPECT_TRUE(finished.load());
  EXPECT_EQ(destroyed.load(), 1U);
}

TEST(VFS, FilesystemPinAdmissionAndRemovalHaveOneWinner) {
  for (size_t attempt = 0; attempt < 64; ++attempt) {
    VFS vfs;
    std::atomic<size_t> destroyed{0};
    auto* filesystem = new MountTestFilesystem(String("race"), nullptr, &destroyed);
    ASSERT_TRUE(vfs.registerFilesystem(filesystem, String("race")).length());
    VFS::FilesystemPin pin;
    StartGate gate(2);
    bool acquired = false, removed = false;
    std::thread pinner([&] {
      gate.arriveAndWait();
      acquired = vfs.pinFilesystem(filesystem, pin);
    });
    std::thread remover([&] {
      gate.arriveAndWait();
      removed = vfs.unregisterFilesystem(filesystem);
    });
    pinner.join();
    remover.join();
    EXPECT_NE(acquired, removed);
    EXPECT_EQ(destroyed.load(), removed ? 1U : 0U);
    if (acquired) {
      EXPECT_EQ(pin.filesystem()->getVolumeLabel(), String("race"));
      pin.reset();
      EXPECT_TRUE(vfs.unregisterFilesystem(filesystem));
    }
    EXPECT_EQ(destroyed.load(), 1U);
  }
}

TEST(VFS, MountRetirementDrainsCallbacksOutsidePublicationLocks) {
  VFS vfs;
  std::atomic<size_t> destroyed{0};
  auto* filesystem = new MountTestFilesystem(String("callback"), nullptr, &destroyed);
  ASSERT_TRUE(vfs.registerFilesystem(filesystem, String("callback")).length());
  VFS::MountOperation operation;
  ASSERT_TRUE(vfs.acquireMount(filesystem, operation));
  auto identity = operation.identity();
  auto* observed = new InodeTestObserver(FileEvents::SourceRetired);
  SharedPointer<FileEventObserver> observer(observed);
  FileEventSubscription retirement;
  ASSERT_TRUE(identity.subscribeRetirement(observer, retirement));
  operation.reset();
  std::thread remover([&] { EXPECT_TRUE(vfs.unregisterFilesystem(filesystem)); });
  const bool entered = observed->waitUntilEntered();
  EXPECT_TRUE(entered);
  if (entered) {
    Vector<VFS::MountSnapshot> mounts;
    vfs.getMounts(mounts);
    EXPECT_EQ(mounts.count(), 0U);
    EXPECT_EQ(destroyed.load(), 0U);
    EXPECT_FALSE(identity.acquire(operation));
  }
  observed->release();
  remover.join();
  EXPECT_EQ(destroyed.load(), 1U);
  retirement.reset();
}

TEST(VFS, RetiredMountIdentityCannotReviveAtTheSameFilesystemAddress) {
  VFS vfs;
  MountTestFilesystem filesystem(String("reused"));
  ASSERT_TRUE(vfs.registerFilesystem(&filesystem, String("reused")).length());
  VFS::MountOperation first;
  ASSERT_TRUE(vfs.acquireMount(&filesystem, first));
  auto oldIdentity = first.identity();
  first.reset();
  ASSERT_TRUE(vfs.unregisterFilesystem(&filesystem, false));
  ASSERT_TRUE(vfs.registerFilesystem(&filesystem, String("reused")).length());
  VFS::MountOperation current;
  ASSERT_TRUE(vfs.acquireMount(&filesystem, current));
  EXPECT_NE(current.id(), oldIdentity.id());
  EXPECT_FALSE(oldIdentity.acquire(first));
  VFS::FilesystemPin pin;
  EXPECT_FALSE(oldIdentity.pin(pin));
  EXPECT_TRUE(current.identity().pin(pin));
  EXPECT_EQ(current.filesystem(), &filesystem);
  current.reset();
  pin.reset();
  EXPECT_TRUE(vfs.unregisterFilesystem(&filesystem, false));
}
