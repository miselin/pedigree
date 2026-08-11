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
#include "pedigree/kernel/linker/KernelElf.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/SyscallManager.h"
#include "pedigree/kernel/utilities/ZombieQueue.h"

#include "DevFs.h"
#include "PosixSubsystem.h"
#include "PosixSyscallManager.h"
#include "ProcFs.h"
#include "UnixFilesystem.h"
#include "modules/Module.h"
#include "modules/system/ramfs/RamFs.h"
#include "modules/system/vfs/VFS.h"
#include "net-syscalls.h"
#include "signal-syscalls.h"
#include "system-syscalls.h"

static PosixSyscallManager g_PosixSyscallManager;

UnixFilesystem* g_pUnixFilesystem = 0;
static RamFs* g_pRunFilesystem = 0;

DevFs* g_pDevFs = 0;
static ProcFs* g_pProcFs = 0;

enum class PosixTerminalLifetimeState {
  Unowned,
  HookOwned,
  Quiesced,
};

static PosixTerminalLifetimeState g_PosixTerminalLifetime = PosixTerminalLifetimeState::Unowned;

static bool retireUnownedSyscallRegistrations(PosixSyscallManager& manager) {
  return manager.shutdown();
}

#if THREADS
namespace {
struct TerminalDrainStats {
  TerminalDrainStats()
      : syntheticOwners(0), zeroThreadProcesses(0), threadedProcesses(0), zombies(0) {}

  size_t syntheticOwners;
  size_t zeroThreadProcesses;
  size_t threadedProcesses;
  size_t zombies;
};

int terminalProcessExit(void* parameter) {
  PosixSubsystem* subsystem = reinterpret_cast<PosixSubsystem*>(parameter);
  subsystem->exit(0);
  return 0;
}

void drainPosixProcesses(TerminalDrainStats& stats) {
  while (true) {
    Scheduler::ProcessLease process;
    if (!Scheduler::instance().acquireFirstProcessOfType(process, Process::Posix)) {
      break;
    }

    const Process::ProcessState state = process->getState();
    if (state == Process::Active || state == Process::Suspended) {
      // A kernel-entry peer can execute its ordinary entry before it reaches
      // any deferred-exit boundary, even if it is only Created or Ready.
      // Reserve a dedicated delayed owner before construction; competing
      // exits then take only the thread path until this owner is installed.
      Process::TerminalOwnerReservation reservation = process->reserveTerminalOwner();
      if (!reservation) {
        const Process::ProcessState currentState = process->getState();
        if (currentState == Process::Active || currentState == Process::Suspended) {
          FATAL("POSIX shutdown could not reserve its active terminal exit owner");
        }
        ++stats.zombies;
      } else {
        if (process->getNumThreads()) {
          ++stats.threadedProcesses;
        } else {
          ++stats.zeroThreadProcesses;
        }

        PosixSubsystem* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
        if (!subsystem) {
          FATAL("POSIX shutdown found a process without its subsystem");
        }
        Thread* ownerIdentity =
            new Thread(process.get(), terminalProcessExit, subsystem, nullptr, false, true, true);
        ownerIdentity->setName("POSIX terminal exit owner");
        reservation.install(ownerIdentity);

        Process::ThreadLease owner;
        if (!process->acquireThread(owner, ownerIdentity)) {
          FATAL("POSIX shutdown lost its reserved terminal exit owner");
        }
        if (!owner->start()) {
          FATAL("POSIX shutdown could not start its reserved terminal exit owner");
        }
        ++stats.syntheticOwners;
        owner.reset();
      }
    } else {
      ++stats.zombies;
    }

    if (!process->waitUntilTerminationReapableForTerminalCoordinator()) {
      FATAL("POSIX shutdown attempted to reap its own exit owner");
    }

    Process* processIdentity = process.get();
    Process::ReaperClaim reaper = process->tryClaimReaper();
    if (reaper) {
      reaper.publish();
    }
    Scheduler::instance().waitUntilProcessRemoved(processIdentity);
    process.reset();

    // Scheduler removal precedes the derived destructor. Draining the queue
    // is the completion barrier that keeps POSIX text mapped until every
    // IntervalTimer and PosixSubsystem destructor has returned.
    if (!ZombieQueue::instance().drain()) {
      FATAL("POSIX shutdown could not drain process destruction");
    }
  }

  // Also covers a POSIX reaper which removed its process before the final
  // enumeration pass but is still running the derived destructor.
  if (!ZombieQueue::instance().drain()) {
    FATAL("POSIX shutdown could not complete its final process drain");
  }
}
}  // namespace
#endif

static bool terminalQuiesce() {
  if (g_PosixTerminalLifetime == PosixTerminalLifetimeState::Quiesced) {
    return true;
  }
  if (g_PosixTerminalLifetime != PosixTerminalLifetimeState::HookOwned) {
    return false;
  }
  if (!Processor::information().getCurrentThread() ||
      Processor::executionContext() != ExecutionContext::WaitableThread) {
    return false;
  }

  // New callbacks must be rejected before process termination starts, but
  // admitted blocking callbacks retain their registration and module text
  // until process teardown has made them return.
  if (!g_PosixSyscallManager.closeAdmission()) {
    return false;
  }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  NOTICE("HOSTED-POSIX-SHUTDOWN: PHASE syscall-admission-closed");
#endif

#if THREADS
  TerminalDrainStats stats;
  drainPosixProcesses(stats);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  NOTICE("HOSTED-POSIX-SHUTDOWN: PHASE initial-process-drain-complete");
#endif
#endif

  if (!g_PosixSyscallManager.finishShutdown()) {
    return false;
  }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  NOTICE("HOSTED-POSIX-SHUTDOWN: PHASE syscall-handler-drain-complete");
#endif

#if THREADS
  // An already-admitted fork or clone can publish after the first empty
  // scheduler scan. Handler retirement closes that final publication window.
  drainPosixProcesses(stats);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  NOTICE("HOSTED-POSIX-SHUTDOWN: PHASE final-process-drain-complete");
#endif
#endif

  g_PosixTerminalLifetime = PosixTerminalLifetimeState::Quiesced;

#if THREADS && HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  NOTICE("HOSTED-POSIX-SHUTDOWN: PASS terminal-drain synthetic="
         << stats.syntheticOwners << " zero-thread=" << stats.zeroThreadProcesses
         << " threaded=" << stats.threadedProcesses << " zombies=" << stats.zombies);
#endif

  return true;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
extern "C" EXPORTED_PUBLIC bool posixDuplicateInitRollbackPreservesProcessForTest(
    Process* processIdentity) {
  PosixSyscallManager duplicate;
  const bool duplicateRejected = !duplicate.initialise();
  if (!retireUnownedSyscallRegistrations(duplicate)) {
    return false;
  }

  Scheduler::ProcessLease process;
  return duplicateRejected && Scheduler::instance().acquireProcess(process, processIdentity) &&
         process->getType() == Process::Posix && process->getState() == Process::Active;
}
#endif

static bool init() {
  if (!g_PosixSyscallManager.initialise()) {
    return false;
  }

  g_pDevFs = new DevFs();
  g_pDevFs->initialise(0);

  g_pProcFs = new ProcFs();
  g_pProcFs->initialise(0);

  g_pUnixFilesystem = new UnixFilesystem();

  g_pRunFilesystem = new RamFs;
  g_pRunFilesystem->initialise(0);
  VFS::instance().registerFilesystem(g_pRunFilesystem, String("posix-runtime"));
  VFS::instance().registerFilesystem(g_pUnixFilesystem, String("unix"));
  VFS::instance().registerFilesystem(g_pDevFs, String("dev"));
  VFS::instance().registerFilesystem(g_pProcFs, String("proc"));

  Filesystem* scratchfs = VFS::instance().getFilesystemAt(String("/media/scratch"));

  // Keep the socket namespace separate from ordinary runtime files while
  // exposing it at a conventional path.
  VFS::instance().createDirectory(String("/media/posix-runtime/sockets"), 0755);

  // Expose the system filesystems at their conventional FHS locations. Their
  // primary mount records remain visible under /media.
  struct reparse {
    String path;
    File* target;
  } reparses[] = {
      {String("/dev"), g_pDevFs->getRoot()},
      {String("/run"), g_pRunFilesystem->getRoot()},
      {String("/run/sockets"), g_pUnixFilesystem->getRoot()},
      {String("/var/run"), g_pRunFilesystem->getRoot()},
      {String("/proc"), g_pProcFs->getRoot()},
      {String("/tmp"), scratchfs ? scratchfs->getRoot() : 0},
  };

  for (auto& p : reparses) {
    if (!p.target) {
      continue;
    }

    File* point = VFS::instance().find(p.path);
    if (point && point->isDirectory()) {
      Directory* pDir = Directory::fromFile(point);
      pDir->setReparsePoint(Directory::fromFile(p.target));
    }
  }

  if (!KernelElf::instance().registerTerminalQuiesce(&init, &terminalQuiesce)) {
    return false;
  }
  g_PosixTerminalLifetime = PosixTerminalLifetimeState::HookOwned;
  return true;
}

static void destroy() {
  if (g_PosixTerminalLifetime == PosixTerminalLifetimeState::HookOwned) {
    if (!KernelElf::instance().unregisterTerminalQuiesce(&init, &terminalQuiesce)) {
      FATAL("POSIX terminal quiesce ownership could not be released safely.");
    }
    // Retain HookOwned across unregister: this module lifetime still owns all
    // live POSIX processes and must perform the same terminal drain itself.
    if (!terminalQuiesce()) {
      FATAL("POSIX syscall handlers could not be retired safely.");
    }
  } else if (g_PosixTerminalLifetime == PosixTerminalLifetimeState::Unowned) {
    // Failed or duplicate initialisation never acquired global POSIX lifetime
    // ownership and therefore must not terminate another module's processes.
    if (!retireUnownedSyscallRegistrations(g_PosixSyscallManager)) {
      FATAL("Partial POSIX syscall handlers could not be retired safely.");
    }
  }

  if (g_pRunFilesystem || g_pUnixFilesystem || g_pProcFs || g_pDevFs) {
    const char* reparsePaths[] = {"/run/sockets", "/var/run", "/run", "/proc", "/dev"};
    for (const char* path : reparsePaths) {
      File* point = VFS::instance().find(String(path));
      if (point && point->isDirectory()) {
        Directory::fromFile(point)->setReparsePoint(nullptr);
      }
    }
  }

  if (g_pProcFs) {
    VFS::instance().unregisterFilesystem(g_pProcFs, false);
  }
  if (g_pDevFs) {
    VFS::instance().unregisterFilesystem(g_pDevFs, false);
  }
  if (g_pUnixFilesystem) {
    VFS::instance().unregisterFilesystem(g_pUnixFilesystem, false);
  }
  if (g_pRunFilesystem) {
    VFS::instance().unregisterFilesystem(g_pRunFilesystem, false);
  }

  delete g_pRunFilesystem;
  delete g_pUnixFilesystem;
  delete g_pProcFs;
  delete g_pDevFs;
  g_pRunFilesystem = nullptr;
  g_pUnixFilesystem = nullptr;
  g_pProcFs = nullptr;
  g_pDevFs = nullptr;
}

MODULE_INFO_RUNTIME_PINNED("posix", &init, &destroy, "console", "network-stack", "mountroot",
                           "ramfs", "lwip");
