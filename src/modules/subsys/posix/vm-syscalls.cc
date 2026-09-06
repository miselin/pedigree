/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Pointers.h"

#include <limits.h>

#include "PosixSubsystem.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/VFS.h"
#include "vm-syscalls.h"

namespace {
constexpr int MayMove = 1, Fixed = 2, DontUnmap = 4;
constexpr int Discard = 4;

bool roundLength(size_t length, size_t& rounded) {
  const size_t mask = PhysicalMemoryManager::getPageSize() - 1;
  if (length > ~size_t(0) - mask)
    return false;
  rounded = (length + mask) & ~mask;
  return true;
}

bool userRange(uintptr_t base, size_t length) {
  auto& space = Processor::information().getVirtualAddressSpace();
  return base >= space.getUserStart() && base < space.getKernelStart() &&
         length <= space.getKernelStart() - base &&
         space.isAddressValid(reinterpret_cast<void*>(base)) &&
         (!length || space.isAddressValid(reinterpret_cast<void*>(base + length - 1)));
}

struct ResidencyCredentials {
  int64_t uid, gid;
  Vector<int64_t> groups;
};

bool mayInspectResidency(File* file, void* context) {
  const auto& credentials = *static_cast<ResidencyCredentials*>(context);
  if (credentials.uid == 0 || file->getAttributes().uid == static_cast<size_t>(credentials.uid))
    return true;
  Thread* thread = Processor::information().getCurrentThread();
  const size_t savedError = thread->getErrno();
  const bool permitted = VFS::checkAccess(file, false, true, false, credentials.uid,
                                          credentials.gid, credentials.groups);
  // A masked residency answer succeeds even when write permission is absent.
  thread->setErrno(savedError);
  return permitted;
}
}  // namespace

void* posix_mremap(void* oldAddress, size_t oldLength, size_t newLength, int flags,
                   void* newAddress) {
  TerminationDeferral lifetime;
  const uintptr_t source = reinterpret_cast<uintptr_t>(oldAddress);
  const uintptr_t destination = reinterpret_cast<uintptr_t>(newAddress);
  const size_t mask = PhysicalMemoryManager::getPageSize() - 1;
  size_t oldSize, newSize;
  if ((source & mask) || !newLength || !roundLength(oldLength, oldSize) ||
      !roundLength(newLength, newSize) || (flags & ~(MayMove | Fixed | DontUnmap)) ||
      ((flags & (Fixed | DontUnmap)) && !(flags & MayMove)) || (!oldLength && !(flags & MayMove))) {
    SYSCALL_ERROR(InvalidArgument);
    return reinterpret_cast<void*>(~uintptr_t(0));
  }
  if (!userRange(source, oldSize)) {
    SYSCALL_ERROR(BadAddress);
    return reinterpret_cast<void*>(~uintptr_t(0));
  }
  if ((flags & Fixed) && ((destination & mask) || !userRange(destination, newSize) ||
                          (source < destination + newSize && destination < source + oldSize))) {
    SYSCALL_ERROR(InvalidArgument);
    return reinterpret_cast<void*>(~uintptr_t(0));
  }
  if (flags & DontUnmap) {
    if (oldSize != newSize)
      SYSCALL_ERROR(InvalidArgument);
    else
      SYSCALL_ERROR(OperationNotSupported);
    return reinterpret_cast<void*>(~uintptr_t(0));
  }

  MemoryMapManager::RemapRequest request{source,  destination,           oldSize,
                                         newSize, bool(flags & MayMove), bool(flags & Fixed)};
  uintptr_t result = 0;
  const auto status = MemoryMapManager::instance().remap(request, result);
  switch (status) {
    case MemoryMapManager::VmStatus::Success:
      Processor::information().getCurrentThread()->setErrno(0);
      return reinterpret_cast<void*>(result);
    case MemoryMapManager::VmStatus::InvalidRange:
      SYSCALL_ERROR(InvalidArgument);
      break;
    case MemoryMapManager::VmStatus::Unmapped:
      SYSCALL_ERROR(BadAddress);
      break;
    case MemoryMapManager::VmStatus::Unsupported:
      SYSCALL_ERROR(OperationNotSupported);
      break;
    case MemoryMapManager::VmStatus::NoMemory:
      SYSCALL_ERROR(OutOfMemory);
      break;
  }
  return reinterpret_cast<void*>(~uintptr_t(0));
}

int posix_mincore(void* address, size_t length, unsigned char* vector) {
  TerminationDeferral lifetime;
  const uintptr_t base = reinterpret_cast<uintptr_t>(address);
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  if (base & (pageSize - 1)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  size_t extent;
  if (!roundLength(length, extent)) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  if (!extent)
    return 0;
  if (!userRange(base, extent)) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  const size_t pages = extent / pageSize;
  const size_t capacity = pages < pageSize ? pages : pageSize;
  auto snapshot = UniqueArray<unsigned char>::allocate(capacity);
  if (!snapshot) {
    SYSCALL_ERROR(NoMoreProcesses);
    return -1;
  }
  Thread* thread = Processor::information().getCurrentThread();
  Process* process = thread->getParent();
  ResidencyCredentials credentials;
  credentials.uid = process->getEffectiveUserId();
  if (credentials.uid < 0)
    credentials.uid = process->getUserId();
  credentials.gid = process->getEffectiveGroupId();
  if (credentials.gid < 0)
    credentials.gid = process->getGroupId();
  if (!credentials.groups.tryReserve(NGROUPS_MAX)) {
    SYSCALL_ERROR(NoMoreProcesses);
    return -1;
  }
  process->getSupplementalGroupIds(credentials.groups);
  for (size_t completed = 0; completed < pages;) {
    const size_t count = pages - completed < capacity ? pages - completed : capacity;
    const auto status =
        MemoryMapManager::instance().residency(base + completed * pageSize, count * pageSize,
                                               snapshot.get(), mayInspectResidency, &credentials);
    if (status != MemoryMapManager::VmStatus::Success) {
      if (status == MemoryMapManager::VmStatus::NoMemory)
        SYSCALL_ERROR(NoMoreProcesses);
      else
        SYSCALL_ERROR(OutOfMemory);
      return -1;
    }
    const uintptr_t output = reinterpret_cast<uintptr_t>(vector);
    if (completed > ~uintptr_t(0) - output ||
        !PosixSubsystem::copyToUser(reinterpret_cast<void*>(output + completed), snapshot.get(),
                                    count)) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    completed += count;
  }
  thread->setErrno(0);
  return 0;
}

int posix_madvise(void* address, size_t length, int advice) {
  TerminationDeferral lifetime;
  const uintptr_t base = reinterpret_cast<uintptr_t>(address);
  const size_t mask = PhysicalMemoryManager::getPageSize() - 1;
  size_t extent;
  if ((base & mask) || !roundLength(length, extent) || advice != Discard) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (!extent)
    return 0;
  if (!userRange(base, extent)) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  const auto status = MemoryMapManager::instance().discard(base, extent);
  if (status == MemoryMapManager::VmStatus::Success) {
    Processor::information().getCurrentThread()->setErrno(0);
    return 0;
  }
  if (status == MemoryMapManager::VmStatus::InvalidRange ||
      status == MemoryMapManager::VmStatus::Unsupported)
    SYSCALL_ERROR(InvalidArgument);
  else
    SYSCALL_ERROR(OutOfMemory);
  return -1;
}
