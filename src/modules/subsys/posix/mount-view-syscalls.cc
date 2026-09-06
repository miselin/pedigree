/* Copyright (c) 2026, Pedigree Developers. */
#include "mount-view-syscalls.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include <limits.h>

#include "PosixSubsystem.h"
#include "ProcFs.h"
#include "ResolvedPath.h"
#include "file-syscalls.h"
#include "modules/system/ramfs/RamFs.h"
#include "modules/system/vfs/MountView.h"

extern ProcFs* g_pProcFs;

namespace {
struct MountResult {
  MountResult(int result)
      : value(result),
        error(result < 0 ? Processor::information().getCurrentThread()->getErrno() : 0) {}
  int value;
  size_t error;
};
bool privileged() {
  if (Processor::information().getCurrentThread()->getParent()->getEffectiveUserId() == 0)
    return true;
  SYSCALL_ERROR(NotEnoughPermissions);
  return false;
}
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
  // Per-attachment policy and propagation require state not represented by this view.
  if (flags || data) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }
  String sourceCopy, targetCopy, typeCopy;
  if ((source && !copyPath(source, sourceCopy)) || !copyPath(target, targetCopy) ||
      !copyPath(type, typeCopy))
    return -1;
  auto* view = VFS::instance().mountView();
  auto context =
      Processor::information().getCurrentThread()->getParent()->acquireFilesystemContext();
  if (!view || !context || !findFilePath(targetCopy, selected, FilesystemPathRef(), true))
    return -1;
  if (!selected.get()->isDirectory()) {
    SYSCALL_ERROR(NotADirectory);
    return -1;
  }
  Filesystem* backing = nullptr;
  bool owned = false;
  if (typeCopy == "proc") {
    backing = g_pProcFs;
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
    if (!VFS::instance().registerFilesystem(filesystem, typeCopy).length()) {
      delete filesystem;
      SYSCALL_ERROR(OutOfMemory);
      return -1;
    }
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
  const auto ownership =
      owned ? VfsMountView::BackingOwnership::Attachment : VfsMountView::BackingOwnership::External;
  if (view->attach(context, selected.path(), backing, ownership))
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
