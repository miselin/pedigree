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
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "modules/system/ramfs/RamFs.h"
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

  bool remove(File*, File*) override {
    return false;
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

 private:
  String m_Label;
  VFS* m_Registry;
  std::atomic<size_t>* m_Destructions;
  std::atomic<size_t>* m_Reentries;
  std::atomic<size_t>* m_ReentryMounts;
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
