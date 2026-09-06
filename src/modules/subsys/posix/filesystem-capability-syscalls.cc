/* Copyright (c) 2026, Pedigree Developers. */
#include "filesystem-capability-syscalls.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include <fcntl.h>
#include <limits.h>

#include "FileDescriptor.h"
#include "PosixSubsystem.h"
#include "ResolvedPath.h"
#include "UnixFilesystem.h"
#include "file-syscalls.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/MountView.h"
#include "modules/system/vfs/VFS.h"

namespace {
constexpr uint64_t MaximumPosition = 0x7fffffffffffffffULL;
constexpr unsigned RenameNoReplace = 1, RenameExchange = 2, RenameWhiteout = 4;

struct FilesystemResult {
  FilesystemResult(int result)
      : value(result),
        error(result < 0 ? Processor::information().getCurrentThread()->getErrno() : 0) {}
  int value;
  int error;
};

struct RenamePath {
  ResolvedPath parent;
  String copied, normalised, basename;
  DescriptorLease descriptor;

  bool copy(const char* user) {
    const auto result = PosixSubsystem::copyUserString(user, copied, PATH_MAX);
    if (result != PosixSubsystem::UserStringSuccess) {
      syscallError(result == PosixSubsystem::UserStringBadAddress ? Error::BadAddress
                                                                  : Error::NameTooLong);
      return false;
    }
    if (!copied.length()) {
      SYSCALL_ERROR(DoesNotExist);
      return false;
    }
    normalisePath(normalised, copied.cstr());
    return true;
  }

  bool resolve(int fd, VfsMountView& view, const FilesystemContextRef& context) {
    Process* process = Processor::information().getCurrentThread()->getParent();
    FilesystemPathRef start;
    if (copied[0] != '/' && fd != AT_FDCWD) {
      auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
      if (!subsystem || !subsystem->acquireFileDescriptor(fd, descriptor) ||
          !descriptor->getFile()) {
        SYSCALL_ERROR(BadFileDescriptor);
        return false;
      }
      start = descriptor->openingPath();
      if (!descriptor->getFile()->isDirectory() || !start) {
        SYSCALL_ERROR(NotADirectory);
        return false;
      }
    }
    FilesystemPathRef selected;
    if (!view.resolveParent(context, start, normalised, selected, basename))
      return false;
    parent.retain(selected);
    return true;
  }
};
FilesystemResult allocate(int fd, int mode, off_t offset, off_t length) {
  TerminationDeferral lifetime;
  Thread* thread = Processor::information().getCurrentThread();
  auto* subsystem = static_cast<PosixSubsystem*>(thread->getParent()->getSubsystem());
  DescriptorLease descriptor;
  if (!subsystem || !subsystem->acquireFileDescriptor(fd, descriptor) ||
      (descriptor->getStatusFlags() & O_PATH) || !descriptor->getFile()) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  if (offset < 0 || length <= 0) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  constexpr unsigned KeepSize = 1;
  if (static_cast<unsigned>(mode) & ~KeepSize) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }
  const int access = descriptor->getStatusFlags() & O_ACCMODE;
  if (access != O_WRONLY && access != O_RDWR) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  File* file = descriptor->getFile();
  if (file->isPipe() || file->isFifo()) {
    SYSCALL_ERROR(IllegalSeek);
    return -1;
  }
  if (file->isDirectory()) {
    SYSCALL_ERROR(IsADirectory);
    return -1;
  }
  if (!file->supportsRegularFileOperations()) {
    SYSCALL_ERROR(DeviceDoesNotExist);
    return -1;
  }
  if (static_cast<uint64_t>(length) > MaximumPosition - static_cast<uint64_t>(offset)) {
    SYSCALL_ERROR(FileTooLarge);
    return -1;
  }
  thread->setErrno(0);
  if (!file->allocateRange(offset, length, mode & KeepSize)) {
    if (!thread->getErrno())
      SYSCALL_ERROR(IoError);
    return -1;
  }
  thread->setErrno(0);
  return 0;
}

FilesystemResult renameWithFlags(int oldDirFd, const char* oldPath, int newDirFd,
                                 const char* newPath, unsigned flags) {
  if ((flags & ~(RenameNoReplace | RenameExchange | RenameWhiteout)) ||
      ((flags & RenameExchange) && (flags & (RenameNoReplace | RenameWhiteout)))) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (flags & (RenameExchange | RenameWhiteout)) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }
  if (!flags)
    return posix_renameat(oldDirFd, oldPath, newDirFd, newPath);
  TerminationDeferral lifetime;
  RenamePath source, destination;
  if (!source.copy(oldPath) || !destination.copy(newPath))
    return -1;
  auto* process = Processor::information().getCurrentThread()->getParent();
  auto context = process->acquireFilesystemContext();
  auto* view = VFS::instance().mountView();
  if (!context || !view) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }
  if (!source.resolve(oldDirFd, *view, context) || !destination.resolve(newDirFd, *view, context))
    return -1;
  LockGuard<Mutex> namespaceGuard(UnixFilesystem::namespaceLock());
  return view->rename(source.parent.path(), source.basename, destination.parent.path(),
                      destination.basename, true,
                      source.copied.endswith('/') || destination.copied.endswith('/'))
             ? 0
             : -1;
}

}  // namespace

int posix_fallocate(int fd, int mode, off_t offset, off_t length) {
  const FilesystemResult result = allocate(fd, mode, offset, length);
  syscallError(result.error);
  return result.value;
}

int posix_renameat2(int oldDirFd, const char* oldPath, int newDirFd, const char* newPath,
                    unsigned flags) {
  const FilesystemResult result = renameWithFlags(oldDirFd, oldPath, newDirFd, newPath, flags);
  syscallError(result.error);
  return result.value;
}
