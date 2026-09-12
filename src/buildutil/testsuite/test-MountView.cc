/* Copyright (c) 2026, Pedigree Developers. */
#include <atomic>

#include "modules/system/ramfs/RamFs.h"
#include "modules/system/vfs/MountView.h"
#include <gtest/gtest.h>

namespace {
class CountedRamFs final : public RamFs {
 public:
  CountedRamFs(VFS& registry, std::atomic<size_t>& count) : vfs(registry), destroyed(count) {}
  ~CountedRamFs() override {
    Vector<VFS::MountSnapshot> remaining;
    vfs.getMounts(remaining);
    VFS::NamespaceMutation writer(vfs);
    ++destroyed;
  }

 private:
  VFS& vfs;
  std::atomic<size_t>& destroyed;
};

class ResettingObserver final : public FileEventObserver {
 public:
  explicit ResettingObserver(VFS::MountOperation& operation) : operation(operation) {}
  void fileEvent(const FileEvent& event) override {
    if (event.mask == FileEvents::SourceRetired) {
      ++calls;
      operation.reset();
    }
  }
  size_t calls = 0;

 private:
  VFS::MountOperation& operation;
};

class ResettingPinObserver final : public FileEventObserver {
 public:
  explicit ResettingPinObserver(VFS::FilesystemPin& pin) : pin(pin) {}
  void fileEvent(const FileEvent& event) override {
    if (event.mask == FileEvents::SourceRetired) {
      ++calls;
      seen = pin.filesystem();
      pin.reset();
    }
  }
  size_t calls = 0;
  Filesystem* seen = nullptr;

 private:
  VFS::FilesystemPin& pin;
};

class MountViewTest : public testing::Test {
 protected:
  void SetUp() override {
    root = new RamFs;
    ASSERT_TRUE(root->initialise(nullptr));
    ASSERT_TRUE(vfs.registerFilesystem(root, String("root")).length());
    ASSERT_TRUE(vfs.setRootFilesystem(root));
    ASSERT_TRUE(vfs.initialiseMountView());
    view = vfs.mountView();
    ASSERT_NE(view, nullptr);
    ASSERT_TRUE(view->createBootContext(context));
    FilesystemPathRef rootPath;
    ASSERT_TRUE(view->bootRootPath(rootPath));
    ASSERT_TRUE(view->createDirectory(rootPath, String("mounted"), 0777));
    VfsMountView::ResolveOptions options;
    ASSERT_TRUE(view->resolve(context.reference(), rootPath, String("mounted"), options, covered));
  }
  void TearDown() override {
    covered.reset();
    context.reset();
    // VFS owns the published view and its root backend; its destructor drains
    // the view after all fixture path/context owners have retired.
  }
  CountedRamFs* fresh() {
    auto* filesystem = new CountedRamFs(vfs, destroyed);
    if (!filesystem->initialise(nullptr) ||
        !vfs.registerFilesystem(filesystem, String("fresh")).length()) {
      delete filesystem;
      return nullptr;
    }
    return filesystem;
  }
  size_t registrations() {
    Vector<VFS::MountSnapshot> mounts;
    vfs.getMounts(mounts);
    return mounts.count();
  }
  VFS::MountIdentity identity(Filesystem* filesystem) {
    VFS::MountOperation operation;
    if (!vfs.acquireMount(filesystem, operation))
      return VFS::MountIdentity();
    return operation.identity();
  }
  std::atomic<size_t> destroyed{0};
  VFS vfs;
  RamFs* root = nullptr;
  VfsMountView* view = nullptr;
  FilesystemContextOwner context;
  FilesystemPathRef covered;
};
}  // namespace

TEST_F(MountViewTest, TerminalShutdownDrainsOwnersBeforeReturningOwnedBackends) {
  auto* filesystem = fresh();
  ASSERT_NE(filesystem, nullptr);
  ASSERT_TRUE(view->attach(context.reference(), covered, filesystem,
                           VfsMountView::BackingOwnership::Attachment));
  Vector<Filesystem*> owned;
  EXPECT_FALSE(vfs.shutdownMountView(owned));
  EXPECT_EQ(owned.count(), 0U);
  context.reset();
  EXPECT_FALSE(vfs.shutdownMountView(owned));
  covered.reset();
  ASSERT_TRUE(vfs.shutdownMountView(owned));
  EXPECT_EQ(vfs.mountView(), nullptr);
  ASSERT_EQ(owned.count(), 1U);
  EXPECT_EQ(owned[0], filesystem);
  EXPECT_EQ(destroyed.load(), 0U);
  EXPECT_TRUE(vfs.unregisterFilesystem(filesystem, true, true));
  EXPECT_TRUE(vfs.unregisterFilesystem(root, true, true));
  EXPECT_EQ(destroyed.load(), 1U);
  root = nullptr;
  view = nullptr;
}

TEST_F(MountViewTest, RepeatedNormalUnmountRetiresOwnedRegistration) {
  uint32_t previous = 0;
  for (size_t i = 0; i < 32; ++i) {
    auto* filesystem = fresh();
    ASSERT_NE(filesystem, nullptr);
    auto retainedIdentity = identity(filesystem);
    ASSERT_GT(retainedIdentity.id(), previous);
    previous = retainedIdentity.id();
    ASSERT_TRUE(view->attach(context.reference(), covered, filesystem,
                             VfsMountView::BackingOwnership::Attachment));
    ASSERT_EQ(registrations(), 2U);
    ASSERT_TRUE(view->detach(context.reference(), String("/mounted"), false));
    EXPECT_EQ(registrations(), 1U);
    EXPECT_EQ(destroyed.load(), i + 1);
    VFS::MountOperation closed;
    EXPECT_FALSE(retainedIdentity.acquire(closed));
  }
}

TEST_F(MountViewTest, LazyUnmountReclaimsOnlyAfterFinalRetainedPath) {
  for (size_t i = 0; i < 16; ++i) {
    auto* old = fresh();
    ASSERT_NE(old, nullptr);
    const auto oldIdentity = identity(old);
    ASSERT_TRUE(view->attach(context.reference(), covered, old,
                             VfsMountView::BackingOwnership::Attachment));
    FilesystemPathRef opened;
    VfsMountView::ResolveOptions options;
    ASSERT_TRUE(view->resolve(context.reference(), FilesystemPathRef(), String("/mounted"), options,
                              opened));
    const uint64_t attachment = view->attachmentId(opened);
    auto inherited = opened;
    ASSERT_FALSE(view->detach(context.reference(), String("/mounted"), false));
    ASSERT_TRUE(view->detach(context.reference(), String("/mounted"), true));
    EXPECT_EQ(destroyed.load(), 2 * i);
    auto* replacement = fresh();
    ASSERT_NE(replacement, nullptr);
    ASSERT_NE(identity(replacement).id(), oldIdentity.id());
    ASSERT_TRUE(view->attach(context.reference(), covered, replacement,
                             VfsMountView::BackingOwnership::Attachment));
    EXPECT_EQ(registrations(), 3U);
    opened.reset();
    EXPECT_EQ(destroyed.load(), 2 * i);
    EXPECT_EQ(view->attachmentId(inherited), attachment);
    EXPECT_EQ(inherited->node(), old->getRoot());
    inherited.reset();
    EXPECT_EQ(registrations(), 2U);
    EXPECT_EQ(destroyed.load(), 2 * i + 1);
    VFS::MountOperation closed;
    EXPECT_FALSE(oldIdentity.acquire(closed));
    ASSERT_TRUE(view->detach(context.reference(), String("/mounted"), false));
    EXPECT_EQ(registrations(), 1U);
    EXPECT_EQ(destroyed.load(), 2 * i + 2);
  }
}

TEST_F(MountViewTest, AdmissionDrainDelaysStorageWithoutKeepingRegistration) {
  for (size_t i = 0; i < 2; ++i) {
    auto* filesystem = fresh();
    ASSERT_NE(filesystem, nullptr);
    ASSERT_TRUE(view->attach(context.reference(), covered, filesystem,
                             VfsMountView::BackingOwnership::Attachment));
    VFS::FilesystemPin pin;
    VFS::MountOperation operation;
    ASSERT_TRUE(vfs.pinFilesystem(filesystem, pin));
    ASSERT_TRUE(vfs.acquireMount(filesystem, operation));
    const auto retainedIdentity = operation.identity();
    ASSERT_TRUE(view->detach(context.reference(), String("/mounted"), false));
    EXPECT_EQ(registrations(), 1U);
    EXPECT_EQ(destroyed.load(), i);
    EXPECT_EQ(operation.filesystem(), filesystem);
    EXPECT_EQ(pin.filesystem(), filesystem);
    VFS::FilesystemPin rejected;
    VFS::MountOperation closed;
    EXPECT_FALSE(retainedIdentity.pin(rejected));
    EXPECT_FALSE(retainedIdentity.acquire(closed));
    auto copied = pin;
    pin.reset();
    if (i == 0) {
      operation.reset();
      EXPECT_EQ(destroyed.load(), i);
      copied.reset();
    } else {
      copied.reset();
      EXPECT_EQ(destroyed.load(), i);
      operation.reset();
    }
    EXPECT_EQ(destroyed.load(), i + 1);
    EXPECT_GT(retainedIdentity.id(), 0U);
  }
}

TEST_F(MountViewTest, SharedBackendKeepsExternalRegistration) {
  auto* filesystem = fresh();
  ASSERT_NE(filesystem, nullptr);
  ASSERT_TRUE(view->attach(context.reference(), covered, filesystem));
  ASSERT_TRUE(view->detach(context.reference(), String("/mounted"), false));
  EXPECT_EQ(registrations(), 2U);
  EXPECT_EQ(destroyed.load(), 0U);
  EXPECT_TRUE(identity(filesystem));
  ASSERT_TRUE(vfs.unregisterFilesystem(filesystem));
  EXPECT_EQ(destroyed.load(), 1U);
}

TEST_F(MountViewTest, RetirementObserverMayReleaseTheAlreadyDrainedOperation) {
  auto* filesystem = fresh();
  ASSERT_NE(filesystem, nullptr);
  ASSERT_TRUE(view->attach(context.reference(), covered, filesystem,
                           VfsMountView::BackingOwnership::Attachment));
  VFS::MountOperation operation;
  ASSERT_TRUE(vfs.acquireMount(filesystem, operation));
  auto retainedIdentity = operation.identity();
  auto* callback = new ResettingObserver(operation);
  auto observer = SharedPointer<FileEventObserver>::tryAdopt(callback);
  ASSERT_TRUE(observer);
  FileEventSubscription subscription;
  ASSERT_TRUE(retainedIdentity.subscribeRetirement(observer, subscription));
  retainedIdentity = VFS::MountIdentity();
  ASSERT_TRUE(view->detach(context.reference(), String("/mounted"), false));
  EXPECT_EQ(destroyed.load(), 0U);
  operation.reset();
  EXPECT_EQ(destroyed.load(), 1U);
  EXPECT_EQ(callback->calls, 1U);
  EXPECT_FALSE(operation);
  subscription.reset();
}

TEST_F(MountViewTest, PinRetirementPublishesEmptyOrReplacementBeforeCallbacks) {
  for (size_t assignment = 0; assignment < 3; ++assignment) {
    auto* filesystem = fresh();
    ASSERT_NE(filesystem, nullptr);
    ASSERT_TRUE(view->attach(context.reference(), covered, filesystem,
                             VfsMountView::BackingOwnership::Attachment));
    VFS::FilesystemPin pin;
    ASSERT_TRUE(vfs.pinFilesystem(filesystem, pin));
    auto retainedIdentity = pin.identity();
    auto* callback = new ResettingPinObserver(pin);
    auto observer = SharedPointer<FileEventObserver>::tryAdopt(callback);
    ASSERT_TRUE(observer);
    FileEventSubscription subscription;
    ASSERT_TRUE(retainedIdentity.subscribeRetirement(observer, subscription));
    retainedIdentity = VFS::MountIdentity();
    ASSERT_TRUE(view->detach(context.reference(), String("/mounted"), false));
    EXPECT_EQ(destroyed.load(), assignment);
    VFS::FilesystemPin replacement;
    ASSERT_TRUE(vfs.pinFilesystem(root, replacement));
    if (assignment == 0)
      pin.reset();
    else if (assignment == 1)
      pin = replacement;
    else
      pin = pedigree_std::move(replacement);
    EXPECT_EQ(destroyed.load(), assignment + 1);
    EXPECT_EQ(callback->calls, 1U);
    EXPECT_EQ(callback->seen, assignment ? root : nullptr);
    EXPECT_FALSE(pin);
    subscription.reset();
  }
}
