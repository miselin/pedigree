/* Copyright (c) 2026, Pedigree Developers. */
#include "advisory-lock-syscalls.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include <fcntl.h>
#include <stddef.h>

#include "FileDescriptor.h"
#include "PosixSubsystem.h"
#include "advisory-lock-state.h"
#include "modules/system/vfs/File.h"
#include <sys/file.h>

using namespace PosixAdvisory;

namespace {
struct LinuxFlock {
  int16_t type;
  int16_t whence;
  uint32_t padding;
  int64_t start;
  int64_t length;
  int32_t pid;
  uint32_t tailPadding;
};
static_assert(sizeof(LinuxFlock) == 32, "Linux amd64 flock size");
static_assert(offsetof(LinuxFlock, type) == 0 && offsetof(LinuxFlock, whence) == 2 &&
                  offsetof(LinuxFlock, start) == 8 && offsetof(LinuxFlock, length) == 16 &&
                  offsetof(LinuxFlock, pid) == 24,
              "Linux amd64 flock offsets");

bool regularFile(File* file) {
  if (!file || !file->supportsRegularFileOperations()) {
    SYSCALL_ERROR(OperationNotSupported);
    return false;
  }
  return true;
}

struct Admission {
  PosixSubsystem& subsystem;
  size_t fd;
  const FileDescriptor::OpenFileDescriptionLease& description;
};

bool admit(void* context) {
  const auto& admission = *static_cast<Admission*>(context);
  return admission.subsystem.descriptorMatchesOpenDescription(admission.fd, admission.description);
}
}  // namespace

int posix_advisory_flock(const DescriptorLease& descriptor, int operation) {
  TerminationDeferral lifetime;
  const int lockType = operation & ~LOCK_NB;
  if (lockType != LOCK_SH && lockType != LOCK_EX && lockType != LOCK_UN) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (!descriptor) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  auto description = descriptor->acquireOpenFileDescription();
  File* file = description->getFile();
  if (!regularFile(file))
    return -1;
  if (descriptor->getStatusFlags() & O_PATH) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  Grant request;
  // Ext2 aliases already share this backing token; paths and File pointers do not.
  request.inode = file->futexIdentity();
  request.name = Namespace::Flock;
  request.type =
      lockType == LOCK_UN ? Type::Unlock : (lockType == LOCK_SH ? Type::Read : Type::Write);
  return posix_advisory_apply(description->advisoryOwner(), request, !(operation & LOCK_NB));
}

int posix_advisory_fcntl(PosixSubsystem& subsystem, int fd, const DescriptorLease& descriptor,
                         int command, void* userLock) {
  TerminationDeferral lifetime;
  const bool ofd = command == F_OFD_GETLK || command == F_OFD_SETLK || command == F_OFD_SETLKW;
  const bool query = command == F_GETLK || command == F_OFD_GETLK;
  const bool wait = command == F_SETLKW || command == F_OFD_SETLKW;
  if (!ofd && command != F_GETLK && command != F_SETLK && command != F_SETLKW) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (!descriptor) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  auto description = descriptor->acquireOpenFileDescription();
  File* file = description->getFile();
  if (!regularFile(file))
    return -1;
  const int flags = descriptor->getStatusFlags();
  if (flags & O_PATH) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  LinuxFlock wire;
  if (!PosixSubsystem::copyFromUser(&wire, userLock, sizeof(wire))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if ((wire.type != F_RDLCK && wire.type != F_WRLCK && wire.type != F_UNLCK) ||
      (command == F_GETLK && wire.type == F_UNLCK) || (ofd && wire.pid)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  Grant request;
  request.inode = file->futexIdentity();
  request.type =
      wire.type == F_UNLCK ? Type::Unlock : (wire.type == F_RDLCK ? Type::Read : Type::Write);
  request.pid =
      static_cast<int32_t>(Processor::information().getCurrentThread()->getParent()->getId());
  uint64_t position = 0, fileSize = 0;
  if (wire.whence == 1) {
    auto positionGuard = descriptor->lockPosition();
    position = positionGuard.offset();
  } else if (wire.whence == 2) {
    auto sizeGuard = file->lockWrites();
    fileSize = file->getSize();
  }
  const RangeResult range =
      normalise(wire.whence, wire.start, wire.length, position, fileSize, request.range);
  if (range != RangeResult::Success) {
    syscallError(range == RangeResult::Overflow ? Error::ValueTooLarge : Error::InvalidArgument);
    return -1;
  }
  AdvisoryOwner& owner = ofd ? description->advisoryOwner() : subsystem.advisoryOwner();
  if (query) {
    Grant conflict;
    bool found = false;
    if (posix_advisory_query(owner, request, conflict, found) < 0)
      return -1;
    wire.type = F_UNLCK;
    if (found) {
      wire.type = conflict.type == Type::Read ? F_RDLCK : F_WRLCK;
      wire.whence = 0;
      wire.start = conflict.range.first;
      wire.length =
          conflict.range.last == LastOffset ? 0 : conflict.range.last - conflict.range.first + 1;
      wire.pid = conflict.pid;
    }
    if (!PosixSubsystem::copyToUser(userLock, &wire, sizeof(wire))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    return 0;
  }
  if ((request.type == Type::Read && (flags & O_ACCMODE) == O_WRONLY) ||
      (request.type == Type::Write && (flags & O_ACCMODE) == O_RDONLY)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  Admission admission{subsystem, static_cast<size_t>(fd), description};
  return posix_advisory_apply(owner, request, wait,
                              !ofd && request.type != Type::Unlock ? admit : nullptr, &admission);
}
