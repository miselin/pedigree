/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/syscallError.h"

#include <fcntl.h>

#include "DevFs-block.h"
#include "file-handle-syscalls.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "swap-syscalls.h"

namespace {
int operate(const char* path, unsigned flags, bool enable) {
  TerminationDeferral lifetime;
  if (!posix_effective_root()) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }
  if (flags & ~0x7ffffU) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (flags) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }
  SwapStatus status;
  {
    PosixHandleTarget target;
    if (!target.resolve(AT_FDCWD, path, true, false))
      return -1;
    if (!target.file->isBlockDevice()) {
      SYSCALL_ERROR(NotABlockDevice);
      return -1;
    }
    const uint64_t number = target.file->deviceNumber();
    if (!PosixBlock::valid(number, PosixBlock::PhysicalMajor)) {
      if (PosixBlock::valid(number, PosixBlock::MountedMajor))
        SYSCALL_ERROR(DeviceBusy);
      else
        SYSCALL_ERROR(OperationNotSupported);
      return -1;
    }
    const uint32_t endpoint = PosixBlock::minor(number);
    auto& manager = MemoryMapManager::instance();
    status = enable ? manager.activateSwap(endpoint) : manager.deactivateSwap(endpoint);
  }
  switch (status) {
    case SwapStatus::Success:
      syscallError(0);
      return 0;
    case SwapStatus::Unsupported:
      SYSCALL_ERROR(OperationNotSupported);
      break;
    case SwapStatus::Busy:
      SYSCALL_ERROR(DeviceBusy);
      break;
    case SwapStatus::NoMemory:
      SYSCALL_ERROR(OutOfMemory);
      break;
    case SwapStatus::IoError:
      SYSCALL_ERROR(IoError);
      break;
    default:
      SYSCALL_ERROR(InvalidArgument);
      break;
  }
  return -1;
}
}  // namespace
int posix_swapon(const char* path, int flags) {
  return operate(path, static_cast<unsigned>(flags), true);
}
int posix_swapoff(const char* path) {
  return operate(path, 0, false);
}
