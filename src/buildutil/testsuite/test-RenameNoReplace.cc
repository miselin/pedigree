/* Copyright (c) 2026, Pedigree Developers. */
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>

#include "modules/system/ramfs/RamFs.h"
#include <condition_variable>
#include <gtest/gtest.h>

namespace {
using namespace std::chrono_literals;

class RenameFilesystem final : public RamFs {
 public:
  bool waitForBackend() {
    std::unique_lock<std::mutex> guard(lock);
    return changed.wait_for(guard, 5s, [&] { return entered; });
  }

  void release(bool success) {
    std::lock_guard<std::mutex> guard(lock);
    commit = success;
    released = true;
    changed.notify_all();
  }

  size_t calls = 0;
  bool pause = false;

 protected:
  bool renameNode(Directory*, const String&, File*, Directory*, const String&, File*) override {
    std::unique_lock<std::mutex> guard(lock);
    ++calls;
    if (!pause)
      return true;
    entered = true;
    changed.notify_all();
    if (!changed.wait_for(guard, 5s, [&] { return released; }))
      return false;
    return commit;
  }

 private:
  std::mutex lock;
  std::condition_variable changed;
  bool entered = false, released = false, commit = false;
};
}  // namespace

TEST(RenameNoReplace, ExistingDestinationAndIdenticalPathNeverReachBackend) {
  RenameFilesystem filesystem;
  ASSERT_TRUE(filesystem.initialise(nullptr));
  Filesystem& fs = filesystem;
  auto* root = Directory::fromFile(fs.getRoot());
  ASSERT_TRUE(fs.createFile(StringView("source"), 0600, root));
  ASSERT_TRUE(fs.createFile(StringView("target"), 0600, root));
  Directory::ChildLease source, target;
  ASSERT_EQ(root->lookupChild(HashedStringView("source"), source), Directory::LookupStatus::Found);
  ASSERT_EQ(root->lookupChild(HashedStringView("target"), target), Directory::LookupStatus::Found);
  EXPECT_FALSE(fs.rename(StringView("source"), root, StringView("target"), root, true));
  EXPECT_FALSE(fs.rename(StringView("source"), root, StringView("source"), root, true));
  EXPECT_EQ(filesystem.calls, 0U);
  Directory::ChildLease sourceAfter, targetAfter;
  ASSERT_EQ(root->lookupChild(HashedStringView("source"), sourceAfter),
            Directory::LookupStatus::Found);
  ASSERT_EQ(root->lookupChild(HashedStringView("target"), targetAfter),
            Directory::LookupStatus::Found);
  EXPECT_EQ(sourceAfter.get(), source.get());
  EXPECT_EQ(targetAfter.get(), target.get());
  EXPECT_TRUE(fs.rename(StringView("source"), root, StringView("target"), root));
  EXPECT_EQ(filesystem.calls, 1U);
}

TEST(RenameNoReplace, MissingDestinationCommitsOnceAndPreservesIdentity) {
  RenameFilesystem filesystem;
  ASSERT_TRUE(filesystem.initialise(nullptr));
  Filesystem& fs = filesystem;
  auto* root = Directory::fromFile(fs.getRoot());
  ASSERT_TRUE(fs.createDirectory(StringView("child"), 0700, root));
  ASSERT_TRUE(fs.createFile(StringView("source"), 0600, root));
  Directory::ChildLease source;
  ASSERT_EQ(root->lookupChild(HashedStringView("source"), source), Directory::LookupStatus::Found);
  ASSERT_TRUE(fs.rename(StringView("source"), root, StringView("child/target"), root, true));
  EXPECT_EQ(filesystem.calls, 1U);
  EXPECT_EQ(source.get()->getName(), String("target"));
  Directory::ChildLease absent;
  EXPECT_EQ(root->lookupChild(HashedStringView("source"), absent),
            Directory::LookupStatus::NotFound);
  EXPECT_EQ(fs.find(StringView("child/target"), root), source.get());
}

TEST(RenameNoReplace, MixedAbsoluteAndRelativePathsUseTheirOwnNamespaceRoots) {
  RenameFilesystem mounted;
  RamFs namespaceFilesystem;
  ASSERT_TRUE(mounted.initialise(nullptr));
  ASSERT_TRUE(namespaceFilesystem.initialise(nullptr));
  Filesystem& namespaceFs = namespaceFilesystem;
  Filesystem& mountedFs = mounted;
  auto* namespaceRoot = Directory::fromFile(namespaceFs.getRoot());
  auto* mountedRoot = Directory::fromFile(mountedFs.getRoot());
  ASSERT_TRUE(namespaceFs.createDirectory(StringView("mount"), 0700, namespaceRoot));
  Directory::ChildLease mount;
  ASSERT_EQ(namespaceRoot->lookupChild(HashedStringView("mount"), mount),
            Directory::LookupStatus::Found);
  auto* mountpoint = Directory::fromFile(mount.get());
  mountpoint->setReparsePoint(mountedRoot);
  struct ReparseReset {
    Directory* directory;
    ~ReparseReset() {
      directory->setReparsePoint(nullptr);
    }
  } reset{mountpoint};
  ASSERT_TRUE(mountedFs.createFile(StringView("source"), 0600, mountedRoot));
  Directory::ChildLease source;
  ASSERT_EQ(mountedRoot->lookupChild(HashedStringView("source"), source),
            Directory::LookupStatus::Found);
  ASSERT_TRUE(mountedFs.rename(StringView("source"), mountedRoot, StringView("/mount/absolute"),
                               namespaceRoot, true));
  EXPECT_EQ(mountedFs.find(StringView("absolute"), mountedRoot), source.get());
  ASSERT_TRUE(namespaceFs.rename(StringView("/mount/absolute"), namespaceRoot,
                                 StringView("relative"), mountedRoot, true));
  EXPECT_EQ(mountedFs.find(StringView("relative"), mountedRoot), source.get());
  EXPECT_FALSE(mountedFs.rename(StringView("relative"), mountedRoot, StringView("/outside"),
                                namespaceRoot, true));
  EXPECT_EQ(mountedFs.find(StringView("relative"), mountedRoot), source.get());
  EXPECT_EQ(mounted.calls, 2U);
}

TEST(RenameNoReplace, ConcurrentCreateWaitsForCommitOrFailureBeforeClaimingDestination) {
  for (bool commit : {false, true}) {
    RenameFilesystem filesystem;
    ASSERT_TRUE(filesystem.initialise(nullptr));
    Filesystem& fs = filesystem;
    auto* root = Directory::fromFile(fs.getRoot());
    ASSERT_TRUE(fs.createFile(StringView("source"), 0600, root));
    Directory::ChildLease source;
    ASSERT_EQ(root->lookupChild(HashedStringView("source"), source),
              Directory::LookupStatus::Found);
    filesystem.pause = true;
    auto renamer = std::async(std::launch::async, [&] {
      return fs.rename(StringView("source"), root, StringView("target"), root, true);
    });
    const bool entered = filesystem.waitForBackend();
    std::promise<void> started;
    auto startedFuture = started.get_future();
    auto creator = std::async(std::launch::async, [&] {
      started.set_value();
      return fs.createFile(StringView("target"), 0600, root);
    });
    startedFuture.wait();
    const bool blocked = creator.wait_for(20ms) == std::future_status::timeout;
    filesystem.release(commit);
    EXPECT_TRUE(entered);
    EXPECT_TRUE(blocked);
    EXPECT_EQ(renamer.get(), commit);
    EXPECT_EQ(creator.get(), !commit);
    Directory::ChildLease target;
    ASSERT_EQ(root->lookupChild(HashedStringView("target"), target),
              Directory::LookupStatus::Found);
    EXPECT_EQ(target.get() == source.get(), commit);
  }
}
