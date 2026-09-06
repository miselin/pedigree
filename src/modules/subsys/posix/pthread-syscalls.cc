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

#include "pedigree/kernel/errors.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/time/Time.h"

#include <limits.h>

#include "PosixSubsystem.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include <pthread-syscalls.h>

/// \todo add paths to include from path/to/musl-<vers>/src/internal/futex.h
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_FD 2
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP 5
#define FUTEX_LOCK_PI 6
#define FUTEX_UNLOCK_PI 7
#define FUTEX_TRYLOCK_PI 8
#define FUTEX_WAIT_BITSET 9
#define FUTEX_PRIVATE 128
#define FUTEX_CLOCK_REALTIME 256

extern "C" {
extern void pthread_stub();
extern char pthread_stub_end;
}

struct FutexKey {
  FutexKey() : owner(0), address(0) {}

  FutexKey(Process* process, int* userAddress, bool privateFutex = true)
      : owner(reinterpret_cast<uintptr_t>(process->getAddressSpace())),
        address(reinterpret_cast<uintptr_t>(userAddress)) {
    uintptr_t identity = 0;
    size_t offset = 0;
    if (!privateFutex &&
        MemoryMapManager::instance().sharedBacking(process, address, identity, offset)) {
      // Address-space objects are aligned; odd owners identify persistent
      // file tokens, whose lifetime is independent of the mapping and File.
      owner = (identity << 1) | 1;
      address = offset;
    }
  }

  uintptr_t owner;
  uintptr_t address;
};

static WaitQueue g_FutexWaiters;

namespace {
struct FutexDiscard {
  void* alarm;
};

void discardFutexWait(void* context) {
  FutexDiscard* discard = reinterpret_cast<FutexDiscard*>(context);
  void* alarm = discard->alarm;
  discard->alarm = nullptr;
  if (alarm) {
    Time::removeAlarm(alarm);
  }
}
}  // namespace

static WaitQueue::Channel futexChannel(const FutexKey& key) {
  return WaitQueue::Channel(reinterpret_cast<const void*>(key.owner), key.address);
}

int posix_futex_wake(Process* process, int* uaddr, int count, bool privateFutex) {
  if (!process || !uaddr || count <= 0) {
    return 0;
  }

  const FutexKey key(process, uaddr, privateFutex);
  auto guard = g_FutexWaiters.acquire();
  int woken = 0;
  for (int i = 0; i < count; ++i) {
    if (!guard.wakeOne(WaitQueue::WakeReason::Signalled, futexChannel(key))) {
      break;
    }
    ++woken;
  }
  return woken;
}

namespace {
bool prepareExitUserAccess(Process* process, uintptr_t address, size_t width, bool write) {
  Thread* current = Processor::information().getCurrentThread();
  if (!current || current->getParent() != process ||
      &Processor::information().getVirtualAddressSpace() != process->getAddressSpace() ||
      !Processor::getInterrupts() || Processor::inDeviceHardIrq() || (address % width)) {
    return false;
  }
  return PosixSubsystem::checkAddress(
             address, width, write ? PosixSubsystem::SafeWrite : PosixSubsystem::SafeRead) &&
         MemoryMapManager::instance().faultIn(address, write);
}

bool readRobustPointer(Process* process, uintptr_t address, uintptr_t& value) {
  VirtualAddressSpace* space = process->getAddressSpace();
  return space->tryReadUserPointer(address, value) ||
         (prepareExitUserAccess(process, address, sizeof(value), false) &&
          space->tryReadUserPointer(address, value));
}

void recoverRobustFutex(Process* process, uintptr_t entry, uintptr_t offset, size_t ownerId) {
  if (!entry || (entry & 1)) {
    return;  // PI-tagged robust entries require the separate PI protocol.
  }
  uintptr_t address = 0;
  if (static_cast<intptr_t>(offset) >= 0) {
    if (entry > ~uintptr_t(0) - offset) {
      return;
    }
    address = entry + offset;
  } else {
    const uintptr_t magnitude = uintptr_t(0) - offset;
    if (entry < magnitude) {
      return;
    }
    address = entry - magnitude;
  }

  VirtualAddressSpace* space = process->getAddressSpace();
  uint32_t observed = 0;
  if (!space->tryReadUser32(address, observed) &&
      !(prepareExitUserAccess(process, address, sizeof(observed), false) &&
        space->tryReadUser32(address, observed))) {
    return;
  }
  constexpr uint32_t OwnerMask = 0x3FFFFFFF;
  constexpr uint32_t OwnerDied = 0x40000000;
  constexpr uint32_t Waiters = 0x80000000;
  for (size_t attempt = 0; attempt < 32 && (observed & OwnerMask) == ownerId; ++attempt) {
    const bool hadWaiters = (observed & Waiters) != 0;
    const uint32_t replacement = (observed & Waiters) | OwnerDied;
    bool exchanged = false;
    if (!space->tryCompareExchangeUser32(address, observed, replacement, exchanged) &&
        !(prepareExitUserAccess(process, address, sizeof(observed), true) &&
          space->tryCompareExchangeUser32(address, observed, replacement, exchanged))) {
      return;
    }
    if (exchanged) {
      if (hadWaiters) {
        posix_futex_wake(process, reinterpret_cast<int*>(address), 1, false);
      }
      return;
    }
  }
}
}  // namespace

bool posix_clear_child_tid(Process* process, uintptr_t address) {
  if (!process || !process->getAddressSpace()) {
    return false;
  }
  MemoryMapManager::OperationGuard mappingGuard(MemoryMapManager::instance());
  VirtualAddressSpace* space = process->getAddressSpace();
  return space->tryWriteUser32(address, 0) ||
         (prepareExitUserAccess(process, address, sizeof(uint32_t), true) &&
          space->tryWriteUser32(address, 0));
}

void posix_robust_list_exit(Thread* thread) {
  size_t ownerId = 0;
  const uintptr_t head = thread->takeRobustList(ownerId);
  Process* process = thread->getParent();
  if (!head || !ownerId || !process || ownerId > 0x3FFFFFFF ||
      head > ~uintptr_t(0) - 2 * sizeof(uintptr_t)) {
    return;
  }

  // Ordinary shutdown runs before leaving the owner's address space, allowing
  // fallible demand/COW resolution. Foreign destructor cleanup uses only the
  // no-fault aliases; it must not replace another process's active mappings.
  MemoryMapManager::OperationGuard mappingGuard(MemoryMapManager::instance());
  VirtualAddressSpace* space = process->getAddressSpace();
  uintptr_t entry = 0;
  uintptr_t offset = 0;
  uintptr_t pending = 0;
  if (!space || !readRobustPointer(process, head, entry) ||
      !readRobustPointer(process, head + sizeof(uintptr_t), offset) ||
      !readRobustPointer(process, head + 2 * sizeof(uintptr_t), pending)) {
    return;
  }

  for (size_t traversed = 0; traversed < 2048 && (entry & ~uintptr_t(1)) != head; ++traversed) {
    uintptr_t next = 0;
    if (!readRobustPointer(process, entry & ~uintptr_t(1), next)) {
      break;
    }
    if ((entry & ~uintptr_t(1)) != (pending & ~uintptr_t(1))) {
      recoverRobustFutex(process, entry, offset, ownerId);
    }
    entry = next;
  }
  recoverRobustFutex(process, pending, offset, ownerId);
}

int posix_futex(int* uaddr, int futex_op, int val, uintptr_t argument4, int* uaddr2, int val3) {
  Thread* pThread = Processor::information().getCurrentThread();
  Process* pProcess = pThread->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  PT_NOTICE("futex(" << Hex << uaddr << ", " << futex_op << ", " << val << ", " << argument4 << ", "
                     << uaddr2 << ", " << val3 << ")");

  if (futex_op & FUTEX_CLOCK_REALTIME) {
    PT_NOTICE(" -> realtime futex waits are not yet supported");
    SYSCALL_ERROR(Unimplemented);
    return -1;
  }

  const bool privateFutex = (futex_op & FUTEX_PRIVATE) != 0;
  futex_op &= ~FUTEX_PRIVATE;

  if (reinterpret_cast<uintptr_t>(uaddr) % alignof(int)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  int r = 0;

  switch (futex_op) {
    case FUTEX_WAIT: {
      PT_NOTICE(" -> FUTEX_WAIT");

      const struct timespec* userTimeout = reinterpret_cast<const struct timespec*>(argument4);
      struct timespec timeout = {};
      if (userTimeout && !PosixSubsystem::copyFromUser(&timeout, userTimeout, sizeof(timeout))) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }

      Time::Timestamp timeoutNanoseconds = Time::Infinity;
      if (userTimeout) {
        if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 ||
            timeout.tv_nsec >= static_cast<decltype(timeout.tv_nsec)>(Time::Multiplier::Second)) {
          SYSCALL_ERROR(InvalidArgument);
          return -1;
        }

        const Time::Timestamp nanoseconds = static_cast<Time::Timestamp>(timeout.tv_nsec);
        const Time::Timestamp seconds = static_cast<Time::Timestamp>(timeout.tv_sec);
        if (seconds > (Time::Infinity - nanoseconds) / Time::Multiplier::Second) {
          SYSCALL_ERROR(InvalidArgument);
          return -1;
        }

        timeoutNanoseconds = seconds * Time::Multiplier::Second + nanoseconds;
      }

      pThread->retainTemporarySignalWaitInterruptionOrClear();
      FutexKey key;
      uint32_t observed = 0;
      bool accessible = false;
      auto guard = [&]() {
        MemoryMapManager& mappings = MemoryMapManager::instance();
        MemoryMapManager::OperationGuard mappingGuard(mappings);
        accessible = PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(uaddr),
                                                  sizeof(*uaddr), PosixSubsystem::SafeRead) &&
                     mappings.faultIn(reinterpret_cast<uintptr_t>(uaddr), false);
        if (accessible) {
          key = FutexKey(pProcess, uaddr, privateFutex);
        }
        // Materialization may sleep. Only the final atomic comparison runs
        // under the queue lock, which also serializes wake and enrolment.
        auto queueGuard = g_FutexWaiters.acquire();
        if (accessible) {
          accessible = pProcess->getAddressSpace()->tryReadUser32(
              reinterpret_cast<uintptr_t>(uaddr), observed);
        }
        return queueGuard;
      }();

      if (!accessible) {
        SYSCALL_ERROR(BadAddress);
        r = -1;
      } else if (observed != static_cast<uint32_t>(val)) {
        SYSCALL_ERROR(NoMoreProcesses);  // EAGAIN
        r = -1;
      } else if (userTimeout && !timeoutNanoseconds) {
        SYSCALL_ERROR(TimedOut);
        r = -1;
      } else {
        void* pAlarm = nullptr;
        if (userTimeout) {
          pAlarm = Time::addAlarm(timeoutNanoseconds);
        }
        FutexDiscard discard = {pAlarm};
        Thread::StackDiscardScope discardScope(pAlarm ? &discardFutexWait : nullptr, &discard);

        PT_NOTICE(" -> waiting...");
        WaitQueue::WakeReason wakeReason =
            guard.wait(futexChannel(key), Thread::FutexWait,
                       reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
        PT_NOTICE(" -> waiting complete!");

        const Thread::InterruptionReason interruption = pThread->getInterruptionReason();
        discardFutexWait(&discard);
        pThread->retainTemporarySignalWaitInterruptionOrClear();

        if (wakeReason != WaitQueue::WakeReason::Signalled) {
          if (userTimeout && interruption == Thread::InterruptedByTimeout) {
            SYSCALL_ERROR(TimedOut);
          } else {
            SYSCALL_ERROR(Interrupted);
          }
          r = -1;
        }
      }
      break;
    }

    case FUTEX_WAKE: {
      PT_NOTICE(" -> FUTEX_WAKE");

      if (val < 0) {
        SYSCALL_ERROR(InvalidArgument);
        r = -1;
        break;
      }

      MemoryMapManager::OperationGuard mappingGuard(MemoryMapManager::instance());
      if (!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(uaddr), sizeof(*uaddr),
                                        PosixSubsystem::SafeRead)) {
        SYSCALL_ERROR(BadAddress);
        r = -1;
        break;
      }
      r = posix_futex_wake(pProcess, uaddr, val, privateFutex);

      break;
    }

    case FUTEX_REQUEUE: {
      PT_NOTICE(" -> FUTEX_REQUEUE");

      const uint32_t rawRequeueCount = static_cast<uint32_t>(argument4);
      if (val < 0 || rawRequeueCount > static_cast<uint32_t>(INT_MAX)) {
        SYSCALL_ERROR(InvalidArgument);
        r = -1;
        break;
      }
      const int requeueCount = static_cast<int>(rawRequeueCount);
      if (reinterpret_cast<uintptr_t>(uaddr2) % alignof(int)) {
        SYSCALL_ERROR(InvalidArgument);
        r = -1;
        break;
      }
      MemoryMapManager::OperationGuard mappingGuard(MemoryMapManager::instance());
      if (!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(uaddr), sizeof(*uaddr),
                                        PosixSubsystem::SafeRead) ||
          !PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(uaddr2), sizeof(*uaddr2),
                                        PosixSubsystem::SafeRead)) {
        SYSCALL_ERROR(BadAddress);
        r = -1;
        break;
      }

      const FutexKey key(pProcess, uaddr, privateFutex);
      const FutexKey destinationKey(pProcess, uaddr2, privateFutex);
      auto guard = g_FutexWaiters.acquire();
      r = static_cast<int>(guard.wakeAndRequeue(futexChannel(key), static_cast<size_t>(val),
                                                futexChannel(destinationKey),
                                                static_cast<size_t>(requeueCount)));
      PT_NOTICE(" -> affected " << Dec << r << " threads.");
      break;
    }

    default:
      PT_NOTICE(" -> unsupported futex operation");
      SYSCALL_ERROR(Unimplemented);
      r = -1;
  }

  PT_NOTICE(" -> " << Dec << r);
  return r;
}

/**
 * Forcefully registers the given thread with the given PosixSubsystem.
 */
void pedigree_copy_posix_thread(Thread* origThread, PosixSubsystem* origSubsystem,
                                Thread* newThread, PosixSubsystem* newSubsystem) {
  PosixSubsystem::PosixThread* pOldPosixThread = origSubsystem->getThread(origThread->getId());
  if (!pOldPosixThread) {
    // Nothing to see here.
    return;
  }

  PosixSubsystem::PosixThread* pNewPosixThread = new PosixSubsystem::PosixThread;
  pNewPosixThread->pThread = newThread;
  pNewPosixThread->returnValue = 0;

  // Copy thread-specific data across.
  for (Tree<size_t, PosixSubsystem::PosixThreadKey*>::Iterator it =
           pOldPosixThread->m_ThreadData.begin();
       it != pOldPosixThread->m_ThreadData.end(); ++it) {
    size_t key = it.key();
    PosixSubsystem::PosixThreadKey* data = it.value();

    pNewPosixThread->addThreadData(key, data);
    pNewPosixThread->m_ThreadKeys.set(key);
  }

  pNewPosixThread->lastDataKey = pOldPosixThread->lastDataKey;
  pNewPosixThread->nextDataKey = pOldPosixThread->nextDataKey;

  newSubsystem->insertThread(newThread->getId(), pNewPosixThread);
}

/**
 * pedigree_init_pthreads
 *
 * This function copies the user mode thread wrapper from the kernel to a known
 * user mode location. The location is already mapped by pedigree_init_signals
 * which must be called before this function.
 */
void pedigree_init_pthreads() {
  PT_NOTICE("init_pthreads");
  // Make sure we can write to the trampoline area.
  Processor::information().getVirtualAddressSpace().setFlags(
      reinterpret_cast<void*>(Event::getTrampoline()), VirtualAddressSpace::Write |
                                                           VirtualAddressSpace::Shared |
                                                           VirtualAddressSpace::RuntimeMapping);
  MemoryCopy(
      reinterpret_cast<void*>(Event::getSecondaryTrampoline()),
      reinterpret_cast<void*>(pthread_stub),
      (reinterpret_cast<uintptr_t>(&pthread_stub_end) - reinterpret_cast<uintptr_t>(pthread_stub)));
  Processor::information().getVirtualAddressSpace().setFlags(
      reinterpret_cast<void*>(Event::getTrampoline()), VirtualAddressSpace::Execute |
                                                           VirtualAddressSpace::Shared |
                                                           VirtualAddressSpace::RuntimeMapping);

  // Make sure the main thread is actually known.
  Thread* pThread = Processor::information().getCurrentThread();
  Process* pProcess = pThread->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return;
  }

  PosixSubsystem::PosixThread* pPosixThread = new PosixSubsystem::PosixThread;
  pPosixThread->pThread = pThread;
  pPosixThread->returnValue = 0;
  pSubsystem->insertThread(pThread->getId(), pPosixThread);
}

void* posix_pedigree_create_waiter() {
  PT_NOTICE("posix_pedigree_create_waiter");

  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return 0;
  }

  Semaphore* sem = new Semaphore(0);
  void* descriptor = pSubsystem->insertThreadWaiter(sem);
  if (!descriptor) {
    delete sem;
  }

  return descriptor;
}

int posix_pedigree_thread_wait_for(void* waiter) {
  PT_NOTICE("posix_pedigree_thread_wait_for");

  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  Semaphore* sem = pSubsystem->getThreadWaiter(waiter);
  if (!sem) {
    return -1;
  }

  // Deadlock detection - don't wait if nothing can wake this waiter.
  /// \todo Check for more than just one thread - there's probably other
  ///       detections we can do here.
  if (pProcess->getNumThreads() <= 1) {
    SYSCALL_ERROR(Deadlock);
    return -1;
  }

  // This descriptor remains owned by the subsystem until its matching
  // trigger. A signal may run while blocked, but cannot abandon the waiter
  // storage or consume the eventual notification.
  if (!sem->acquireForCompletion()) {
    FATAL("POSIX thread-waiter completion barrier failed.");
  }

  return 0;
}

int posix_pedigree_thread_trigger(void* waiter) {
  PT_NOTICE("posix_pedigree_thread_trigger");

  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return 0;
  }

  Semaphore* sem = pSubsystem->getThreadWaiter(waiter);
  if (!sem)
    return 0;
  if (sem->getValue())
    return 0;  // Nothing to wake up.

  // Wake up a waiter.
  sem->release();
  return 1;
}

void posix_pedigree_destroy_waiter(void* waiter) {
  PT_NOTICE("posix_pedigree_destroy_waiter");

  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return;
  }

  Semaphore* sem = pSubsystem->getThreadWaiter(waiter);
  if (!sem) {
    return;
  }
  pSubsystem->removeThreadWaiter(waiter);
  delete sem;
}

pid_t posix_gettid(bool linuxAbi) {
  // Go caches this value before creating another thread, so it must not
  // change when the process transitions from one thread to several.
  Thread* current = Processor::information().getCurrentThread();
  return linuxAbi ? current->getTaskId() : current->getId();
}

pid_t posix_set_tid_address(int* tidptr, bool linuxAbi) {
  Thread* thread = Processor::information().getCurrentThread();
  thread->setClearChildTid(reinterpret_cast<uintptr_t>(tidptr));
  return linuxAbi ? thread->getTaskId() : thread->getId();
}
