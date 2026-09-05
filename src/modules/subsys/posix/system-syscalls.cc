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
#include "pedigree/kernel/Version.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/linker/Elf.h"
#include "pedigree/kernel/linker/KernelElf.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/StackFrame.h"
#include "pedigree/kernel/processor/SyscallManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/ZombieQueue.h"
#include "pedigree/kernel/utilities/lib.h"
#include "pedigree/kernel/utilities/utility.h"

#include "file-syscalls.h"
#include "linux-resource-abi.h"
#include "modules/system/linker/DynamicLinker.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/Symlink.h"
#include "modules/system/vfs/VFS.h"
#include "pipe-syscalls.h"
#include "posixSyscallNumbers.h"
#include "pthread-syscalls.h"
#include "signal-syscalls.h"
#include "system-syscalls.h"

#define MACHINE_FORWARD_DECL_ONLY
#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Timer.h"

#include <PosixProcess.h>
#include <PosixSubsystem.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <sched.h>
#include <syslog.h>

#include "modules/system/console/Console.h"
#include "modules/system/users/Group.h"
#include "modules/system/users/User.h"
#include "modules/system/users/UserManager.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#if X64 && !HOSTED
static_assert(RUSAGE_SELF == 0, "musl RUSAGE_SELF selector changed");
static_assert(RUSAGE_CHILDREN == -1, "musl RUSAGE_CHILDREN selector changed");
static_assert(RUSAGE_THREAD == 1, "musl RUSAGE_THREAD selector changed");
static_assert(offsetof(struct rusage, __reserved) == sizeof(LinuxRusage64),
              "musl rusage prefix no longer matches the Linux amd64 syscall ABI");
static_assert(sizeof(struct rusage) == sizeof(LinuxRusage64) + 16 * sizeof(long),
              "musl rusage reserve changed");
#endif

// arch_prctl
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

// Linux prctl operations used by musl's current-thread naming helpers.
#define LINUX_PR_SET_NAME 15
#define LINUX_PR_GET_NAME 16
#define LINUX_TASK_NAME_LENGTH 16

// capget/capset
#define _LINUX_CAPABILITY_VERSION_1 0x19980330

#define LINUX_GRND_NONBLOCK 0x1
#define LINUX_GRND_RANDOM 0x2

namespace {
class CloneInterruptScope {
 public:
  CloneInterruptScope() : m_Previous(Processor::getInterrupts()) {
    Processor::setInterrupts(false);
  }

  ~CloneInterruptScope() {
    Processor::setInterrupts(m_Previous);
  }

 private:
  bool m_Previous;
};

enum class CloneRoute { Process, Thread, Invalid };

CloneRoute cloneRoute(unsigned long flags) {
  constexpr unsigned long ExitSignalMask = 0xff;
  constexpr unsigned long SpawnFlags = CLONE_VM | CLONE_VFORK | SIGCHLD;
  constexpr unsigned long ThreadRequired =
      CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD;
  constexpr unsigned long ThreadAllowed = ThreadRequired | CLONE_SYSVSEM | CLONE_SETTLS |
                                          CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID |
                                          CLONE_DETACHED | CLONE_CHILD_SETTID;

  if (flags & CLONE_THREAD) {
    if ((flags & ExitSignalMask) || (flags & ThreadRequired) != ThreadRequired ||
        (flags & ~ThreadAllowed)) {
      return CloneRoute::Invalid;
    }
    return CloneRoute::Thread;
  }

  // The private-CoW process path can faithfully provide fork-like clone and
  // musl's pipe-synchronised posix_spawn trampoline. Other sharing and
  // namespace combinations must not silently receive fork semantics.
  if (flags == 0 || flags == SIGCHLD || flags == SpawnFlags) {
    return CloneRoute::Process;
  }
  return CloneRoute::Invalid;
}
}  // namespace

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
namespace {
using CloneBeforeStartHook = void (*)(Thread*, size_t, void*);

CloneBeforeStartHook g_CloneBeforeStartHook = nullptr;
void* g_CloneBeforeStartHookContext = nullptr;
}  // namespace

extern "C" EXPORTED_PUBLIC void posixSetCloneBeforeStartHookForTest(CloneBeforeStartHook hook,
                                                                    void* context) {
  if (hook) {
    __atomic_store_n(&g_CloneBeforeStartHookContext, context, __ATOMIC_RELEASE);
    __atomic_store_n(&g_CloneBeforeStartHook, hook, __ATOMIC_RELEASE);
  } else {
    __atomic_store_n(&g_CloneBeforeStartHook, static_cast<CloneBeforeStartHook>(nullptr),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_CloneBeforeStartHookContext, static_cast<void*>(nullptr), __ATOMIC_RELEASE);
  }
}

extern "C" EXPORTED_PUBLIC int posixCloneRouteForTest(unsigned long flags) {
  switch (cloneRoute(flags)) {
    case CloneRoute::Process:
      return 0;
    case CloneRoute::Thread:
      return 1;
    case CloneRoute::Invalid:
      return -1;
  }

  return -1;
}
#endif

struct cap_header {
  uint32_t version;
  int pid;
};

struct cap_data {
  uint32_t effective;
  uint32_t permitted;
  uint32_t inheritable;
};

//
// Syscalls pertaining to system operations.
//

static PosixProcess* getPosixProcess() {
  // Not a POSIX process
  Process* pStockProcess = Processor::information().getCurrentThread()->getParent();
  if (pStockProcess->getType() != Process::Posix) {
    return nullptr;
  }

  return static_cast<PosixProcess*>(pStockProcess);
}

static bool copyUserString(const char* userString, String& copy) {
  PosixSubsystem::UserStringResult result =
      PosixSubsystem::copyUserString(userString, copy, PATH_MAX);
  if (result == PosixSubsystem::UserStringBadAddress) {
    SYSCALL_ERROR(BadAddress);
    return false;
  }
  if (result == PosixSubsystem::UserStringTooLong) {
    SYSCALL_ERROR(NameTooLong);
    return false;
  }
  return true;
}

ssize_t posix_getrandom(void* buffer, size_t length, unsigned int flags) {
  if (flags & ~(LINUX_GRND_NONBLOCK | LINUX_GRND_RANDOM)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  uint8_t snapshot[256];
  const size_t requested = length < sizeof(snapshot) ? length : sizeof(snapshot);
  const size_t produced = hardware_random_bytes(snapshot, requested);
  if (requested && !produced) {
    SYSCALL_ERROR(NoMoreProcesses);
    return -1;
  }
  if (!PosixSubsystem::copyToUser(buffer, snapshot, produced)) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return static_cast<ssize_t>(produced);
}

/// Saves a char** array in the Vector of String*s given.
static size_t save_string_array(const char** array, Vector<SharedPointer<String>>& rArray) {
  size_t result = 0;
  while (*array) {
    String* pStr = new String(*array);
    rArray.pushBack(SharedPointer<String>(pStr));
    array++;

    result += pStr->length() + 1;
  }

  return result;
}

/// Creates a char** array, properly null-terminated, from the Vector of
/// String*s given, at the location "arrayLoc", returning the end of the char**
/// array created in arrayEndLoc and the start as the function return value.
static char** load_string_array(Vector<SharedPointer<String>>& rArray, uintptr_t arrayLoc,
                                uintptr_t& arrayEndLoc) {
  char** pMasterArray = reinterpret_cast<char**>(arrayLoc);

  char* pPtr = reinterpret_cast<char*>(arrayLoc + sizeof(char*) * (rArray.count() + 1));
  int i = 0;
  for (auto it = rArray.begin(); it != rArray.end(); it++) {
    const String* pStr = it->get();

    StringCopy(pPtr, pStr->cstr());
    pPtr[pStr->length()] = '\0';  // Ensure NULL-termination.

    pMasterArray[i] = pPtr;

    pPtr += pStr->length() + 1;
    i++;
  }

  pMasterArray[i] = 0;  // Null terminate.
  arrayEndLoc = reinterpret_cast<uintptr_t>(pPtr);

  return pMasterArray;
}

long posix_sbrk(int delta) {
  SC_NOTICE("sbrk(" << delta << ")");

  long ret = reinterpret_cast<long>(Processor::information().getVirtualAddressSpace().expandHeap(
      delta, VirtualAddressSpace::Write));
  SC_NOTICE("    -> " << ret);
  if (ret == 0) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  } else
    return ret;
}

uintptr_t posix_brk(uintptr_t theBreak) {
  SC_NOTICE("brk(" << theBreak << ")");

  void* newBreak = reinterpret_cast<void*>(theBreak);

  void* currentBreak = Processor::information().getVirtualAddressSpace().getEndOfHeap();
  if (newBreak < currentBreak) {
    SC_NOTICE(" -> " << currentBreak);
    return reinterpret_cast<uintptr_t>(currentBreak);
  }

  intptr_t difference = pointer_diff(currentBreak, newBreak);
  if (!difference) {
    SC_NOTICE(" -> " << currentBreak);
    return reinterpret_cast<uintptr_t>(currentBreak);
  }

  // OK, good to go.
  void* result = Processor::information().getVirtualAddressSpace().expandHeap(
      difference, VirtualAddressSpace::Write);
  if (!result) {
    SYSCALL_ERROR(OutOfMemory);
    SC_NOTICE(" -> ENOMEM");
    return -1;
  }

  // Return new end of heap.
  currentBreak = Processor::information().getVirtualAddressSpace().getEndOfHeap();

  SC_NOTICE(" -> " << currentBreak);
  return reinterpret_cast<uintptr_t>(currentBreak);
}

SyscallState posix_copy_clone_state(const SyscallState& state) {
  SyscallState clonedState = state;
#if HOSTED
  // The hosted bridge's errno destination is stack-local to the parent's
  // translator frame and cannot survive in the child return state.
  clonedState.error_ptr = 0;
#endif
  return clonedState;
}

long posix_clone(SyscallState& state, unsigned long flags, void* child_stack, int* ptid, int* ctid,
                 unsigned long newtls, bool linuxAbi) {
  SC_NOTICE("clone(" << Hex << flags << ", " << child_stack << ", " << ptid << ", " << ctid << ", "
                     << newtls << ")");

  Process* pParentProcess = Processor::information().getCurrentThread()->getParent();
  Process::ThreadCreationScope creation(*pParentProcess);
  if (!creation) {
    SYSCALL_ERROR(NoMoreProcesses);
    return -1;
  }

  // Cloning switches address spaces while assembling the child image, but
  // the syscall return path still needs the caller's IRQ state restored.
  CloneInterruptScope interrupts;

  // Must clone state as we make modifications for the new thread here.
  SyscallState clonedState = posix_copy_clone_state(state);

  // Basic warnings to start with.
  if (flags & CLONE_PARENT) {
    SC_NOTICE(" -> CLONE_PARENT is not yet supported!");
  }
  if (flags & CLONE_VFORK) {
    // Halts parent until child ruins execve() or exit(), just like vfork.
    // We should support this properly.
    SC_NOTICE(" -> CLONE_VFORK is not yet supported!");
  }
#if 0
    if (flags & CLONE_VM) SC_NOTICE("\t\t-> CLONE_VM");
    if (flags & CLONE_FS) SC_NOTICE("\t\t-> CLONE_FS");
    if (flags & CLONE_FILES) SC_NOTICE("\t\t-> CLONE_FILES");
    if (flags & CLONE_SIGHAND) SC_NOTICE("\t\t-> CLONE_SIGHAND");
    if (flags & CLONE_PTRACE) SC_NOTICE("\t\t-> CLONE_PTRACE");
    if (flags & CLONE_VFORK) SC_NOTICE("\t\t-> CLONE_VFORK");
    if (flags & CLONE_PARENT) SC_NOTICE("\t\t-> CLONE_PARENT");
    if (flags & CLONE_THREAD) SC_NOTICE("\t\t-> CLONE_THREAD");
    if (flags & CLONE_NEWNS) SC_NOTICE("\t\t-> CLONE_NEWNS");
    if (flags & CLONE_SYSVSEM) SC_NOTICE("\t\t-> CLONE_SYSVSEM");
    if (flags & CLONE_SETTLS) SC_NOTICE("\t\t-> CLONE_SETTLS");
    if (flags & CLONE_PARENT_SETTID) SC_NOTICE("\t\t-> CLONE_PARENT_SETTID");
    if (flags & CLONE_CHILD_CLEARTID) SC_NOTICE("\t\t-> CLONE_CHILD_CLEARTID");
    if (flags & CLONE_DETACHED) SC_NOTICE("\t\t-> CLONE_DETACHED");
    if (flags & CLONE_UNTRACED) SC_NOTICE("\t\t-> CLONE_UNTRACED");
    if (flags & CLONE_CHILD_SETTID) SC_NOTICE("\t\t-> CLONE_CHILD_SETTID");
    if (flags & CLONE_NEWUTS) SC_NOTICE("\t\t-> CLONE_NEWUTS");
    if (flags & CLONE_NEWIPC) SC_NOTICE("\t\t-> CLONE_NEWIPC");
    if (flags & CLONE_NEWUSER) SC_NOTICE("\t\t-> CLONE_NEWUSER");
    if (flags & CLONE_NEWPID) SC_NOTICE("\t\t-> CLONE_NEWPID");
    if (flags & CLONE_NEWNET) SC_NOTICE("\t\t-> CLONE_NEWNET");
    if (flags & CLONE_IO) SC_NOTICE("\t\t-> CLONE_IO");
#endif

  const CloneRoute route = cloneRoute(flags);
  if (route == CloneRoute::Invalid) {
    SYSCALL_ERROR(InvalidArgument);
    SC_NOTICE(" -> EINVAL (unsupported or inconsistent clone flags)");
    return -1;
  }

  if (route == CloneRoute::Thread) {
    // clone vm doesn't actually copy the address space, it shares it

    // New child's stack. Must be valid as we're sharing the address space.
    if (!child_stack) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }

    // Set up stack for new thread.
    clonedState.setStackPointer(reinterpret_cast<uintptr_t>(child_stack));

    // Child returns 0 -- parent returns the new thread ID.
    clonedState.setSyscallReturnValue(0);

    Thread* pThread = nullptr;
    size_t threadId = 0;
    bool copiedIds = false;
    {
      MemoryMapManager& mappings = MemoryMapManager::instance();
      MemoryMapManager::OperationGuard mappingGuard(mappings);
      auto writableId = [&](int* address) {
        const uintptr_t target = reinterpret_cast<uintptr_t>(address);
        return PosixSubsystem::checkAddress(target, sizeof(int), PosixSubsystem::SafeWrite) &&
               mappings.faultIn(target, true) && mappings.faultIn(target + sizeof(int) - 1, true);
      };
      if (((flags & CLONE_CHILD_SETTID) && !writableId(ctid)) ||
          ((flags & CLONE_PARENT_SETTID) && !writableId(ptid))) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
      const bool setTls = !linuxAbi || (flags & CLONE_SETTLS);
      if (setTls && (newtls >= pParentProcess->getAddressSpace()->getKernelStart()
#if X64
                     || newtls >= 0x0000800000000000ULL
#endif
                     )) {
        SYSCALL_ERROR(NotEnoughPermissions);
        return -1;
      }
      // The native ABI initializes its TLS self pointer. Linux supplies an
      // opaque FS base and owns initialization of any memory it points to.
      if (!linuxAbi &&
          !PosixSubsystem::copyToUser(reinterpret_cast<void*>(newtls), &newtls, sizeof(newtls))) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }

      pThread = new Thread(pParentProcess, clonedState, true);
      pThread->setName("posix clone() thread");
      if (setTls) {
        pThread->setTlsBase(newtls);
      }
      threadId = linuxAbi ? pThread->getTaskId() : pThread->getId();
      const int id = static_cast<int>(threadId);
      copiedIds =
          (!(flags & CLONE_CHILD_SETTID) || PosixSubsystem::copyToUser(ctid, &id, sizeof(id))) &&
          (!(flags & CLONE_PARENT_SETTID) || PosixSubsystem::copyToUser(ptid, &id, sizeof(id)));
      if (copiedIds && (flags & CLONE_CHILD_CLEARTID)) {
        pThread->setClearChildTid(reinterpret_cast<uintptr_t>(ctid));
      }
    }
    if (!copiedIds) {
      // The existing delayed-start cancellation path owns retirement. A
      // published Thread cannot be deleted directly on this error path.
      pThread->setUnwindState(Thread::TerminateThread);
      pThread->startDetached();
      SYSCALL_ERROR(BadAddress);
      return -1;
    }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    void* hookContext = __atomic_load_n(&g_CloneBeforeStartHookContext, __ATOMIC_ACQUIRE);
    CloneBeforeStartHook hook = __atomic_load_n(&g_CloneBeforeStartHook, __ATOMIC_ACQUIRE);
    if (hook) {
      hook(pThread, threadId, hookContext);
    }
#endif

    if (!pThread->startDetached()) {
      FATAL("clone(): delayed thread could not be started.");
    }

    // Parent gets the new thread ID.
    SC_NOTICE(" -> " << threadId << " [new thread]");
    return threadId;
  }

  if (flags & CLONE_VM) {
    // Pedigree cannot safely share one address space between distinct
    // processes yet. A private CoW child preserves process identity for
    // posix_spawn without exposing the parent to the child's exec or exit.
    SC_NOTICE(" -> normalizing process CLONE_VM to a private address space");
  }

  // No child stack means CoW the existing one, but if one is specified we
  // should use it instead!
  if (child_stack) {
    clonedState.setStackPointer(reinterpret_cast<uintptr_t>(child_stack));
  }

  PosixSubsystem* pParentSubsystem = static_cast<PosixSubsystem*>(pParentProcess->getSubsystem());
  if (!pParentSubsystem) {
    ERROR("No subsystem for the parent process!");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  // Inhibit signals to the parent
  for (size_t sig = 0; sig < PosixSubsystem::SignalDispositionCount; sig++)
    Processor::information().getCurrentThread()->inhibitEvent(sig, true);

  // Create a new process.
  PosixProcess* pProcess = new PosixProcess(pParentProcess);
  if (!pProcess) {
    for (size_t sig = 0; sig < PosixSubsystem::SignalDispositionCount; sig++)
      Processor::information().getCurrentThread()->inhibitEvent(sig, false);
    SYSCALL_ERROR(OutOfMemory);
    SC_NOTICE(" -> ENOMEM");
    return -1;
  }

  PosixSubsystem* pSubsystem = new PosixSubsystem(*pParentSubsystem);
  if (!pSubsystem) {
    ERROR("Could not create a subsystem for the child process!");
    delete pProcess;

    SYSCALL_ERROR(OutOfMemory);

    // Allow signals again, something went wrong
    for (size_t sig = 0; sig < PosixSubsystem::SignalDispositionCount; sig++)
      Processor::information().getCurrentThread()->inhibitEvent(sig, false);
    SC_NOTICE(" -> ENOMEM");
    return -1;
  }
  pProcess->setSubsystem(pSubsystem);
  pSubsystem->setProcess(pProcess);

  // Copy POSIX Process Group information if needed
  if (pParentProcess->getType() == Process::Posix) {
    PosixProcess* p = static_cast<PosixProcess*>(pParentProcess);

    // Do not adopt leadership status.
    if (p->getGroupMembership() == PosixProcess::Leader) {
      SC_NOTICE("fork parent was a group leader.");
    } else {
      SC_NOTICE("fork parent had status " << static_cast<int>(p->getGroupMembership()) << "...");
    }
    pProcess->inheritProcessGroup(p);
  }

  // Register with the dynamic linker.
  DynamicLinker* oldLinker = pProcess->getLinker();
  if (oldLinker) {
    DynamicLinker* newLinker = new DynamicLinker(*oldLinker);
    pProcess->setLinker(newLinker);
  }

  MemoryMapManager::instance().clone(pProcess);

  // Copy the file descriptors from the parent
  pSubsystem->copyDescriptors(pParentSubsystem);

  // Child returns 0.
  clonedState.setSyscallReturnValue(0);

  // Allow signals to the parent again
  for (size_t sig = 0; sig < PosixSubsystem::SignalDispositionCount; sig++)
    Processor::information().getCurrentThread()->inhibitEvent(sig, false);

  // Set ctid in the new address space if we are required to.
  if (flags & CLONE_CHILD_SETTID) {
    VirtualAddressSpace& curr = Processor::information().getVirtualAddressSpace();
    VirtualAddressSpace* va = pProcess->getAddressSpace();
    const int childId = static_cast<int>(pProcess->getId());
    bool copied = false;
    {
      MemoryMapManager::OperationGuard mappingGuard(MemoryMapManager::instance());
      Processor::switchAddressSpace(*va);
      copied = PosixSubsystem::copyToUser(ctid, &childId, sizeof(childId));
      Processor::switchAddressSpace(curr);
    }
    if (!copied) {
      delete pProcess;
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
  }

  // Create a new thread for the new process.
  Thread* pThread = new Thread(pProcess, clonedState, true);
  pThread->setName("posix clone() forked thread");
  pThread->detach();
  if (flags & CLONE_CHILD_CLEARTID) {
    // The child has its own address space, so its exit hook can perform
    // the Linux clear-child-TID write without shared-VM semantics.
    pThread->setClearChildTid(reinterpret_cast<uintptr_t>(ctid));
  }

  // Finish publishing the child-side POSIX state before it can execute.
  pedigree_copy_posix_thread(Processor::information().getCurrentThread(), pParentSubsystem, pThread,
                             pSubsystem);
  pProcess->publish();
  if (!pThread->start()) {
    FATAL("fork(): delayed child thread could not be started.");
  }

  // Parent returns child ID.
  SC_NOTICE(" -> " << pProcess->getId() << " [new process]");
  return pProcess->getId();
}

int posix_fork(SyscallState& state) {
  SC_NOTICE("fork");

  return posix_clone(state, 0, 0, 0, 0, 0);
}

int posix_execve(const char* name, const char** argv, const char** env, SyscallState& state) {
  String nameCopy;
  if (!copyUserString(name, nameCopy)) {
    SC_NOTICE("execve -> invalid address");
    return -1;
  }

  SC_NOTICE("execve(\"" << nameCopy << "\")");

  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("No subsystem for this process!");
    return -1;
  }

  Vector<String> listArgv, listEnv;
  {
    MemoryMapManager::OperationGuard mappingGuard(MemoryMapManager::instance());
    size_t remaining = PosixSubsystem::MaximumExecArgumentBytes - 2 * sizeof(uintptr_t);
    if (nameCopy.length() >= remaining) {
      SYSCALL_ERROR(TooBig);
      return -1;
    }
    remaining -= nameCopy.length() + 1;
    auto snapshot = [&](const char** pointers, Vector<String>& output) {
      uintptr_t cursor = reinterpret_cast<uintptr_t>(pointers);
      while (cursor) {
        const char* argument = nullptr;
        if (!PosixSubsystem::copyFromUser(&argument, reinterpret_cast<void*>(cursor),
                                          sizeof(argument))) {
          SYSCALL_ERROR(BadAddress);
          return false;
        }
        if (!argument) {
          return true;
        }
        if (remaining <= sizeof(uintptr_t)) {
          SYSCALL_ERROR(TooBig);
          return false;
        }
        remaining -= sizeof(uintptr_t);
        String value;
        const auto result = PosixSubsystem::copyUserString(argument, value, remaining);
        if (result != PosixSubsystem::UserStringSuccess) {
          syscallError(result == PosixSubsystem::UserStringBadAddress ? Error::BadAddress
                                                                      : Error::TooBig);
          return false;
        }
        remaining -= value.length() + 1;
        output.pushBack(value);
        if (cursor > ~uintptr_t(0) - sizeof(uintptr_t)) {
          SYSCALL_ERROR(BadAddress);
          return false;
        }
        cursor += sizeof(uintptr_t);
      }
      return true;
    };
    if (!snapshot(argv, listArgv) || !snapshot(env, listEnv)) {
      return -1;
    }
  }

  // Normalise path to ensure we have the correct path to invoke.
  String invokePath;
  normalisePath(invokePath, nameCopy.cstr());

  if (!pSubsystem->invoke(invokePath.cstr(), listArgv, listEnv, state)) {
    SC_NOTICE(" -> execve failed in invoke");
    return -1;
  }

  // Technically, we never get here.
  return 0;
}

static bool waitpidEligibleChild(PosixProcess* pParent, bool parentHasGroup, size_t parentGroupId,
                                 Process* pCandidate, int pid) {
  if (!pCandidate || pCandidate == pParent || pCandidate->getType() != Process::Posix ||
      pCandidate->getParent() != pParent || pCandidate->getState() == Process::Reaped) {
    return false;
  }

  if (pid > 0) {
    return static_cast<int>(pCandidate->getId()) == pid;
  }

  if (pid == -1) {
    return true;
  }

  PosixProcess* pPosixCandidate = static_cast<PosixProcess*>(pCandidate);
  size_t candidateGroupId = 0;
  if (!pPosixCandidate->getProcessGroupId(candidateGroupId)) {
    return false;
  }

  if (pid == 0) {
    return parentHasGroup && candidateGroupId == parentGroupId;
  }

  return static_cast<int64_t>(candidateGroupId) == -static_cast<int64_t>(pid);
}

int posix_waitpid(const int pid, int* status, int options, LinuxRusage64* usage) {
  Thread* currentThread = Processor::information().getCurrentThread();
  currentThread->retainTemporarySignalWaitInterruptionOrClear();
  struct InterruptionScope {
    Thread* thread;
    ~InterruptionScope() {
      // Consume this syscall's marker without removing pending signal events
      // or an enclosing temporary-mask wait's interruption ownership.
      thread->retainTemporarySignalWaitInterruptionOrClear();
    }
  } interruptionScope{currentThread};

  if (status && !PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(status), sizeof(int),
                                              PosixSubsystem::SafeWrite)) {
    SC_NOTICE("waitpid -> invalid address");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (usage && !PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(usage), sizeof(*usage),
                                             PosixSubsystem::SafeWrite)) {
    SC_NOTICE("wait4 -> invalid rusage address");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  SC_NOTICE("waitpid(" << pid << " [" << Dec << pid << Hex << "], " << options << ")");

  // Metadata about the calling process.
  PosixProcess* pThisProcess = static_cast<PosixProcess*>(currentThread->getParent());

  const bool bBlock = (options & WNOHANG) != WNOHANG;
  if (bBlock) {
    SC_NOTICE(" -> blocking until a process reports status");
  } else {
    SC_NOTICE(" -> WNOHANG");
  }

  WaitQueue::WakeReason previousWake = WaitQueue::WakeReason::Signalled;
  while (true) {
    Process::ReaperClaim reaper;
    int resultPid = -1;
    int resultStatus = 0;
    Time::Timestamp resultUser = 0;
    Time::Timestamp resultKernel = 0;
    bool hasResult = false;
    Process* reapedProcess = nullptr;

    {
      // This guard is both the concurrent-reaper lock and the atomic
      // predicate-to-sleep handoff for every child of this process.
      auto guard = pThisProcess->acquireChildStateWait();
      bool hasEligibleChild = false;

      // Rebuild the candidates after every wake. No Process pointer is
      // retained across a blocking point.
      size_t parentGroupId = 0;
      const bool parentHasGroup = pThisProcess->getProcessGroupId(parentGroupId);
      for (size_t i = 0;; ++i) {
        Process* pProcess = Scheduler::instance().getChildProcess(pThisProcess, i);
        if (!pProcess) {
          break;
        }

        if (!waitpidEligibleChild(pThisProcess, parentHasGroup, parentGroupId, pProcess, pid)) {
          continue;
        }

        hasEligibleChild = true;
        resultPid = static_cast<int>(pProcess->getId());

        if (pProcess->getState() == Process::Terminated) {
          resultStatus = pProcess->getExitStatus();
          pProcess->reap();
          reaper = pProcess->tryClaimReaper();
          if (!reaper) {
            FATAL("waitpid lost sole reaper ownership for pid " << Dec << resultPid << ".");
          }
          reapedProcess = pProcess;
          hasResult = true;
          SC_NOTICE("waitpid: " << Dec << resultPid << " reaped [" << resultStatus << "]");
          break;
        }

        Process::ChildTransition transition;
        if (pProcess->takePendingChildTransition(options & WUNTRACED, options & WCONTINUED,
                                                 transition)) {
          if (transition.kind == Process::ChildTransitionKind::Stopped) {
            resultStatus = ((transition.stopSignal & 0xFF) << 8) | 0x7F;
            SC_NOTICE("waitpid: " << Dec << resultPid << " stopped by " << transition.stopSignal
                                  << ".");
          } else {
            resultStatus = 0xFFFF;
            SC_NOTICE("waitpid: " << Dec << resultPid << " continued.");
          }
          resultUser = pProcess->getUserTime() + pProcess->getReapedChildrenUserTime();
          resultKernel = pProcess->getKernelTime() + pProcess->getReapedChildrenKernelTime();
          hasResult = true;
          break;
        }

        SC_NOTICE("waitpid: " << Dec << resultPid << " has no status change");
      }

      if (!hasResult) {
        if (!hasEligibleChild) {
          SYSCALL_ERROR(NoChildren);
          SC_NOTICE("waitpid: no eligible children");
          return -1;
        }

        if (!bBlock) {
          SC_NOTICE("waitpid: not blocking, no status to report");
          return 0;
        }

        // Exact-user-return handlers remain pending until this syscall exits.
        // A child status found above still takes precedence over interruption.
        const bool signalInterrupted =
            currentThread->getInterruptionReason() == Thread::InterruptedBySignal;
        if (signalInterrupted || previousWake == WaitQueue::WakeReason::Unwinding ||
            previousWake == WaitQueue::WakeReason::Terminating) {
          SYSCALL_ERROR(Interrupted);
          SC_NOTICE("waitpid: interrupted");
          return -1;
        }

        previousWake = guard.wait(WaitQueue::Channel(), Thread::ProcessWait,
                                  reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
      }
    }

    if (hasResult) {
      bool copied = true;
      LinuxRusage64 resultUsage = {};
      if (reaper) {
        // Terminated is published before the exiting thread's final scheduler
        // accounting. ReaperClaim keeps the child alive while this barrier
        // makes both its self and descendant totals final.
        if (!reapedProcess->waitUntilTerminationReapable()) {
          FATAL("waitpid attempted to reap its own terminating process");
        }

        pThisProcess->accountReapedChild(reapedProcess, resultUser, resultKernel);
      }

      if (usage) {
        resultUsage.userSeconds = resultUser / Time::Multiplier::Second;
        resultUsage.userMicroseconds =
            (resultUser % Time::Multiplier::Second) / Time::Multiplier::Microsecond;
        resultUsage.systemSeconds = resultKernel / Time::Multiplier::Second;
        resultUsage.systemMicroseconds =
            (resultKernel % Time::Multiplier::Second) / Time::Multiplier::Microsecond;
      }

      if (status && !PosixSubsystem::copyToUser(status, &resultStatus, sizeof(resultStatus))) {
        copied = false;
      }
      if (usage && !PosixSubsystem::copyToUser(usage, &resultUsage, sizeof(resultUsage))) {
        copied = false;
      }

      // The claim's termination deferral keeps this stack—and its sole
      // destruction ownership—alive after the parent guard is dropped.
      if (reaper) {
        reaper.publish();
      }

      if (!copied) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }

      return resultPid;
    }
  }
}

int posix_getpid() {
  SC_NOTICE("getpid");

  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  return pProcess->getId();
}

int posix_getppid() {
  SC_NOTICE("getppid");

  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  while (true) {
    Process* expectedParent = pProcess->getParent();
    if (!expectedParent) {
      return 0;
    }

    Scheduler::ProcessLease parent;
    if (!Scheduler::instance().acquireProcess(parent, expectedParent)) {
      if (pProcess->getParent() != expectedParent) {
        continue;
      }
      return 0;
    }
    if (pProcess->getParent() == parent.get()) {
      return parent->getId();
    }
  }
}

int posix_gettimeofday(timeval* tv, struct timezone* tz) {
  SC_NOTICE("gettimeofday");

  const Time::Timestamp nanoseconds = Time::getTimeNanoseconds();
  if (tv) {
    struct timeval result = {};
    result.tv_sec = nanoseconds / Time::Multiplier::Second;
    result.tv_usec = (nanoseconds % Time::Multiplier::Second) / Time::Multiplier::Microsecond;
    if (!PosixSubsystem::copyToUser(tv, &result, sizeof(result))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
  }

  if (tz) {
    const struct timezone result = {};
    if (!PosixSubsystem::copyToUser(tz, &result, sizeof(result))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
  }

  return 0;
}

int posix_settimeofday(const timeval* tv, const struct timezone* tz) {
  SC_NOTICE("settimeofday");
  SYSCALL_ERROR(Unimplemented);
  return -1;
}

time_t posix_time(time_t* tval) {
  SC_NOTICE("time");

  time_t result = Time::getTime();
  if (tval && !PosixSubsystem::copyToUser(tval, &result, sizeof(result))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  return result;
}

clock_t posix_times(struct tms* tm) {
  SC_NOTICE("times");

  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  constexpr Time::Timestamp nanosecondsPerClockTick = Time::Multiplier::Second / 100;

  struct tms result = {};
  result.tms_utime = pProcess->getUserTime() / nanosecondsPerClockTick;
  result.tms_stime = pProcess->getKernelTime() / nanosecondsPerClockTick;
  result.tms_cutime = pProcess->getReapedChildrenUserTime() / nanosecondsPerClockTick;
  result.tms_cstime = pProcess->getReapedChildrenKernelTime() / nanosecondsPerClockTick;
  if (tm && !PosixSubsystem::copyToUser(tm, &result, sizeof(result))) {
    SC_NOTICE("posix_times -> invalid address");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  SC_NOTICE("times: u=" << pProcess->getUserTime() << ", s=" << pProcess->getKernelTime());

  return Time::getTicks() / nanosecondsPerClockTick;
}

int posix_getrusage(int who, struct rusage* r) {
  SC_NOTICE("getrusage who=" << who);

  if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN && who != RUSAGE_THREAD) {
    SC_NOTICE("posix_getrusage -> unsupported selector");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  Thread* currentThread = Processor::information().getCurrentThread();
  Process* pProcess = currentThread->getParent();
  const Time::Timestamp user = who == RUSAGE_THREAD     ? currentThread->getUserTime()
                               : who == RUSAGE_CHILDREN ? pProcess->getReapedChildrenUserTime()
                                                        : pProcess->getUserTime();
  const Time::Timestamp kernel = who == RUSAGE_THREAD     ? currentThread->getKernelTime()
                                 : who == RUSAGE_CHILDREN ? pProcess->getReapedChildrenKernelTime()
                                                          : pProcess->getKernelTime();

  struct rusage result = {};
  result.ru_utime.tv_sec = user / Time::Multiplier::Second;
  result.ru_utime.tv_usec = (user % Time::Multiplier::Second) / Time::Multiplier::Microsecond;
  result.ru_stime.tv_sec = kernel / Time::Multiplier::Second;
  result.ru_stime.tv_usec = (kernel % Time::Multiplier::Second) / Time::Multiplier::Microsecond;

  if (!PosixSubsystem::copyToUser(r, &result, sizeof(result))) {
    SC_NOTICE("posix_getrusage -> invalid address");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  return 0;
}

int posix_linux_getrusage(int who, LinuxRusage64* r) {
  SC_NOTICE("Linux getrusage who=" << who);

  if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN && who != RUSAGE_THREAD) {
    SC_NOTICE("posix_linux_getrusage -> unsupported selector");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  Thread* currentThread = Processor::information().getCurrentThread();
  Process* pProcess = currentThread->getParent();
  const Time::Timestamp user = who == RUSAGE_THREAD     ? currentThread->getUserTime()
                               : who == RUSAGE_CHILDREN ? pProcess->getReapedChildrenUserTime()
                                                        : pProcess->getUserTime();
  const Time::Timestamp kernel = who == RUSAGE_THREAD     ? currentThread->getKernelTime()
                                 : who == RUSAGE_CHILDREN ? pProcess->getReapedChildrenKernelTime()
                                                          : pProcess->getKernelTime();

  LinuxRusage64 result = {};
  result.userSeconds = user / Time::Multiplier::Second;
  result.userMicroseconds = (user % Time::Multiplier::Second) / Time::Multiplier::Microsecond;
  result.systemSeconds = kernel / Time::Multiplier::Second;
  result.systemMicroseconds = (kernel % Time::Multiplier::Second) / Time::Multiplier::Microsecond;

  if (!PosixSubsystem::copyToUser(r, &result, sizeof(result))) {
    SC_NOTICE("posix_linux_getrusage -> invalid address");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  return 0;
}

namespace {
int copyPasswd(User* user, passwd* output, char* userStrings) {
  if (!user) {
    return -1;
  }
  // The native glue supplies one 256-byte buffer for all passwd strings.
  char strings[256] = {};
  passwd result = {};
  size_t used = 0;
  const uintptr_t base = reinterpret_cast<uintptr_t>(userStrings);
  const String empty;
  const String* values[] = {&user->getUsername(), &empty, &user->getFullName(), &user->getHome(),
                            &user->getShell()};
  char** fields[] = {&result.pw_name, &result.pw_passwd, &result.pw_gecos, &result.pw_dir,
                     &result.pw_shell};
  for (size_t i = 0; i < 5; ++i) {
    const size_t length = values[i]->length();
    if (length >= sizeof(strings) - used) {
      SYSCALL_ERROR(BadRange);
      return -1;
    }
    *fields[i] = reinterpret_cast<char*>(base + used);
    MemoryCopy(strings + used, values[i]->cstr(), length + 1);
    used += length + 1;
  }
  result.pw_uid = user->getId();
  result.pw_gid = user->getDefaultGroup()->getId();
  if (!PosixSubsystem::copyToUser(userStrings, strings, used) ||
      !PosixSubsystem::copyToUser(output, &result, sizeof(result))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return 0;
}

int copyGroup(Group* group, struct group* output) {
  if (!group) {
    return -1;
  }
  struct group result = {};
  if (!PosixSubsystem::copyFromUser(&result, output, sizeof(result))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  // The native getgr* glue allocates 256 bytes for the caller-owned name.
  const String& name = group->getName();
  if (name.length() >= 256) {
    SYSCALL_ERROR(BadRange);
    return -1;
  }
  result.gr_gid = group->getId();
  if (!PosixSubsystem::copyToUser(result.gr_name, name.cstr(), name.length() + 1) ||
      !PosixSubsystem::copyToUser(output, &result, sizeof(result))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return 0;
}
}  // namespace

int posix_getpwent(passwd* pw, int n, char* str) {
  return copyPasswd(UserManager::instance().getUser(n), pw, str);
}

int posix_getpwnam(passwd* pw, const char* name, char* str) {
  String nameCopy;
  if (!copyUserString(name, nameCopy)) {
    return -1;
  }
  return copyPasswd(UserManager::instance().getUser(nameCopy), pw, str);
}

int posix_getgrnam(const char* name, struct group* out) {
  String nameCopy;
  if (!copyUserString(name, nameCopy)) {
    return -1;
  }
  return copyGroup(UserManager::instance().getGroup(nameCopy), out);
}

int posix_getgrgid(gid_t id, struct group* out) {
  return copyGroup(UserManager::instance().getGroup(id), out);
}

uid_t posix_getuid() {
  SC_NOTICE("getuid");

  return Processor::information().getCurrentThread()->getParent()->getUserId();
}

gid_t posix_getgid() {
  SC_NOTICE("getgid");

  return Processor::information().getCurrentThread()->getParent()->getGroupId();
}

uid_t posix_geteuid() {
  SC_NOTICE("geteuid");

  return Processor::information().getCurrentThread()->getParent()->getEffectiveUserId();
}

gid_t posix_getegid() {
  SC_NOTICE("getegid");

  return Processor::information().getCurrentThread()->getParent()->getEffectiveGroupId();
}

int posix_setuid(uid_t uid) {
  SC_NOTICE("setuid(" << uid << ")");
  return posix_setresuid(uid, uid, -1);
}

int posix_setgid(gid_t gid) {
  SC_NOTICE("setgid(" << gid << ")");
  return posix_setresgid(gid, gid, -1);
}

int posix_seteuid(uid_t euid) {
  SC_NOTICE("seteuid(" << euid << ")");
  return posix_setresuid(-1, euid, -1);
}

int posix_setegid(gid_t egid) {
  SC_NOTICE("setegid(" << egid << ")");
  return posix_setresgid(-1, egid, -1);
}

EXPORTED_PUBLIC int pedigree_login(int uid) {
  // Grab the given user.
  User* pUser = UserManager::instance().getUser(uid);
  if (!pUser)
    return -1;

  pUser->login();
  return 0;
}

int posix_setsid() {
  SC_NOTICE("setsid");

  // Not a POSIX process
  Process* pStockProcess = Processor::information().getCurrentThread()->getParent();
  if (pStockProcess->getType() != Process::Posix) {
    ERROR("setsid called on something not a POSIX process");
    return -1;
  }

  PosixProcess* pProcess = static_cast<PosixProcess*>(pStockProcess);

  // Already in a group?
  PosixProcess::Membership myMembership = pProcess->getGroupMembership();
  if (myMembership != PosixProcess::NoGroup) {
    size_t oldGroupId = 0;
    const bool hasOldGroup = pProcess->getProcessGroupId(oldGroupId);
    // If we don't actually have a group, something's gone wrong
    if (!hasOldGroup)
      FATAL(
          "Process' is apparently a member of a group, but its group "
          "pointer is invalid.");

    // Are we the group leader of that other group?
    if (myMembership == PosixProcess::Leader) {
      SC_NOTICE("setsid() called while the leader of another group");
      SYSCALL_ERROR(PermissionDenied);
      return -1;
    } else {
      SC_NOTICE("setsid() called while a member of another group [" << oldGroupId << "]");
    }
  }

  pProcess->leaveProcessGroup();

  // Create the new session.
  PosixSession* pNewSession = new PosixSession();
  pNewSession->Leader = pProcess;
  pProcess->setSession(pNewSession);

  // Create a new process group and join it.
  ProcessGroup* pNewGroup = new ProcessGroup;
  pNewGroup->processGroupId = pProcess->getId();
  pNewGroup->Leader = pProcess;
  pNewGroup->Members.clear();

  // We're now a group leader - we got promoted!
  pProcess->setProcessGroup(pNewGroup);
  pProcess->setGroupMembership(PosixProcess::Leader);

  // Remove controlling terminal.
  pProcess->setCtty(0);

  const size_t newGroupId = pProcess->getId();
  SC_NOTICE("setsid: now part of a group [id=" << newGroupId << "]!");

  // Success!
  return newGroupId;
}

int posix_setpgid(int pid_, int pgid) {
  size_t pid = pid_;
  SC_NOTICE("setpgid(" << pid << ", " << pgid << ")");

  // Handle invalid group ID
  if (pgid < 0) {
    SYSCALL_ERROR(InvalidArgument);
    SC_NOTICE(" -> EINVAL");
    return -1;
  }

  Process* pBaseProcess = Processor::information().getCurrentThread()->getParent();
  if (pBaseProcess->getType() != Process::Posix) {
    SC_NOTICE("  -> not a posix process");
    return -1;
  }

  // Are we already a leader of a session?
  PosixProcess* pProcess = static_cast<PosixProcess*>(pBaseProcess);
  Scheduler::ProcessLease targetProcessLease;

  // Handle zero PID and PGID.
  if (!pid) {
    pid = pProcess->getId();
  }
  if (!pgid) {
    pgid = pid;
  }

  size_t currentGroupId = 0;
  bool hasCurrentGroup = pProcess->getProcessGroupId(currentGroupId);
  PosixSession* pSession = pProcess->getSession();

  // Is this us or a child of us?
  /// \todo pid == child, but child not in this session = EPERM
  if (pid != pProcess->getId()) {
    if (!Scheduler::instance().acquireProcessById(targetProcessLease, pid) ||
        targetProcessLease->getType() != Process::Posix) {
      SC_NOTICE("  -> process doesn't exist");
      SYSCALL_ERROR(NoSuchProcess);
      return -1;
    }
    Process* pTargetProcess = targetProcessLease.get();

    // Is this process a child of us?
    Process* parent = pTargetProcess->getParent();
    while (parent && parent != pProcess) {
      Scheduler::ProcessLease parentLease;
      if (!Scheduler::instance().acquireProcess(parentLease, parent)) {
        parent = nullptr;
        break;
      }
      parent = parentLease->getParent();
    }

    if (parent != pProcess) {
      // Not a child!
      SC_NOTICE(
          "  -> target process is not a descendant of the current "
          "process");
      SYSCALL_ERROR(NoSuchProcess);
      return -1;
    }

    if (static_cast<PosixProcess*>(pTargetProcess)->getSession() != pSession) {
      SC_NOTICE("  -> target process is in a different session");
      SYSCALL_ERROR(NotEnoughPermissions);
      return -1;
    }

    pBaseProcess = pTargetProcess;
    pProcess = static_cast<PosixProcess*>(pTargetProcess);
    hasCurrentGroup = pProcess->getProcessGroupId(currentGroupId);
    pSession = pProcess->getSession();
  }

  if (hasCurrentGroup && currentGroupId == static_cast<size_t>(pgid)) {
    // Already a member.
    SC_NOTICE(" -> OK, already a member!");
    return 0;
  }

  if (pSession && (pSession->Leader == pProcess)) {
    // Already a session leader.
    SYSCALL_ERROR(PermissionDenied);
    SC_NOTICE(" -> EPERM (already leader)");
    return -1;
  }

  // Allocate outside the group spinlock. A concurrent creator can win while
  // we search; the locked recheck below then joins its group and discards
  // this unregistered candidate.
  ProcessGroup* newGroup = new ProcessGroup;
  newGroup->processGroupId = pProcess->getId();
  newGroup->Leader = pProcess;
  newGroup->Members.clear();

  bool joined = false;
  bool created = false;
  {
    RecursingLockGuard<Spinlock> groupGuard(ProcessGroupManager::instance().lock());

    ProcessGroup* targetGroup = pProcess->getProcessGroup();
    if (targetGroup &&
        static_cast<size_t>(targetGroup->processGroupId) == static_cast<size_t>(pgid)) {
      joined = true;
    } else if (ProcessGroup* existingGroup = ProcessGroupManager::instance().findGroup(pgid)) {
      pProcess->setProcessGroup(existingGroup);
      pProcess->setGroupMembership(PosixProcess::Member);
      joined = true;
    } else if (static_cast<size_t>(pgid) == pProcess->getId() &&
               !ProcessGroupManager::instance().isGroupIdValid(pgid)) {
      pProcess->setProcessGroup(newGroup);
      pProcess->setGroupMembership(PosixProcess::Leader);
      created = true;
    }
  }

  if (!created) {
    delete newGroup;
  }
  if (!joined && !created) {
    SYSCALL_ERROR(PermissionDenied);
    SC_NOTICE(" -> EPERM (group does not exist)");
    return -1;
  }

  SC_NOTICE(created ? " -> OK, created!" : " -> OK, joined!");
  return 0;
}

int posix_getpgid(int pid) {
  if (!pid) {
    return posix_getpgrp();
  }

  size_t pid_ = pid;

  SC_NOTICE("getpgid(" << pid << ")");

  Scheduler::ProcessLease target;
  if (!Scheduler::instance().acquireProcessById(target, pid_) ||
      target->getType() != Process::Posix) {
    SC_NOTICE(" -> target process not found");
    SYSCALL_ERROR(NoSuchProcess);
    return -1;
  }

  PosixProcess* pProcess = static_cast<PosixProcess*>(target.get());
  size_t groupId = 0;
  if (pProcess->getProcessGroupId(groupId)) {
    SC_NOTICE(" -> " << groupId);
    return groupId;
  }

  SC_NOTICE(" -> target process did not have a group");
  SYSCALL_ERROR(NoSuchProcess);
  return -1;
}

int posix_getpgrp() {
  SC_NOTICE("getpgrp");

  PosixProcess* pProcess =
      static_cast<PosixProcess*>(Processor::information().getCurrentThread()->getParent());

  int result = 0;
  size_t groupId = 0;
  if (pProcess->getProcessGroupId(groupId)) {
    SC_NOTICE(" -> using existing group id");
    result = static_cast<int>(groupId);
  } else {
    SC_NOTICE(" -> using pid only");
    result = pProcess->getId();  // Fallback if no ProcessGroup pointer yet
  }

  SC_NOTICE(" -> " << result);
  return result;
}

mode_t posix_umask(mode_t mask) {
  SC_NOTICE("umask(" << Oct << mask << ")");

  // Not a POSIX process
  Process* pStockProcess = Processor::information().getCurrentThread()->getParent();
  if (pStockProcess->getType() != Process::Posix) {
    SC_NOTICE("umask -> called on something not a POSIX process");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  PosixProcess* pProcess = static_cast<PosixProcess*>(pStockProcess);

  uint32_t previous = pProcess->getMask();
  pProcess->setMask(mask);

  return previous;
}

int posix_linux_syslog(int type, char* buf, int len) {
  if (!PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(buf), len,
                                    PosixSubsystem::SafeRead)) {
    SC_NOTICE("linux_syslog -> invalid address");
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  SC_NOTICE("linux_syslog");

  if (len > 512)
    len = 512;

  switch (type) {
    case 0:
      SC_NOTICE(" -> close log");
      return 0;

    case 1:
      SC_NOTICE(" -> open log");
      return 0;

    case 2:
      /// \todo expose kernel log via this interface
      // NOTE: blocking call...
      SC_NOTICE(" -> read log");
      Processor::information().getCurrentThread()->waitForEvent();
      return 0;

    case 3:
      /// \todo expose kernel log via this interface
      SC_NOTICE(" -> read up to last 4k");
      return 0;

    case 4:
      /// \todo expose kernel log via this interface
      SC_NOTICE(" -> read and clear last 4k");
      return 0;

    case 5:
      SC_NOTICE(" -> clear");
      return 0;

    case 6:
      SC_NOTICE(" -> disable write to console");
      return 0;

    case 7:
      SC_NOTICE(" -> enable write to console");
      return 0;

    case 8:
      SC_NOTICE(" -> set console write level");
      return 0;

    default:
      SC_NOTICE(" -> unknown!");
      SYSCALL_ERROR(InvalidArgument);
      return -1;
  }
}

int posix_syslog(const char* msg, int prio) {
  String msgCopy;
  if (!copyUserString(msg, msgCopy)) {
    SC_NOTICE("klog -> invalid address");
    return -1;
  }

  uint64_t id = Processor::information().getCurrentThread()->getParent()->getId();
  if (id <= 1) {
    if (prio <= LOG_CRIT)
      FATAL("[" << Dec << id << Hex << "]\tklog: " << msgCopy);
  }

  if (prio <= LOG_ERR)
    ERROR("[" << Dec << id << Hex << "]\tklog: " << msgCopy);
  else if (prio == LOG_WARNING)
    WARNING("[" << Dec << id << Hex << "]\tklog: " << msgCopy);
  else if (prio == LOG_NOTICE || prio == LOG_INFO)
    NOTICE("[" << Dec << id << Hex << "]\tklog: " << msgCopy);
#if DEBUGGER
  else
    NOTICE("[" << Dec << id << Hex << "]\tklog: " << msgCopy);
#endif
  return 0;
}

EXPORTED_PUBLIC int pedigree_reboot() {
  // Are we superuser?
  User* pUser = Processor::information().getCurrentThread()->getParent()->getUser();
  if (pUser->getId()) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }

  if (!SyscallManager::instance().requestReboot()) {
    FATAL("Reboot was not dispatched.");
  }
  return 0;
}

int posix_uname(struct utsname* n) {
  struct utsname result = {};

  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());

  StringCopy(result.sysname, "Pedigree");

  if (pSubsystem->getAbi() == PosixSubsystem::LinuxAbi) {
    // Lie a bit to Linux ABI callers.
    StringCopy(result.release, "2.6.32-generic");
    StringCopy(result.version, g_pBuildRevision);
  } else {
    StringCopy(result.release, g_pBuildRevision);
    StringCopy(result.version, "Foster");
  }

  StringCopy(result.machine, g_pBuildTarget);

  /// \todo: better handle node name
  StringCopy(result.nodename, "pedigree");
  if (!PosixSubsystem::copyToUser(n, &result, sizeof(result))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return 0;
}

int posix_prctl(int option, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
  NOTICE("prctl(" << Hex << option << ", " << arg2 << ", " << arg3 << ", " << arg4 << ", " << arg5
                  << ")");

  Thread* thread = Processor::information().getCurrentThread();
  if (option == LINUX_PR_SET_NAME) {
    String requested;
    const PosixSubsystem::UserStringResult result = PosixSubsystem::copyUserString(
        reinterpret_cast<const char*>(arg2), requested, LINUX_TASK_NAME_LENGTH);
    if (result == PosixSubsystem::UserStringBadAddress) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }

    // Linux task names are a 16-byte field including the terminator. Direct
    // prctl callers are truncated; pthread_setname_np applies its own ERANGE.
    const size_t length = requested.length() < (LINUX_TASK_NAME_LENGTH - 1)
                              ? requested.length()
                              : (LINUX_TASK_NAME_LENGTH - 1);
    thread->setName(String(requested.cstr(), length, true));
    return 0;
  }

  if (option == LINUX_PR_GET_NAME) {
    char name[LINUX_TASK_NAME_LENGTH] = {};
    const String& current = thread->getName();
    const size_t length = current.length() < (LINUX_TASK_NAME_LENGTH - 1)
                              ? current.length()
                              : (LINUX_TASK_NAME_LENGTH - 1);
    MemoryCopy(name, current.cstr(), length);
    if (!PosixSubsystem::copyToUser(reinterpret_cast<void*>(arg2), name, sizeof(name))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    return 0;
  }

  SYSCALL_ERROR(InvalidArgument);
  return -1;
}

int posix_arch_prctl(int code, unsigned long addr) {
  Thread* current = Processor::information().getCurrentThread();
  switch (code) {
    case ARCH_SET_FS:
      if (addr >= current->getParent()->getAddressSpace()->getKernelStart()
#if X64
          || addr >= 0x0000800000000000ULL
#endif
      ) {
        SYSCALL_ERROR(NotEnoughPermissions);
        return -1;
      }
      current->setTlsBase(addr);
      break;

    case ARCH_GET_FS: {
      const unsigned long base = current->getTlsBase();
      if (!PosixSubsystem::copyToUser(reinterpret_cast<void*>(addr), &base, sizeof(base))) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
      break;
    }

    default:
      SYSCALL_ERROR(InvalidArgument);
      return -1;
  }
  return 0;
}

int posix_pause() {
  SC_NOTICE("pause");

  Processor::information().getCurrentThread()->waitForEvent();

  SYSCALL_ERROR(Interrupted);
  return -1;
}

int posix_setgroups(size_t size, const gid_t* list) {
  SC_NOTICE("setgroups(" << size << ", " << list << ")");
  if (size > NGROUPS_MAX) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  gid_t snapshot[NGROUPS_MAX] = {};
  if (!PosixSubsystem::copyFromUser(snapshot, list, size, sizeof(gid_t))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  PosixProcess* pProcess = getPosixProcess();
  if (!pProcess) {
    return -1;
  }

  Vector<int64_t> newGroups;
  for (size_t i = 0; i < size; ++i) {
    newGroups.pushBack(snapshot[i]);
  }
  pProcess->setSupplementalGroupIds(newGroups);
  return 0;
}

int posix_getgroups(size_t size, gid_t* list) {
  SC_NOTICE("getgroups(" << size << ", " << list << ")");
  if (size > INT_MAX) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  PosixProcess* pProcess = getPosixProcess();
  if (!pProcess) {
    return -1;
  }
  Vector<int64_t> groups;
  pProcess->getSupplementalGroupIds(groups);
  if (!size) {
    return groups.count();
  }
  if (size < groups.count()) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  Vector<gid_t> snapshot;
  for (size_t i = 0; i < groups.count(); ++i) {
    snapshot.pushBack(static_cast<gid_t>(groups[i]));
  }
  if (!PosixSubsystem::copyToUser(list, snapshot.count() ? &snapshot[0] : nullptr, snapshot.count(),
                                  sizeof(gid_t))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return groups.count();
}

namespace {
bool getRlimitValue(int resource, struct rlimit& result) {
  result = {};
  switch (resource) {
    case RLIMIT_CPU:
      result.rlim_cur = result.rlim_max = RLIM_INFINITY;
      break;
    case RLIMIT_FSIZE:
      result.rlim_cur = result.rlim_max = RLIM_INFINITY;
      break;
    case RLIMIT_DATA:
      result.rlim_cur = result.rlim_max = RLIM_INFINITY;
      break;
    case RLIMIT_STACK:
      result.rlim_cur = result.rlim_max = RLIM_INFINITY;
      break;
    case RLIMIT_CORE:
      result.rlim_cur = 0;
      result.rlim_max = RLIM_INFINITY;
      break;
    case RLIMIT_RSS:
      result.rlim_cur = result.rlim_max = 1ULL << 48ULL;
      break;
    case RLIMIT_NPROC:
      result.rlim_cur = result.rlim_max = RLIM_INFINITY;
      break;
    case RLIMIT_NOFILE:
      result.rlim_cur = result.rlim_max = 16384;
      break;
    case RLIMIT_MEMLOCK:
      result.rlim_cur = result.rlim_max = 1ULL << 24ULL;
      break;
    case RLIMIT_AS:
      result.rlim_cur = result.rlim_max = 1ULL << 48ULL;
      break;
    case RLIMIT_LOCKS:
      result.rlim_cur = result.rlim_max = 1024;
      break;
    case RLIMIT_SIGPENDING:
      result.rlim_cur = result.rlim_max = 16;
      break;
    case RLIMIT_MSGQUEUE:
      result.rlim_cur = result.rlim_max = 0x100000;
      break;
    case RLIMIT_NICE:
      result.rlim_cur = result.rlim_max = 1;
      break;
    case RLIMIT_RTPRIO:
      result.rlim_cur = result.rlim_max = 0;
      break;
#ifdef RLIMIT_RTTIME
    case RLIMIT_RTTIME:
      result.rlim_cur = result.rlim_max = RLIM_INFINITY;
      break;
#endif
    default:
      SYSCALL_ERROR(InvalidArgument);
      return false;
  }

  return true;
}
}  // namespace

int posix_getrlimit(int resource, struct rlimit* rlim) {
  SC_NOTICE("getrlimit(" << Dec << resource << ")");

  struct rlimit result = {};
  if (!getRlimitValue(resource, result)) {
    SC_NOTICE(" -> unsupported resource");
    return -1;
  }

  if (!PosixSubsystem::copyToUser(rlim, &result, sizeof(result))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  SC_NOTICE(" -> cur = " << result.rlim_cur);
  SC_NOTICE(" -> max = " << result.rlim_max);
  return 0;
}

int posix_setrlimit(int resource, const struct rlimit* rlim) {
  SC_NOTICE("setrlimit(" << Dec << resource << ")");

  struct rlimit current = {};
  if (!getRlimitValue(resource, current)) {
    return -1;
  }

  (void)rlim;
  SYSCALL_ERROR(Unimplemented);
  return -1;
}

int posix_prlimit64(int pid, int resource, const LinuxRlimit64* newLimit, LinuxRlimit64* oldLimit) {
  SC_NOTICE("prlimit64(" << Dec << pid << ", " << resource << ")");

  Process* current = Processor::information().getCurrentThread()->getParent();
  Scheduler::ProcessLease targetLease;
  if (pid < 0) {
    SYSCALL_ERROR(NoSuchProcess);
    return -1;
  }
  if (pid && static_cast<size_t>(pid) != current->getId()) {
    if (!Scheduler::instance().acquireProcessById(targetLease, static_cast<size_t>(pid)) ||
        targetLease->getType() != Process::Posix) {
      SYSCALL_ERROR(NoSuchProcess);
      return -1;
    }
    // Reported limits are not stored per-process yet, so returning the
    // caller's synthetic values for another process would be misleading.
    SYSCALL_ERROR(Unimplemented);
    return -1;
  }

  struct rlimit currentLimit = {};
  if (!getRlimitValue(resource, currentLimit)) {
    return -1;
  }

  // Limits are not yet stored or enforced per-process. Refuse mutation instead
  // of claiming success while still supporting the query used by modern libc.
  if (newLimit) {
    SYSCALL_ERROR(Unimplemented);
    return -1;
  }

  if (oldLimit) {
    const LinuxRlimit64 result = {static_cast<uint64_t>(currentLimit.rlim_cur),
                                  static_cast<uint64_t>(currentLimit.rlim_max)};
    if (!PosixSubsystem::copyToUser(oldLimit, &result, sizeof(result))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
  }

  return 0;
}

int posix_membarrier(int command, unsigned int flags, int cpuId) {
  SC_NOTICE("membarrier(" << Dec << command << ", " << flags << ", " << cpuId << ")");

  // A zero query result truthfully advertises that no membarrier commands are
  // available. Callers can then select their ordinary synchronization path.
  if (command == 0 && flags == 0) {
    return 0;
  }

  SYSCALL_ERROR(InvalidArgument);
  return -1;
}

int posix_getpriority(int which, int who, bool linuxAbi) {
  SC_NOTICE("getpriority(" << which << ", " << Dec << who << ")");
  if (which != PRIO_PROCESS && which != PRIO_PGRP && which != PRIO_USER) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (who < 0) {
    SYSCALL_ERROR(NoSuchProcess);
    return -1;
  }

  PosixProcess* caller = getPosixProcess();
  if (!caller) {
    SYSCALL_ERROR(NoSuchProcess);
    return -1;
  }

  // POSIX nice values are not mutable yet. Linux returns 20 minus nice,
  // while the native POSIX service exposes the public value directly.
  const int priority = linuxAbi ? 20 : 0;
  if (which == PRIO_PROCESS) {
    if (!who || static_cast<size_t>(who) == caller->getId()) {
      return priority;
    }
    Scheduler::ProcessLease candidate;
    if (Scheduler::instance().acquireProcessById(candidate, static_cast<size_t>(who)) &&
        candidate->getType() == Process::Posix && candidate->getState() != Process::Reaped) {
      return priority;
    }
    SYSCALL_ERROR(NoSuchProcess);
    return -1;
  }

  if (which == PRIO_USER) {
    if (!who || static_cast<int64_t>(who) == caller->getUserId()) {
      return priority;
    }
    // The scheduler has no stable process snapshot for another user's set.
    // Index-based scans can miss a live match when an earlier process exits.
    SYSCALL_ERROR(Unimplemented);
    return -1;
  }

  size_t callerGroup = 0;
  const bool hasCallerGroup = caller->getProcessGroupId(callerGroup);
  if (hasCallerGroup && (!who || static_cast<size_t>(who) == callerGroup)) {
    return priority;
  }
  if (who) {
    SYSCALL_ERROR(Unimplemented);
    return -1;
  }

  SYSCALL_ERROR(NoSuchProcess);
  return -1;
}

int posix_setpriority(int which, int who, int prio) {
  /// \todo could do more with this
  SC_NOTICE("setpriority(" << which << ", " << Dec << who << ", " << prio << ")");
  return 0;
}

int posix_setreuid(uid_t ruid, uid_t euid) {
  SC_NOTICE("setreuid(" << ruid << ", " << euid << ")");
  return posix_setresuid(ruid, euid, -1);
}

int posix_setregid(gid_t rgid, gid_t egid) {
  SC_NOTICE("setregid(" << rgid << ", " << egid << ")");
  return posix_setresgid(rgid, egid, -1);
}

int posix_setresuid(uid_t ruid, uid_t euid, uid_t suid) {
  SC_NOTICE("setresuid(" << ruid << ", " << euid << ", " << suid << ")");

  PosixProcess* pProcess = getPosixProcess();
  if (!pProcess) {
    /// \todo errno
    return -1;
  }

  if (ruid != static_cast<uid_t>(-1)) {
    pProcess->setUserId(ruid);
  }
  if (euid != static_cast<uid_t>(-1)) {
    pProcess->setEffectiveUserId(euid);
  }
  if (suid != static_cast<uid_t>(-1)) {
    pProcess->setSavedUserId(suid);
  }

  return 0;
}

int posix_setresgid(gid_t rgid, gid_t egid, gid_t sgid) {
  SC_NOTICE("setresgid(" << rgid << ", " << egid << ", " << sgid << ")");

  PosixProcess* pProcess = getPosixProcess();
  if (!pProcess) {
    /// \todo errno
    return -1;
  }

  if (rgid != static_cast<gid_t>(-1)) {
    pProcess->setGroupId(rgid);
  }
  if (egid != static_cast<gid_t>(-1)) {
    pProcess->setEffectiveGroupId(egid);
  }
  if (sgid != static_cast<gid_t>(-1)) {
    pProcess->setSavedGroupId(sgid);
  }

  return 0;
}

int posix_getresuid(uid_t* ruid, uid_t* euid, uid_t* suid) {
  Process* process = Processor::information().getCurrentThread()->getParent();
  const uid_t real = process->getUserId();
  const uid_t effective = process->getEffectiveUserId();
  PosixProcess* posix = getPosixProcess();
  const uid_t saved = posix ? posix->getSavedUserId() : 0;
  if ((ruid && !PosixSubsystem::copyToUser(ruid, &real, sizeof(real))) ||
      (euid && !PosixSubsystem::copyToUser(euid, &effective, sizeof(effective))) ||
      (suid && posix && !PosixSubsystem::copyToUser(suid, &saved, sizeof(saved)))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return 0;
}

int posix_getresgid(gid_t* rgid, gid_t* egid, gid_t* sgid) {
  Process* process = Processor::information().getCurrentThread()->getParent();
  const gid_t real = process->getGroupId();
  const gid_t effective = process->getEffectiveGroupId();
  PosixProcess* posix = getPosixProcess();
  const gid_t saved = posix ? posix->getSavedGroupId() : 0;
  if ((rgid && !PosixSubsystem::copyToUser(rgid, &real, sizeof(real))) ||
      (egid && !PosixSubsystem::copyToUser(egid, &effective, sizeof(effective))) ||
      (sgid && posix && !PosixSubsystem::copyToUser(sgid, &saved, sizeof(saved)))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return 0;
}

int posix_get_robust_list(int pid, struct robust_list_head** head_ptr, size_t* len_ptr,
                          bool linuxAbi) {
  SC_NOTICE("get_robust_list");
  Thread* current = Processor::information().getCurrentThread();
  Thread* target = current;
  Process::ThreadLease targetLease;
  const size_t currentId = linuxAbi ? current->getTaskId() : current->getId();
  if (pid < 0 || (pid && static_cast<size_t>(pid) != currentId &&
                  !(linuxAbi ? Scheduler::instance().acquireThreadByTaskId(targetLease, pid)
                             : current->getParent()->acquireThreadById(targetLease, pid)))) {
    SYSCALL_ERROR(NoSuchProcess);
    return -1;
  }
  if (targetLease) {
    target = targetLease.get();
  }
  if (target->getParent() != current->getParent()) {
    // Cross-process inspection needs a ptrace access policy which the
    // subsystem does not yet implement.
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }

  auto* head = reinterpret_cast<struct robust_list_head*>(target->getRobustList());
  const size_t length = 3 * sizeof(uintptr_t);
  if (!PosixSubsystem::copyToUser(head_ptr, &head, sizeof(head)) ||
      !PosixSubsystem::copyToUser(len_ptr, &length, sizeof(length))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return 0;
}

int posix_set_robust_list(struct robust_list_head* head, size_t len, bool linuxAbi) {
  SC_NOTICE("set_robust_list");

  if (len != 3 * sizeof(uintptr_t)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  // The list is mutable userspace state. Linux registers its address without
  // touching it; exit processing must bound and validate each later access.
  Thread* current = Processor::information().getCurrentThread();
  current->setRobustList(reinterpret_cast<uintptr_t>(head),
                         linuxAbi ? current->getTaskId() : current->getId());

  return 0;
}

int posix_ioperm(unsigned long from, unsigned long num, int turn_on) {
  SC_NOTICE("ioperm(" << from << ", " << num << ", " << turn_on << ")");

  /// \todo set the io permissions bitmap properly and use this to enable
  /// stuff
  return 0;
}

int posix_iopl(int level) {
  SC_NOTICE("iopl(" << level << ")");
  return 0;
}

#undef SC_NOTICE
#define SC_NOTICE(x)

namespace {
constexpr Time::Timestamp MaximumLinuxTimerNanoseconds = 0x7FFFFFFFFFFFFFFFULL;

IntervalTimer* selectIntervalTimer(PosixProcess* process, int which) {
  switch (which) {
    case ITIMER_REAL:
      return &process->getRealIntervalTimer();
    case ITIMER_VIRTUAL:
      return &process->getVirtualIntervalTimer();
    case ITIMER_PROF:
      return &process->getProfileIntervalTimer();
    default:
      return nullptr;
  }
}

bool validIntervalTimeval(const struct timeval& value) {
  return value.tv_sec >= 0 && value.tv_usec >= 0 && value.tv_usec < 1000000;
}

Time::Timestamp intervalTimevalToNanoseconds(const struct timeval& value) {
  const Time::Timestamp microseconds =
      static_cast<Time::Timestamp>(value.tv_usec) * Time::Multiplier::Microsecond;
  const Time::Timestamp seconds = static_cast<Time::Timestamp>(value.tv_sec);
  if (seconds >= MaximumLinuxTimerNanoseconds / Time::Multiplier::Second) {
    return MaximumLinuxTimerNanoseconds;
  }
  return seconds * Time::Multiplier::Second + microseconds;
}

struct itimerval intervalTimerToUser(Time::Timestamp interval, Time::Timestamp value) {
  struct itimerval result = {};
  result.it_interval.tv_sec = interval / Time::Multiplier::Second;
  result.it_interval.tv_usec =
      (interval % Time::Multiplier::Second) / Time::Multiplier::Microsecond;
  result.it_value.tv_sec = value / Time::Multiplier::Second;
  result.it_value.tv_usec = (value % Time::Multiplier::Second) / Time::Multiplier::Microsecond;
  return result;
}
}  // namespace

int posix_getitimer(int which, struct itimerval* curr_value) {
  SC_NOTICE("posix_getitimer(" << which << ", " << curr_value << ")");

  Thread* currentThread = Processor::information().getCurrentThread();
  PosixProcess* pProcess = static_cast<PosixProcess*>(currentThread->getParent());

  Time::Timestamp interval = 0;
  Time::Timestamp value = 0;

  IntervalTimer* itimer = selectIntervalTimer(pProcess, which);
  if (!itimer) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  if (which != ITIMER_REAL) {
    currentThread->trackTime(CpuTimeMode::Kernel);
  }
  itimer->getIntervalAndValue(interval, value);

  const struct itimerval result = intervalTimerToUser(interval, value);
  if (!PosixSubsystem::copyToUser(curr_value, &result, sizeof(result))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  SC_NOTICE(" -> period = " << Dec << result.it_interval.tv_sec << "s "
                            << result.it_interval.tv_usec << "us");
  SC_NOTICE(" -> value = " << Dec << result.it_value.tv_sec << "s " << result.it_value.tv_usec
                           << "us");

  return 0;
}

int posix_setitimer(int which, const struct itimerval* new_value, struct itimerval* old_value) {
  SC_NOTICE("posix_setitimer(" << which << ", " << new_value << ", " << old_value << ")");

  struct itimerval requested = {};
  if (new_value && !PosixSubsystem::copyFromUser(&requested, new_value, sizeof(requested))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (!validIntervalTimeval(requested.it_interval) || !validIntervalTimeval(requested.it_value)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  SC_NOTICE(" -> period = " << Dec << requested.it_interval.tv_sec << "s "
                            << requested.it_interval.tv_usec << "us");
  SC_NOTICE(" -> value = " << Dec << requested.it_value.tv_sec << "s " << requested.it_value.tv_usec
                           << "us");

  Thread* currentThread = Processor::information().getCurrentThread();
  PosixProcess* pProcess = static_cast<PosixProcess*>(currentThread->getParent());

  const Time::Timestamp interval = intervalTimevalToNanoseconds(requested.it_interval);
  const Time::Timestamp value = intervalTimevalToNanoseconds(requested.it_value);
  Time::Timestamp prevInterval = 0;
  Time::Timestamp prevValue = 0;

  IntervalTimer* itimer = selectIntervalTimer(pProcess, which);
  if (!itimer) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  if (which != ITIMER_REAL) {
    currentThread->trackTime(CpuTimeMode::Kernel);
  }
  itimer->setIntervalAndValue(interval, value, &prevInterval, &prevValue);

  if (old_value) {
    const struct itimerval previous = intervalTimerToUser(prevInterval, prevValue);
    if (!PosixSubsystem::copyToUser(old_value, &previous, sizeof(previous))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
  }

  return 0;
}

int posix_capget(void* hdrp, void* datap) {
  if (!getPosixProcess()) {
    return -1;
  }
  cap_header header = {};
  if (!PosixSubsystem::copyFromUser(&header, hdrp, sizeof(header))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (header.version != _LINUX_CAPABILITY_VERSION_1) {
    const uint32_t version = _LINUX_CAPABILITY_VERSION_1;
    if (!PosixSubsystem::copyToUser(hdrp, &version, sizeof(version))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (datap) {
    const cap_data data = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
    if (!PosixSubsystem::copyToUser(datap, &data, sizeof(data))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
  }
  return 0;
}

int posix_capset(void* hdrp, const void* datap) {
  cap_header header = {};
  if (!PosixSubsystem::copyFromUser(&header, hdrp, sizeof(header))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (header.version != _LINUX_CAPABILITY_VERSION_1) {
    const uint32_t version = _LINUX_CAPABILITY_VERSION_1;
    if (!PosixSubsystem::copyToUser(hdrp, &version, sizeof(version))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  cap_data data = {};
  if (!PosixSubsystem::copyFromUser(&data, datap, sizeof(data))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  // Capability policy remains the existing all-granted no-op contract.
  return 0;
}
