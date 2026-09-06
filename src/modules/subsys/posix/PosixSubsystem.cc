/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/linker/Elf.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/SignalEvent.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/Uninterruptible.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/SyscallManager.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Pointers.h"
#include "pedigree/kernel/utilities/RadixTree.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Tree.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/lib.h"

#include <PosixSubsystem.h>
#include <signal.h>
#include <vdso.h>  // Header with the vdso.so binary in it.

#include "FileDescriptor.h"
#include "PosixProcess.h"
#include "ProcFs.h"
#include "eventfd-syscalls.h"
#include "file-syscalls.h"
#include "linux-amd64-signal.h"
#include "logging.h"
#include "modules/system/linker/DynamicLinker.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/LockedFile.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/Symlink.h"
#include "modules/system/vfs/VFS.h"
#include "mqueue-syscalls.h"
#include "posix-timer-syscalls.h"
#include "pthread-syscalls.h"
#include "queued-signal.h"
#include "signal-syscalls.h"
#include "signalfd-syscalls.h"
#include "system-syscalls.h"
#include "sysv-semaphore-syscalls.h"
#include "timerfd-syscalls.h"

extern char __posix_compat_vsyscall_base;

#define POSIX_VSYSCALL_ADDRESS 0xffffffffff600000

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2

#define FD_CLOEXEC 1

typedef Tree<size_t, PosixSubsystem::SignalHandler*> sigHandlerTree;
typedef Tree<size_t, SharedPointer<FileDescriptor>> FdMap;

ProcessGroupManager ProcessGroupManager::m_Instance;

extern void pedigree_init_sigret();
extern void pedigree_init_pthreads();

struct PosixSubsystem::ExecutableImage {
  File* file = nullptr;
  size_t fileSize = 0;
  Elf::ExecutableMetadata metadata{};
  UniqueArray<uint8_t> programHeaders;
  uintptr_t programHeaderAddress = 0;
  String interpreter;
};

namespace {
bool prepareUserCopy(uintptr_t address, size_t extent, bool write) {
#if POSIX_NO_EFAULT
  return true;
#else
  if (!PosixSubsystem::checkAddress(address, extent,
                                    write ? PosixSubsystem::SafeWrite : PosixSubsystem::SafeRead)) {
    return false;
  }
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const uintptr_t lastPage = (address + extent - 1) & ~(pageSize - 1);
  for (uintptr_t page = address & ~(pageSize - 1);; page += pageSize) {
    if (!MemoryMapManager::instance().faultIn(page, write)) {
      return false;
    }
    if (page == lastPage) {
      return true;
    }
  }
#endif
}

void ignoredSignal(int) {}

bool defaultSignalActionIsIgnore(size_t signal) {
  return signal == SIGCHLD || signal == SIGURG || signal == SIGWINCH;
}

bool defaultSignalActionIsStop(size_t signal) {
  return signal == SIGSTOP || signal == SIGTSTP || signal == SIGTTIN || signal == SIGTTOU;
}

void stampStopDelivery(Process* process, size_t signal, SignalEvent* delivery) {
  if (process && delivery && defaultSignalActionIsStop(signal)) {
    delivery->setContinuationEpoch(process->getContinuationEpoch());
  }
}

bool rebindQueuedSignalEvents(Thread& thread, size_t signal, SignalEvent& prototype) {
  static uint64_t nextGeneration = 0;
  const uint64_t generation = __atomic_add_fetch(&nextGeneration, 1, __ATOMIC_RELAXED);
  // Mark replacements so every queued RT instance is rebound exactly once,
  // including when asynchronous consumers remove entries during this pass.
  while (true) {
    SignalEvent* pending = static_cast<SignalEvent*>(prototype.cloneForDelivery());
    if (!thread.replaceSignalEvent(signal, pending, -1, generation)) {
      delete pending;
      return true;
    }
  }
}

void setExecutableValidationError(Elf::ExecutableValidationResult result, bool isInterpreter) {
  if (isInterpreter) {
    SYSCALL_ERROR(BadSharedLibrary);
  } else if (result == Elf::ExecutableValidationResult::MultipleInterpreters) {
    SYSCALL_ERROR(InvalidArgument);
  } else {
    SYSCALL_ERROR(ExecFormatError);
  }
}
}  // namespace

ProcessGroupManager::ProcessGroupManager() : m_GroupIds(), m_Groups(), m_GroupLock(false) {
  m_GroupIds.set(0);
}

ProcessGroupManager::~ProcessGroupManager() {}

size_t ProcessGroupManager::allocateGroupId() {
  RecursingLockGuard<Spinlock> guard(m_GroupLock);
  size_t bit = m_GroupIds.getFirstClear();
  m_GroupIds.set(bit);
  return bit;
}

void ProcessGroupManager::setGroupId(size_t gid) {
  RecursingLockGuard<Spinlock> guard(m_GroupLock);
  if (m_GroupIds.test(gid)) {
    PS_NOTICE(
        "ProcessGroupManager: setGroupId called on a group ID that "
        "existed already!");
  }
  m_GroupIds.set(gid);
}

bool ProcessGroupManager::isGroupIdValid(size_t gid) const {
  RecursingLockGuard<Spinlock> guard(m_GroupLock);
  return m_GroupIds.test(gid);
}

void ProcessGroupManager::returnGroupId(size_t gid) {
  RecursingLockGuard<Spinlock> guard(m_GroupLock);
  m_GroupIds.clear(gid);
}

void ProcessGroupManager::registerGroup(size_t gid, ProcessGroup* group) {
  RecursingLockGuard<Spinlock> guard(m_GroupLock);
  ProcessGroup* existing = m_Groups.lookup(gid);
  if (existing && existing != group) {
    FATAL("Two concrete POSIX process groups claimed one group ID.");
  }
  if (!existing) {
    m_Groups.insert(gid, group);
  }
  m_GroupIds.set(gid);
}

void ProcessGroupManager::unregisterGroup(size_t gid, ProcessGroup* group) {
  RecursingLockGuard<Spinlock> guard(m_GroupLock);
  if (m_Groups.lookup(gid) == group) {
    m_Groups.remove(gid);
    m_GroupIds.clear(gid);
  }
}

ProcessGroup* ProcessGroupManager::findGroup(size_t gid) const {
  return m_Groups.lookup(gid);
}

PosixSubsystem::PosixSubsystem(PosixSubsystem& s)
    : Subsystem(s),
      m_SignalHandlers(),
      m_SignalHandlersLock(),
      m_CallbackSchedulingDomain(s.m_CallbackSchedulingDomain),
      m_AdvisoryOwner(AdvisoryOwner::Kind::Process),
      m_MemoryLockAccount(s.m_MemoryLockAccount),
      m_FdMap(),
      m_NextFd(s.m_NextFd),
      m_FdLock(),
      m_FdBitmap(),
      m_LastFd(0),
      m_FreeCount(s.m_FreeCount),
      m_SyncObjects(),
      m_Threads(),
      m_ThreadWaiters(),
      m_NextThreadWaiter(1),
      m_Abi(s.m_Abi),
      m_bAcquired(false),
      m_pAcquiredThread(nullptr) {
  m_SignalHandlersLock.acquire();
  s.m_SignalHandlersLock.enter();

  // Tree iterators store their cursor in the tree, so iterate a shallow copy.
  sigHandlerTree signalHandlers(s.m_SignalHandlers);

  // Copy all signal handlers
  for (sigHandlerTree::Iterator it = signalHandlers.begin(); it != signalHandlers.end(); it++) {
    size_t key = it.key();
    void* value = it.value();
    if (!value)
      continue;

    SignalHandler* newSig = new SignalHandler(*reinterpret_cast<SignalHandler*>(value));
    m_SignalHandlers.insert(key, newSig);
  }

  s.m_SignalHandlersLock.leave();
  m_SignalHandlersLock.release();

  // Copy across waiter state.
  Tree<void*, Semaphore*> threadWaiters(s.m_ThreadWaiters);
  for (Tree<void*, Semaphore*>::Iterator it = threadWaiters.begin(); it != threadWaiters.end();
       ++it) {
    void* key = it.key();

    Semaphore* sem = new Semaphore(0);
    m_ThreadWaiters.insert(key, sem);
  }

  m_NextThreadWaiter = s.m_NextThreadWaiter;
}

void PosixSubsystem::setProcess(Process* process) {
  Subsystem::setProcess(process);
  m_PendingSignals->attach(m_pProcess);
  if (process && m_Namespaces)
    m_Namespaces->attach(*process);
  if (process) {
    auto& space = *process->getAddressSpace();
    MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
    MemoryMapManager::instance().bindMemoryLockPolicy(space);
    space.setMemoryLockAccount(&m_MemoryLockAccount);
  }
}

bool PosixSubsystem::snapshotUserImage(UserImageToken& token) const {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  token = {};
  if (!m_UserImageActive)
    return false;
  token.space = m_UserImageSpace;
  token.generation = m_UserImageGeneration;
  return true;
}

bool PosixSubsystem::matchesUserImage(const UserImageToken& token) const {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  return m_UserImageActive && token.space == m_UserImageSpace &&
         token.generation == m_UserImageGeneration;
}

void PosixSubsystem::invalidateUserImage() {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  m_UserImageActive = false;
  m_UserImageSpace = nullptr;
}

bool PosixSubsystem::publishUserImage(VirtualAddressSpace& space) {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  if (m_UserImageActive || !m_pProcess || m_pProcess->getAddressSpace() != &space ||
      m_UserImageGeneration == ~uint64_t(0))
    return false;
  ++m_UserImageGeneration;
  m_UserImageSpace = &space;
  m_UserImageActive = true;
  return true;
}

PosixSubsystem::~PosixSubsystem() {
  if (m_Namespaces)
    m_Namespaces->close();
  m_PendingSignals->close();
  assert(--m_FreeCount == 0);

  acquire();

  // Destroy all signal handlers
  for (sigHandlerTree::Iterator it = m_SignalHandlers.begin(); it != m_SignalHandlers.end(); it++) {
    // Get the signal handler and remove it. Note that there shouldn't be
    // null SignalHandlers, at all.
    SignalHandler* sig = it.value();
    assert(sig);

    // SignalHandler's destructor will delete the Event itself
    delete sig;
  }

  // And now that the signals are destroyed, remove them from the Tree
  m_SignalHandlers.clear();

  release();

  // Process destruction may arrive with the table pre-acquired. Registry
  // teardown must follow that release, including construction-failure fallback.
  posix_advisory_owner_closed(m_AdvisoryOwner);

  // For sanity's sake, destroy any remaining descriptors
  freeMultipleFds();

  // Remove any POSIX threads that might still be lying around
  for (Tree<size_t, PosixThread*>::Iterator it = m_Threads.begin(); it != m_Threads.end(); it++) {
    PosixThread* thread = it.value();
    assert(thread);  // There shouldn't have ever been a null PosixThread in
                     // there

    // If the thread is still running, it should be killed
    if (!thread->isRunning.isComplete()) {
      WARNING("PosixSubsystem object freed when a thread is still running?");
      // Thread will just stay running, won't be deallocated or killed
    }

    // Clean up any thread-specific data
    for (Tree<size_t, PosixThreadKey*>::Iterator it2 = thread->m_ThreadData.begin();
         it2 != thread->m_ThreadData.end(); it2++) {
      PosixThreadKey* p = reinterpret_cast<PosixThreadKey*>(it.value());
      assert(p);

      /// \todo Call the destructor (need a way to call into userspace and
      /// return back here)
      delete p;
    }

    thread->m_ThreadData.clear();
    delete thread;
  }

  m_Threads.clear();

  // Clean up synchronisation objects
  for (Tree<size_t, PosixSyncObject*>::Iterator it = m_SyncObjects.begin();
       it != m_SyncObjects.end(); it++) {
    PosixSyncObject* p = it.value();
    assert(p);

    if (p->pObject) {
      if (p->isMutex)
        delete reinterpret_cast<Mutex*>(p->pObject);
      else
        delete reinterpret_cast<Semaphore*>(p->pObject);
    }
  }

  m_SyncObjects.clear();

  for (Tree<void*, Semaphore*>::Iterator it = m_ThreadWaiters.begin(); it != m_ThreadWaiters.end();
       ++it) {
    // Process teardown has already quiesced every peer thread. Waking a
    // waiter and immediately deleting its queue would manufacture a
    // use-after-free; Semaphore/WaitQueue destruction instead verifies
    // that the quiescence invariant is true.
    delete it.value();
  }

  m_ThreadWaiters.clear();

  // Take the memory map lock before we become uninterruptible.
  MemoryMapManager::instance().acquireLock();
  invalidateUserImage();

  // Spinlock as a quick way of disabling interrupts.
  Spinlock spinlock;
  spinlock.acquire();

  // Switch to the address space of the process we're destroying.
  // We need to unmap memory maps, and we can't do that in our address space.
  VirtualAddressSpace& curr = Processor::information().getVirtualAddressSpace();
  VirtualAddressSpace* va = m_pProcess->getAddressSpace();

  if (va != &curr) {
    // Switch into the address space we want to unmap inside.
    Processor::switchAddressSpace(*va);
  }

  // Remove all existing mappings, if any.
  MemoryMapManager::instance().unmapAllUnlocked();

  if (va != &curr) {
    Processor::switchAddressSpace(curr);
  }

  spinlock.release();

  va->rawUserMemory().clear();
  m_MemoryLockAccount.publish({}, MemoryLockMode::None);
  va->setMemoryLockAccount(nullptr);

  // Give back the memory map lock now - we're interruptible again.
  MemoryMapManager::instance().releaseLock();
}

void PosixSubsystem::acquire() {
  Thread* me = Processor::information().getCurrentThread();

  m_Lock.acquire();
  if (m_bAcquired && m_pAcquiredThread == me) {
    m_Lock.release();
    return;  // already acquired
  }
  m_Lock.release();

  // Ensure that no descriptor operations are taking place (and then, will
  // take place)
  m_FdLock.acquire();

  // Modifying signal handlers, ensure that they are not in use
  m_SignalHandlersLock.acquire();

  // Safe to do without spinlock as we hold the other locks now.
  m_pAcquiredThread = me;
  m_bAcquired = true;
}

void PosixSubsystem::release() {
  // Opposite order to acquire()
  m_Lock.acquire();
  m_bAcquired = false;
  m_pAcquiredThread = nullptr;

  m_SignalHandlersLock.release();
  m_FdLock.release();

  m_Lock.release();
}

bool PosixSubsystem::checkAddress(uintptr_t addr, size_t extent, size_t flags) {
#if POSIX_NO_EFAULT
  return true;
#endif

  Uninterruptible while_checking;

#if VERBOSE_KERNEL
  PS_NOTICE("PosixSubsystem::checkAddress(" << Hex << addr << ", " << Dec << extent << ", " << Hex
                                            << flags << ")");
#endif

  // No memory access expected, all good.
  if (!extent) {
#if VERBOSE_KERNEL
    PS_NOTICE("  -> zero extent, address is sane.");
#endif
    return true;
  }

  uintptr_t aa = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#if VERBOSE_KERNEL
  PS_NOTICE(" -> ret: " << aa);
#endif

  if (extent - 1 > (~static_cast<uintptr_t>(0) - addr)) {
    return false;
  }
  uintptr_t end = addr + extent - 1;

  // Check the complete address range.
  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  if ((addr < va.getUserStart()) || (addr >= va.getKernelStart()) || (end >= va.getKernelStart()) ||
      !va.isAddressValid(reinterpret_cast<void*>(addr)) ||
      !va.isAddressValid(reinterpret_cast<void*>(end))) {
#if VERBOSE_KERNEL
    PS_NOTICE("  -> outside of user address area.");
#endif
    return false;
  }

  // Keep fallback PTE inspection stable even for callers that only validate.
  MemoryMapManager::OperationGuard mappingGuard(MemoryMapManager::instance());
  MemoryMappedObject::Permissions mmapPermissions = MemoryMappedObject::None;
  if (flags & SafeRead) {
    mmapPermissions |= MemoryMappedObject::Read;
  }
  if (flags & SafeWrite) {
    mmapPermissions |= MemoryMappedObject::Write;
  }
  if (flags & SafeExecute) {
    mmapPermissions |= MemoryMappedObject::Exec;
  }

  // Demand-paged mappings may not have PTEs yet. Accept them only when
  // objects cover the complete range with the requested permissions.
  if (mmapPermissions != MemoryMappedObject::None &&
      MemoryMapManager::instance().allows(addr, extent, mmapPermissions)) {
#if VERBOSE_KERNEL
    PS_NOTICE("  -> inside memory map.");
#endif
    return true;
  }

  // Check each page touched by the range, including a short final page after
  // an unaligned start.
  size_t pageSize = PhysicalMemoryManager::getPageSize();
  uintptr_t page = addr - (addr % pageSize);
  uintptr_t finalPage = end - (end % pageSize);
  while (true) {
    void* pAddr = reinterpret_cast<void*>(page);
    if (!va.isMapped(pAddr)) {
#if VERBOSE_KERNEL
      PS_NOTICE("  -> page " << Hex << pAddr << " is not mapped.");
#endif
      return false;
    }

    size_t vFlags = 0;
    physical_uintptr_t phys = 0;
    va.getMapping(pAddr, phys, vFlags);

    if (vFlags & (VirtualAddressSpace::KernelMode | VirtualAddressSpace::NoAccess)) {
#if VERBOSE_KERNEL
      PS_NOTICE("  -> not userspace-accessible.");
#endif
      return false;
    }

    if (flags & SafeWrite) {
      if ((vFlags & VirtualAddressSpace::WriteProtected) ||
          !(vFlags & (VirtualAddressSpace::Write | VirtualAddressSpace::CopyOnWrite))) {
#if VERBOSE_KERNEL
        PS_NOTICE("  -> not writeable.");
#endif
        return false;
      }
    }

    if ((flags & SafeExecute) && !(vFlags & VirtualAddressSpace::Execute)) {
#if VERBOSE_KERNEL
      PS_NOTICE("  -> not executable.");
#endif
      return false;
    }

    if (page == finalPage) {
      break;
    }
    page += pageSize;
  }

#if VERBOSE_KERNEL
  PS_NOTICE("  -> mapped and available.");
#endif
  return true;
}

bool PosixSubsystem::checkedUserBufferSize(size_t count, size_t elementSize, size_t& extent) {
  extent = 0;
  if (!count || !elementSize) {
    return true;
  }

  if (count > (~static_cast<size_t>(0) / elementSize)) {
    return false;
  }

  extent = count * elementSize;
  return true;
}

bool PosixSubsystem::checkUserBuffer(uintptr_t addr, size_t count, size_t elementSize, size_t flags,
                                     size_t* extent) {
  if (extent) {
    *extent = 0;
  }

  size_t byteExtent = 0;
  if (!checkedUserBufferSize(count, elementSize, byteExtent)) {
    return false;
  }

  if (extent) {
    *extent = byteExtent;
  }
  return checkAddress(addr, byteExtent, flags);
}

bool PosixSubsystem::copyFromUser(void* destination, const void* source, size_t count,
                                  size_t elementSize) {
  size_t extent = 0;
  if (!checkedUserBufferSize(count, elementSize, extent)) {
    return false;
  }
  if (!extent) {
    return true;
  }
  if (!destination || !source) {
    return false;
  }

  MemoryMapManager::OperationGuard mappingGuard(MemoryMapManager::instance());
  if (!prepareUserCopy(reinterpret_cast<uintptr_t>(source), extent, false)) {
    return false;
  }

  MemoryCopy(destination, source, extent);
  return true;
}

bool PosixSubsystem::copyToUser(void* destination, const void* source, size_t count,
                                size_t elementSize) {
  size_t extent = 0;
  if (!checkedUserBufferSize(count, elementSize, extent)) {
    return false;
  }
  if (!extent) {
    return true;
  }
  if (!destination || !source) {
    return false;
  }

  MemoryMapManager::OperationGuard mappingGuard(MemoryMapManager::instance());
  if (!prepareUserCopy(reinterpret_cast<uintptr_t>(destination), extent, true)) {
    return false;
  }

  MemoryCopy(destination, source, extent);
  return true;
}

PosixSubsystem::UserStringResult PosixSubsystem::copyUserString(const char* userString,
                                                                String& copy, size_t maxLength) {
  copy.clear();

  if (!userString) {
    return UserStringBadAddress;
  }

  if (!maxLength) {
    return UserStringTooLong;
  }

  MemoryMapManager::OperationGuard mappingGuard(MemoryMapManager::instance());

  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t chunkSize = 256;
  uintptr_t current = reinterpret_cast<uintptr_t>(userString);
  size_t copied = 0;

  while (copied < maxLength) {
    size_t length = maxLength - copied;
    if (length > chunkSize) {
      length = chunkSize;
    }

    const size_t pageOffset = current % pageSize;
    const size_t pageRemaining = pageSize - pageOffset;
    if (length > pageRemaining) {
      length = pageRemaining;
    }

    if (!prepareUserCopy(current, length, false)) {
      return UserStringBadAddress;
    }

    char buffer[chunkSize];
    MemoryCopy(buffer, reinterpret_cast<const void*>(current), length);

    size_t partLength = 0;
    while (partLength < length && buffer[partLength]) {
      ++partLength;
    }

    if (partLength != length) {
      copy += String(buffer, partLength, true);
      return UserStringSuccess;
    }

    copy += String(buffer, length, true);
    copied += length;
    if (copied == maxLength) {
      return UserStringTooLong;
    }

    if (current > (~static_cast<uintptr_t>(0) - length)) {
      return UserStringBadAddress;
    }
    current += length;
  }

  return UserStringTooLong;
}

void PosixSubsystem::exit(int code, ExitCause cause) {
  if (!Processor::getInterrupts() || Processor::inDeviceHardIrq()) {
    FATAL_NOLOCK(
        "PosixSubsystem::exit requires an IRQ-enabled thread "
        "boundary.");
  }

  Thread* pThread = Processor::information().getCurrentThread();

  Process* pProcess = pThread->getParent();
  NOTICE("PosixSubsystem::exit(" << Dec << pProcess->getId() << ", code=" << code << ")");

  if (!pProcess->beginTermination(code, cause)) {
    // Another thread owns or has reserved process-wide cleanup. A competitor
    // must take only the thread exit path; the owner will retire every peer.
    Processor::information().getScheduler().commitCurrentThreadExit();
  }

  if (cause == ExitCause::Signal) {
    pProcess->setExitStatus(code & 0x7F);
  } else {
    pProcess->setExitStatus((code & 0xFF) << 8);
  }
  if (code) {
    pThread->unexpectedExit();
  }

  // Exit has reached the final cleanup context. Blocking cleanup below must
  // not recursively transfer back into exit at every WaitQueue boundary.
  pThread->setUnwindState(Thread::Continue);

  if (!pProcess->quiesceTermination()) {
    FATAL("POSIX exit owner could not claim process teardown for pid " << Dec << pProcess->getId()
                                                                       << ".");
  }

  // quiesceTermination() may block while peers leave their stacks. Its
  // completion is the final handoff into shared process cleanup.
  if (!Processor::getInterrupts() || Processor::inDeviceHardIrq()) {
    FATAL_NOLOCK(
        "POSIX process teardown escaped its IRQ-enabled thread "
        "boundary.");
  }

  // We're the lowest in the stack, so we can proceed with the exit function.

  posix_advisory_owner_closed(m_AdvisoryOwner);

  // Peer shutdown has consumed their registrations. The final owner must
  // retire its user-memory exit state before process teardown removes it.
  m_PendingSignals->close();
  posix_timer_process_exit(pProcess);
  invalidateUserImage();
  pThread->notifySubsystemExit();

  delete pProcess->getLinker();

  MemoryMapManager::instance().unmapAll();

  {
    MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
    auto& space = *pProcess->getAddressSpace();
    space.rawUserMemory().clear();
    m_MemoryLockAccount.publish({}, MemoryLockMode::None);
    space.setMemoryLockAccount(nullptr);
  }

  // If it's a POSIX process, remove group membership
  if (pProcess->getType() == Process::Posix) {
    PosixProcess* p = static_cast<PosixProcess*>(pProcess);
    p->leaveProcessGroup();
  }

  posix_mqueue_process_exit(pProcess->getId());

  // Clean up the descriptor table
  freeMultipleFds();

  // Tell some interesting info
  NOTICE("at exit for pid " << Dec << pProcess->getId() << "...");

  pProcess->finishTermination(true);

  // Should NEVER get here.
  FATAL("PosixSubsystem::exit() running after Process teardown!");
}

bool PosixSubsystem::kill(KillReason killReason, Thread* pThread) {
  if (!pThread)
    pThread = Processor::information().getCurrentThread();
  Process* pProcess = pThread->getParent();
  if (pProcess->getType() != Process::Posix) {
    ERROR("PosixSubsystem::kill called with a non-POSIX process!");
    return false;
  }
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());

  int signal = SIGKILL;
  switch (killReason) {
    case Interrupted:
      signal = SIGINT;
      break;

    case Terminated:
      signal = SIGTERM;
      break;

    default:
      break;
  }

  if (pSubsystem->queueSignalDelivery(pThread, signal, nullptr, 0, true) ==
      SignalDeliveryResult::Queued) {
    PS_NOTICE("PosixSubsystem - killing " << pThread->getParent()->getId());

    // Allow the event to run
    Processor::setInterrupts(true);
    Scheduler::instance().yield();
  }

  return true;
}

void PosixSubsystem::threadException(Thread* pThread, ExceptionType eType, InterruptState* pState,
                                     uintptr_t faultAddress, uintptr_t errorCode) {
  // The native event path does not consume machine context yet.
  (void)pState;
  (void)faultAddress;
  (void)errorCode;

  PS_NOTICE("PosixSubsystem::threadException -> " << Dec << pThread->getParent()->getId() << ":"
                                                  << pThread->getId());

  // What was the exception?
  int signal = -1;
  switch (eType) {
    case PageFault:
      PS_NOTICE("    (Page fault)");
      // Send SIGSEGV
      signal = SIGSEGV;
      break;
    case FileMappingFault:
      signal = SIGBUS;
      break;
    case InvalidOpcode:
      PS_NOTICE("    (Invalid opcode)");
      // Send SIGILL
      signal = SIGILL;
      break;
    case GeneralProtectionFault:
      PS_NOTICE("    (General Fault)");
      // Send SIGBUS
      signal = SIGBUS;
      break;
    case DivideByZero:
      PS_NOTICE("    (Division by zero)");
      // Send SIGFPE
      signal = SIGFPE;
      break;
    case FpuError:
      PS_NOTICE("    (FPU error)");
      // Send SIGFPE
      signal = SIGFPE;
      break;
    case SpecialFpuError:
      PS_NOTICE("    (FPU error - special)");
      // Send SIGFPE
      signal = SIGFPE;
      break;
    case TerminalInput:
      PS_NOTICE(
          "    (Attempt to read from terminal by non-foreground "
          "process)");
      // Send SIGTTIN
      signal = SIGTTIN;
      break;
    case TerminalOutput:
      PS_NOTICE("    (Output to terminal by non-foreground process)");
      // Send SIGTTOU
      signal = SIGTTOU;
      break;
    case Continue:
      PS_NOTICE("    (Continuing a stopped process)");
      // Send SIGCONT
      signal = SIGCONT;
      break;
    case Stop:
      PS_NOTICE("    (Stopping a process)");
      // Send SIGTSTP
      signal = SIGTSTP;
      break;
    case Interrupt:
      PS_NOTICE("    (Interrupting a process)");
      // Send SIGINT
      signal = SIGINT;
      break;
    case Quit:
      PS_NOTICE("    (Requesting quit)");
      // Send SIGTERM
      signal = SIGTERM;
      break;
    case Child:
      PS_NOTICE("    (Child status changed)");
      // Send SIGCHLD
      signal = SIGCHLD;
      break;
    case Pipe:
      PS_NOTICE("    (Pipe broken)");
      // Send SIGPIPE
      signal = SIGPIPE;
      break;
    default:
      PS_NOTICE("    (Unknown)");
      // Unknown exception
      ERROR("Unknown exception type in threadException - POSIX subsystem");
      break;
  }

#if X64
  if (signal > 0 && pState && getAbi() == LinuxAbi) {
    SignalDisposition disposition;
    if (getSignalDisposition(signal, disposition, true) && disposition.type == 0) {
      LinuxAmd64Signal::DeliveryResult result = LinuxAmd64Signal::deliverSynchronous(
          pThread, signal, disposition, eType, *pState, faultAddress, errorCode);
      if (result == LinuxAmd64Signal::Delivered) {
        return;
      }
      if (result == LinuxAmd64Signal::Failed) {
        // The raw exception frame still owns interrupt accounting and
        // handler cleanup. Preserve the fatal status and let the
        // return-to-user tail enter process teardown after it unwinds.
        pThread->deferSignalExit(SIGSEGV);
        return;
      }
    }
  }
#endif

  // A raw exception frame cannot dispatch a handler or terminal callback.
  // Its return-to-user tail consumes the queued signal after accounting and
  // handler cleanup have completed.
  const bool processDirected = eType == TerminalInput || eType == TerminalOutput ||
                               eType == Continue || eType == Stop || eType == Interrupt ||
                               eType == Quit || eType == Child;
  sendSignal(pThread, signal, pState == nullptr, processDirected);
}

void PosixSubsystem::sendSignal(Thread* pThread, int signal, bool yield, bool processDirected) {
  PS_NOTICE("PosixSubsystem::sendSignal #" << signal << " -> pid:tid " << Dec
                                           << pThread->getParent()->getId() << ":"
                                           << pThread->getId());

  Process* pProcess = pThread->getParent();
  if (pProcess->getType() != Process::Posix) {
    ERROR("PosixSubsystem::threadException called with a non-POSIX process!");
    return;
  }
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());

  const SignalDeliveryResult result =
      pSubsystem->queueSignalDelivery(pThread, signal, nullptr, 0, processDirected);
  if (result == SignalDeliveryResult::Unavailable) {
    ERROR("Unknown signal in sendSignal - POSIX subsystem");
  }

  if (result == SignalDeliveryResult::Queued && yield) {
    Thread* pCurrentThread = Processor::information().getCurrentThread();
    if (pCurrentThread == pThread) {
      // Attempt to execute the new event immediately.
      Processor::information().getScheduler().checkEventState(0);
    } else {
      // Yield so the event can fire.
      Scheduler::instance().yield();
    }
  } else if (result == SignalDeliveryResult::Rejected) {
    // PS_NOTICE("No event configured for signal #" << signal << ", silently
    // dropping!");
    NOTICE("No event configured for signal #" << signal << ", silently dropping!");
  }
}

bool PosixSubsystem::admitLegacyUserSignals() {
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  if (m_CallbackSchedulingDomain == CallbackSchedulingDomain::Affinity)
    return false;
  // A serialized legacy handler retains a lower kernel continuation after
  // its Event lease retires. Its image must keep that continuation's CPU.
  m_CallbackSchedulingDomain = CallbackSchedulingDomain::LegacySignals;
  return true;
}

void PosixSubsystem::setSignalHandler(size_t sig, SignalHandler* handler) {
  if (sig > MaximumSupportedSignal) {
    delete handler;
    ERROR("Cannot install unsupported signal disposition " << Dec << sig << ".");
    return;
  }

  PendingSignalNotification notification(m_PendingSignals);
  LockGuard<Mutex> pendingGuard(m_PendingSignals->lock);
  m_SignalHandlersLock.acquire();

  SignalHandler* removal = nullptr;

  if (handler) {
    removal = m_SignalHandlers.lookup(sig);
    if (removal) {
      // Remove from the list
      m_SignalHandlers.remove(sig);
    }

    // Insert into the signal handler table
    handler->sig = sig;

    m_SignalHandlers.insert(sig, handler);

    const bool discardPending =
        handler->type == 2 || (handler->type == 1 && defaultSignalActionIsIgnore(sig));
    if (m_pProcess) {
      // Descending indices remain exhaustive when an exiting thread is
      // removed and shifts the remaining vector entries to the left.
      for (size_t i = m_pProcess->getNumThreads(); i > 0; --i) {
        Process::ThreadLease thread;
        if (m_pProcess->acquireThread(thread, i - 1)) {
          if (discardPending) {
            thread->cullSignalEvent(sig);
          } else if (handler->pEvent) {
            // A concurrent dequeue already owns its old delivery. Only
            // events still pending must acquire the new disposition.
            rebindQueuedSignalEvents(*thread.get(), sig, *handler->pEvent);
          }
        }
      }
    }
  }

  m_PendingSignals->recordChange();
  m_SignalHandlersLock.release();

  // Complete the destruction of the handler (waiting for deletion) with no
  // lock held.
  if (removal) {
    delete removal;
  }
}

void PosixSubsystem::resetSignalHandlersForExec(
    Thread* thread, SignalHandler* const handlers[SignalDispositionCount]) {
  if (!thread || thread->getParent() != m_pProcess || m_pProcess->getNumThreads() != 1) {
    FATAL("Exec signal reset requires the sole surviving process thread.");
  }

  for (size_t signal = 0; signal < SignalDispositionCount; ++signal) {
    if (!handlers[signal] || !handlers[signal]->pEvent ||
        handlers[signal]->pEvent->getNumber() != signal) {
      FATAL("Exec signal reset received an incomplete disposition table.");
    }
  }

  PendingSignalNotification notification(m_PendingSignals);
  LockGuard<Mutex> pendingGuard(m_PendingSignals->lock);
  SignalHandler* removals[SignalDispositionCount] = {};
  m_SignalHandlersLock.acquire();

  for (size_t signal = 0; signal < SignalDispositionCount; ++signal) {
    SignalHandler* replacement = handlers[signal];
    replacement->sig = signal;

    removals[signal] = m_SignalHandlers.lookup(signal);
    if (removals[signal]) {
      m_SignalHandlers.remove(signal);
    }
    m_SignalHandlers.insert(signal, replacement);

    if (!rebindQueuedSignalEvents(*thread, signal, *replacement->pEvent)) {
      FATAL("Exec signal reset could not rebind a pending signal.");
    }
  }

  m_PendingSignals->recordChange();
  m_SignalHandlersLock.release();

  for (SignalHandler* removal : removals) {
    delete removal;
  }
}

bool PosixSubsystem::getSignalDisposition(size_t sig, SignalDisposition& disposition,
                                          bool beginDelivery) {
  if (sig > MaximumSupportedSignal) {
    return false;
  }

  if (beginDelivery) {
    m_SignalHandlersLock.acquire();
  } else {
    m_SignalHandlersLock.enter();
  }

  SignalEvent* retired = nullptr;
  SignalHandler* handler = m_SignalHandlers.lookup(sig);
  if (handler) {
    disposition.handler = handler->pEvent ? handler->pEvent->getHandlerAddress() : 0;
    disposition.signalMask = handler->sigMask;
    disposition.flags = handler->flags;
    disposition.restorer = handler->restorer;
    disposition.type = handler->type;

    if (beginDelivery && handler->type == 0 && (handler->flags & SA_RESETHAND)) {
      // Queueing and competing deliveries use this same lock. Existing
      // AsyncEvents also resolve here, so no queued snapshot can catch twice.
      retired = handler->pEvent;
      handler->pEvent = new SignalEvent(pedigree_default_signal_handler(sig), sig, ~0UL, 0, true,
                                        false, Event::HandlerPrivilege::Kernel,
                                        SignalEvent::DeliveryDisposition::DefaultAction);
      handler->type = 1;
      handler->sigMask = 0;
      handler->flags = 0;
      handler->restorer = 0;
    }
  }

  if (beginDelivery) {
    m_SignalHandlersLock.release();
  } else {
    m_SignalHandlersLock.leave();
  }
  if (retired) {
    retired->retire();
  }
  return handler != nullptr;
}

PosixSubsystem::SignalDeliveryResult PosixSubsystem::queueSignalDelivery(
    Thread* target, size_t sig, uint32_t* flags, int32_t signalCode, bool processDirected,
    uint64_t signalValue, const SharedPointer<SignalEventState>& state) {
  PendingSignalNotification notification(m_PendingSignals);
  LockGuard<Mutex> pendingGuard(m_PendingSignals->lock);
  if (flags) {
    *flags = 0;
  }

  Process::ThreadLease processTarget;
  m_SignalHandlersLock.acquire();

  if (!target || !target->getParent() || target->getParent()->getSubsystem() != this || !sig ||
      sig > MaximumSupportedSignal) {
    m_SignalHandlersLock.release();
    return SignalDeliveryResult::Unavailable;
  }

  constexpr size_t StopSignals[] = {SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU};
  const size_t* signalsToDiscard = nullptr;
  size_t signalsToDiscardCount = 0;
  size_t continueSignal = SIGCONT;
  if (sig == SIGCONT) {
    signalsToDiscard = StopSignals;
    signalsToDiscardCount = sizeof(StopSignals) / sizeof(StopSignals[0]);
  } else {
    for (size_t stopSignal : StopSignals) {
      if (sig == stopSignal) {
        signalsToDiscard = &continueSignal;
        signalsToDiscardCount = 1;
        break;
      }
    }
  }

  Process* process = target->getParent();
  if (processDirected) {
    // Exec's pending-signal handoff holds this same lock after publishing
    // its owner, so an old-target publication must finish before the move.
    if (!process->acquireProcessSignalThread(processTarget)) {
      m_SignalHandlersLock.release();
      return SignalDeliveryResult::Rejected;
    }
    target = processTarget.get();
    // A registered synchronous waiter takes priority over asynchronous delivery.
    for (unsigned pass = 0; pass < 2; ++pass) {
      bool selected = false;
      for (size_t i = process->getNumThreads(); i > 0; --i) {
        Process::ThreadLease candidate;
        if (!process->acquireThread(candidate, i - 1) || !candidate->acceptingEvents() ||
            candidate->getUnwindState() != Thread::Continue)
          continue;
        const uint64_t bit = uint64_t(1) << (sig - 1);
        if ((pass == 0 && (candidate->getSynchronousSignalMask() & bit)) ||
            (pass == 1 && !(candidate->getSignalMask() & bit))) {
          processTarget = pedigree_std::move(candidate);
          target = processTarget.get();
          selected = true;
          break;
        }
      }
      if (selected)
        break;
    }
  }
  if (signalsToDiscard) {
    // Pending stop and continue signals are mutually exclusive across the
    // whole process, including thread-directed signals.
    for (size_t i = process->getNumThreads(); i > 0; --i) {
      Process::ThreadLease thread;
      if (process->acquireThread(thread, i - 1)) {
        for (size_t n = 0; n < signalsToDiscardCount; ++n) {
          thread->cullSignalEvent(signalsToDiscard[n]);
        }
      }
    }
  }

  // Continuation is a generation-time effect even when SIGCONT is blocked or
  // ignored. Publish Active before a caught handler can enter the event queue.
  if (sig == SIGCONT) {
    process->resume();
  }

  SignalHandler* handler = m_SignalHandlers.lookup(sig);
  SignalEvent* delivery = nullptr;
  const bool ignored =
      handler && (handler->type == 2 || (handler->type == 1 && defaultSignalActionIsIgnore(sig)));
  const uint64_t bit = uint64_t(1) << (sig - 1);
  const bool blocked = (target->getSignalMask() | target->getSynchronousSignalMask()) & bit;
  const bool suppressDelivery = ignored && !blocked;
  // A blocked ignored signal still belongs to sigwait/sigpending. Its inert
  // prototype also handles a later unblock without invoking a user handler.
  if (ignored && !handler->pEvent) {
    handler->pEvent = new SignalEvent(reinterpret_cast<uintptr_t>(ignoredSignal), sig, ~0UL, 0,
                                      true, false, Event::HandlerPrivilege::Kernel,
                                      SignalEvent::DeliveryDisposition::DefaultAction);
  }
  SignalDeliveryResult result = SignalDeliveryResult::Unavailable;
  if (suppressDelivery) {
    result = SignalDeliveryResult::Ignored;
  } else if (handler && handler->pEvent) {
    if (sig < LinuxPrivateSignalFirst && !state) {
      bool duplicate = false;
      if (processDirected) {
        for (size_t i = process->getNumThreads(); i > 0; --i) {
          Process::ThreadLease sibling;
          if (process->acquireThread(sibling, i - 1) && sibling->hasSignalEvent(sig, 1)) {
            duplicate = true;
            break;
          }
        }
      } else
        duplicate = target->hasSignalEvent(sig, 0);
      if (duplicate) {
        m_SignalHandlersLock.release();
        return SignalDeliveryResult::Queued;
      }
    }
    SharedPointer<SignalEventState> deliveryState = state;
    if (!deliveryState && (signalCode == -1 || sig >= 35)) {
      deliveryState = posix_signal_reserve_queue(process);
      if (!deliveryState) {
        m_SignalHandlersLock.release();
        return SignalDeliveryResult::Full;
      }
    }
    delivery = static_cast<SignalEvent*>(handler->pEvent->cloneForDelivery());
    Thread* sender = Processor::information().getCurrentThread();
    Process* senderProcess = sender ? sender->getParent() : nullptr;
    int32_t senderPid = 0;
    uint32_t senderUid = 0;
    if (senderProcess && senderProcess->getType() == Process::Posix) {
      senderPid = static_cast<int32_t>(senderProcess->getId());
      const int64_t uid = senderProcess->getUserId();
      if (uid >= 0) {
        senderUid = static_cast<uint32_t>(uid);
      }
    }
    static uint64_t nextSignalSequence = 0;
    delivery->setQueueSequence(__atomic_add_fetch(&nextSignalSequence, 1, __ATOMIC_RELAXED));
    delivery->setDeliveryState(deliveryState);
    if (sig == SIGCHLD && signalCode == 0 && senderProcess &&
        senderProcess->getParent() == process && senderProcess->getState() == Process::Terminated) {
      const int status = senderProcess->getExitStatus();
      signalCode = (status & 0x7f) ? ((status & 0x80) ? 3 : 2) : 1;
      delivery->setChildStatus((status & 0x7f) ? (status & 0x7f) : ((status >> 8) & 0xff),
                               senderProcess->getUserTime() / (Time::Multiplier::Second / 100),
                               senderProcess->getKernelTime() / (Time::Multiplier::Second / 100));
    }
    delivery->setSignalOrigin(signalCode, senderPid, senderUid);
    delivery->setSignalValue(signalValue);
    delivery->setProcessDirected(processDirected);
    stampStopDelivery(process, sig, delivery);
    if (flags) {
      *flags = handler->flags;
    }
    while (true) {
      if (target->sendEvent(delivery)) {
        result = SignalDeliveryResult::Queued;
        break;
      }
      // A recipient can start ordinary thread exit after selection. Its
      // closed queue does not discard a signal belonging to the process.
      if (!processDirected || !process->acquireProcessSignalThread(processTarget)) {
        result = SignalDeliveryResult::Rejected;
        break;
      }
      target = processTarget.get();
    }
  }

  m_SignalHandlersLock.release();
  m_PendingSignals->recordChange(result == SignalDeliveryResult::Queued ? sig : 0, target,
                                 processDirected);
  if (delivery && result == SignalDeliveryResult::Rejected) {
    delivery->rejectSignalDelivery();
    delete delivery;
  }
  return result;
}

/**
 * Note: POSIX  requires open()/accept()/etc to be safe during a signal
 * handler, which requires us to not allow signals during these file descriptor
 * calls. They cannot re-enter as they take process-specific locks.
 */

size_t PosixSubsystem::getFd(size_t minimum) {
  Uninterruptible throughout;

  // Enter critical section for writing.
  m_FdLock.acquire();

  // Try to recycle if possible
  const bool advancesGlobalHint = minimum <= m_LastFd;
  const size_t firstCandidate = minimum > m_LastFd ? minimum : m_LastFd;
  for (size_t i = firstCandidate; i < m_NextFd; i++) {
    if (!(m_FdBitmap.test(i))) {
      // A constrained F_DUPFD search must not hide lower holes from the
      // ordinary lowest-descriptor allocator.
      if (advancesGlobalHint) {
        m_LastFd = i;
      }
      m_FdBitmap.set(i);
      m_FdLock.release();
      return i;
    }
  }

  // Otherwise, allocate
  // m_NextFd will always be one beyond the highest allocated fd.
  const size_t ret = minimum > m_NextFd ? minimum : m_NextFd;
  m_FdBitmap.set(ret);
  m_NextFd = ret + 1;
  m_FdLock.release();
  return ret;
}

void PosixSubsystem::allocateFd(size_t fdNum) {
  Uninterruptible throughout;

  // Enter critical section for writing.
  m_FdLock.acquire();

  if (fdNum >= m_NextFd)
    m_NextFd = fdNum + 1;
  m_FdBitmap.set(fdNum);

  m_FdLock.release();
}

void PosixSubsystem::freeFd(size_t fdNum) {
  SharedPointer<FileDescriptor> retiring;

  {
    Uninterruptible throughout;

    // Unpublish atomically. Keep a private reference so the descriptor's
    // teardown cannot run while the table lock is held.
    m_FdLock.acquire();

    m_FdBitmap.clear(fdNum);
    m_FdMap.take(fdNum, retiring);

    if (fdNum < m_LastFd)
      m_LastFd = fdNum;

    m_FdLock.release();
  }

  // File/socket/event retirement can block and can re-enter unrelated
  // registries. It must happen after the descriptor-table lock is gone.
  if (retiring) {
    retireDescriptor(retiring.get());
  }
  retiring.reset();
}

bool PosixSubsystem::copyDescriptors(PosixSubsystem* pSubsystem) {
  assert(pSubsystem);

  Vector<SharedPointer<FileDescriptor>> retiring;

  // We're totally resetting our local state, ensure there's no files hanging
  // around.
  freeMultipleFds();

  {
    Uninterruptible throughout;

    // Totally changing everything... Don't allow other functions to
    // meddle.
    m_FdLock.acquire();
    pSubsystem->m_FdLock.acquire();

    // Copy each descriptor across from the original subsystem.
    FdMap& map = pSubsystem->m_FdMap;
    for (FdMap::Iterator it = map.begin(); it != map.end(); it++) {
      SharedPointer<FileDescriptor> pFd = it.value();
      if (!pFd)
        continue;
      size_t newFd = it.key();

      SharedPointer<FileDescriptor> pNewFd(new FileDescriptor(*pFd));
      assert(!pFd->networkImpl || pNewFd->networkPublished());

      // Perform the same action as addFileDescriptor. We need to
      // duplicate here because we currently hold the FD lock, which will
      // deadlock if we call any function which attempts to acquire it.
      if (newFd >= m_NextFd)
        m_NextFd = newFd + 1;
      m_FdBitmap.set(newFd);
      SharedPointer<FileDescriptor> previous;
      if (m_FdMap.take(newFd, previous)) {
        retiring.pushBack(pedigree_std::move(previous));
      }
      m_FdMap.insert(newFd, pedigree_std::move(pNewFd));
    }

    pSubsystem->m_FdLock.release();
    m_FdLock.release();
  }

  for (auto& descriptor : retiring) {
    retireDescriptor(descriptor.get());
  }
  retiring.clear(true);
  return true;
}

void PosixSubsystem::freeMultipleFds(bool bOnlyCloExec, size_t iFirst, size_t iLast) {
  assert(iFirst < iLast);

  // Table ownership is moved here before each node is erased. Destroying
  // this vector after unlocking performs potentially blocking descriptor
  // teardown outside the table critical section.
  Vector<SharedPointer<FileDescriptor>> retiring;

  {
    Uninterruptible throughout;

    m_FdLock.acquire();  // Don't allow any access to the FD data

    // Because removing FDs as we go from the Tree can actually leave the
    // Tree iterators in a dud state, remember all keys until traversal is
    // complete.
    List<void*> fdsToRemove;

    // Are all FDs to be freed? Or only a selection?
    bool bAllToBeFreed = ((iFirst == 0 && iLast == ~0UL) && !bOnlyCloExec);
    if (bAllToBeFreed)
      m_LastFd = 0;

    FdMap& map = m_FdMap;
    for (FdMap::Iterator it = map.begin(); it != map.end(); it++) {
      size_t Fd = it.key();
      SharedPointer<FileDescriptor> pFd = it.value();
      if (!pFd)
        continue;

      if (!(Fd >= iFirst && Fd <= iLast))
        continue;

      if (bOnlyCloExec) {
        if (!(pFd->fdflags & FD_CLOEXEC))
          continue;
      }

      // No longer usable.
      m_FdBitmap.clear(Fd);
      fdsToRemove.pushBack(reinterpret_cast<void*>(Fd));

      // Reset the "last freed" tracking variable, if this is lower than
      // it already.
      if (Fd < m_LastFd)
        m_LastFd = Fd;
    }

    for (List<void*>::Iterator it = fdsToRemove.begin(); it != fdsToRemove.end(); it++) {
      SharedPointer<FileDescriptor> descriptor;
      if (m_FdMap.take(reinterpret_cast<size_t>(*it), descriptor)) {
        retiring.pushBack(pedigree_std::move(descriptor));
      }
    }

    m_FdLock.release();
  }

  for (auto& descriptor : retiring) {
    retireDescriptor(descriptor.get());
  }
  retiring.clear(true);
}

bool PosixSubsystem::acquireFileDescriptor(size_t fd, DescriptorLease& descriptor) {
  descriptor.reset();
  {
    Uninterruptible throughout;
    m_FdLock.enter();
    SharedPointer<FileDescriptor> retained = m_FdMap.lookup(fd);
    m_FdLock.leave();
    descriptor.retain(retained);
  }
  return static_cast<bool>(descriptor);
}

bool PosixSubsystem::descriptorMatchesOpenDescription(
    size_t fd, const FileDescriptor::OpenFileDescriptionLease& expected) {
  if (!expected) {
    return false;
  }

  const SharedPointer<FileDescriptor> missing;
  Uninterruptible throughout;
  m_FdLock.enter();
  const SharedPointer<FileDescriptor>& current = m_FdMap.lookupRef(fd, missing);
  const bool matches = current && current->m_OpenFile.get() == expected.get();
  m_FdLock.leave();
  return matches;
}

bool PosixSubsystem::closeFileDescriptor(size_t fd, const DescriptorLease& descriptor) {
  if (!descriptor) {
    return false;
  }

  SharedPointer<FileDescriptor> current;
  SharedPointer<FileDescriptor> retiring;
  bool removed = false;

  {
    Uninterruptible throughout;

    m_FdLock.acquire();
    current = m_FdMap.lookup(fd);
    if (current == descriptor.m_Descriptor) {
      // Transfer the table owner rather than destroying it under the
      // lock. The lease supplied by close keeps the exact generation
      // alive while any descriptor-specific cleanup is performed.
      removed = m_FdMap.take(fd, retiring);
      if (removed) {
        m_FdBitmap.clear(fd);
        if (fd < m_LastFd) {
          m_LastFd = fd;
        }
      }
    }
    m_FdLock.release();
  }

  current.reset();
  if (retiring) {
    retireDescriptor(retiring.get());
  }
  retiring.reset();
  return removed;
}

void PosixSubsystem::addFileDescriptor(size_t fd, FileDescriptor* pFd) {
  SharedPointer<FileDescriptor> replacement(pFd);
  SharedPointer<FileDescriptor> retiring;

  {
    Uninterruptible throughout;

    // Publish the replacement and update allocation metadata in one
    // critical section. The old freeFd()/allocateFd() sequence briefly
    // exposed fd as available and allowed another allocator to steal it.
    m_FdLock.acquire();

    m_FdMap.take(fd, retiring);
    if (fd >= m_NextFd)
      m_NextFd = fd + 1;
    m_FdBitmap.set(fd);
    m_FdMap.insert(fd, replacement);

    m_FdLock.release();
  }

  if (retiring) {
    retireDescriptor(retiring.get());
  }
  retiring.reset();
}

PosixSubsystem::DescriptorDuplicationResult PosixSubsystem::duplicateFileDescriptor(
    size_t sourceFd, size_t targetFd, bool closeOnExec) {
  SharedPointer<FileDescriptor> source;
  SharedPointer<FileDescriptor> currentTarget;
  SharedPointer<FileDescriptor> replacement;
  SharedPointer<FileDescriptor> retiring;
  DescriptorDuplicationResult result = DescriptorDuplicationResult::BadSource;

  {
    Uninterruptible throughout;

    m_FdLock.acquire();
    source = m_FdMap.lookup(sourceFd);
    if (source) {
      currentTarget = m_FdMap.lookup(targetFd);
      if (!currentTarget && m_FdBitmap.test(targetFd)) {
        // getFd reserves a number before its creator publishes the table
        // entry. Linux reports EBUSY rather than allowing dup3 to steal that
        // in-flight allocation.
        result = DescriptorDuplicationResult::TargetBusy;
      } else {
        replacement.reset(new FileDescriptor(*source));

        // FileDescriptor's copy path only nests OFD/socket/eventfd owner-admission
        // locks, neither of which enters the descriptor table. Keeping
        // m_FdLock held makes final-close admission and publication atomic.
        if ((!source->networkImpl || replacement->networkPublished()) &&
            (!source->getEventFdImpl() || replacement->eventFdPublished()) &&
            (!source->getSignalFdImpl() || replacement->signalFdPublished()) &&
            (!source->getTimerFdImpl() || replacement->timerFdPublished())) {
          replacement->fd = targetFd;
          replacement->fdflags = closeOnExec ? FD_CLOEXEC : 0;
          m_FdMap.take(targetFd, retiring);
          if (targetFd >= m_NextFd) {
            m_NextFd = targetFd + 1;
          }
          m_FdBitmap.set(targetFd);
          m_FdMap.insert(targetFd, replacement);
          result = DescriptorDuplicationResult::Success;
        }
      }
    }
    m_FdLock.release();
  }

  source.reset();
  currentTarget.reset();
  if (retiring) {
    retireDescriptor(retiring.get());
  }
  retiring.reset();
  replacement.reset();
  return result;
}

size_t PosixSubsystem::installFileDescriptor(FileDescriptor* descriptor, DescriptorLease& lease,
                                             size_t minimum) {
  SharedPointer<FileDescriptor> published(descriptor);
  lease.reset();

  Uninterruptible throughout;
  m_FdLock.acquire();

  const bool advancesGlobalHint = minimum <= m_LastFd;
  const size_t firstCandidate = minimum > m_LastFd ? minimum : m_LastFd;
  size_t fd = minimum > m_NextFd ? minimum : m_NextFd;
  for (size_t candidate = firstCandidate; candidate < m_NextFd; ++candidate) {
    if (!m_FdBitmap.test(candidate)) {
      fd = candidate;
      if (advancesGlobalHint) {
        m_LastFd = candidate;
      }
      break;
    }
  }

  if (fd >= m_NextFd) {
    m_NextFd = fd + 1;
  }
  descriptor->fd = fd;
  m_FdBitmap.set(fd);
  m_FdMap.insert(fd, published);
  lease.retain(published);

  m_FdLock.release();
  return fd;
}

void PosixSubsystem::prepareThreadsForExec(Thread* owner) {
  PendingSignalNotification notification(m_PendingSignals);
  LockGuard<Mutex> pendingGuard(m_PendingSignals->lock);
  LockGuard<UnlikelyLock> guard(m_SignalHandlersLock);
  for (size_t i = m_pProcess->getNumThreads(); i > 0; --i) {
    Process::ThreadLease source;
    if (m_pProcess->acquireThread(source, i - 1) && source.get() != owner &&
        !source->transferProcessSignalsTo(*owner)) {
      FATAL("Exec owner rejected a pending process signal.");
    }
  }
  m_PendingSignals->recordChange();
}

void PosixSubsystem::preserveProcessSignalsForThreadExit(Thread* thread) {
  PendingSignalNotification notification(m_PendingSignals);
  LockGuard<Mutex> pendingGuard(m_PendingSignals->lock);
  LockGuard<UnlikelyLock> guard(m_SignalHandlersLock);
  Process::ThreadLease target;
  while (m_pProcess->acquireProcessSignalThread(target)) {
    if (thread->transferProcessSignalsTo(*target.get())) {
      m_PendingSignals->recordChange();
      return;
    }
  }
}

void PosixSubsystem::retireDescriptor(FileDescriptor* descriptor) {
  if (descriptor->file) {
    posix_advisory_descriptor_closed(m_AdvisoryOwner, descriptor->file->futexIdentity());
  }
  // Queue descriptors have no VFS backing. File retirement must not wait for
  // an in-flight operation holding the shared file-position mutex.
  if (!descriptor->file) {
    SharedPointer<PosixMessageQueue> queue = descriptor->getMqueueImpl();
    if (queue && m_pProcess) {
      posix_mqueue_close(queue.get(), m_pProcess->getId());
    }
  }
  descriptor->unpublish();
}

void PosixSubsystem::threadExiting(Thread* pThread) {
  if (!pThread) {
    return;
  }

  if (m_Namespaces) {
    const size_t taskId = pThread->getTaskId();
    m_Namespaces->retireThread(*pThread);
    procfsInvalidateNamespaceTask(m_Namespaces, pThread->getParent()->getId(), taskId);
  }
  m_PendingSignals->retireThread(pThread);
  posix_timer_thread_exit(pThread);
  posix_sem_thread_exit(pThread);
  posix_robust_list_exit(pThread);

  const uintptr_t address = pThread->takeClearChildTid();
  if (!address) {
    return;
  }

  Process* process = pThread->getParent();
  if (!process) {
    return;
  }

  const bool cleared = posix_clear_child_tid(process, address);
  if (!cleared) {
    PS_NOTICE("clear-child-TID could not access target at " << Hex << address << " for tid " << Dec
                                                            << pThread->getId());
  }

  // The registration is already consumed. Wake even if the restricted
  // validated store could not reach the word, so no waiter is stranded in the
  // kernel after an invalid registration or concurrent unmap.
  posix_futex_wake(process, reinterpret_cast<int*>(address), 1);
}

void PosixSubsystem::threadRemoved(Thread* pThread) {
  for (Tree<size_t, PosixThread*>::Iterator it = m_Threads.begin(); it != m_Threads.end(); it++) {
    PosixThread* thread = it.value();
    if (thread->pThread != pThread)
      continue;

    // Can safely assert that this thread is no longer running.
    // We do not however kill the thread object yet. It can be cleaned up
    // when the PosixSubsystem quits (if this was the last thread). Or, it
    // will be cleaned up by a join().
    thread->isRunning.complete();
    break;
  }
}

bool PosixSubsystem::checkAccess(const DescriptorLease& pFileDescriptor, bool bRead, bool bWrite,
                                 bool bExecute) const {
  return VFS::checkAccess(pFileDescriptor->file, bRead, bWrite, bExecute);
}

bool PosixSubsystem::prepareExecutable(File* pFile, ExecutableImage& image, bool isInterpreter) {
  image.file = pFile;
  image.fileSize = pFile->getSize();

  uint8_t header[sizeof(Elf::ElfHeader_t)];
  if (pFile->read(0, sizeof(header), reinterpret_cast<uintptr_t>(header)) != sizeof(header)) {
    setExecutableValidationError(Elf::ExecutableValidationResult::Malformed, isInterpreter);
    return false;
  }

  Elf::ExecutableValidationResult result =
      Elf::validateExecutableHeader(header, sizeof(header), image.fileSize, image.metadata);
  if (result != Elf::ExecutableValidationResult::Valid) {
    setExecutableValidationError(result, isInterpreter);
    return false;
  }

  image.programHeaders = UniqueArray<uint8_t>::allocate(image.metadata.programHeaderSize);
  if (!image.programHeaders) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  if (pFile->read(image.metadata.programHeaderOffset, image.metadata.programHeaderSize,
                  reinterpret_cast<uintptr_t>(image.programHeaders.get())) !=
      image.metadata.programHeaderSize) {
    setExecutableValidationError(Elf::ExecutableValidationResult::Malformed, isInterpreter);
    return false;
  }

  result = Elf::validateExecutableProgramHeaders(
      image.programHeaders.get(), image.metadata.programHeaderSize, image.fileSize, image.metadata);
  if (result != Elf::ExecutableValidationResult::Valid) {
    setExecutableValidationError(result, isInterpreter);
    return false;
  }

  VirtualAddressSpace& addressSpace = Processor::information().getVirtualAddressSpace();
  if (image.metadata.type == ET_EXEC &&
      (image.metadata.loadStart < addressSpace.getUserStart() ||
       image.metadata.loadEnd > addressSpace.getUserReservedStart())) {
    setExecutableValidationError(Elf::ExecutableValidationResult::UnsupportedLayout, isInterpreter);
    return false;
  }

  bool foundProgramHeaders = false;
  for (size_t i = 0; i < image.metadata.programHeaderCount; ++i) {
    Elf::ElfProgramHeader_t programHeader;
    MemoryCopy(&programHeader, image.programHeaders.get() + (i * sizeof(Elf::ElfProgramHeader_t)),
               sizeof(programHeader));
    if (programHeader.type != PT_LOAD ||
        image.metadata.programHeaderOffset < programHeader.offset) {
      continue;
    }

    const size_t offsetInSegment = image.metadata.programHeaderOffset - programHeader.offset;
    if (offsetInSegment > programHeader.filesz ||
        image.metadata.programHeaderSize > programHeader.filesz - offsetInSegment ||
        programHeader.vaddr > ~uintptr_t{0} - offsetInSegment) {
      continue;
    }

    image.programHeaderAddress = programHeader.vaddr + offsetInSegment;
    foundProgramHeaders = true;
    break;
  }
  if (!foundProgramHeaders) {
    setExecutableValidationError(Elf::ExecutableValidationResult::UnsupportedLayout, isInterpreter);
    return false;
  }

  if (!image.metadata.hasInterpreter) {
    return true;
  }

  UniqueArray<uint8_t> interpreter = UniqueArray<uint8_t>::allocate(image.metadata.interpreterSize);
  if (!interpreter) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  if (pFile->read(image.metadata.interpreterOffset, image.metadata.interpreterSize,
                  reinterpret_cast<uintptr_t>(interpreter.get())) !=
      image.metadata.interpreterSize) {
    setExecutableValidationError(Elf::ExecutableValidationResult::Malformed, isInterpreter);
    return false;
  }

  result = Elf::validateExecutableInterpreter(interpreter.get(), image.metadata.interpreterSize,
                                              image.metadata);
  if (result != Elf::ExecutableValidationResult::Valid) {
    setExecutableValidationError(result, isInterpreter);
    return false;
  }
  for (size_t i = 0; i + 1 < image.metadata.interpreterSize; ++i) {
    if (!interpreter.get()[i]) {
      setExecutableValidationError(Elf::ExecutableValidationResult::Malformed, isInterpreter);
      return false;
    }
  }

  image.interpreter.assign(reinterpret_cast<const char*>(interpreter.get()),
                           image.metadata.interpreterSize - 1, true);
  return true;
}

bool PosixSubsystem::loadElf(const ExecutableImage& image, uintptr_t& loadBias) {
  PS_NOTICE("PosixSubsystem::loadElf(" << image.file->getName() << ")");

  Elf::ExecutableMetadata metadata = image.metadata;
  if (Elf::validateExecutableProgramHeaders(image.programHeaders.get(),
                                            image.metadata.programHeaderSize, image.fileSize,
                                            metadata) != Elf::ExecutableValidationResult::Valid) {
    return false;
  }

  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  const size_t allocationSize = metadata.loadEnd - metadata.loadStart;
  if (metadata.type == ET_DYN) {
    uintptr_t allocation = 0;
    if (!pProcess->allocateUserRange(Process::UserRegion::Dynamic, allocationSize, allocation) &&
        !pProcess->allocateUserRange(Process::UserRegion::Normal, allocationSize, allocation)) {
      return false;
    }
    loadBias = allocation - metadata.loadStart;
  } else {
    if (!pProcess->allocateSpecificUserRange(Process::UserRegion::Normal, metadata.loadStart,
                                             allocationSize)) {
      return false;
    }
    loadBias = 0;
  }

  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const uintptr_t pageMask = pageSize - 1;
  for (size_t i = 0; i < metadata.programHeaderCount; ++i) {
    Elf::ElfProgramHeader_t programHeader;
    MemoryCopy(&programHeader, image.programHeaders.get() + (i * sizeof(Elf::ElfProgramHeader_t)),
               sizeof(programHeader));
    if (programHeader.type != PT_LOAD || !programHeader.memsz) {
      continue;
    }

    if (programHeader.vaddr > ~uintptr_t{0} - loadBias) {
      return false;
    }
    const uintptr_t segmentAddress = loadBias + programHeader.vaddr;
    const uintptr_t pageOffset = segmentAddress & pageMask;
    if (programHeader.memsz > ~size_t{0} - pageOffset) {
      return false;
    }
    size_t length = programHeader.memsz + pageOffset;
    if (length > ~size_t{0} - pageMask) {
      return false;
    }
    length = (length + pageMask) & ~pageMask;

    uintptr_t base = segmentAddress & ~pageMask;
    const size_t fileOffset = programHeader.offset & ~pageMask;
    MemoryMappedObject::Permissions perms = MemoryMappedObject::Read;
    if (programHeader.flags & PF_X) {
      perms |= MemoryMappedObject::Exec;
    }
    if (programHeader.flags & PF_W) {
      perms |= MemoryMappedObject::Write;
    }

    const uintptr_t fileEnd = segmentAddress + programHeader.filesz;
    const bool hasPartialBssPage =
        programHeader.memsz > programHeader.filesz && (fileEnd & pageMask) != 0;
    MemoryMappedObject::Permissions mappingPerms = perms;
    if (hasPartialBssPage) {
      mappingPerms |= MemoryMappedObject::Write;
    }

    PS_NOTICE(image.file->getName()
              << " PHDR[" << i << "]: @" << Hex << base << " -> " << base + length);
    if (!MemoryMapManager::instance().mapFile(image.file, base, length, mappingPerms, fileOffset,
                                              true)) {
      ERROR("PosixSubsystem::loadElf: failed to map PT_LOAD section");
      return false;
    }

    if (programHeader.memsz > programHeader.filesz) {
      const uintptr_t end = segmentAddress + programHeader.memsz;
      uintptr_t zeroStart = segmentAddress + programHeader.filesz;
      if (hasPartialBssPage) {
        const size_t numBytes = pageSize - (zeroStart & pageMask);
        ByteSet(reinterpret_cast<void*>(zeroStart), 0, numBytes);
        zeroStart += numBytes;
      }

      if (zeroStart < end) {
        uintptr_t anonymousAddress = zeroStart;
        if (!MemoryMapManager::instance().mapAnon(anonymousAddress, end - zeroStart,
                                                  mappingPerms)) {
          ERROR(
              "PosixSubsystem::loadElf: failed to map anonymous "
              "pages for filesz/memsz mismatch");
          return false;
        }
      }
    }

    if (hasPartialBssPage && !MemoryMapManager::instance().setPermissions(base, length, perms)) {
      ERROR("PosixSubsystem::loadElf: failed to restore PT_LOAD permissions");
      return false;
    }
  }

  return true;
}

File* PosixSubsystem::findFile(const String& path, File* workingDir) {
  Process::FileContextLease workingDirLease;
  if (workingDir == nullptr) {
    assert(m_pProcess);
    workingDir = m_pProcess->acquireCwd(workingDirLease);
    if (!workingDir) {
      return nullptr;
    }
  }

  bool mountAwareAbi = getAbi() != PosixSubsystem::LinuxAbi;

  // Non-mount-aware ABIs resolve absolute paths from the root filesystem,
  // independent of the current working directory.
  if (mountAwareAbi || (path[0] != '/')) {
    // no fall back for mount-aware ABIs (e.g. Pedigree's ABI)
    // or it's a non-absolute path on a non-mount-aware ABI, and therefore
    // needs to be based on the working directory - not a different FS
    return VFS::instance().find(path, workingDir);
  }

  // fall back to the current root filesystem
  Filesystem* rootFs = VFS::instance().getRootFilesystem();
  if (rootFs) {
    return VFS::instance().find(path, rootFs->getRoot());
  }

  return nullptr;
}

File* PosixSubsystem::findFileRetained(const String& path, Directory::ChildLease& result,
                                       File* workingDir) {
  Process::FileContextLease workingDirLease;
  if (workingDir == nullptr) {
    assert(m_pProcess);
    workingDir = m_pProcess->acquireCwd(workingDirLease);
    if (!workingDir) {
      return nullptr;
    }
  }

  const bool mountAwareAbi = getAbi() != PosixSubsystem::LinuxAbi;
  if (mountAwareAbi || (path[0] != '/')) {
    return VFS::instance().findRetained(path, result, workingDir);
  }

  Filesystem* rootFs = VFS::instance().getRootFilesystem();
  if (rootFs) {
    return VFS::instance().findRetained(path, result, rootFs->getRoot());
  }

  return nullptr;
}

#define STACK_PUSH(stack, value) *--stack = value
#define STACK_PUSH2(stack, value1, value2) \
  STACK_PUSH(stack, value1);               \
  STACK_PUSH(stack, value2)
#define STACK_PUSH_COPY(stack, value, length) \
  stack = adjust_pointer(stack, -(length));   \
  MemoryCopy(stack, value, length)
#define STACK_PUSH_STRING(stack, str, length) \
  stack = adjust_pointer(stack, -(length));   \
  StringCopyN(reinterpret_cast<char*>(stack), str, length)
#define STACK_PUSH_ZEROES(stack, length)    \
  stack = adjust_pointer(stack, -(length)); \
  ByteSet(stack, 0, length)
#define STACK_ALIGN(stack, to) \
  STACK_PUSH_ZEROES(stack, (to) - ((to) - (reinterpret_cast<uintptr_t>(stack) & ((to) - 1))))

bool PosixSubsystem::invoke(const char* name, Vector<String>& argv, Vector<String>& env) {
  return invoke(name, argv, env, 0);
}

bool PosixSubsystem::invoke(const char* name, Vector<String>& argv, Vector<String>& env,
                            SyscallState& state) {
  return invoke(name, argv, env, &state);
}

bool PosixSubsystem::parseShebang(File* pFile, String& interpreter, String& optionalArgument,
                                  bool& hasOptionalArgument) {
  PS_NOTICE("Attempting to parse shebang in " << pFile->getFullPath());

  static constexpr size_t ShebangBufferSize = 256;
  char contents[ShebangBufferSize];
  const size_t bytesRead = pFile->read(0, sizeof(contents), reinterpret_cast<uintptr_t>(contents));

  interpreter.clear();
  optionalArgument.clear();
  hasOptionalArgument = false;

  if (bytesRead < 2 || contents[0] != '#' || contents[1] != '!') {
    PS_NOTICE("no shebang found");
    return true;
  }

  size_t lineEnd = bytesRead;
  bool terminated = bytesRead < sizeof(contents);
  for (size_t i = 2; i < bytesRead; ++i) {
    if (contents[i] == '\n' || !contents[i]) {
      lineEnd = i;
      terminated = true;
      break;
    }
  }

  const size_t boundedLineEnd = lineEnd;
  while (lineEnd > 2 && (contents[lineEnd - 1] == ' ' || contents[lineEnd - 1] == '\t')) {
    --lineEnd;
  }

  size_t interpreterBegin = 2;
  while (interpreterBegin < lineEnd &&
         (contents[interpreterBegin] == ' ' || contents[interpreterBegin] == '\t')) {
    ++interpreterBegin;
  }
  if (interpreterBegin == lineEnd) {
    PS_NOTICE("empty shebang interpreter");
    SYSCALL_ERROR(ExecFormatError);
    return false;
  }

  size_t interpreterEnd = interpreterBegin;
  while (interpreterEnd < lineEnd && contents[interpreterEnd] != ' ' &&
         contents[interpreterEnd] != '\t') {
    ++interpreterEnd;
  }
  if (interpreterEnd == lineEnd && lineEnd == boundedLineEnd && !terminated) {
    PS_NOTICE("truncated shebang interpreter");
    SYSCALL_ERROR(ExecFormatError);
    return false;
  }

  interpreter.assign(contents + interpreterBegin, interpreterEnd - interpreterBegin, true);

  size_t argumentBegin = interpreterEnd;
  while (argumentBegin < lineEnd &&
         (contents[argumentBegin] == ' ' || contents[argumentBegin] == '\t')) {
    ++argumentBegin;
  }
  if (argumentBegin < lineEnd) {
    optionalArgument.assign(contents + argumentBegin, lineEnd - argumentBegin, true);
    hasOptionalArgument = true;
  }

  return true;
}

static File* traverseForInvoke(File* pFile, Directory::ChildLease& lease) {
  // Do symlink traversal.
  Tree<File*, File*> loopDetect;
  while (pFile && pFile->isSymlink()) {
    Directory::ChildLease nextLease;
    pFile = Symlink::fromFile(pFile)->followLinkRetained(nextLease);
    if (pFile) {
      lease.swap(nextLease);
      if (loopDetect.lookup(pFile)) {
        SYSCALL_ERROR(LoopExists);
        return nullptr;
      }
      loopDetect.insert(pFile, pFile);
    }
  }
  if (!pFile) {
    PS_NOTICE("PosixSubsystem::invoke: symlink traversal failed");
    SYSCALL_ERROR(DoesNotExist);
    return 0;
  }

  // Check for directory.
  if (pFile->isDirectory()) {
    PS_NOTICE("PosixSubsystem::invoke: target is a directory");
    SYSCALL_ERROR(IsADirectory);
    return 0;
  }

  return pFile;
}

bool PosixSubsystem::invoke(const char* name, Vector<String>& argv, Vector<String>& env,
                            SyscallState* state) {
  // Save the original name before we trash the old stack.
  String originalName(name);

  // Try and find the target file we want to invoke.
  Directory::ChildLease originalLease;
  File* originalFile = findFileRetained(originalName, originalLease, nullptr);
  if (!originalFile) {
    PS_NOTICE("PosixSubsystem::invoke: could not find file '" << originalName << "'");
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }

  return invoke(originalFile, originalName, argv, env, state);
}

bool PosixSubsystem::invoke(File* originalFile, const String& originalName, Vector<String>& argv,
                            Vector<String>& env) {
  return invoke(originalFile, originalName, argv, env, 0);
}

bool PosixSubsystem::invoke(File* originalFile, const String& originalName, Vector<String>& argv,
                            Vector<String>& env, SyscallState& state) {
  return invoke(originalFile, originalName, argv, env, &state);
}

bool PosixSubsystem::invoke(File* originalFile, const String& originalName, Vector<String>& argv,
                            Vector<String>& env, SyscallState* state) {
  PS_NOTICE("PosixSubsystem::invoke(" << originalName << ")");

  uint8_t execRandom[16];
  ByteSet(execRandom, 0, sizeof(execRandom));
#if X64 && !HOSTED
  const bool hasExecRandom =
      hardware_random_bytes(execRandom, sizeof(execRandom)) == sizeof(execRandom);
  if (!hasExecRandom) {
    PS_NOTICE("PosixSubsystem::invoke: AT_RANDOM unavailable on this CPU");
  }
#else
  const bool hasExecRandom = false;
#endif

  Thread* pThread = Processor::information().getCurrentThread();
  Process* pProcess = pThread->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());

  Process::ExecScope execScope(*pProcess, state != nullptr);
  if (!execScope) {
    SYSCALL_ERROR(NoMoreProcesses);
    return false;
  }

  Directory::ChildLease originalTargetLease;
  originalFile = traverseForInvoke(originalFile, originalTargetLease);
  if (!originalFile) {
    // traverseForInvoke does a SYSCALL_ERROR for us
    return false;
  }

  uint8_t magic[4];
  String candidateName(originalName);
  static constexpr size_t MaximumShebangRewrites = 4;
  size_t shebangRewrites = 0;
  bool allExecutableFilesReadable = true;
  while (true) {
    // Execute permission checks precede all format reads for every candidate,
    // including nested shebang interpreters.
    if (!VFS::checkAccess(originalFile, false, false, true)) {
      return false;
    }
    allExecutableFilesReadable &= posix_exec_file_readable(originalFile);

    const size_t bytesRead =
        originalFile->read(0, sizeof(magic), reinterpret_cast<uintptr_t>(magic));
    if (bytesRead == sizeof(magic) && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' &&
        magic[3] == 'F') {
      break;
    }

    PS_NOTICE("PosixSubsystem::invoke: '" << originalFile->getName()
                                          << "' is not an ELF binary, looking for shebang...");

    String shebangInterpreter;
    String shebangArgument;
    bool hasShebangArgument = false;
    if (!parseShebang(originalFile, shebangInterpreter, shebangArgument, hasShebangArgument)) {
      PS_NOTICE("PosixSubsystem::invoke: failed to parse shebang line in '"
                << originalFile->getName() << "'");
      return false;
    }

    if (!shebangInterpreter.length()) {
      SYSCALL_ERROR(ExecFormatError);
      return false;
    }

    if (shebangRewrites == MaximumShebangRewrites) {
      SYSCALL_ERROR(LoopExists);
      return false;
    }
    ++shebangRewrites;

    String resolvedInterpreter(shebangInterpreter);
    String normalisedInterpreter;
    if (normalisePath(normalisedInterpreter, resolvedInterpreter.cstr())) {
      resolvedInterpreter = normalisedInterpreter;
    }

    Directory::ChildLease nextLease;
    File* shebangFile = findFileRetained(resolvedInterpreter, nextLease, nullptr);
    if (!shebangFile) {
      PS_NOTICE("PosixSubsystem::invoke: could not find shebang interpreter '"
                << resolvedInterpreter << "'");
      SYSCALL_ERROR(DoesNotExist);
      return false;
    }

    shebangFile = traverseForInvoke(shebangFile, nextLease);
    if (!shebangFile) {
      return false;
    }

    if (argv.count()) {
      argv.popFront();
    }
    argv.pushFront(candidateName);
    if (hasShebangArgument) {
      argv.pushFront(shebangArgument);
    }
    argv.pushFront(shebangInterpreter);

    originalFile = shebangFile;
    candidateName = shebangInterpreter;
    originalTargetLease.swap(nextLease);
  }

  // Recheck after shebang rewriting, which adds kernel-owned arguments.
  // The budget includes the vectors and AT_EXECFN's copied string.
  size_t argumentBytes = 2 * sizeof(uintptr_t);
  if (originalName.length() >= MaximumExecArgumentBytes - argumentBytes) {
    SYSCALL_ERROR(TooBig);
    return false;
  }
  argumentBytes += originalName.length() + 1;
  Vector<String>* argumentLists[] = {&argv, &env};
  for (Vector<String>* list : argumentLists) {
    for (size_t i = 0; i < list->count(); ++i) {
      const size_t remaining = MaximumExecArgumentBytes - argumentBytes;
      if (remaining <= sizeof(uintptr_t) || (*list)[i].length() >= remaining - sizeof(uintptr_t)) {
        SYSCALL_ERROR(TooBig);
        return false;
      }
      argumentBytes += sizeof(uintptr_t) + (*list)[i].length() + 1;
    }
  }

  Directory::ChildLease interpreterLease;
  File* interpreterFile = 0;

  // A failed validation path must restore both event and termination
  // delivery, while a successful exec keeps both deferred until the new
  // userspace transition has been scheduled.
  Uninterruptible execCriticalSection;

  ExecutableImage originalImage;
  if (!prepareExecutable(originalFile, originalImage, false)) {
    return false;
  }

  ExecutableImage interpreterImage;
  if (originalImage.metadata.hasInterpreter) {
    String interpreter(originalImage.interpreter);

    // Existing binaries and PUP packages may still name the interpreter
    // using Pedigree's pre-FHS layout.
    String normalisedInterpreter;
    if (normalisePath(normalisedInterpreter, interpreter.cstr())) {
      interpreter = normalisedInterpreter;
    }

    // Ensure we can actually find the interpreter.
    interpreterFile = findFileRetained(interpreter, interpreterLease, nullptr);
    interpreterFile = traverseForInvoke(interpreterFile, interpreterLease);
    if (!interpreterFile) {
      PS_NOTICE("PosixSubsystem::invoke: could not find interpreter '" << interpreter << "'");
      return false;
    }

    if (!VFS::checkAccess(interpreterFile, false, false, true)) {
      return false;
    }
    allExecutableFilesReadable &= posix_exec_file_readable(interpreterFile);

    if (!prepareExecutable(interpreterFile, interpreterImage, true)) {
      return false;
    }
    if (interpreterImage.metadata.hasInterpreter) {
      SYSCALL_ERROR(BadSharedLibrary);
      return false;
    }
  } else {
    // Static binaries enter at their own entry point. Loading the target
    // again as its own interpreter would reserve every PT_LOAD range twice.
    interpreterFile = 0;
  }

  if (interpreterFile && originalImage.metadata.type == ET_EXEC &&
      interpreterImage.metadata.type == ET_EXEC &&
      originalImage.metadata.loadStart < interpreterImage.metadata.loadEnd &&
      interpreterImage.metadata.loadStart < originalImage.metadata.loadEnd) {
    SYSCALL_ERROR(BadSharedLibrary);
    return false;
  }

  UniquePointer<PreparedUtsThread> initialUts;
  if (!m_Namespaces || !m_Namespaces->valid()) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  if (!state) {
    UtsRef currentUts;
    if (!m_Namespaces->acquireThread(*pThread, currentUts)) {
      posix_uts_error(UtsStatus::Missing);
      return false;
    }
    const UtsStatus prepared = posix_uts_prepare_thread(currentUts, false, initialUts);
    if (prepared != UtsStatus::Success) {
      posix_uts_error(prepared);
      return false;
    }
  }

  // Validation leaves the old process intact. Siblings must finish their
  // user-memory exit hooks and release their mappings before replacement.
  if (!execScope.commit()) {
    SYSCALL_ERROR(Interrupted);
    return false;
  }

  invalidateUserImage();

  posix_timer_process_exit(pProcess);

  // A descriptor can survive exec after clearing FD_CLOEXEC, but its old
  // image's notification registration must not target the replacement image.
  posix_mqueue_process_exit(pProcess->getId());

  // Wipe out old address space.
  // Earlier failures preserve the registration. From this irreversible
  // point onward its target belongs to the discarded image.
  pThread->setClearChildTid(0);
  posix_robust_list_exit(pThread);
  const size_t previousTaskId = pThread->getTaskId();
  execScope.adoptLeaderIdentity();
  m_Namespaces->promoteExec(*pThread);
  procfsInvalidateNamespaceTask(m_Namespaces, pProcess->getId(), previousTaskId);
  procfsInvalidateNamespaceTask(m_Namespaces, pProcess->getId(), pProcess->getId());
  DynamicLinker* oldLinker = pProcess->getLinker();
  pProcess->setLinker(nullptr);
  pThread->retireInputUserStack();
  {
    MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
    // Exec is irreversible here. Old continuations cannot resume, and the
    // clean user-return gate runs only after their remaining cleanup.
    if (m_CallbackSchedulingDomain == CallbackSchedulingDomain::LegacySignals)
      m_CallbackSchedulingDomain = CallbackSchedulingDomain::Unrestricted;
    MemoryMapManager::instance().unmapAll();
    delete oldLinker;

    // We now need to clean up the process' address space.
    pProcess->resetUserReservations();
    pProcess->getAddressSpace()->rawUserMemory().clear();
    m_MemoryLockAccount.publish({}, MemoryLockMode::None);
    pProcess->getAddressSpace()->revertToKernelAddressSpace();
    pProcess->getAddressSpace()->rawUserMemory().setCompleteInventory(X64 && !HOSTED);
  }

  // The old mappings are gone, but Thread state levels still own their Stack
  // descriptors. Drop only that metadata: freeStack could otherwise unmap a
  // replacement mapping which reuses an old stack address.
  pThread->discardUserStackMetadataForExec();

  // Pending signal deliveries must no longer refer to handlers in the old
  // image before any post-commit operation can fail and unwind this call.
  pedigree_init_sigret();

  auto failAfterCommit = [pThread](Error::PosixError error) {
    syscallError(error);
    pThread->deferSignalExit(SIGSEGV);
    return false;
  };

  // Load the target application first.
  uintptr_t originalLoadBias = 0;
  if (!loadElf(originalImage, originalLoadBias)) {
    PS_NOTICE("PosixSubsystem::invoke: failed to load target");
    return failAfterCommit(Error::OutOfMemory);
  }

  // Now load the interpreter.
  uintptr_t interpreterLoadBias = 0;
  if (interpreterFile && !loadElf(interpreterImage, interpreterLoadBias)) {
    PS_NOTICE("PosixSubsystem::invoke: failed to load interpreter");
    return failAfterCommit(Error::OutOfMemory);
  }

  const uintptr_t originalEntryPoint = originalLoadBias + originalImage.metadata.entryPoint;
  uintptr_t interpreterEntryPoint = 0;
  if (interpreterFile) {
    interpreterEntryPoint = interpreterLoadBias + interpreterImage.metadata.entryPoint;
  }
  if (!interpreterFile) {
    interpreterEntryPoint = originalEntryPoint;
  }

  // Past point of no return, so set up the process for the new image.
  pProcess->description() = originalName;
  pProcess->resetCounts();
  pThread->resetTlsBase();
  if (pSubsystem)
    pSubsystem->freeMultipleFds(true);
  PosixProcess::CredentialSnapshot execCredentials;
  if (pProcess->getType() == Process::Posix) {
    PosixProcess* p = static_cast<PosixProcess*>(pProcess);
    MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
    p->commitExecCredentials(*pThread, allExecutableFilesReadable);
    execCredentials = p->snapshotCredentials();
  } else {
    execCredentials.ruid = pProcess->getUserId();
    execCredentials.euid = pProcess->getEffectiveUserId();
    execCredentials.rgid = pProcess->getGroupId();
    execCredentials.egid = pProcess->getEffectiveGroupId();
  }

  // Allocate some space for the VDSO
  MemoryMappedObject::Permissions vdsoPerms =
      MemoryMappedObject::Read | MemoryMappedObject::Write | MemoryMappedObject::Exec;
  uintptr_t vdsoAddress = 0;
  MemoryMappedObject* pVdso = MemoryMapManager::instance().mapAnon(
      vdsoAddress, __vdso_so_pages * PhysicalMemoryManager::getPageSize(), vdsoPerms);
  if (!pVdso) {
    PS_NOTICE("PosixSubsystem::invoke: failed to map VDSO");
  } else {
    // All good, copy in the VDSO ELF image now.
    MemoryCopy(reinterpret_cast<void*>(vdsoAddress), __vdso_so, __vdso_so_len);

    // Readjust permissions to remove write access now that the image is
    // loaded.
    MemoryMapManager::instance().setPermissions(
        vdsoAddress, __vdso_so_pages * PhysicalMemoryManager::getPageSize(),
        vdsoPerms & ~MemoryMappedObject::Write);
  }

// The hosted process owns the Linux host's fixed vsyscall address. Its musl
// userspace uses the syscall bridge instead.
#if !HOSTED
  // Map in the vsyscall space.
  if (!Processor::information().getVirtualAddressSpace().isMapped(
          reinterpret_cast<void*>(POSIX_VSYSCALL_ADDRESS))) {
    physical_uintptr_t vsyscallBase = 0;
    size_t vsyscallFlags = 0;
    Processor::information().getVirtualAddressSpace().getMapping(&__posix_compat_vsyscall_base,
                                                                 vsyscallBase, vsyscallFlags);
    Processor::information().getVirtualAddressSpace().map(
        vsyscallBase, reinterpret_cast<void*>(POSIX_VSYSCALL_ADDRESS),
        VirtualAddressSpace::Execute);
  }
#endif

  // We can now build the auxiliary vector to pass to the dynamic linker.
  VirtualAddressSpace::Stack* stack =
      Processor::information().getVirtualAddressSpace().allocateStack();
  if (!stack || !stack->getTop()) {
    delete stack;
    ERROR("PosixSubsystem::invoke: failed to allocate initial user stack");
    return failAfterCommit(Error::OutOfMemory);
  }
  // Auxiliary vectors, fixed strings, argc, and alignment consume fewer
  // than 512 bytes in addition to the already bounded argument payload.
  if (stack->getSize() < argumentBytes + 512) {
    Processor::information().getVirtualAddressSpace().freeStack(stack);
    return failAfterCommit(Error::TooBig);
  }
  if (state) {
    pThread->adoptInitialUserStackForExec(stack);
  }
  uintptr_t* loaderStack = reinterpret_cast<uintptr_t*>(stack->getTop());

  // Top of stack = zero to mark end
  STACK_PUSH(loaderStack, 0);

  // Align to 16 byte stack
  STACK_ALIGN(loaderStack, 16);

  // Push argv/env.
  char** envs = new char*[env.count()];
  size_t envc = 0;
  for (size_t i = 0; i < env.count(); ++i) {
    String& str = env[i];
    STACK_PUSH_STRING(loaderStack, static_cast<const char*>(str), str.length() + 1);
    PS_NOTICE("env[" << envc << "]: " << str);
    envs[envc++] = reinterpret_cast<char*>(loaderStack);
  }

  // Align to 16 bytes between env and argv
  STACK_ALIGN(loaderStack, 16);

  char** argvs = new char*[argv.count()];
  size_t argc = 0;
  for (size_t i = 0; i < argv.count(); ++i) {
    String& str = argv[i];
    STACK_PUSH_STRING(loaderStack, static_cast<const char*>(str), str.length() + 1);
    PS_NOTICE("argv[" << argc << "]: " << str);
    argvs[argc++] = reinterpret_cast<char*>(loaderStack);
  }

  // Align to 16 bytes between argv and remaining strings
  STACK_ALIGN(loaderStack, 16);

  /// \todo platform assumption here.
  STACK_PUSH_STRING(loaderStack, "x86_64", 7);
  void* platform = loaderStack;

  STACK_PUSH_STRING(loaderStack, originalName.cstr(), originalName.length() + 1);
  void* execfn = loaderStack;

  // Align to 16 bytes to prepare for the auxv entries
  STACK_ALIGN(loaderStack, 16);

  STACK_PUSH_COPY(loaderStack, execRandom, sizeof(execRandom));
  void* random = loaderStack;

  // Ensure argc aligns to 16 bytes.
  if (((argc + envc) % 2) == 0) {
    STACK_PUSH_ZEROES(loaderStack, 8);
  }

  // Build the aux vector now.
  STACK_PUSH2(loaderStack, 0, 0);                                       // AT_NULL
  STACK_PUSH2(loaderStack, reinterpret_cast<uintptr_t>(platform), 15);  // AT_PLATFORM
  if (hasExecRandom) {
    STACK_PUSH2(loaderStack, reinterpret_cast<uintptr_t>(random), 25);  // AT_RANDOM
  } else {
    STACK_PUSH2(loaderStack, 0, 1);  // AT_IGNORE
  }
  STACK_PUSH2(loaderStack, 0, 23);
  STACK_PUSH2(loaderStack, execCredentials.egid, 14);                 // AT_EGID
  STACK_PUSH2(loaderStack, execCredentials.rgid, 13);                 // AT_GID
  STACK_PUSH2(loaderStack, execCredentials.euid, 12);                 // AT_EUID
  STACK_PUSH2(loaderStack, execCredentials.ruid, 11);                 // AT_UID
  STACK_PUSH2(loaderStack, reinterpret_cast<uintptr_t>(execfn), 31);  // AT_EXECFN

  // The hosted vDSO artifact is not a loadable DSO, so advertising it makes
  // musl attempt to decode a nonexistent dynamic table.
#if !HOSTED
  // Push the vDSO shared object.
  if (pVdso) {
    STACK_PUSH2(loaderStack, 0, 32);            // AT_SYSINFO - not present
    STACK_PUSH2(loaderStack, vdsoAddress, 33);  // AT_SYSINFO_EHDR
  }
#endif

  // ELF parts in the aux vector.
  STACK_PUSH2(loaderStack, originalEntryPoint, 9);                         // AT_ENTRY
  STACK_PUSH2(loaderStack, interpreterLoadBias, 7);                        // AT_BASE
  STACK_PUSH2(loaderStack, PhysicalMemoryManager::getPageSize(), 6);       // AT_PAGESZ
  STACK_PUSH2(loaderStack, originalImage.metadata.programHeaderCount, 5);  // AT_PHNUM
  STACK_PUSH2(loaderStack, sizeof(Elf::ElfProgramHeader_t), 4);            // AT_PHENT
  STACK_PUSH2(loaderStack, originalLoadBias + originalImage.programHeaderAddress,
              3);  // AT_PHDR

  // env
  STACK_PUSH(loaderStack, 0);  // env[N]
  for (size_t i = 0; i < envc; ++i) {
    STACK_PUSH(loaderStack, reinterpret_cast<uintptr_t>(envs[i]));
  }
  delete[] envs;

  // argv
  STACK_PUSH(loaderStack, 0);  // argv[N]
  for (ssize_t i = argc - 1; i >= 0; --i) {
    STACK_PUSH(loaderStack, reinterpret_cast<uintptr_t>(argvs[i]));
  }
  delete[] argvs;

  // argc
  STACK_PUSH(loaderStack, argc);

  // pedigree_init_pthreads();

  Processor::setInterrupts(true);

  if (!state) {
    if (!publishUserImage(*pProcess->getAddressSpace())) {
      delete stack;
      return failAfterCommit(Error::ValueTooLarge);
    }
    // Publish the user Thread only after its initial stack has an owner.
    const ThreadPlacement placement = ThreadPlacement::initialUser();
    Thread* pNewThread =
        new Thread(pProcess, reinterpret_cast<Thread::ThreadStartFunc>(interpreterEntryPoint), 0,
                   loaderStack, false, false, true, &placement);
    if (!pNewThread) {
      invalidateUserImage();
      delete stack;
      return failAfterCommit(Error::OutOfMemory);
    }
    m_Namespaces->publishThread(initialUts, *pNewThread, true);
    pNewThread->adoptInitialUserStackForExec(stack);
    pNewThread->setName("ld.so thread");
    if (!pNewThread->startDetached()) {
      FATAL("PosixSubsystem::invoke: initial user Thread could not be started.");
    }

    return true;
  } else {
    // This is a replace and requires a jump to userspace.
    SchedulerState s;
    ByteSet(&s, 0, sizeof(s));
    pThread->state() = s;

    if (!SyscallManager::instance().requestUserJump(interpreterEntryPoint,
                                                    reinterpret_cast<uintptr_t>(loaderStack))) {
      ERROR("PosixSubsystem::invoke: exec userspace jump was not dispatched");
      return failAfterCommit(Error::IoError);
    }
    if (!publishUserImage(*pProcess->getAddressSpace()))
      return failAfterCommit(Error::ValueTooLarge);
    return true;
  }

  // unreachable
}
