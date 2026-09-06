/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Pointers.h"

#include <fcntl.h>
#include <limits.h>

#include "FileDescriptor.h"
#include "PosixSubsystem.h"
#include "file-syscalls.h"
#include "modules/system/vfs/ExtendedAttributes.h"
#include "modules/system/vfs/Symlink.h"
#include "xattr-syscalls.h"

namespace {
constexpr size_t MaximumName = Xattr::MaximumNameLength, MaximumValue = Xattr::MaximumValueLength;
enum class TargetKind { Follow, NoFollow, Descriptor };

bool copyString(const char* user, String& snapshot, size_t limit, Error::PosixError tooLong) {
  const auto result = PosixSubsystem::copyUserString(user, snapshot, limit);
  if (result == PosixSubsystem::UserStringSuccess)
    return true;
  syscallError(result == PosixSubsystem::UserStringBadAddress ? Error::BadAddress : tooLong);
  return false;
}

bool copyName(const char* user, String& snapshot) {
  if (!copyString(user, snapshot, MaximumName + 1, Error::BadRange))
    return false;
  if (!snapshot.length()) {
    SYSCALL_ERROR(BadRange);
    return false;
  }
  return true;
}

struct Target {
  File* file = nullptr;
  DescriptorLease descriptor;
  Directory::ChildLease pathLease;

  bool resolve(TargetKind kind, const char* userPath, int fd) {
    auto* thread = Processor::information().getCurrentThread();
    auto* subsystem = static_cast<PosixSubsystem*>(thread->getParent()->getSubsystem());
    if (kind == TargetKind::Descriptor) {
      if (!subsystem || !subsystem->acquireFileDescriptor(fd, descriptor) ||
          (descriptor->getStatusFlags() & O_PATH)) {
        SYSCALL_ERROR(BadFileDescriptor);
        return false;
      }
      file = descriptor->file;
      if (!file) {
        SYSCALL_ERROR(OperationNotSupported);
        return false;
      }
      return true;
    }

    String path;
    if (!copyString(userPath, path, PATH_MAX, Error::NameTooLong))
      return false;
    if (!path.length()) {
      SYSCALL_ERROR(DoesNotExist);
      return false;
    }
    String normalised;
    normalisePath(normalised, path.cstr());
    const bool requireDirectory = path[path.length() - 1] == '/';
    thread->setErrno(0);
    file = findFileWithAbiFallbacks(normalised, pathLease);
    if (kind == TargetKind::Follow || requireDirectory) {
      unsigned followed = 0;
      while (file && file->isSymlink()) {
        if (++followed > 40) {
          SYSCALL_ERROR(LoopExists);
          return false;
        }
        Directory::ChildLease next;
        file = Symlink::fromFile(file)->followLinkRetained(next);
        pathLease.swap(next);
      }
    }
    if (!file) {
      if (!thread->getErrno())
        SYSCALL_ERROR(DoesNotExist);
      return false;
    }
    if (requireDirectory && !file->isDirectory()) {
      SYSCALL_ERROR(NotADirectory);
      return false;
    }
    return true;
  }
};

bool permitted(File* file, const String& name, bool write) {
  if (write && file->getFilesystem() && file->getFilesystem()->isReadOnly()) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return false;
  }
  // Storing policy namespaces requires the corresponding access-control
  // implementation, not just a place to keep their bytes.
  if (name.length() < 5 || !StringView(name.cstr(), 5).compare("user.", 5)) {
    SYSCALL_ERROR(OperationNotSupported);
    return false;
  }
  if (!file->isDirectory() && !file->supportsRegularFileOperations()) {
    syscallError(write ? Error::NotEnoughPermissions : Error::NoData);
    return false;
  }
  if (file->isDirectory() && write) {
    const auto attributes = file->getAttributes();
    Process* process = Processor::information().getCurrentThread()->getParent();
    int64_t uid = process->getEffectiveUserId();
    if (uid < 0)
      uid = process->getUserId();
    if ((attributes.permissions & FILE_STICKY) && uid != 0 &&
        static_cast<uint64_t>(uid) != attributes.uid) {
      SYSCALL_ERROR(NotEnoughPermissions);
      return false;
    }
  }
  if (!VFS::checkAccess(file, !write, write, false))
    return false;
  if (name.length() == 5) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  return true;
}

ssize_t finish(XattrStatus status, size_t count = 0) {
  switch (status) {
    case XattrStatus::Success:
      Processor::information().getCurrentThread()->setErrno(0);
      return static_cast<ssize_t>(count);
    case XattrStatus::Missing:
      SYSCALL_ERROR(NoData);
      break;
    case XattrStatus::Exists:
      SYSCALL_ERROR(FileExists);
      break;
    case XattrStatus::Range:
      SYSCALL_ERROR(BadRange);
      break;
    case XattrStatus::NoSpace:
      SYSCALL_ERROR(NoSpaceLeftOnDevice);
      break;
    case XattrStatus::NoMemory:
      SYSCALL_ERROR(OutOfMemory);
      break;
    case XattrStatus::Unsupported:
      SYSCALL_ERROR(OperationNotSupported);
      break;
    case XattrStatus::Denied:
      SYSCALL_ERROR(PermissionDenied);
      break;
    case XattrStatus::IoError:
      SYSCALL_ERROR(IoError);
      break;
    case XattrStatus::Invalid:
      SYSCALL_ERROR(InvalidArgument);
      break;
    case XattrStatus::ReadOnly:
      SYSCALL_ERROR(ReadOnlyFilesystem);
      break;
  }
  return -1;
}

ssize_t readAttribute(TargetKind kind, const char* path, int fd, const char* userName, void* output,
                      size_t size, bool list) {
  TerminationDeferral lifetime;
  Target target;
  if (!target.resolve(kind, path, fd))
    return -1;
  String name;
  if (!list && !copyName(userName, name))
    return -1;
  const size_t capacity = size < MaximumValue ? size : MaximumValue;
  auto buffer = UniqueArray<uint8_t>::allocate(capacity);
  if (capacity && !buffer) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  // Listing reports supported names even when the caller cannot read values.
  if (!list && !permitted(target.file, name, false))
    return -1;
  size_t required = 0;
  const auto status =
      list ? target.file->listExtendedAttributes(buffer.get(), capacity, required)
           : target.file->getExtendedAttribute(name.view(), buffer.get(), capacity, required);
  if ((status == XattrStatus::Range && capacity == MaximumValue) ||
      (status == XattrStatus::Success && required > MaximumValue)) {
    SYSCALL_ERROR(TooBig);
    return -1;
  }
  if (status != XattrStatus::Success)
    return finish(status);
  if (capacity && required) {
    if (required > capacity) {
      SYSCALL_ERROR(IoError);
      return -1;
    }
    if (!PosixSubsystem::copyToUser(output, buffer.get(), required)) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
  }
  return finish(status, required);
}

int changeAttribute(TargetKind kind, const char* path, int fd, const char* userName,
                    const void* value, size_t size, int flags, bool remove) {
  TerminationDeferral lifetime;
  Target target;
  if (kind == TargetKind::Descriptor && !target.resolve(kind, path, fd))
    return -1;
  if (flags & ~3) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  String name;
  if (!copyName(userName, name))
    return -1;
  if (size > MaximumValue) {
    SYSCALL_ERROR(TooBig);
    return -1;
  }
  auto buffer = UniqueArray<uint8_t>::allocate(size);
  if (size) {
    if (!buffer) {
      SYSCALL_ERROR(OutOfMemory);
      return -1;
    }
    if (!PosixSubsystem::copyFromUser(buffer.get(), value, size)) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
  }
  if (kind != TargetKind::Descriptor && !target.resolve(kind, path, fd))
    return -1;
  if (!permitted(target.file, name, true))
    return -1;
  return static_cast<int>(
      finish(remove ? target.file->removeExtendedAttribute(name.view())
                    : target.file->setExtendedAttribute(name.view(), buffer.get(), size, flags)));
}
}  // namespace

int posix_setxattr(const char* path, const char* name, const void* value, size_t size, int flags) {
  return changeAttribute(TargetKind::Follow, path, -1, name, value, size, flags, false);
}
int posix_lsetxattr(const char* path, const char* name, const void* value, size_t size, int flags) {
  return changeAttribute(TargetKind::NoFollow, path, -1, name, value, size, flags, false);
}
int posix_fsetxattr(int fd, const char* name, const void* value, size_t size, int flags) {
  return changeAttribute(TargetKind::Descriptor, nullptr, fd, name, value, size, flags, false);
}
ssize_t posix_getxattr(const char* path, const char* name, void* value, size_t size) {
  return readAttribute(TargetKind::Follow, path, -1, name, value, size, false);
}
ssize_t posix_lgetxattr(const char* path, const char* name, void* value, size_t size) {
  return readAttribute(TargetKind::NoFollow, path, -1, name, value, size, false);
}
ssize_t posix_fgetxattr(int fd, const char* name, void* value, size_t size) {
  return readAttribute(TargetKind::Descriptor, nullptr, fd, name, value, size, false);
}
ssize_t posix_listxattr(const char* path, char* list, size_t size) {
  return readAttribute(TargetKind::Follow, path, -1, nullptr, list, size, true);
}
ssize_t posix_llistxattr(const char* path, char* list, size_t size) {
  return readAttribute(TargetKind::NoFollow, path, -1, nullptr, list, size, true);
}
ssize_t posix_flistxattr(int fd, char* list, size_t size) {
  return readAttribute(TargetKind::Descriptor, nullptr, fd, nullptr, list, size, true);
}
int posix_removexattr(const char* path, const char* name) {
  return changeAttribute(TargetKind::Follow, path, -1, name, nullptr, 0, 0, true);
}
int posix_lremovexattr(const char* path, const char* name) {
  return changeAttribute(TargetKind::NoFollow, path, -1, name, nullptr, 0, 0, true);
}
int posix_fremovexattr(int fd, const char* name) {
  return changeAttribute(TargetKind::Descriptor, nullptr, fd, name, nullptr, 0, 0, true);
}
