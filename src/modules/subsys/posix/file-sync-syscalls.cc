/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include <fcntl.h>

#include "FileDescriptor.h"
#include "PosixSubsystem.h"
#include "file-sync-syscalls.h"
#include "modules/system/vfs/File.h"

namespace {
constexpr uint64_t MaximumPosition = 0x7fffffffffffffffULL;
constexpr size_t PrefetchWindow = 2 * 1024 * 1024;
constexpr int AdviceNormal = 0, AdviceWillNeed = 3, AdviceNoReuse = 5;

bool acquireSyncDescriptor(int fd, DescriptorLease& descriptor) {
  Thread* thread = Processor::information().getCurrentThread();
  auto* subsystem = static_cast<PosixSubsystem*>(thread->getParent()->getSubsystem());
  if (!subsystem || !subsystem->acquireFileDescriptor(fd, descriptor) ||
      (descriptor->getStatusFlags() & O_PATH)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return false;
  }
  return true;
}

int finishSync(Thread* thread, bool succeeded) {
  if (!succeeded) {
    if (!thread->getErrno())
      SYSCALL_ERROR(IoError);
    return -1;
  }
  thread->setErrno(0);
  return 0;
}

int prefetch(File* file, uint64_t offset, uint64_t length, bool throughEof) {
  Thread* thread = Processor::information().getCurrentThread();
  // Freeze the EOF bound while a bounded cache fill runs, without changing the OFD position.
  auto guard = file->lockWrites();
  const uint64_t size = file->getSize();
  if (offset >= size)
    return 0;
  uint64_t count = size - offset;
  if (!throughEof && length < count)
    count = length;
  if (count > PrefetchWindow)
    count = PrefetchWindow;
  thread->setErrno(0);
  const uint64_t read = file->read(offset, count, 0);
  return finishSync(thread, read == count);
}
}  // namespace

int posix_fdatasync(int fd) {
  TerminationDeferral lifetime;
  DescriptorLease descriptor;
  if (!acquireSyncDescriptor(fd, descriptor))
    return -1;
  File* file = descriptor->getFile();
  if (!file || (!file->supportsRegularFileOperations() && !file->isDirectory())) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  Thread* thread = Processor::information().getCurrentThread();
  thread->setErrno(0);
  // The existing backend also flushes metadata not required for data retrieval.
  return finishSync(thread, file->sync());
}

int posix_readahead(int fd, off_t offset, size_t count) {
  TerminationDeferral lifetime;
  DescriptorLease descriptor;
  if (!acquireSyncDescriptor(fd, descriptor))
    return -1;
  const int access = descriptor->getStatusFlags() & O_ACCMODE;
  if (access != O_RDONLY && access != O_RDWR) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  File* file = descriptor->getFile();
  if (!file || !file->supportsRegularFileOperations() || offset < 0 || count > MaximumPosition) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  // Linux routes this through WILLNEED, whose zero length covers the suffix.
  return prefetch(file, offset, count, !count);
}

int posix_fadvise64(int fd, off_t offset, off_t length, int advice) {
  TerminationDeferral lifetime;
  DescriptorLease descriptor;
  if (!acquireSyncDescriptor(fd, descriptor))
    return -1;
  File* file = descriptor->getFile();
  if (file && (file->isPipe() || file->isFifo())) {
    SYSCALL_ERROR(IllegalSeek);
    return -1;
  }
  if (!file || !file->supportsRegularFileOperations() || offset < 0 || length < 0 ||
      advice < AdviceNormal || advice > AdviceNoReuse) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (advice != AdviceWillNeed) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }
  return prefetch(file, offset, length, !length);
}

int posix_sync_file_range(int fd, off_t offset, off_t length, unsigned flags) {
  TerminationDeferral lifetime;
  DescriptorLease descriptor;
  if (!acquireSyncDescriptor(fd, descriptor))
    return -1;
  constexpr unsigned ValidFlags = 1 | 2 | 4;
  if ((flags & ~ValidFlags) || offset < 0 || length < 0 ||
      static_cast<uint64_t>(length) > MaximumPosition - static_cast<uint64_t>(offset)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  File* file = descriptor->getFile();
  if (!file || (!file->supportsRegularFileOperations() && !file->isDirectory())) {
    SYSCALL_ERROR(IllegalSeek);
    return -1;
  }
  if (!flags)
    return 0;
  Thread* thread = Processor::information().getCurrentThread();
  thread->setErrno(0);
  // All admitted combinations complete range writeback synchronously, including waits.
  return finishSync(thread, file->syncRange(offset, length));
}
