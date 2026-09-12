/* Copyright (c) 2026, Pedigree Developers. */
#include "modules/system/ramfs/RamFs.h"
#include "modules/system/vfs/Symlink.h"
#include "modules/system/vfs/VFS.h"
#include <gtest/gtest.h>

namespace {
class DynamicDirectory final : public Directory {
 public:
  DynamicDirectory(Filesystem* filesystem, File* parent, size_t& destroyed)
      : Directory(String("dynamic"), 0, 0, 0, 100, filesystem, 0, parent), m_Destroyed(destroyed) {}
  ~DynamicDirectory() override {
    ++m_Destroyed;
  }
  using Directory::markDetached;
  bool present = true;
  size_t generation = 200;
  size_t resolves = 0;

 protected:
  bool cacheResolvedChildren() const override {
    return false;
  }
  LookupStatus resolveChild(const StringView& name, File*& child) override {
    ++resolves;
    child = nullptr;
    if (!present || !(name == "3"))
      return LookupStatus::NotFound;
    child = new File(String(name), 0, 0, 0, generation, getFilesystem(), 0, this);
    return child ? LookupStatus::Found : LookupStatus::IoError;
  }

 private:
  size_t& m_Destroyed;
};
}  // namespace

TEST(DynamicDirectory, ClosingAndReusingNameDoesNotReusePositiveCache) {
  RamFs filesystem;
  ASSERT_TRUE(filesystem.initialise(nullptr));
  size_t destroyed = 0;
  auto* directory = new DynamicDirectory(&filesystem, filesystem.getRoot(), destroyed);
  VFS::instance().trackFile(directory);
  Directory::ChildLease original, alias, absent, replacement;
  ASSERT_EQ(directory->lookupChild(HashedStringView("3"), original),
            Directory::LookupStatus::Found);
  ASSERT_EQ(directory->lookupChild(HashedStringView("3"), alias), Directory::LookupStatus::Found);
  EXPECT_NE(original.get(), alias.get());
  EXPECT_EQ(original.get()->getInode(), alias.get()->getInode());
  directory->present = false;
  EXPECT_EQ(directory->lookupChild(HashedStringView("3"), absent),
            Directory::LookupStatus::NotFound);
  directory->present = true;
  ++directory->generation;
  ASSERT_EQ(directory->lookupChild(HashedStringView("3"), replacement),
            Directory::LookupStatus::Found);
  EXPECT_NE(original.get()->getInode(), replacement.get()->getInode());
  EXPECT_EQ(directory->resolves, 4U);
  EXPECT_EQ(directory->lookup(HashedStringView("3")), nullptr);
  EXPECT_EQ(directory->getChild(2), nullptr);
  EXPECT_EQ(filesystem.find(StringView("3"), directory), nullptr);
  EXPECT_EQ(directory->resolves, 4U);
  directory->markDetached();
  EXPECT_EQ(directory->lookupChild(HashedStringView("3"), absent),
            Directory::LookupStatus::NotFound);
  EXPECT_FALSE(VFS::instance().untrackFile(directory));
  EXPECT_EQ(destroyed, 0U);
  {
    File::ParentLease parent;
    String name;
    original.get()->getNamespace(parent, name);
    EXPECT_EQ(parent.get(), directory);
  }
  original.reset();
  alias.reset();
  EXPECT_EQ(destroyed, 0U);
  replacement.reset();
  EXPECT_EQ(destroyed, 1U);
}

TEST(DynamicDirectory, MountedAbsoluteSymlinkUsesNamespaceRoot) {
  RamFs root, source, target;
  ASSERT_TRUE(root.initialise(nullptr));
  ASSERT_TRUE(source.initialise(nullptr));
  ASSERT_TRUE(target.initialise(nullptr));
  VFS& vfs = VFS::instance();
  struct MountScope {
    VFS& vfs;
    Filesystem* previous;
    Filesystem* root;
    Filesystem* source;
    Filesystem* target;
    ~MountScope() {
      vfs.setRootFilesystem(previous);
      vfs.unregisterFilesystem(target, false);
      vfs.unregisterFilesystem(source, false);
      vfs.unregisterFilesystem(root, false);
    }
  } mounts{vfs, vfs.getRootFilesystem(), &root, &source, &target};
  ASSERT_FALSE(vfs.registerFilesystem(&root, String("symlink-root")).length() == 0);
  ASSERT_TRUE(vfs.setRootFilesystem(&root));
  ASSERT_FALSE(vfs.registerFilesystem(&source, String("symlink-source")).length() == 0);
  ASSERT_FALSE(vfs.registerFilesystem(&target, String("symlink-target")).length() == 0);
  ASSERT_TRUE(static_cast<Filesystem&>(root).createFile(StringView("/root-target"), 0644));
  ASSERT_TRUE(static_cast<Filesystem&>(source).createFile(StringView("/neighbor"), 0644));
  ASSERT_TRUE(static_cast<Filesystem&>(target).createFile(StringView("/target"), 0644));
  String targetMount;
  ASSERT_TRUE(vfs.getMountPath(&target, targetMount));
  targetMount += "/target";
  const String targets[] = {String("/root-target"), targetMount, String("neighbor")};
  File* expected[] = {root.find(StringView("/root-target")), target.find(StringView("/target")),
                      source.find(StringView("/neighbor"))};
  const char* names[] = {"absolute-root", "absolute-mount", "relative"};
  for (size_t i = 0; i < 3; ++i) {
    ASSERT_TRUE(static_cast<Filesystem&>(source).createSymlink(StringView(names[i]), targets[i],
                                                               source.getRoot()));
    Directory::ChildLease link, followed;
    File* file = source.findRetained(StringView(names[i]), link, source.getRoot());
    ASSERT_NE(file, nullptr);
    ASSERT_TRUE(file->isSymlink());
    char text[128] = {};
    ASSERT_EQ(Symlink::fromFile(file)->followLink(text, sizeof(text)),
              static_cast<int>(targets[i].length()));
    EXPECT_EQ(String(text, targets[i].length()), targets[i]);
    EXPECT_EQ(Symlink::fromFile(file)->followLinkRetained(followed), expected[i]);
  }
  auto& api = static_cast<Filesystem&>(source);
  ASSERT_TRUE(api.createFile(StringView("target "), 0644, source.getRoot()));
  ASSERT_TRUE(api.createSymlink(StringView("spaced"), String("target "), source.getRoot()));
  Directory::ChildLease link, targetLease;
  File* file = source.findRetained(StringView("spaced"), link, source.getRoot());
  ASSERT_NE(file, nullptr);
  char text[16] = {};
  EXPECT_EQ(Symlink::fromFile(file)->followLink(text, sizeof(text)), 7);
  EXPECT_EQ(String(text, 7), String("target "));
  EXPECT_EQ(Symlink::fromFile(file)->followLinkRetained(targetLease),
            source.find(StringView("target "), source.getRoot()));
  ASSERT_TRUE(api.createSymlink(StringView("replaced"), String("neighbor"), source.getRoot()));
  source.setProcessOwnership(true);
  ASSERT_TRUE(
      api.rename(StringView("spaced"), source.getRoot(), StringView("replaced"), source.getRoot()));
  EXPECT_EQ(source.find(StringView("spaced"), source.getRoot()), nullptr);
  EXPECT_EQ(source.find(StringView("replaced"), source.getRoot()), file);
  ASSERT_TRUE(api.remove(StringView("replaced"), source.getRoot()));
  EXPECT_EQ(source.find(StringView("replaced"), source.getRoot()), nullptr);
  EXPECT_EQ(Symlink::fromFile(file)->followLinkRetained(targetLease),
            source.find(StringView("target "), source.getRoot()));
}
