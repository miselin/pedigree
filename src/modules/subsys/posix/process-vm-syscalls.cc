/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Pointers.h"

#include "process-vm-syscalls.h"
#if PEDIGREE_PROCESS_MEMORY_TESTS
#include "pedigree/kernel/utilities/assert.h"
#endif

#include <limits.h>

#include "PosixProcess.h"
#include "PosixSubsystem.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include <sys/uio.h>

namespace {
constexpr size_t MaximumVectors = 1024, MaximumTransfer = 0x7ffff000;
#if PEDIGREE_PROCESS_MEMORY_TESTS
Thread* fragmentCaller = nullptr;
ProcessVmAfterFragmentHook fragmentHook = nullptr;
void* fragmentContext = nullptr;
#endif

class ProcessMemoryResult {
 public:
  explicit ProcessMemoryResult(Thread& thread) : m_Thread(thread) {}
  ssize_t finish(ssize_t result) {
    m_Error = result < 0 ? m_Thread.getErrno() : 0;
    return result;
  }
  ~ProcessMemoryResult() {
    m_Thread.setErrno(m_Error);
  }

 private:
  Thread& m_Thread;
  size_t m_Error = 0;
};

bool localRange(uintptr_t address, size_t length) {
  uintptr_t limit = Processor::information().getVirtualAddressSpace().getKernelStart();
#if X64
  if (limit > 0x0000800000000000ULL)
    limit = 0x0000800000000000ULL;
#endif
  // This is only an arithmetic access_ok check. Residency and permissions
  // belong to the copy, after target lookup and authorization.
  return address < limit && length <= limit - address;
}

bool importVectors(const iovec* user, size_t count, bool local, UniqueArray<iovec>& owner,
                   size_t& total) {
  total = 0;
  if (count > MaximumVectors) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  if (!count)
    return true;
  owner = UniqueArray<iovec>::allocate(count);
  if (!owner) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  if (!PosixSubsystem::copyFromUser(owner.get(), user, count, sizeof(iovec))) {
    SYSCALL_ERROR(BadAddress);
    return false;
  }
  for (size_t n = 0; n < count; ++n) {
    auto& item = owner.get()[n];
    if (item.iov_len > static_cast<size_t>(SSIZE_MAX)) {
      SYSCALL_ERROR(InvalidArgument);
      return false;
    }
    if (local) {
      // Linux's one-vector import applies its transfer cap before access_ok.
      if (count == 1 && item.iov_len > MaximumTransfer)
        item.iov_len = MaximumTransfer;
      if (!localRange(reinterpret_cast<uintptr_t>(item.iov_base), item.iov_len)) {
        SYSCALL_ERROR(BadAddress);
        return false;
      }
      if (item.iov_len > MaximumTransfer - total)
        item.iov_len = MaximumTransfer - total;
      total += item.iov_len;
    } else if (item.iov_len) {
      // Remote lengths need not be summed: the bounded local stream decides
      // completion, and remote payload addresses are checked only on access.
      total = 1;
    }
  }
  return true;
}

bool acquireTarget(int pid, Scheduler::ProcessLease& process) {
  if (pid <= 0)
    return false;
  auto& scheduler = Scheduler::instance();
  if (scheduler.acquireProcessByUserspaceId(process, static_cast<size_t>(pid)))
    return true;
  Process::ThreadLease task;
  if (!scheduler.acquireThreadByTaskId(task, static_cast<size_t>(pid)))
    return false;
  // An exec must be able to retire this Thread while the caller waits for a
  // mapping operation. Retain only its parent beyond the lookup.
  return scheduler.acquireProcess(process, task->getParent());
}

bool permitted(PosixProcess& caller, PosixProcess& target) {
  if (&caller == &target)
    return true;
  const auto source = caller.snapshotCredentials();
  const auto destination = target.snapshotCredentials();
  return destination.dumpable && source.ruid == destination.ruid &&
         source.ruid == destination.euid && source.ruid == destination.suid &&
         source.rgid == destination.rgid && source.rgid == destination.egid &&
         source.rgid == destination.sgid;
}

struct Cursor {
  const iovec* vectors;
  size_t count, index = 0, offset = 0;

  bool advanceEmpty() {
    while (index < count && offset == vectors[index].iov_len) {
      ++index;
      offset = 0;
    }
    return index < count;
  }
  bool position(uintptr_t& address, size_t& remaining) const {
    const auto& vector = vectors[index];
    const uintptr_t base = reinterpret_cast<uintptr_t>(vector.iov_base);
    if (offset > ~uintptr_t(0) - base)
      return false;
    address = base + offset;
    remaining = vector.iov_len - offset;
    return true;
  }
};

ssize_t transfer(int pid, const iovec* local, size_t localCount, const iovec* remote,
                 size_t remoteCount, unsigned long flags, bool write) {
  auto* thread = Processor::information().getCurrentThread();
  ProcessMemoryResult completion(*thread);
  TerminationDeferral lifetime;
  if (flags) {
    SYSCALL_ERROR(InvalidArgument);
    return completion.finish(-1);
  }
  UniqueArray<iovec> localVectors, remoteVectors;
  size_t total = 0, nonemptyRemote = 0;
  if (!importVectors(local, localCount, true, localVectors, total))
    return completion.finish(-1);
  if (!total)
    return completion.finish(0);
  if (!importVectors(remote, remoteCount, false, remoteVectors, nonemptyRemote))
    return completion.finish(-1);
  if (!nonemptyRemote)
    return completion.finish(0);

  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  auto bounce = UniqueArray<uint8_t>::allocate(pageSize);
  if (!bounce) {
    SYSCALL_ERROR(OutOfMemory);
    return completion.finish(-1);
  }
  Scheduler::ProcessLease process;
  if (!acquireTarget(pid, process) || process->getType() != Process::Posix ||
      !process->getSubsystem()) {
    SYSCALL_ERROR(NoSuchProcess);
    return completion.finish(-1);
  }
  Process* callerProcess = thread->getParent();
  if (callerProcess->getType() != Process::Posix) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return completion.finish(-1);
  }
  auto& caller = *static_cast<PosixProcess*>(callerProcess);
  auto& target = *static_cast<PosixProcess*>(process.get());
  auto& subsystem = *static_cast<PosixSubsystem*>(process->getSubsystem());
  PosixSubsystem::UserImageToken image;
  auto& manager = MemoryMapManager::instance();
  {
    MemoryMapManager::OperationGuard operation(manager);
    if (!permitted(caller, target)) {
      SYSCALL_ERROR(NotEnoughPermissions);
      return completion.finish(-1);
    }
    if (!subsystem.snapshotUserImage(image)) {
      SYSCALL_ERROR(NoSuchProcess);
      return completion.finish(-1);
    }
  }

  Cursor localCursor{localVectors.get(), localCount},
      remoteCursor{remoteVectors.get(), remoteCount};
  size_t copied = 0;
  while (copied < total && localCursor.advanceEmpty() && remoteCursor.advanceEmpty()) {
    if (thread->getUnwindState() != Thread::Continue) {
      SYSCALL_ERROR(Interrupted);
      break;
    }
    uintptr_t localAddress, remoteAddress;
    size_t localRemaining, remoteRemaining;
    if (!localCursor.position(localAddress, localRemaining) ||
        !remoteCursor.position(remoteAddress, remoteRemaining)) {
      SYSCALL_ERROR(BadAddress);
      break;
    }
    size_t length = total - copied;
    if (length > localRemaining)
      length = localRemaining;
    if (length > remoteRemaining)
      length = remoteRemaining;
    const size_t localPage = pageSize - (localAddress % pageSize);
    const size_t remotePage = pageSize - (remoteAddress % pageSize);
    if (length > localPage)
      length = localPage;
    if (length > remotePage)
      length = remotePage;

    {
      MemoryMapManager::OperationGuard operation(manager);
      if (!subsystem.matchesUserImage(image)) {
        SYSCALL_ERROR(NoSuchProcess);
        break;
      }
      if (!permitted(caller, target)) {
        SYSCALL_ERROR(NotEnoughPermissions);
        break;
      }
      if (write && !PosixSubsystem::copyFromUser(
                       bounce.get(), reinterpret_cast<const void*>(localAddress), length)) {
        SYSCALL_ERROR(BadAddress);
        break;
      }
      const auto status =
          manager.copyUserPage(*image.space, remoteAddress, bounce.get(), length, write);
      if (status != MemoryMapManager::UserPageCopyStatus::Success) {
        // Linux reports a remote page fault as EFAULT, including failure to
        // allocate or load that page. Scratch allocation has its own ENOMEM.
        syscallError(status == MemoryMapManager::UserPageCopyStatus::Unsupported
                         ? Error::OperationNotSupported
                         : Error::BadAddress);
        break;
      }
      if (!write && !PosixSubsystem::copyToUser(reinterpret_cast<void*>(localAddress), bounce.get(),
                                                length)) {
        SYSCALL_ERROR(BadAddress);
        break;
      }
      copied += length;
      localCursor.offset += length;
      remoteCursor.offset += length;
    }
#if PEDIGREE_PROCESS_MEMORY_TESTS
    if (__atomic_load_n(&fragmentCaller, __ATOMIC_ACQUIRE) == thread) {
      const auto hook = __atomic_load_n(&fragmentHook, __ATOMIC_ACQUIRE);
      if (hook)
        hook(copied, __atomic_load_n(&fragmentContext, __ATOMIC_ACQUIRE));
    }
#endif
  }
  return completion.finish(copied ? static_cast<ssize_t>(copied) : -1);
}
}  // namespace

#if PEDIGREE_PROCESS_MEMORY_TESTS
void setProcessVmAfterFragmentHookForTest(Thread* expectedCaller, ProcessVmAfterFragmentHook hook,
                                          void* context) {
  assert(!expectedCaller || expectedCaller == Processor::information().getCurrentThread());
  __atomic_store_n(&fragmentCaller, static_cast<Thread*>(nullptr), __ATOMIC_RELEASE);
  __atomic_store_n(&fragmentHook, hook, __ATOMIC_RELEASE);
  __atomic_store_n(&fragmentContext, context, __ATOMIC_RELEASE);
  __atomic_store_n(&fragmentCaller, expectedCaller, __ATOMIC_RELEASE);
}
#endif

ssize_t posix_process_vm_readv(int pid, const iovec* local, size_t localCount, const iovec* remote,
                               size_t remoteCount, unsigned long flags) {
  return transfer(pid, local, localCount, remote, remoteCount, flags, false);
}

ssize_t posix_process_vm_writev(int pid, const iovec* local, size_t localCount, const iovec* remote,
                                size_t remoteCount, unsigned long flags) {
  return transfer(pid, local, localCount, remote, remoteCount, flags, true);
}
