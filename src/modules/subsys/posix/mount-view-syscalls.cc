/* Copyright (c) 2026, Pedigree Developers. */
#include "mount-view-syscalls.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include <limits.h>

#include "DevFs.h"
#include "PosixSubsystem.h"
#include "ProcFs.h"
#include "ResolvedPath.h"
#include "file-syscalls.h"
#include "modules/system/ramfs/RamFs.h"
#include "modules/system/vfs/MountView.h"
#include "user-namespace.h"

extern ProcFs* g_pProcFs;

namespace {
uint32_t permissionsForMode(uint32_t mode) {
  uint32_t permissions = mode & FILE_AMASK;
  if (mode & 0400) {
    permissions |= FILE_UR;
  }
  if (mode & 0200) {
    permissions |= FILE_UW;
  }
  if (mode & 0100) {
    permissions |= FILE_UX;
  }
  if (mode & 0040) {
    permissions |= FILE_GR;
  }
  if (mode & 0020) {
    permissions |= FILE_GW;
  }
  if (mode & 0010) {
    permissions |= FILE_GX;
  }
  if (mode & 0004) {
    permissions |= FILE_OR;
  }
  if (mode & 0002) {
    permissions |= FILE_OW;
  }
  if (mode & 0001) {
    permissions |= FILE_OX;
  }
  return permissions;
}
struct MountResult {
  MountResult(int result)
      : value(result),
        error(result < 0 ? Processor::information().getCurrentThread()->getErrno() : 0) {}
  int value;
  size_t error;
};
bool privileged() {
  auto* view = VFS::instance().mountView();
  if (!view) {
    SYSCALL_ERROR(NoSuchDevice);
    return false;
  }
  auto owner = posix_user_namespace(*Processor::information().getCurrentThread());
  while (owner && owner->identity() != view->ownerNamespace()) {
    owner = owner->parent();
  }
  if ((owner ? owner->identity() : 0) == view->ownerNamespace() &&
      posix_namespace_capable(owner, PosixCapabilities::SysAdmin)) {
    return true;
  }
  SYSCALL_ERROR(NotEnoughPermissions);
  return false;
}

class DevPts final : public Filesystem {
 public:
  ~DevPts() override {
    delete m_Root;
  }
  bool initialise(Disk*) override {
    m_Root = new DevFsDirectory(String(""), 0, 0, 0, 1, this, 0, nullptr);
    if (!m_Root) {
      return false;
    }
    m_Root->setPermissions(permissionsForMode(0755));
    auto* multiplexer =
        new PtmxFile(String("ptmx"), 2, this, m_Root, m_Root, permissionsForMode(0620), true);
    if (!multiplexer) {
      return false;
    }
    if (!m_Root->addEntry(String("ptmx"), multiplexer)) {
      delete multiplexer;
      return false;
    }
    return true;
  }
  File* getRoot() const override {
    return m_Root;
  }
  const String& getVolumeLabel() const override {
    static String name("devpts");
    return name;
  }
  SyncStatus sync() override {
    return SyncStatus::Success;
  }
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
  DevFsDirectory* m_Root = nullptr;
};

bool copyPath(const char* user, String& result) {
  auto status = PosixSubsystem::copyUserString(user, result, PATH_MAX);
  if (status == PosixSubsystem::UserStringSuccess)
    return true;
  syscallError(status == PosixSubsystem::UserStringBadAddress ? Error::BadAddress
                                                              : Error::NameTooLong);
  return false;
}
bool initialiseSelinux(RamFs& filesystem) {
  struct Entry {
    const char* name;
    uint32_t mode;
    const char* value;
  };
  const Entry entries[] = {
      {"enforce", 0444, "0\n"}, {"policyvers", 0444, "0\n"}, {"disable", 0666, ""}};
  for (const auto& entry : entries) {
    if (!filesystem.Filesystem::createFile(String(entry.name), entry.mode, filesystem.getRoot()))
      return false;
    Directory::ChildLease file;
    if (Directory::fromFile(filesystem.getRoot())
            ->lookupChild(HashedStringView(entry.name), file) != Directory::LookupStatus::Found)
      return false;
    const size_t bytes = StringLength(entry.value);
    if (bytes && file.get()->write(0, bytes, reinterpret_cast<uintptr_t>(entry.value)) != bytes)
      return false;
  }
  return true;
}
MountResult mount(const char* source, const char* target, const char* type, size_t flags,
                  const void* data) {
  ResolvedPath selected;
  if (!privileged())
    return -1;
  constexpr size_t Bind = 4096, Recursive = 16384, Remount = 32;
  constexpr size_t Silent = 32768, Private = 1UL << 18, Slave = 1UL << 19;
  constexpr size_t Policy = VfsMountView::SupportedMountFlags;
  if ((flags & 0xffff0000UL) == 0xc0ed0000UL) {
    flags &= 0xffff;
  }
  flags &= ~Silent;
  if (flags & ~(Policy | Bind | Recursive | Remount | Private | Slave)) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }
  String sourceCopy, targetCopy, typeCopy, options;
  if ((source && !copyPath(source, sourceCopy)) || !copyPath(target, targetCopy) ||
      (type && !copyPath(type, typeCopy)) ||
      (data && !copyPath(static_cast<const char*>(data), options))) {
    return -1;
  }
  auto* view = VFS::instance().mountView();
  auto context =
      Processor::information().getCurrentThread()->getParent()->acquireFilesystemContext();
  if (!view || !context || !findFilePath(targetCopy, selected, FilesystemPathRef(), true)) {
    return -1;
  }
  if (flags & (Private | Slave)) {
    if ((flags & ~(Private | Slave | Recursive)) ||
        (flags & (Private | Slave)) == (Private | Slave)) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    // Graph copies never propagate mount mutations to another namespace.
    return 0;
  }
  if (flags & Remount) {
    if (!(flags & Bind)) {
      SYSCALL_ERROR(OperationNotSupported);
      return -1;
    }
    return view->remount(context, selected.path(), flags & Policy, flags & Recursive) ? 0 : -1;
  }
  if (flags & Bind) {
    ResolvedPath original;
    if (!source || !findFilePath(sourceCopy, original, FilesystemPathRef(), true)) {
      return -1;
    }
    return view->bind(context, original.path(), selected.path(), flags & Recursive) ? 0 : -1;
  }
  if ((flags & Recursive) || !selected.get()->isDirectory()) {
    syscallError((flags & Recursive) ? Error::InvalidArgument : Error::NotADirectory);
    return -1;
  }
  uint32_t rootMode = 0755;
  if (options.length() && typeCopy != "devpts") {
    if (options.length() < 6 || String(options.cstr(), 5) != "mode=") {
      SYSCALL_ERROR(OperationNotSupported);
      return -1;
    }
    rootMode = 0;
    for (size_t i = 5; i < options.length(); ++i) {
      if (options[i] < '0' || options[i] > '7' || rootMode > 0777) {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
      }
      rootMode = rootMode * 8 + options[i] - '0';
    }
  }
  Filesystem* backing = nullptr;
  bool owned = false;
  if (typeCopy == "proc") {
    auto* filesystem =
        new ProcFs(Processor::information().getCurrentThread()->getParent()->pidNamespace());
    if (!filesystem || !filesystem->initialise(nullptr)) {
      delete filesystem;
      SYSCALL_ERROR(OutOfMemory);
      return -1;
    }
    backing = filesystem;
    owned = true;
  } else if (typeCopy == "devpts") {
    if (options != "newinstance,ptmxmode=0666,mode=620") {
      SYSCALL_ERROR(OperationNotSupported);
      return -1;
    }
    auto* filesystem = new DevPts;
    if (!filesystem || !filesystem->initialise(nullptr)) {
      delete filesystem;
      SYSCALL_ERROR(OutOfMemory);
      return -1;
    }
    backing = filesystem;
    owned = true;
  } else if (typeCopy == "tmpfs" || typeCopy == "ramfs" || typeCopy == "selinuxfs") {
    auto* filesystem = new RamFs;
    if (!filesystem) {
      SYSCALL_ERROR(OutOfMemory);
      return -1;
    }
    if (!filesystem->initialise(nullptr) ||
        (typeCopy == "selinuxfs" && !initialiseSelinux(*filesystem))) {
      delete filesystem;
      SYSCALL_ERROR(IoError);
      return -1;
    }
    filesystem->getRoot()->setPermissions(permissionsForMode(rootMode));
    backing = filesystem;
    owned = true;
  } else {
    SYSCALL_ERROR(DeviceDoesNotExist);
    return -1;
  }
  if (!backing) {
    SYSCALL_ERROR(DeviceDoesNotExist);
    return -1;
  }
  if (owned && !VFS::instance().registerFilesystem(backing, typeCopy).length()) {
    delete backing;
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  const auto ownership =
      owned ? VfsMountView::BackingOwnership::Attachment : VfsMountView::BackingOwnership::External;
  if (view->attach(context, selected.path(), backing, ownership, flags & Policy))
    return 0;
  const auto error = Processor::information().getCurrentThread()->getErrno();
  if (owned && !VFS::instance().retireOwnedFilesystem(backing))
    FATAL("Unpublished mount backing retained unexpectedly");
  syscallError(error);
  return -1;
}
MountResult unmount(const char* target, int flags) {
  TerminationDeferral lifetime;
  if (!privileged())
    return -1;
  constexpr int lazy = 2;
  if (flags & ~lazy) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }
  String copied;
  if (!copyPath(target, copied))
    return -1;
  auto context =
      Processor::information().getCurrentThread()->getParent()->acquireFilesystemContext();
  auto* view = VFS::instance().mountView();
  if (!view || !context) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }
  return view->detach(context, copied, flags & lazy) ? 0 : -1;
}
MountResult pivot(const char* newRoot, const char* putOld) {
  TerminationDeferral lifetime;
  if (!privileged())
    return -1;
  String next, previous;
  if (!copyPath(newRoot, next) || !copyPath(putOld, previous))
    return -1;
  auto context =
      Processor::information().getCurrentThread()->getParent()->acquireFilesystemContext();
  auto* view = VFS::instance().mountView();
  if (!view || !context) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }
  return view->pivot(context, next, previous) ? 0 : -1;
}
}  // namespace
int posix_mount(const char* source, const char* target, const char* type, size_t flags,
                const void* data) {
  const MountResult result = mount(source, target, type, flags, data);
  syscallError(result.error);
  return result.value;
}
int posix_umount2(const char* target, int flags) {
  const MountResult result = unmount(target, flags);
  syscallError(result.error);
  return result.value;
}
int posix_pivot_root(const char* newRoot, const char* putOld) {
  const MountResult result = pivot(newRoot, putOld);
  syscallError(result.error);
  return result.value;
}
