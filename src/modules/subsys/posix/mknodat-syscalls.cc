/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/FilesystemCredentials.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include <fcntl.h>
#include <limits.h>

#include "FileDescriptor.h"
#include "PosixProcess.h"
#include "PosixSubsystem.h"
#include "file-syscalls.h"
#include "metadata-syscalls.h"
#include "modules/system/vfs/MountView.h"
#include "modules/system/vfs/Pipe.h"
#include "modules/system/vfs/VFS.h"
#include <sys/stat.h>

namespace {
struct MknodResult {
  MknodResult(int result)
      : value(result),
        error(result < 0 ? Processor::information().getCurrentThread()->getErrno() : 0) {}
  int value, error;
};

MknodResult createNode(int dirfd, const char* userPath, mode_t suppliedMode) {
  TerminationDeferral lifetime;
  const mode_t mode = static_cast<uint16_t>(suppliedMode);
  const mode_t type = mode & S_IFMT;
  if (type != 0 && type != S_IFREG && type != S_IFIFO) {
    syscallError(type == S_IFBLK || type == S_IFCHR || type == S_IFSOCK
                     ? Error::OperationNotSupported
                 : type == S_IFDIR ? Error::NotEnoughPermissions
                                   : Error::InvalidArgument);
    return -1;
  }
  if (mode & (S_ISUID | S_ISGID)) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }
  String copied;
  const auto copiedStatus = PosixSubsystem::copyUserString(userPath, copied, PATH_MAX);
  if (copiedStatus != PosixSubsystem::UserStringSuccess) {
    syscallError(copiedStatus == PosixSubsystem::UserStringBadAddress ? Error::BadAddress
                                                                      : Error::NameTooLong);
    return -1;
  }
  if (!copied.length()) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }
  Process* process = Processor::information().getCurrentThread()->getParent();
  auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  auto* view = VFS::instance().mountView();
  auto context = process->acquireFilesystemContext();
  if (!subsystem || !view || !context) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }
  DescriptorLease descriptor;
  FilesystemPathRef start, parent;
  if (copied[0] != '/' && dirfd != AT_FDCWD) {
    if (!subsystem->acquireFileDescriptor(dirfd, descriptor) || !descriptor->getFile()) {
      SYSCALL_ERROR(BadFileDescriptor);
      return -1;
    }
    start = descriptor->openingPath();
    if (!descriptor->getFile()->isDirectory() || !start) {
      SYSCALL_ERROR(NotADirectory);
      return -1;
    }
  }
  String path, basename;
  normalisePath(path, copied.cstr());
  if (!view->resolveParent(context, start, path, parent, basename))
    return -1;
  if (!basename.length() || basename == "." || basename == "..") {
    SYSCALL_ERROR(FileExists);
    return -1;
  }
  if (basename.length() > NAME_MAX) {
    SYSCALL_ERROR(NameTooLong);
    return -1;
  }
  if (copied[copied.length() - 1] == '/') {
    Directory::ChildLease existing;
    const auto status =
        Directory::fromFile(parent->node())->lookupChild(HashedStringView(basename), existing);
    syscallError(status == Directory::LookupStatus::Found      ? Error::FileExists
                 : status == Directory::LookupStatus::NotFound ? Error::DoesNotExist
                 : status == Directory::LookupStatus::Retry    ? Error::NoMoreProcesses
                                                               : Error::IoError);
    return -1;
  }
  mode_t permissions = mode & 01777;
  if (process->getType() == Process::Posix)
    permissions &= ~static_cast<PosixProcess*>(process)->getMask();
  if (type != S_IFIFO)
    return view->createFile(parent, basename, permissions) ? 0 : -1;

  // The retained attachment and directory admission protect this exact parent;
  // a second pathname lookup could publish into a different mount after pivot.
  if (!parent || parent->provider() != view || !parent->node()->isDirectory()) {
    SYSCALL_ERROR(NotADirectory);
    return -1;
  }
  Directory* directory = Directory::fromFile(parent->node());
  if (directory->getFilesystem()->isReadOnly()) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return -1;
  }
  if (!VFS::checkAccess(directory, false, true, true))
    return -1;
  FilesystemCredentials credentials;
  if (!Process::currentFilesystemCredentials(credentials)) {
    SYSCALL_ERROR(PermissionDenied);
    return -1;
  }
  Pipe* pipe = new Pipe(basename, 0, 0, 0, 0, directory->getFilesystem(), 0, directory);
  if (!pipe) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  // VFS owner rights occupy the low bits, unlike Unix modes.
  const uint32_t vfsPermissions =
      ((permissions & 0400) ? FILE_UR : 0) | ((permissions & 0200) ? FILE_UW : 0) |
      ((permissions & 0100) ? FILE_UX : 0) | ((permissions & 0040) ? FILE_GR : 0) |
      ((permissions & 0020) ? FILE_GW : 0) | ((permissions & 0010) ? FILE_GX : 0) |
      ((permissions & 0004) ? FILE_OR : 0) | ((permissions & 0002) ? FILE_OW : 0) |
      ((permissions & 0001) ? FILE_OX : 0) | ((permissions & S_ISVTX) ? FILE_STICKY : 0);
  pipe->setPermissions(vfsPermissions);
  pipe->setUid(credentials.uid);
  pipe->setGid(credentials.gid);
  const Directory::AddStatus added = directory->addEphemeralFile(pipe);
  if (added != Directory::AddStatus::Added) {
    delete pipe;
    syscallError(added == Directory::AddStatus::IoError    ? Error::IoError
                 : added == Directory::AddStatus::Detached ? Error::DoesNotExist
                                                           : Error::FileExists);
    return -1;
  }
  return 0;
}
}  // namespace

int posix_mknodat(int dirfd, const char* path, mode_t mode, dev_t device) {
  // Linux ignores the device argument for regular files and FIFOs.
  (void)device;
  const MknodResult result = createNode(dirfd, path, mode);
  syscallError(result.error);
  return result.value;
}
