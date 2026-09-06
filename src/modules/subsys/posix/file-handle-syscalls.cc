/* Copyright (c) 2026, Pedigree Developers. */
#include "file-handle-syscalls.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include <fcntl.h>
#include <limits.h>

#include "file-syscalls.h"
#include "modules/system/vfs/MountView.h"

namespace {
struct UserHandleHeader {
  uint32_t length;
  int32_t type;
};
static_assert(sizeof(UserHandleHeader) == 8, "Linux file_handle header");
static_assert(__builtin_offsetof(FileHandle, bytes) == 8, "FileHandle payload layout");
constexpr size_t MaximumHandle = sizeof(FileHandle::bytes);

Thread* currentThread() {
  return Processor::information().getCurrentThread();
}

// Backing retirement can run on the last lexical File release. Preserve the
// syscall's selected result across those callbacks, including late copy faults.
class HandleResult {
 public:
  int finish(int value) {
    error = value < 0 ? currentThread()->getErrno() : 0;
    return value;
  }
  ~HandleResult() {
    currentThread()->setErrno(error);
  }

 private:
  int error = 0;
};

bool importHeader(const void* user, UserHandleHeader& header, bool allowZero) {
  if (!PosixSubsystem::copyFromUser(&header, user, sizeof(header))) {
    SYSCALL_ERROR(BadAddress);
    return false;
  }
  if (header.length > MaximumHandle || (!allowZero && !header.length)) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  return true;
}

bool validOpenFlags(int flags) {
  constexpr int accepted = O_RDONLY | O_WRONLY | O_RDWR | O_CLOEXEC | O_NONBLOCK | O_APPEND |
                           O_TRUNC | O_LARGEFILE | O_DIRECTORY;
  constexpr int unsupported = O_CREAT | O_EXCL | O_NOCTTY | O_DSYNC | O_ASYNC | O_DIRECT | O_SYNC |
                              O_NOFOLLOW | O_NOATIME | O_PATH | (O_TMPFILE & ~O_DIRECTORY);
  if ((flags & ~(accepted | unsupported)) || (flags & 3) == 3) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  if (flags & unsupported) {
    SYSCALL_ERROR(OperationNotSupported);
    return false;
  }
  return true;
}
}  // namespace

int posix_handle_error(FileHandleStatus status) {
  switch (status) {
    case FileHandleStatus::Success:
      currentThread()->setErrno(0);
      return 0;
    case FileHandleStatus::Unsupported:
      SYSCALL_ERROR(OperationNotSupported);
      break;
    case FileHandleStatus::Stale:
      SYSCALL_ERROR(StaleFileHandle);
      break;
    case FileHandleStatus::Invalid:
      SYSCALL_ERROR(InvalidArgument);
      break;
    case FileHandleStatus::NoMemory:
      SYSCALL_ERROR(OutOfMemory);
      break;
    case FileHandleStatus::IoError:
      SYSCALL_ERROR(IoError);
      break;
  }
  return -1;
}

bool posix_effective_root() {
  Process* process = currentThread()->getParent();
  int64_t uid = process->getEffectiveUserId();
  if (uid < 0)
    uid = process->getUserId();
  return uid == 0;
}

bool PosixHandleTarget::resolve(int dirfd, const char* userPath, bool follow, bool allowEmpty,
                                bool nullAsDescriptor) {
  file = nullptr;
  pathLease.reset();
  descriptor.reset();
  mount.reset();
  attachmentId = 0;
  Process* process = currentThread()->getParent();
  auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  if (!subsystem) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  String path;
  const bool nullTarget = !userPath && nullAsDescriptor;
  if (!nullTarget) {
    const auto status = PosixSubsystem::copyUserString(userPath, path, PATH_MAX);
    if (status != PosixSubsystem::UserStringSuccess) {
      syscallError(status == PosixSubsystem::UserStringTooLong ? Error::NameTooLong
                                                               : Error::BadAddress);
      return false;
    }
    if (!path.length() && !allowEmpty) {
      SYSCALL_ERROR(DoesNotExist);
      return false;
    }
  }
  const bool direct = nullTarget || !path.length();
  const bool absolute = !direct && path[0] == '/';
  FilesystemPathRef start;
  if (!absolute) {
    if (dirfd == AT_FDCWD) {
      if (direct) {
        auto context = process->acquireFilesystemContext();
        FilesystemContextSnapshot snapshot;
        if (!context || !context->snapshot(snapshot) || !snapshot.cwd) {
          SYSCALL_ERROR(DoesNotExist);
          return false;
        }
        start = snapshot.cwd;
        file = start->node();
      }
    } else {
      if (!subsystem->acquireFileDescriptor(dirfd, descriptor)) {
        SYSCALL_ERROR(BadFileDescriptor);
        return false;
      }
      file = descriptor->getFile();
      start = descriptor->openingPath();
      if (!file) {
        syscallError(direct ? Error::OperationNotSupported : Error::NotADirectory);
        return false;
      }
      if (!direct && (!start || !file->isDirectory())) {
        SYSCALL_ERROR(NotADirectory);
        return false;
      }
    }
  }
  if (direct) {
    pathLease.retain(start);
  } else {
    String normalised;
    normalisePath(normalised, path.cstr());
    currentThread()->setErrno(0);
    const bool directoryRequired = path[path.length() - 1] == '/';
    file = findFilePath(normalised, pathLease, start, follow || directoryRequired);
    if (!file) {
      if (!currentThread()->getErrno())
        SYSCALL_ERROR(DoesNotExist);
      return false;
    }
    if (directoryRequired && !file->isDirectory()) {
      SYSCALL_ERROR(NotADirectory);
      return false;
    }
  }
  if (!file->getFilesystem()) {
    SYSCALL_ERROR(OperationNotSupported);
    return false;
  }
  if (!VFS::instance().acquireMount(file->getFilesystem(), mount)) {
    // Private filesystems such as memfd have no mounted export lifetime.
    SYSCALL_ERROR(OperationNotSupported);
    return false;
  }
  auto* view = VFS::instance().mountView();
  attachmentId = view ? view->attachmentId(pathLease.path()) : 0;
  return true;
}

int posix_name_to_handle_at(int dirfd, const char* path, void* userHandle, int* userMount,
                            int flags) {
  HandleResult completion;
  TerminationDeferral lifetime;
  if (flags & ~(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH)) {
    SYSCALL_ERROR(InvalidArgument);
    return completion.finish(-1);
  }
  PosixHandleTarget target;
  if (!target.resolve(dirfd, path, flags & AT_SYMLINK_FOLLOW, flags & AT_EMPTY_PATH))
    return completion.finish(-1);
  if (!target.attachmentId) {
    SYSCALL_ERROR(OperationNotSupported);
    return completion.finish(-1);
  }
  if (target.attachmentId > INT_MAX) {
    SYSCALL_ERROR(ValueTooLarge);
    return completion.finish(-1);
  }
  UserHandleHeader input;
  if (!importHeader(userHandle, input, true))
    return completion.finish(-1);
  FileHandle encoded;
  const auto status = target.mount.filesystem()->encodeFileHandle(*target.file, encoded);
  if (status != FileHandleStatus::Success)
    return completion.finish(posix_handle_error(status));
  if (!encoded.length || encoded.length > MaximumHandle) {
    SYSCALL_ERROR(IoError);
    return completion.finish(-1);
  }
  const bool overflow = input.length < encoded.length;
  if (overflow)
    encoded.type = 255;
  const int mountId = static_cast<int>(target.attachmentId);
  if (!PosixSubsystem::copyToUser(userMount, &mountId, sizeof(mountId)) ||
      !PosixSubsystem::copyToUser(userHandle, &encoded,
                                  sizeof(UserHandleHeader) + (overflow ? 0 : encoded.length))) {
    SYSCALL_ERROR(BadAddress);
    return completion.finish(-1);
  }
  if (overflow) {
    SYSCALL_ERROR(ValueTooLarge);
    return completion.finish(-1);
  }
  return completion.finish(0);
}

int posix_open_by_handle_at(int mountfd, const void* userHandle, int flags) {
  HandleResult completion;
  TerminationDeferral lifetime;
  PosixHandleTarget target;
  if (!target.resolve(mountfd, nullptr, false, false, true))
    return completion.finish(-1);
  if (!posix_effective_root()) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return completion.finish(-1);
  }
  if (!target.attachmentId) {
    SYSCALL_ERROR(OperationNotSupported);
    return completion.finish(-1);
  }
  UserHandleHeader input;
  if (!importHeader(userHandle, input, false))
    return completion.finish(-1);
  FileHandle handle;
  handle.length = input.length;
  handle.type = input.type;
  const uintptr_t user = reinterpret_cast<uintptr_t>(userHandle);
  if (user > ~uintptr_t(0) - sizeof(input) ||
      !PosixSubsystem::copyFromUser(
          handle.bytes, reinterpret_cast<const void*>(user + sizeof(input)), handle.length)) {
    SYSCALL_ERROR(BadAddress);
    return completion.finish(-1);
  }
  RetainedFile decoded;
  const auto status = target.mount.filesystem()->decodeFileHandle(handle, decoded);
  if (status != FileHandleStatus::Success)
    return completion.finish(posix_handle_error(status));
  if (!validOpenFlags(flags))
    return completion.finish(-1);
  File* file = decoded.get();
  if (!file || !file->supportsRegularFileOperations()) {
    SYSCALL_ERROR(OperationNotSupported);
    return completion.finish(-1);
  }
  if (flags & O_DIRECTORY) {
    SYSCALL_ERROR(NotADirectory);
    return completion.finish(-1);
  }
  const int access = flags & 3;
  const bool writes = access != O_RDONLY || (flags & O_TRUNC);
  if (writes && target.mount.filesystem()->isReadOnly()) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return completion.finish(-1);
  }
  if (!VFS::checkAccess(file, access != O_WRONLY, writes, false))
    return completion.finish(-1);
  FilesystemPathRef openedPath;
  auto* view = VFS::instance().mountView();
  if (!view || !view->pathForNode(target.pathLease.path(), file, openedPath)) {
    if (!currentThread()->getErrno())
      SYSCALL_ERROR(StaleFileHandle);
    return completion.finish(-1);
  }
  const int statusFlags = flags & (3 | O_APPEND | O_NONBLOCK | O_LARGEFILE);
  auto* descriptor = new FileDescriptor(openedPath, 0, 0xffffffff,
                                        flags & O_CLOEXEC ? FD_CLOEXEC : 0, statusFlags);
  if (!descriptor || !descriptor->acquireOpenFileDescription()) {
    const int result = completion.finish(posix_handle_error(FileHandleStatus::NoMemory));
    delete descriptor;
    return result;
  }
  if ((flags & O_TRUNC) && !file->resize(0)) {
    if (!currentThread()->getErrno())
      SYSCALL_ERROR(IoError);
    const int result = completion.finish(-1);
    delete descriptor;
    return result;
  }
  auto* subsystem = static_cast<PosixSubsystem*>(currentThread()->getParent()->getSubsystem());
  DescriptorLease published;
  const size_t fd = subsystem->installFileDescriptor(descriptor, published);
  file->publishEvent(FileEvents::Open);
  return completion.finish(static_cast<int>(fd));
}
