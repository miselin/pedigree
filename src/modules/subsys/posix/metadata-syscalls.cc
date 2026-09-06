/* Copyright (c) 2026, Pedigree Developers. */
#include "metadata-syscalls.h"
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
#include "file-metadata.h"
#include "file-syscalls.h"
#include "metadata-abi.h"
#include "modules/system/vfs/MountView.h"
#include "modules/system/vfs/VFS.h"
#include <sys/stat.h>

namespace {
struct MetadataResult {
  MetadataResult(int result)
      : value(result),
        error(result < 0 ? Processor::information().getCurrentThread()->getErrno() : 0) {}
  int value, error;
};

class MetadataPath {
 private:
  TerminationDeferral lifetime;

 public:
  enum class Input { Path, OpenDescriptor, NullablePath };
  bool resolve(int dirfd, const char* user, int flags, Input input = Input::Path) {
    const bool descriptorOnly = input == Input::OpenDescriptor;
    if (!descriptorOnly && (user || input != Input::NullablePath)) {
      const auto status = PosixSubsystem::copyUserString(user, copied, PATH_MAX);
      if (status != PosixSubsystem::UserStringSuccess) {
        syscallError(status == PosixSubsystem::UserStringBadAddress ? Error::BadAddress
                                                                    : Error::NameTooLong);
        return false;
      }
      if (!copied.length() && !(flags & AT_EMPTY_PATH)) {
        SYSCALL_ERROR(DoesNotExist);
        return false;
      }
    }
    auto* process = Processor::information().getCurrentThread()->getParent();
    auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
    context = process->acquireFilesystemContext();
    auto* view = VFS::instance().mountView();
    const bool absolute = copied.length() && copied[0] == '/';
    FilesystemPathRef start;
    if (!absolute && (descriptorOnly || dirfd != AT_FDCWD)) {
      if (!subsystem || !subsystem->acquireFileDescriptor(dirfd, descriptor) ||
          !descriptor->getFile() || (descriptorOnly && (descriptor->getStatusFlags() & O_PATH))) {
        SYSCALL_ERROR(BadFileDescriptor);
        return false;
      }
      file = descriptor->getFile();
      retainedPath = descriptor->openingPath();
      if (!copied.length())
        return true;
      if (!file->isDirectory() || !retainedPath) {
        SYSCALL_ERROR(NotADirectory);
        return false;
      }
      start = retainedPath;
    }
    if (!context || !view) {
      SYSCALL_ERROR(DoesNotExist);
      return false;
    }
    if (!copied.length()) {
      FilesystemContextSnapshot snapshot;
      if (!context->snapshot(snapshot) || !snapshot.cwd) {
        SYSCALL_ERROR(DoesNotExist);
        return false;
      }
      retainedPath = snapshot.cwd;
    } else {
      String normalised;
      normalisePath(normalised, copied.cstr());
      VfsMountView::ResolveOptions options;
      options.followFinal = !(flags & AT_SYMLINK_NOFOLLOW);
      if (!view->resolve(context, start, normalised, options, retainedPath))
        return false;
    }
    file = retainedPath->node();
    return file != nullptr;
  }

  File* file = nullptr;
  String copied;
  FilesystemPathRef retainedPath;

 private:
  DescriptorLease descriptor;
  FilesystemContextRef context;
};

bool writableFilesystem(File* file) {
  if (file->getFilesystem() && file->getFilesystem()->isReadOnly()) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return false;
  }
  return true;
}

MetadataResult truncatePath(const char* path, off_t length) {
  if (length < 0) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  MetadataPath target;
  if (!target.resolve(AT_FDCWD, path, 0))
    return -1;
  if (target.file->isDirectory()) {
    SYSCALL_ERROR(IsADirectory);
    return -1;
  }
  if (!target.file->supportsRegularFileOperations()) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (!writableFilesystem(target.file) || !VFS::checkAccess(target.file, false, true, false))
    return -1;
  return target.file->resize(static_cast<size_t>(length)) ? 0 : -1;
}

MetadataResult linkOwnership(const char* path, uid_t owner, gid_t group) {
  MetadataPath target;
  if (!target.resolve(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW) || !writableFilesystem(target.file))
    return -1;
  return posix_chown_file(target.file, owner, group) ? 0 : -1;
}

MetadataResult chmodAt(int dirfd, const char* path, mode_t mode, int flags) {
  if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  MetadataPath target;
  if (!target.resolve(dirfd, path, flags) || !writableFilesystem(target.file))
    return -1;
  if (target.file->isSymlink()) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }
  return posix_chmod_file(target.file, mode) ? 0 : -1;
}

MetadataResult updateTimes(int dirfd, const char* path, const void* userTimes, int flags) {
  using namespace PosixMetadata;
  Timespec times[2] = {{0, TimeNow}, {0, TimeNow}};
  if (userTimes && !PosixSubsystem::copyFromUser(times, userTimes, sizeof(times))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  // The Linux ABI treats two OMIT values as a no-op before path or flag lookup.
  if (times[0].nanoseconds == TimeOmit && times[1].nanoseconds == TimeOmit)
    return 0;
  for (const auto& time : times) {
    if (!validNanoseconds(time.nanoseconds)) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    if (time.nanoseconds != TimeNow && time.nanoseconds != TimeOmit &&
        (time.seconds < 0 || static_cast<uint64_t>(time.seconds) > UINT32_MAX)) {
      SYSCALL_ERROR(ValueTooLarge);
      return -1;
    }
  }
  const bool descriptorOnly = !path && dirfd != AT_FDCWD;
  if ((flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)) || (descriptorOnly && flags)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  MetadataPath target;
  if (!target.resolve(
          dirfd, path, flags,
          descriptorOnly ? MetadataPath::Input::OpenDescriptor : MetadataPath::Input::Path) ||
      !writableFilesystem(target.file))
    return -1;
  FilesystemCredentials credentials;
  if (!Process::currentFilesystemCredentials(credentials)) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }
  const bool owner = !credentials.uid || credentials.uid == target.file->getUid();
  const bool touch = times[0].nanoseconds == TimeNow && times[1].nanoseconds == TimeNow;
  if (!owner && !touch) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }
  if (!owner && !VFS::checkAccess(target.file, false, true, false))
    return -1;
  const Time::Timestamp now = Time::getTime();
  target.file->setTimes(times[0].nanoseconds == TimeNow ? now : times[0].seconds,
                        times[1].nanoseconds == TimeNow ? now : times[1].seconds,
                        times[0].nanoseconds != TimeOmit, times[1].nanoseconds != TimeOmit);
  return 0;
}

MetadataResult extendedStat(int dirfd, const char* path, int flags, unsigned mask, void* output) {
  constexpr unsigned SyncFlags = 0x6000, NoAutomount = 0x800, ReservedMask = 0x80000000U;
  if ((static_cast<unsigned>(flags) &
       ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW | NoAutomount | SyncFlags)) ||
      (flags & SyncFlags) == SyncFlags || (mask & ReservedMask)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  MetadataPath target;
  if (!target.resolve(
          dirfd, path, flags,
          (flags & AT_EMPTY_PATH) ? MetadataPath::Input::NullablePath : MetadataPath::Input::Path))
    return -1;
  struct stat ordinary = {};
  if (!posix_stat_file(target.copied.cstr(), target.file, &ordinary))
    return -1;
  PosixMetadata::Statx snapshot = {};
  snapshot.mask = 0x7ff;
  snapshot.blockSize = ordinary.st_blksize;
  snapshot.links = ordinary.st_nlink;
  snapshot.uid = ordinary.st_uid;
  snapshot.gid = ordinary.st_gid;
  snapshot.mode = ordinary.st_mode;
  snapshot.inode = ordinary.st_ino;
  snapshot.size = ordinary.st_size;
  snapshot.blocks = ordinary.st_blocks;
  snapshot.accessed.seconds = ordinary.st_atime;
  snapshot.changed.seconds = ordinary.st_ctime;
  snapshot.modified.seconds = ordinary.st_mtime;
  if (target.retainedPath) {
    snapshot.mountId = VFS::instance().mountView()->attachmentId(target.retainedPath);
    if (snapshot.mountId)
      snapshot.mask |= 0x1000;
  }
  snapshot.deviceMajor = PosixMetadata::deviceMajor(ordinary.st_rdev);
  snapshot.deviceMinor = PosixMetadata::deviceMinor(ordinary.st_rdev);
  snapshot.filesystemMajor = PosixMetadata::deviceMajor(ordinary.st_dev);
  snapshot.filesystemMinor = PosixMetadata::deviceMinor(ordinary.st_dev);
  if (!PosixSubsystem::copyToUser(output, &snapshot, sizeof(snapshot))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return 0;
}
}  // namespace

int posix_truncate(const char* path, off_t length) {
  const MetadataResult result = truncatePath(path, length);
  syscallError(result.error);
  return result.value;
}
int posix_lchown(const char* path, uid_t owner, gid_t group) {
  const MetadataResult result = linkOwnership(path, owner, group);
  syscallError(result.error);
  return result.value;
}
int posix_utimensat(int dirfd, const char* path, const void* times, int flags) {
  const MetadataResult result = updateTimes(dirfd, path, times, flags);
  syscallError(result.error);
  return result.value;
}
int posix_statx(int dirfd, const char* path, int flags, unsigned mask, void* output) {
  const MetadataResult result = extendedStat(dirfd, path, flags, mask, output);
  syscallError(result.error);
  return result.value;
}
int posix_fchmodat2(int dirfd, const char* path, mode_t mode, int flags) {
  const MetadataResult result = chmodAt(dirfd, path, mode, flags);
  syscallError(result.error);
  return result.value;
}
