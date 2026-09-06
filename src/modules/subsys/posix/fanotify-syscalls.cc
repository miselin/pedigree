/* Copyright (c) 2026, Pedigree Developers. */
#include "fanotify-syscalls.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include <fcntl.h>

#include "FileDescriptor.h"
#include "PosixSubsystem.h"
#include "file-handle-syscalls.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/Filesystem.h"

namespace {
class FanotifyResult {
 public:
  int finish(int result) {
    m_Error = result < 0 ? Processor::information().getCurrentThread()->getErrno() : 0;
    return result;
  }
  ~FanotifyResult() {
    Processor::information().getCurrentThread()->setErrno(m_Error);
  }

 private:
  int m_Error = 0;
};
}  // namespace

int posix_fanotify_init(unsigned flags, unsigned eventFlags) {
  FanotifyResult completion;
  constexpr unsigned CloseOnExec = 1, NonBlock = 2, Classes = 0x0c, ReportFid = 0x200;
  constexpr unsigned KnownFlags =
      0x01 | 0x02 | 0x04 | 0x08 | 0x10 | 0x20 | 0x40 | 0x100 | 0x200 | 0x400 | 0x800;
  constexpr unsigned EventFlags =
      3 | O_APPEND | O_NONBLOCK | O_SYNC | O_DSYNC | O_CLOEXEC | O_LARGEFILE | O_NOATIME;
  if ((flags & ~KnownFlags) || (flags & Classes) == Classes || (eventFlags & ~EventFlags) ||
      (eventFlags & 3) == 3) {
    SYSCALL_ERROR(InvalidArgument);
    return completion.finish(-1);
  }
  if (!posix_effective_root()) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return completion.finish(-1);
  }
  if (!(flags & ReportFid) || (flags & ~(CloseOnExec | NonBlock | ReportFid))) {
    SYSCALL_ERROR(OperationNotSupported);
    return completion.finish(-1);
  }
  TerminationDeferral lifetime;
  auto* thread = Processor::information().getCurrentThread();
  auto* subsystem = static_cast<PosixSubsystem*>(thread->getParent()->getSubsystem());
  if (!subsystem) {
    SYSCALL_ERROR(InvalidArgument);
    return completion.finish(-1);
  }
  auto instance = FanotifyInstance::create();
  if (!instance)
    return completion.finish(-1);
  auto* descriptor =
      new FileDescriptor(nullptr, 0, 0xFFFFFFFF, flags & CloseOnExec ? FD_CLOEXEC : 0,
                         O_RDWR | (flags & NonBlock ? O_NONBLOCK : 0));
  if (!descriptor || !descriptor->acquireOpenFileDescription()) {
    delete descriptor;
    SYSCALL_ERROR(OutOfMemory);
    return completion.finish(-1);
  }
  descriptor->setFanotifyImpl(instance);
  DescriptorLease published;
  const size_t fd = subsystem->installFileDescriptor(descriptor, published);
  thread->setErrno(0);
  return completion.finish(static_cast<int>(fd));
}

int posix_fanotify_mark(int fd, unsigned flags, uint64_t mask, int dirfd, const char* path) {
  FanotifyResult completion;
  constexpr unsigned Add = 1, Remove = 2, DontFollow = 4, OnlyDirectory = 8, Flush = 0x80;
  constexpr unsigned KnownFlags = 0x1ff;
  const unsigned operation = flags & (Add | Remove | Flush);
  if ((flags & ~KnownFlags) || (operation != Add && operation != Remove && operation != Flush) ||
      (flags & 0x110) == 0x110) {
    SYSCALL_ERROR(InvalidArgument);
    return completion.finish(-1);
  }
  if (operation == Flush && flags != Flush) {
    SYSCALL_ERROR(InvalidArgument);
    return completion.finish(-1);
  }
  TerminationDeferral lifetime;
  auto* thread = Processor::information().getCurrentThread();
  auto* subsystem = static_cast<PosixSubsystem*>(thread->getParent()->getSubsystem());
  DescriptorLease descriptor;
  if (!subsystem || !subsystem->acquireFileDescriptor(fd, descriptor)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return completion.finish(-1);
  }
  auto instance = descriptor->getFanotifyImpl();
  if (!instance) {
    SYSCALL_ERROR(InvalidArgument);
    return completion.finish(-1);
  }
  if (flags & ~(Add | Remove | DontFollow | OnlyDirectory | Flush)) {
    SYSCALL_ERROR(OperationNotSupported);
    return completion.finish(-1);
  }
  if (operation == Flush) {
    const int result = instance->flushMarks();
    if (!result)
      thread->setErrno(0);
    return completion.finish(result);
  }
  constexpr uint64_t KnownMask = 0x00001fff | 0x00004000 | 0x000f0000 | 0x08000000 | 0x40000000;
  if (!mask || (mask & ~KnownMask) || (mask & (0x00070000 | 0x00004000))) {
    SYSCALL_ERROR(InvalidArgument);
    return completion.finish(-1);
  }
  constexpr uint64_t SupportedMask = 0x02 | 0x04 | 0x08 | 0x10 | 0x20;
  if (mask & ~SupportedMask) {
    SYSCALL_ERROR(OperationNotSupported);
    return completion.finish(-1);
  }
  PosixHandleTarget target;
  if (!path && dirfd == AT_FDCWD) {
    SYSCALL_ERROR(BadFileDescriptor);
    return completion.finish(-1);
  }
  if (!target.resolve(dirfd, path, !(flags & DontFollow), false, true))
    return completion.finish(-1);
  if ((flags & OnlyDirectory) && !target.file->isDirectory()) {
    SYSCALL_ERROR(NotADirectory);
    return completion.finish(-1);
  }
  if (!VFS::checkAccess(target.file, true, false, false))
    return completion.finish(-1);
  FileHandle handle;
  FileSystemId fsid;
  auto status = target.mount.filesystem()->encodeFileHandle(*target.file, handle);
  if (status != FileHandleStatus::Success)
    return completion.finish(posix_handle_error(status));
  status = target.mount.filesystem()->fileHandleFsid(fsid);
  if (status != FileHandleStatus::Success)
    return completion.finish(posix_handle_error(status));
  const int result =
      instance->changeMark(*target.file, target.mount, handle, fsid, mask, operation == Remove);
  if (!result)
    thread->setErrno(0);
  return completion.finish(result);
}
