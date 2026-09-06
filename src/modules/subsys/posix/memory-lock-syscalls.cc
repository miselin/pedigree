/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/syscallError.h"

#include "PosixSubsystem.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "vm-syscalls.h"

namespace {
constexpr unsigned OnFault = 1;
constexpr int Current = 1, Future = 2, AllOnFault = 4;

int lockResult(MemoryLockStatus status) {
  switch (status) {
    case MemoryLockStatus::Success:
      Processor::information().getCurrentThread()->setErrno(0);
      return 0;
    case MemoryLockStatus::InvalidRange:
      SYSCALL_ERROR(InvalidArgument);
      break;
    case MemoryLockStatus::Unsupported:
      SYSCALL_ERROR(OperationNotSupported);
      break;
    case MemoryLockStatus::PopulationNoMemory:
      SYSCALL_ERROR(NoMoreProcesses);
      break;
    case MemoryLockStatus::PopulationIoError:
      SYSCALL_ERROR(IoError);
      break;
    case MemoryLockStatus::NoMemory:
    case MemoryLockStatus::Unmapped:
    case MemoryLockStatus::LockLimit:
    case MemoryLockStatus::PopulationInaccessible:
      SYSCALL_ERROR(OutOfMemory);
      break;
  }
  return -1;
}

bool mayLock(PosixSubsystem& subsystem, bool privileged) {
  if (!privileged && !subsystem.memoryLockAccount().limit().current) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return false;
  }
  return true;
}

int lockRange(const void* address, size_t length, MemoryLockMode mode) {
  GRAB_POSIX_SUBSYSTEM(-1);
  MemoryMapManager& manager = MemoryMapManager::instance();
  MemoryMapManager::OperationGuard operation(manager);
  auto& space = Processor::information().getVirtualAddressSpace();
  const bool privileged =
      Processor::information().getCurrentThread()->getParent()->getEffectiveUserId() == 0;
  if (mode != MemoryLockMode::None && !mayLock(*pSubsystem, privileged))
    return -1;

  const size_t mask = PhysicalMemoryManager::getPageSize() - 1;
  const uintptr_t at = reinterpret_cast<uintptr_t>(address);
  const uintptr_t base = at & ~mask;
  if (length > ~size_t(0) - (at & mask) || length + (at & mask) > ~size_t(0) - mask)
    return lockResult(MemoryLockStatus::InvalidRange);
  const size_t extent = (length + (at & mask) + mask) & ~mask;
  if (extent > ~uintptr_t(0) - base)
    return lockResult(MemoryLockStatus::InvalidRange);
  if (!extent)
    return lockResult(MemoryLockStatus::Success);
  if (base < space.getUserStart() || base >= space.getKernelStart() ||
      extent > space.getKernelStart() - base ||
      !space.isAddressValid(reinterpret_cast<void*>(base)) ||
      !space.isAddressValid(reinterpret_cast<void*>(base + extent - 1)))
    return lockResult(MemoryLockStatus::Unmapped);
  return lockResult(manager.lockMemory(space, base, extent, mode, privileged));
}
}  // namespace

int posix_mlock(const void* address, size_t length, unsigned int flags) {
  if (flags & ~OnFault) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  return lockRange(address, length,
                   flags & OnFault ? MemoryLockMode::OnFault : MemoryLockMode::Eager);
}

int posix_munlock(const void* address, size_t length) {
  return lockRange(address, length, MemoryLockMode::None);
}

int posix_mlockall(int flags) {
  if (!flags || flags & ~(Current | Future | AllOnFault) || flags == AllOnFault) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  GRAB_POSIX_SUBSYSTEM(-1);
  auto& manager = MemoryMapManager::instance();
  MemoryMapManager::OperationGuard operation(manager);
  const bool privileged =
      Processor::information().getCurrentThread()->getParent()->getEffectiveUserId() == 0;
  if (!mayLock(*pSubsystem, privileged))
    return -1;
  const auto mode = flags & AllOnFault ? MemoryLockMode::OnFault : MemoryLockMode::Eager;
  return lockResult(
      manager.lockAllMemory(Processor::information().getVirtualAddressSpace(), flags & Current,
                            mode, flags & Future ? mode : MemoryLockMode::None, privileged));
}

int posix_munlockall() {
  GRAB_POSIX_SUBSYSTEM(-1);
  return lockResult(MemoryMapManager::instance().lockAllMemory(
      Processor::information().getVirtualAddressSpace(), true, MemoryLockMode::None,
      MemoryLockMode::None, false));
}
