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

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/RelayEvent.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/SignalEvent.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/Uninterruptible.h"

#include <PosixSubsystem.h>
#define MACHINE_FORWARD_DECL_ONLY
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/linker/Elf.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/SyscallManager.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/RadixTree.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Tree.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/lib.h"

#include <signal.h>
#include <vdso.h>  // Header with the vdso.so binary in it.

#include "FileDescriptor.h"
#include "PosixProcess.h"
#include "file-syscalls.h"
#include "linux-amd64-signal.h"
#include "logging.h"
#include "modules/system/linker/DynamicLinker.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/LockedFile.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/Symlink.h"
#include "modules/system/vfs/VFS.h"
#include "pthread-syscalls.h"
#include "system-syscalls.h"

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

namespace {
void posixAlarmEventHandler(Thread* thread) {
  if (!thread || thread->getParent()->getType() != Process::Posix) {
    return;
  }

  PosixSubsystem* subsystem = static_cast<PosixSubsystem*>(thread->getParent()->getSubsystem());
  if (!subsystem) {
    return;
  }

  SignalEvent* delivery = subsystem->createSignalDelivery(SIGALRM);
  if (delivery && !thread->sendEvent(delivery)) {
    delete delivery;
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
      m_AlarmLock(false),
      m_pAlarmEvent(nullptr),
      m_pAlarmThread(nullptr),
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

  // Copy all signal handlers
  for (sigHandlerTree::Iterator it = s.m_SignalHandlers.begin(); it != s.m_SignalHandlers.end();
       it++) {
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
  for (Tree<void*, Semaphore*>::Iterator it = s.m_ThreadWaiters.begin();
       it != s.m_ThreadWaiters.end(); ++it) {
    void* key = it.key();

    Semaphore* sem = new Semaphore(0);
    m_ThreadWaiters.insert(key, sem);
  }

  m_NextThreadWaiter = s.m_NextThreadWaiter;
}

PosixSubsystem::~PosixSubsystem() {
  assert(--m_FreeCount == 0);

  cancelAlarm();
  if (m_pAlarmEvent) {
    m_pAlarmEvent->retire();
    m_pAlarmEvent = nullptr;
  }

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
  if ((addr < va.getUserStart()) || (addr >= va.getKernelStart()) || (end >= va.getKernelStart())) {
#if VERBOSE_KERNEL
    PS_NOTICE("  -> outside of user address area.");
#endif
    return false;
  }

  MemoryMappedObject::Permissions mmapPermissions = MemoryMappedObject::None;
  if (flags & SafeRead) {
    mmapPermissions |= MemoryMappedObject::Read;
  }
  if (flags & SafeWrite) {
    mmapPermissions |= MemoryMappedObject::Write;
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

    if (flags & SafeWrite) {
      size_t vFlags = 0;
      physical_uintptr_t phys = 0;
      va.getMapping(pAddr, phys, vFlags);

      if (!(vFlags & (VirtualAddressSpace::Write | VirtualAddressSpace::CopyOnWrite))) {
#if VERBOSE_KERNEL
        PS_NOTICE("  -> not writeable.");
#endif
        return false;
      }
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

PosixSubsystem::UserStringResult PosixSubsystem::copyUserString(const char* userString,
                                                                String& copy, size_t maxLength) {
  copy.clear();

  if (!userString) {
    return UserStringBadAddress;
  }

  if (!maxLength) {
    return UserStringTooLong;
  }

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

    if (!checkAddress(current, length, SafeRead)) {
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

void PosixSubsystem::exit(int code) {
  if (!Processor::getInterrupts() || Processor::inDeviceHardIrq()) {
    FATAL_NOLOCK(
        "PosixSubsystem::exit requires an IRQ-enabled thread "
        "boundary.");
  }

  Thread* pThread = Processor::information().getCurrentThread();

  Process* pProcess = pThread->getParent();
  NOTICE("PosixSubsystem::exit(" << Dec << pProcess->getId() << ", code=" << code << ")");

  if (!pProcess->beginTermination()) {
    // Another thread owns or has reserved process-wide cleanup. A competitor
    // must take only the thread exit path; the owner will retire every peer.
    Processor::information().getScheduler().commitCurrentThreadExit();
  }

  if (pProcess->getExitStatus() == 0 ||     // Normal exit.
      pProcess->getExitStatus() == 0x7F ||  // Suspended.
      pProcess->getExitStatus() == 0xFF)    // Continued.
    pProcess->setExitStatus((code & 0xFF) << 8);
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

  delete pProcess->getLinker();

  MemoryMapManager::instance().unmapAll();

  // If it's a POSIX process, remove group membership
  if (pProcess->getType() == Process::Posix) {
    PosixProcess* p = static_cast<PosixProcess*>(pProcess);
    p->leaveProcessGroup();
  }

  // Pin both objects before dropping the parent/child guard. Notification can
  // allocate an Event, so it must not run while that spinlock is held.
  while (true) {
    Process* expectedParent = pProcess->getParent();
    if (!expectedParent) {
      break;
    }

    Scheduler::ProcessLease parentLease;
    if (!Scheduler::instance().acquireProcess(parentLease, expectedParent)) {
      if (pProcess->getParent() != expectedParent) {
        continue;
      }
      break;
    }

    Process::ThreadLease parentThread;
    const bool parentThreadAcquired =
        parentLease->acquireThread(parentThread, static_cast<size_t>(0));
    bool relationshipValid = false;
    bool parentAcceptsSignal = false;
    {
      auto childGuard = parentLease->acquireChildStateWait();
      relationshipValid = pProcess->getParent() == parentLease.get();
      const Process::ProcessState parentState = parentLease->getState();
      parentAcceptsSignal = parentState == Process::Active || parentState == Process::Suspended;
    }

    if (!relationshipValid) {
      continue;
    }
    if (parentThreadAcquired && parentAcceptsSignal && parentLease->getSubsystem()) {
      parentLease->getSubsystem()->threadException(parentThread.get(), Child);
    }
    break;
  }

  // Clean up the descriptor table
  freeMultipleFds();

  // Tell some interesting info
  NOTICE("at exit for pid " << Dec << pProcess->getId() << "...");

  pProcess->finishTermination();

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

  SignalEvent* event = pSubsystem->createSignalDelivery(signal);
  if (event) {
    PS_NOTICE("PosixSubsystem - killing " << pThread->getParent()->getId());

    // Send the kill event
    /// \todo we probably want to avoid allocating a new stack..
    if (!pThread->sendEvent(event)) {
      delete event;
    }

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
    if (getSignalDisposition(signal, disposition) && disposition.type == 0) {
      LinuxAmd64Signal::DeliveryResult result = LinuxAmd64Signal::deliverSynchronous(
          pThread, signal, disposition, eType, *pState, faultAddress, errorCode);
      if (result == LinuxAmd64Signal::Delivered) {
        return;
      }
      if (result == LinuxAmd64Signal::Failed) {
        // The raw exception frame still owns interrupt accounting and
        // handler cleanup. Preserve the fatal status and let the
        // return-to-user tail enter process teardown after it unwinds.
        pThread->deferProcessExit(128 + SIGSEGV);
        return;
      }
    }
  }
#endif

  // A raw exception frame cannot dispatch a handler or terminal callback.
  // Its return-to-user tail consumes the queued signal after accounting and
  // handler cleanup have completed.
  sendSignal(pThread, signal, pState == nullptr);
}

void PosixSubsystem::sendSignal(Thread* pThread, int signal, bool yield) {
  PS_NOTICE("PosixSubsystem::sendSignal #" << signal << " -> pid:tid " << Dec
                                           << pThread->getParent()->getId() << ":"
                                           << pThread->getId());

  Process* pProcess = pThread->getParent();
  if (pProcess->getType() != Process::Posix) {
    ERROR("PosixSubsystem::threadException called with a non-POSIX process!");
    return;
  }
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());

  SignalEvent* event = pSubsystem->createSignalDelivery(signal);
  if (!event) {
    ERROR("Unknown signal in sendSignal - POSIX subsystem");
  }

  // If we're good to go, send the signal.
  if (event) {
    if (!pThread->sendEvent(event)) {
      delete event;
    } else if (yield) {
      Thread* pCurrentThread = Processor::information().getCurrentThread();
      if (pCurrentThread == pThread) {
        // Attempt to execute the new event immediately.
        Processor::information().getScheduler().checkEventState(0);
      } else {
        // Yield so the event can fire.
        Scheduler::instance().yield();
      }
    }
  } else {
    // PS_NOTICE("No event configured for signal #" << signal << ", silently
    // dropping!");
    NOTICE("No event configured for signal #" << signal << ", silently dropping!");
  }
}

void PosixSubsystem::setSignalHandler(size_t sig, SignalHandler* handler) {
  m_SignalHandlersLock.acquire();

  SignalHandler* removal = nullptr;

  sig %= 32;
  if (handler) {
    removal = m_SignalHandlers.lookup(sig);
    if (removal) {
      // Remove from the list
      m_SignalHandlers.remove(sig);
    }

    // Insert into the signal handler table
    handler->sig = sig;

    m_SignalHandlers.insert(sig, handler);
  }

  m_SignalHandlersLock.release();

  // Complete the destruction of the handler (waiting for deletion) with no
  // lock held.
  if (removal) {
    delete removal;
  }
}

bool PosixSubsystem::getSignalDisposition(size_t sig, SignalDisposition& disposition) {
  if (sig >= 32) {
    return false;
  }

  m_SignalHandlersLock.enter();

  SignalHandler* handler = m_SignalHandlers.lookup(sig);
  if (handler) {
    disposition.handler = handler->pEvent ? handler->pEvent->getHandlerAddress() : 0;
    disposition.signalMask = handler->sigMask;
    disposition.flags = handler->flags;
    disposition.restorer = handler->restorer;
    disposition.type = handler->type;
  }

  m_SignalHandlersLock.leave();
  return handler != nullptr;
}

SignalEvent* PosixSubsystem::createSignalDelivery(size_t sig, uint32_t* flags) {
  if (flags) {
    *flags = 0;
  }
  if (sig >= 32) {
    return nullptr;
  }

  m_SignalHandlersLock.enter();

  SignalHandler* handler = m_SignalHandlers.lookup(sig);
  SignalEvent* delivery = nullptr;
  if (handler && handler->pEvent) {
    delivery = static_cast<SignalEvent*>(handler->pEvent->cloneForDelivery());
    if (flags) {
      *flags = handler->flags;
    }
  }

  m_SignalHandlersLock.leave();
  return delivery;
}

size_t PosixSubsystem::setAlarm(size_t seconds) {
  LockGuard<Spinlock> guard(m_AlarmLock);
  if (!m_pAlarmEvent) {
    m_pAlarmEvent = new RelayEvent(&posixAlarmEventHandler, 0x414C4152);
  }

  Timer* timer = Machine::instance().getTimer();
  if (!timer) {
    return 0;
  }

  size_t remaining = timer->removeAlarm(m_pAlarmEvent, false);
  m_pAlarmThread = nullptr;
  if (seconds) {
    timer->addAlarm(m_pAlarmEvent, seconds);
    m_pAlarmThread = Processor::information().getCurrentThread();
  }
  return remaining;
}

void PosixSubsystem::cancelAlarm() {
  LockGuard<Spinlock> guard(m_AlarmLock);
  if (m_pAlarmEvent) {
    Timer* timer = Machine::instance().getTimer();
    if (timer) {
      timer->removeAlarm(m_pAlarmEvent);
    }
  }
  m_pAlarmThread = nullptr;
}

/**
 * Note: POSIX  requires open()/accept()/etc to be safe during a signal
 * handler, which requires us to not allow signals during these file descriptor
 * calls. They cannot re-enter as they take process-specific locks.
 */

size_t PosixSubsystem::getFd() {
  Uninterruptible throughout;

  // Enter critical section for writing.
  m_FdLock.acquire();

  // Try to recycle if possible
  for (size_t i = m_LastFd; i < m_NextFd; i++) {
    if (!(m_FdBitmap.test(i))) {
      m_LastFd = i;
      m_FdBitmap.set(i);
      m_FdLock.release();
      return i;
    }
  }

  // Otherwise, allocate
  // m_NextFd will always contain the highest allocated fd
  m_FdBitmap.set(m_NextFd);
  size_t ret = m_NextFd++;
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

  retiring.reset();
}

void PosixSubsystem::threadExiting(Thread* pThread) {
  if (!pThread) {
    return;
  }

  const uintptr_t address = pThread->takeClearChildTid();
  if (!address) {
    return;
  }

  Process* process = pThread->getParent();
  if (!process) {
    return;
  }

  VirtualAddressSpace* addressSpace = process->getAddressSpace();
  const bool cleared = addressSpace && addressSpace->tryWriteUser32(address, 0);
  if (!cleared) {
    PS_NOTICE(
        "clear-child-TID skipped a non-resident, read-only, kernel, or "
        "copy-on-write target at "
        << Hex << address << " for tid " << Dec << pThread->getId());
  }

  // The registration is already consumed. Wake even if the restricted
  // no-fault store could not reach the word, so no waiter is stranded in the
  // kernel after an invalid registration or concurrent unmap.
  posix_futex_wake(process, reinterpret_cast<int*>(address), 1);
}

void PosixSubsystem::threadRemoved(Thread* pThread) {
  Event* alarmEvent = nullptr;
  {
    LockGuard<Spinlock> guard(m_AlarmLock);
    alarmEvent = m_pAlarmEvent;
    if (m_pAlarmThread == pThread) {
      Timer* timer = Machine::instance().getTimer();
      if (timer && alarmEvent) {
        timer->removeAlarm(alarmEvent);
      }
      m_pAlarmThread = nullptr;
    }
  }
  if (alarmEvent) {
    pThread->cullEvent(alarmEvent);
  }

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

bool PosixSubsystem::loadElf(File* pFile, uintptr_t mappedAddress, uintptr_t& newAddress,
                             uintptr_t& finalAddress, bool& relocated) {
  PS_NOTICE("PosixSubsystem::loadElf(" << pFile->getName() << ")");

  Process* pProcess = Processor::information().getCurrentThread()->getParent();

  // Grab the file header to check magic and find program headers.
  Elf::ElfHeader_t* pHeader = reinterpret_cast<Elf::ElfHeader_t*>(mappedAddress);
  if ((pHeader->ident[1] != 'E') || (pHeader->ident[2] != 'L') || (pHeader->ident[3] != 'F') ||
      (pHeader->ident[0] != 127)) {
    return false;
  }

  size_t phnum = pHeader->phnum;
  Elf::ElfProgramHeader_t* phdrs =
      reinterpret_cast<Elf::ElfProgramHeader_t*>(mappedAddress + pHeader->phoff);

  // Find full memory size that we need to map in.
  uintptr_t startAddress = ~0U;
  uintptr_t unalignedStartAddress = 0;
  uintptr_t endAddress = 0;
  for (size_t i = 0; i < phnum; ++i) {
    if (phdrs[i].type != PT_LOAD) {
      continue;
    }

    if (phdrs[i].vaddr < startAddress) {
      startAddress = phdrs[i].vaddr;
    }

    uintptr_t maybeEndAddress = phdrs[i].vaddr + phdrs[i].memsz;
    if (maybeEndAddress > endAddress) {
      endAddress = maybeEndAddress;
    }
  }

  // Align to page boundaries.
  size_t pageSz = PhysicalMemoryManager::getPageSize();
  unalignedStartAddress = startAddress;
  startAddress &= ~(pageSz - 1);
  if (endAddress & (pageSz - 1)) {
    endAddress = (endAddress + pageSz) & ~(pageSz - 1);
  }

  // OK, we can allocate space for the file now.
  bool bRelocated = false;
  if (pHeader->type == ET_REL || pHeader->type == ET_DYN) {
    if (!pProcess->getDynamicSpaceAllocator().allocate(endAddress - startAddress, newAddress))
      if (!pProcess->getSpaceAllocator().allocate(endAddress - startAddress, newAddress))
        return false;

    bRelocated = true;
    unalignedStartAddress = newAddress + (startAddress & (pageSz - 1));
    startAddress = newAddress;

    newAddress = unalignedStartAddress;

    relocated = true;
  } else {
    if (!pProcess->getDynamicSpaceAllocator().allocateSpecific(startAddress,
                                                               endAddress - startAddress))
      if (!pProcess->getSpaceAllocator().allocateSpecific(startAddress, endAddress - startAddress))
        return false;

    newAddress = unalignedStartAddress;
  }

  finalAddress = startAddress + (endAddress - startAddress);

  // Can now do another pass, mapping in as needed.
  for (size_t i = 0; i < phnum; ++i) {
    if (phdrs[i].type != PT_LOAD) {
      continue;
    }

    uintptr_t base = phdrs[i].vaddr;
    if (bRelocated) {
      base += startAddress;
    }
    uintptr_t unalignedBase = base;
    if (base & (pageSz - 1)) {
      base &= ~(pageSz - 1);
    }

    uintptr_t offset = phdrs[i].offset;
    if (offset & (pageSz - 1)) {
      offset &= ~(pageSz - 1);
    }

    // if we don't add the unaligned part to the length, we can map only
    // enough to cover the aligned page even though the alignment may lead
    // to the region covering two pages...
    size_t length = phdrs[i].memsz + (unalignedBase & (pageSz - 1));
    if (length & (pageSz - 1)) {
      length = (length + pageSz) & ~(pageSz - 1);
    }

    // Map.
    MemoryMappedObject::Permissions perms = MemoryMappedObject::Read;
    if (phdrs[i].flags & PF_X) {
      perms |= MemoryMappedObject::Exec;
    }
    if (phdrs[i].flags & PF_R) {
      perms |= MemoryMappedObject::Read;
    }
    if (phdrs[i].flags & PF_W) {
      perms |= MemoryMappedObject::Write;
    }

    PS_NOTICE(pFile->getName() << " PHDR[" << i << "]: @" << Hex << base << " -> "
                               << base + length);
    MemoryMappedObject* pObject =
        MemoryMapManager::instance().mapFile(pFile, base, length, perms, offset);
    if (!pObject) {
      ERROR("PosixSubsystem::loadElf: failed to map PT_LOAD section");
      return false;
    }

    if (phdrs[i].memsz > phdrs[i].filesz) {
      uintptr_t end = unalignedBase + phdrs[i].memsz;
      uintptr_t zeroStart = unalignedBase + phdrs[i].filesz;
      if (zeroStart & (pageSz - 1)) {
        size_t numBytes = pageSz - (zeroStart & (pageSz - 1));
        if ((zeroStart + numBytes) > end) {
          numBytes = end - zeroStart;
        }
        ByteSet(reinterpret_cast<void*>(zeroStart), 0, numBytes);
        zeroStart += numBytes;
      }

      if (zeroStart < end) {
        MemoryMappedObject* pAnonymousRegion =
            MemoryMapManager::instance().mapAnon(zeroStart, end - zeroStart, perms);
        if (!pAnonymousRegion) {
          ERROR(
              "PosixSubsystem::loadElf: failed to map anonymous "
              "pages for filesz/memsz mismatch");
          return false;
        }
      }
    }
  }

  return true;
}

File* PosixSubsystem::findFile(const String& path, File* workingDir) {
  if (workingDir == nullptr) {
    assert(m_pProcess);
    workingDir = m_pProcess->getCwd();
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

  // fall back to root filesystem
  if (!m_pRootFs) {
    m_pRootFs = VFS::instance().getRootFilesystem();
  }

  if (m_pRootFs) {
    return VFS::instance().find(path, m_pRootFs->getRoot());
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

bool PosixSubsystem::parseShebang(File* pFile, File*& pOutFile, Vector<String>& argv) {
  PS_NOTICE("Attempting to parse shebang in " << pFile->getFullPath());

  // Try and read the shebang, if any.
  /// \todo this loop could terminate MUCH faster
  String fileContents;
  bool bSearchDone = false;
  size_t offset = 0;
  while (!bSearchDone) {
    char buff[129];
    size_t nRead = pFile->read(offset, 128, reinterpret_cast<uintptr_t>(buff));
    buff[nRead] = 0;
    offset += nRead;

    if (nRead) {
      // Truncate at the newline if one is found (and then stop
      // iterating).
      char* newline = const_cast<char*>(StringFind(buff, '\n'));
      if (newline) {
        bSearchDone = true;
        *newline = 0;
      }
      fileContents += String(buff);
    }

    if (nRead < 128) {
      bSearchDone = true;
      break;
    }
  }

  // Is this even a shebang line?
  if (!fileContents.startswith("#!")) {
    PS_NOTICE("no shebang found");
    return true;
  }

  // Strip the shebang.
  fileContents.lchomp();
  fileContents.lchomp();

  // OK, we have a shebang line. We need to tokenize.
  Vector<String> additionalArgv = fileContents.tokenise(' ');
  if (!additionalArgv.count()) {
    // Not a true shebang line.
    PS_NOTICE("split didn't find anything");
    return true;
  }

  // Normalise path to ensure we have the correct path to invoke.
  String invokePath;
  String newTarget = *additionalArgv.begin();
  if (normalisePath(invokePath, static_cast<const char*>(newTarget))) {
    // rewrote, update argv[0] accordingly.
    newTarget = invokePath;
  }

  // Can we load the new program?
  File* pNewTarget = findFileWithAbiFallbacks(newTarget);
  if (!pNewTarget) {
    // No, we cannot.
    PS_NOTICE("target not found");
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }

  // OK, we can now insert to argv - we do so backwards so it's just a simple
  // pushFront.
  while (additionalArgv.count()) {
    argv.pushFront(additionalArgv.popBack());
  }

  pOutFile = pNewTarget;

  return true;
}

static File* traverseForInvoke(File* pFile) {
  // Do symlink traversal.
  while (pFile && pFile->isSymlink()) {
    pFile = Symlink::fromFile(pFile)->followLink();
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
  File* originalFile = findFileWithAbiFallbacks(originalName);
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

  // Ensure we only have one thread running (us).
  if (pProcess->getNumThreads() > 1) {
    /// \todo actually we are supposed to kill them all here
    PS_NOTICE("invoke attempted with multiple threads in this process");
    return false;
  }

  originalFile = traverseForInvoke(originalFile);
  if (!originalFile) {
    // traverseForInvoke does a SYSCALL_ERROR for us
    return false;
  }

  uint8_t validateBuffer[128];
  size_t nBytes = originalFile->read(0, 128, reinterpret_cast<uintptr_t>(validateBuffer));

  Elf* validElf = new Elf();
  if (!validElf->validate(validateBuffer, nBytes)) {
    PS_NOTICE("PosixSubsystem::invoke: '" << originalFile->getName()
                                          << "' is not an ELF binary, looking for shebang...");

    File* shebangFile = 0;
    if (!parseShebang(originalFile, shebangFile, argv)) {
      PS_NOTICE("PosixSubsystem::invoke: failed to parse shebang line in '"
                << originalFile->getName() << "'");
      return false;
    }

    // Switch to the real target if we must; parseShebang adjusts argv for
    // us.
    if (shebangFile) {
      originalFile = shebangFile;

      // Handle symlinks in shebang target.
      originalFile = traverseForInvoke(originalFile);
      if (!originalFile) {
        return false;
      }
    }
  }

  // Can we read & execute the given target?
  if (!VFS::checkAccess(originalFile, true, false, true)) {
    // checkAccess does a SYSCALL_ERROR for us.
    return -1;
  }

  File* interpreterFile = 0;

  // Inhibit all signals from coming in while we trash the address space...
  for (int sig = 0; sig < 32; sig++)
    Processor::information().getCurrentThread()->inhibitEvent(sig, true);

  // Determine if the target uses an interpreter or not.
  String interpreter("");
  DynamicLinker* pLinker = new DynamicLinker();
  pProcess->setLinker(pLinker);
  if (pLinker->checkInterpreter(originalFile, interpreter)) {
    // Existing binaries and PUP packages may still name the interpreter
    // using Pedigree's pre-FHS layout.
    String normalisedInterpreter;
    if (normalisePath(normalisedInterpreter, interpreter.cstr())) {
      interpreter = normalisedInterpreter;
    }

    // Ensure we can actually find the interpreter.
    interpreterFile = findFileWithAbiFallbacks(interpreter);
    interpreterFile = traverseForInvoke(interpreterFile);
    if (!interpreterFile) {
      PS_NOTICE("PosixSubsystem::invoke: could not find interpreter '" << interpreter << "'");
      SYSCALL_ERROR(ExecFormatError);
      return false;
    }
  } else {
    // Static binaries enter at their own entry point. Loading the target
    // again as its own interpreter would reserve every PT_LOAD range twice.
    interpreterFile = 0;
  }

  // No longer need the DynamicLinker instance.
  delete pLinker;
  pLinker = 0;
  pProcess->setLinker(pLinker);

  // Wipe out old address space.
  // Earlier failures preserve the registration. From this irreversible
  // point onward its target belongs to the discarded image.
  pThread->setClearChildTid(0);
  MemoryMapManager::instance().unmapAll();

  // We now need to clean up the process' address space.
  pProcess->getSpaceAllocator().clear();
  pProcess->getDynamicSpaceAllocator().clear();
  pProcess->getSpaceAllocator().free(pProcess->getAddressSpace()->getUserStart(),
                                     pProcess->getAddressSpace()->getUserReservedStart() -
                                         pProcess->getAddressSpace()->getUserStart());
  if (pProcess->getAddressSpace()->getDynamicStart()) {
    pProcess->getDynamicSpaceAllocator().free(pProcess->getAddressSpace()->getDynamicStart(),
                                              pProcess->getAddressSpace()->getDynamicEnd() -
                                                  pProcess->getAddressSpace()->getDynamicStart());
  }
  pProcess->getAddressSpace()->revertToKernelAddressSpace();

  // Map in the two ELF files so we can load them into the address space.
  uintptr_t originalBase = 0, interpreterBase = 0;
  MemoryMappedObject::Permissions perms =
      MemoryMappedObject::Read | MemoryMappedObject::Write | MemoryMappedObject::Exec;
  MemoryMappedObject* pOriginal = MemoryMapManager::instance().mapFile(
      originalFile, originalBase, originalFile->getSize(), perms);
  if (!pOriginal) {
    PS_NOTICE("PosixSubsystem::invoke: failed to map target");
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }

  MemoryMappedObject* pInterpreter = 0;
  if (interpreterFile) {
    pInterpreter = MemoryMapManager::instance().mapFile(interpreterFile, interpreterBase,
                                                        interpreterFile->getSize(), perms);
    if (!pInterpreter) {
      PS_NOTICE("PosixSubsystem::invoke: failed to map interpreter");
      MemoryMapManager::instance().unmap(pOriginal);
      SYSCALL_ERROR(OutOfMemory);
      return false;
    }
  }

  // Load the target application first.
  uintptr_t originalLoadedAddress = 0;
  uintptr_t originalFinalAddress = 0;
  bool originalRelocated = false;
  if (!loadElf(originalFile, originalBase, originalLoadedAddress, originalFinalAddress,
               originalRelocated)) {
    /// \todo cleanup
    PS_NOTICE("PosixSubsystem::invoke: failed to load target");
    SYSCALL_ERROR(ExecFormatError);
    return false;
  }

  // Now load the interpreter.
  uintptr_t interpreterLoadedAddress = 0;
  uintptr_t interpreterFinalAddress = 0;
  bool interpreterRelocated = false;
  if (interpreterFile && !loadElf(interpreterFile, interpreterBase, interpreterLoadedAddress,
                                  interpreterFinalAddress, interpreterRelocated)) {
    /// \todo cleanup
    PS_NOTICE("PosixSubsystem::invoke: failed to load interpreter");
    SYSCALL_ERROR(ExecFormatError);
    return false;
  }

  // Extract entry points.
  uintptr_t originalEntryPoint = 0, interpreterEntryPoint = 0;
  Elf::extractEntryPoint(reinterpret_cast<uint8_t*>(originalBase), originalFile->getSize(),
                         originalEntryPoint);
  if (interpreterFile) {
    Elf::extractEntryPoint(reinterpret_cast<uint8_t*>(interpreterBase), interpreterFile->getSize(),
                           interpreterEntryPoint);
  }

  if (originalRelocated) {
    originalEntryPoint += originalLoadedAddress;
  }
  if (interpreterRelocated) {
    interpreterEntryPoint += interpreterLoadedAddress;
  }
  if (!interpreterFile) {
    interpreterEntryPoint = originalEntryPoint;
  }

  // Pull out the ELF header information for the original image.
  Elf::ElfHeader_t* originalHeader = reinterpret_cast<Elf::ElfHeader_t*>(originalBase);

  // Past point of no return, so set up the process for the new image.
  pProcess->description() = originalName;
  pProcess->resetCounts();
  pThread->resetTlsBase();
  if (pSubsystem)
    pSubsystem->freeMultipleFds(true);
  if (pProcess->getType() == Process::Posix) {
    /// \todo should only do this for setuid/setgid programs
    PosixProcess* p = static_cast<PosixProcess*>(pProcess);
    p->setSavedUserId(p->getEffectiveUserId());
    p->setSavedGroupId(p->getEffectiveGroupId());
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
    ERROR("PosixSubsystem::invoke: failed to allocate initial user stack");
    SYSCALL_ERROR(OutOfMemory);
    return false;
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
  STACK_PUSH2(loaderStack, pProcess->getEffectiveGroupId(), 14);      // AT_EGID
  STACK_PUSH2(loaderStack, pProcess->getGroupId(), 13);               // AT_GID
  STACK_PUSH2(loaderStack, pProcess->getEffectiveUserId(), 12);       // AT_EUID
  STACK_PUSH2(loaderStack, pProcess->getUserId(), 11);                // AT_UID
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
  STACK_PUSH2(loaderStack, originalEntryPoint, 9);                    // AT_ENTRY
  STACK_PUSH2(loaderStack, interpreterLoadedAddress, 7);              // AT_BASE
  STACK_PUSH2(loaderStack, PhysicalMemoryManager::getPageSize(), 6);  // AT_PAGESZ
  STACK_PUSH2(loaderStack, originalHeader->phnum, 5);                 // AT_PHNUM
  STACK_PUSH2(loaderStack, originalHeader->phentsize, 4);             // AT_PHENT
  STACK_PUSH2(loaderStack, originalLoadedAddress + originalHeader->phoff,
              3);  // AT_PHDR

  // env
  STACK_PUSH(loaderStack, 0);  // env[N]
  for (size_t i = 0; i < envc; ++i) {
    STACK_PUSH(loaderStack, reinterpret_cast<uintptr_t>(envs[i]));
  }

  // argv
  STACK_PUSH(loaderStack, 0);  // argv[N]
  for (ssize_t i = argc - 1; i >= 0; --i) {
    STACK_PUSH(loaderStack, reinterpret_cast<uintptr_t>(argvs[i]));
  }

  // argc
  STACK_PUSH(loaderStack, argc);

  // We can now unmap both original objects as they've been loaded and
  // consumed.
  if (pInterpreter) {
    MemoryMapManager::instance().unmap(pInterpreter);
  }
  MemoryMapManager::instance().unmap(pOriginal);
  pInterpreter = pOriginal = 0;

  // Initialise the sigret if not already done for this process
  pedigree_init_sigret();
  // pedigree_init_pthreads();

  Processor::setInterrupts(true);
  Thread* currentThread = Processor::information().getCurrentThread();
  if (currentThread && currentThread->getParent() == pProcess) {
    currentThread->recordTime(CpuTimeMode::User);
  }

  if (!state) {
    // Just create a new thread, this is not a full replace.
    Thread* pNewThread = new Thread(
        pProcess, reinterpret_cast<Thread::ThreadStartFunc>(interpreterEntryPoint), 0, loaderStack);
    pNewThread->setName("ld.so thread");
    pNewThread->detach();

    return true;
  } else {
    // This is a replace and requires a jump to userspace.
    SchedulerState s;
    ByteSet(&s, 0, sizeof(s));
    pThread->state() = s;

    // Allow signals again now that everything's loaded
    for (int sig = 0; sig < 32; sig++) {
      Processor::information().getCurrentThread()->inhibitEvent(sig, false);
    }

    if (!SyscallManager::instance().requestUserJump(interpreterEntryPoint,
                                                    reinterpret_cast<uintptr_t>(loaderStack))) {
      FATAL("exec userspace jump was not dispatched.");
    }
    return true;
  }

  // unreachable
}
