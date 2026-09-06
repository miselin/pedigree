/* Copyright (c) 2026, Pedigree Developers. */
#include "execveat-syscalls.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/StaticString.h"

#include <fcntl.h>
#include <limits.h>

#include "PosixSubsystem.h"
#include "file-syscalls.h"
#include "modules/system/vfs/MemoryMappedFile.h"

namespace {
struct ExecResult {
  ExecResult(int result)
      : value(result),
        error(result < 0 ? Processor::information().getCurrentThread()->getErrno() : 0) {}
  int value;
  int error;
};

bool snapshotArguments(const char** pointers, Vector<String>& output, size_t& remaining) {
  uintptr_t cursor = reinterpret_cast<uintptr_t>(pointers);
  while (cursor) {
    const char* argument = nullptr;
    if (!PosixSubsystem::copyFromUser(&argument, reinterpret_cast<void*>(cursor),
                                      sizeof(argument))) {
      SYSCALL_ERROR(BadAddress);
      return false;
    }
    if (!argument)
      return true;
    if (remaining <= sizeof(uintptr_t)) {
      SYSCALL_ERROR(TooBig);
      return false;
    }
    remaining -= sizeof(uintptr_t);
    String value;
    const auto result = PosixSubsystem::copyUserString(argument, value, remaining);
    if (result != PosixSubsystem::UserStringSuccess) {
      syscallError(result == PosixSubsystem::UserStringBadAddress ? Error::BadAddress
                                                                  : Error::TooBig);
      return false;
    }
    remaining -= value.length() + 1;
    output.pushBack(value);
    if (cursor > ~uintptr_t(0) - sizeof(uintptr_t)) {
      SYSCALL_ERROR(BadAddress);
      return false;
    }
    cursor += sizeof(uintptr_t);
  }
  return true;
}

ExecResult execveat(int dirfd, const char* path, const char** argv, const char** env, int flags,
                    SyscallState& state) {
  if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  String pathname;
  const auto copied = PosixSubsystem::copyUserString(path, pathname, PATH_MAX);
  if (copied != PosixSubsystem::UserStringSuccess) {
    syscallError(copied == PosixSubsystem::UserStringBadAddress ? Error::BadAddress
                                                                : Error::NameTooLong);
    return -1;
  }
  if (!pathname.length() && !(flags & AT_EMPTY_PATH)) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }

  auto* process = Processor::information().getCurrentThread()->getParent();
  auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  ResolvedPath targetLease;
  DescriptorLease descriptor;
  FilesystemPathRef start;
  File* target = nullptr;
  const bool absolute = pathname.length() && pathname[0] == '/';
  const bool descriptorPath = !absolute && dirfd != AT_FDCWD;
  if (descriptorPath) {
    if (dirfd < 0 || !subsystem->acquireFileDescriptor(dirfd, descriptor)) {
      SYSCALL_ERROR(BadFileDescriptor);
      return -1;
    }
    target = descriptor->getFile();
    start = descriptor->openingPath();
    if (!target || (pathname.length() && (!target->isDirectory() || !start))) {
      syscallError(pathname.length() ? Error::NotADirectory : Error::PermissionDenied);
      return -1;
    }
    targetLease.retain(start);
  } else if (!pathname.length()) {
    auto context = process->acquireFilesystemContext();
    FilesystemContextSnapshot snapshot;
    if (!context || !context->snapshot(snapshot) || !snapshot.cwd) {
      SYSCALL_ERROR(DoesNotExist);
      return -1;
    }
    targetLease.retain(snapshot.cwd);
    target = targetLease.get();
  }

  if (pathname.length()) {
    String resolvedPath;
    normalisePath(resolvedPath, pathname.cstr());
    target = subsystem->findFileRetained(resolvedPath, targetLease, start);
  }
  if (!target) {
    if (!Processor::information().getCurrentThread()->getErrno())
      SYSCALL_ERROR(DoesNotExist);
    return -1;
  }
  if ((flags & AT_SYMLINK_NOFOLLOW) && target->isSymlink()) {
    SYSCALL_ERROR(LoopExists);
    return -1;
  }
  if (target->isSymlink()) {
    target = subsystem->followFile(targetLease);
    if (!target)
      return -1;
  }
  if (target->isDirectory() || target->isPipe() || target->isFifo() || target->isSocket()) {
    SYSCALL_ERROR(PermissionDenied);
    return -1;
  }

  String originalName(pathname);
  if (descriptorPath) {
    NormalStaticString prefix("/dev/fd/");
    prefix.append(static_cast<size_t>(dirfd));
    originalName.assign(prefix, prefix.length());
    if (pathname.length()) {
      originalName += "/";
      originalName += pathname;
    }
  }
  Vector<String> arguments, environment;
  {
    MemoryMapManager::OperationGuard mappingGuard(MemoryMapManager::instance());
    size_t remaining = PosixSubsystem::MaximumExecArgumentBytes - 2 * sizeof(uintptr_t);
    if (originalName.length() >= remaining) {
      SYSCALL_ERROR(TooBig);
      return -1;
    }
    remaining -= originalName.length() + 1;
    if (!snapshotArguments(argv, arguments, remaining) ||
        !snapshotArguments(env, environment, remaining))
      return -1;
  }
  const bool inaccessible = descriptorPath && (descriptor->fdflags & FD_CLOEXEC);
  const auto opening = targetLease.path()
                           ? targetLease.path()
                           : (descriptor ? descriptor->openingPath() : FilesystemPathRef());
  const bool invoked =
      opening
          ? subsystem->invoke(opening, originalName, arguments, environment, state, inaccessible)
          : subsystem->invoke(target, originalName, arguments, environment, state, inaccessible);
  return invoked ? 0 : -1;
}

}  // namespace

int posix_execveat(int dirfd, const char* path, const char** argv, const char** env, int flags,
                   SyscallState& state) {
  const ExecResult result = execveat(dirfd, path, argv, env, flags, state);
  syscallError(result.error);
  return result.value;
}
