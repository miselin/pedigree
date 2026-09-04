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
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Uninterruptible.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/SyscallManager.h"
#include "pedigree/kernel/syscallError.h"

#include "file-syscalls.h"
#include "linux-amd64-signal-abi.h"
#include "pthread-syscalls.h"
#include "signal-syscalls.h"
#include "system-syscalls.h"

#define MACHINE_FORWARD_DECL_ONLY
#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Timer.h"

#include <PosixProcess.h>
#include <PosixSubsystem.h>
#include <signal.h>
#include <time.h>

extern "C" {
extern void sigret_stub();
extern char sigret_stub_end;
}

static int doProcessKill(Process* p, int sig);
static int doThreadKill(Thread* p, int sig);

/// \todo These are ok initially, but it'll all have to change at some point

#define SIGNAL_HANDLER_EXIT(name, errcode)                                  \
  static void name(int) {                                                   \
    Processor::information().getCurrentThread()->deferProcessExit(errcode); \
  }
#define SIGNAL_HANDLER_EMPTY(name) \
  static void name(int s) {        \
    NOTICE("EMPTY handler.");      \
  }
#define SIGNAL_HANDLER_EXITMSG(name, errcode, msg)                          \
  static void name(int) {                                                   \
    Processor::setInterrupts(true);                                         \
    posix_write(1, msg, StringLength(msg), true);                           \
    Scheduler::instance().yield();                                          \
    Processor::information().getCurrentThread()->deferProcessExit(errcode); \
  }
#define SIGNAL_HANDLER_SUSPEND(name)                                             \
  static void name(int s) {                                                      \
    Process* pParent = Processor::information().getCurrentThread()->getParent(); \
    NOTICE("SUSPEND [pid=" << pParent->getId() << ", signal " << s << "]");      \
    pParent->suspend();                                                          \
  }
#define SIGNAL_HANDLER_RESUME(name)                                     \
  static void name(int s) {                                             \
    NOTICE("RESUME [signal " << s << "]");                              \
    Processor::information().getCurrentThread()->getParent()->resume(); \
  }

static char SSIGILL[] = "Illegal instruction.\n";
static char SSIGSEGV[] = "Segmentation fault.\n";
static char SSIGBUS[] = "Bus error.\n";
static char SSIGABRT[] = "Abort.\n";

SIGNAL_HANDLER_EXITMSG(sigabrt, SIGABRT, SSIGABRT)
SIGNAL_HANDLER_EXIT(sigalrm, SIGALRM)
SIGNAL_HANDLER_EXITMSG(sigbus, SIGBUS, SSIGBUS)
SIGNAL_HANDLER_EMPTY(sigchld)
SIGNAL_HANDLER_RESUME(sigcont)
SIGNAL_HANDLER_EXIT(sigfpe, SIGFPE)  // floating point exception signal
SIGNAL_HANDLER_EXIT(sighup, SIGHUP)
SIGNAL_HANDLER_EXITMSG(sigill, SIGILL, SSIGILL)
SIGNAL_HANDLER_EXIT(sigint, SIGINT)
SIGNAL_HANDLER_EXIT(sigkill, SIGKILL)
SIGNAL_HANDLER_EXIT(sigpipe, SIGPIPE)
SIGNAL_HANDLER_EXIT(sigquit, SIGQUIT)
SIGNAL_HANDLER_EXITMSG(sigsegv, SIGSEGV, SSIGSEGV)
SIGNAL_HANDLER_SUSPEND(sigstop)
SIGNAL_HANDLER_EXIT(sigterm, SIGTERM)
SIGNAL_HANDLER_SUSPEND(sigtstp)  // terminal stop
SIGNAL_HANDLER_SUSPEND(sigttin)  // background process attempts read
SIGNAL_HANDLER_SUSPEND(sigttou)  // background process attempts write
SIGNAL_HANDLER_EMPTY(sigusr1)
SIGNAL_HANDLER_EMPTY(sigusr2)
SIGNAL_HANDLER_EMPTY(sigurg)  // high bandwdith data available at a sockeet

SIGNAL_HANDLER_EMPTY(sigign);

static _sig_func_ptr default_sig_handlers[32] = {
    sigign,   // 0
    sighup,   // SIGHUP
    sigint,   // SIGINT
    sigquit,  // SIGQUIT
    sigill,   // SIGILL
    sigign,   // SIGTRAP
    sigabrt,  // SIGABRT
    sigbus,   // SIGBUS
    sigfpe,   // SIGFPE
    sigkill,  // SIGKILL
    sigusr1,  // SIGUSR1
    sigsegv,  // SIGSEGV
    sigusr2,  // SIGUSR2
    sigpipe,  // SIGPIPE
    sigalrm,  // SIGALRM
    sigterm,  // SIGTERM
    sigign,   // SIGSTKFLT
    sigchld,  // SIGCHLD
    sigcont,  // SIGCONT
    sigstop,  // SIGSTOP
    sigtstp,  // SIGTSTP
    sigttin,  // SIGTTIN
    sigttou,  // SIGTTOU
    sigurg,   // SIGURG
    sigign,   // SIGXCPU
    sigign,   // SIGXFSZ
    sigign,   // SIGVTALRM
    sigign,   // SIGWINCH
    sigign,   // SIGIO
    sigign,   // SIGPOLL
    sigign,   // SIGPWR
    sigign,   // SIGSYS
};

static int posix_sigaction_impl(int sig, const struct sigaction* act, struct sigaction* oact) {
  Thread* pThread = Processor::information().getCurrentThread();
  Process* pProcess = pThread->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("posix_sigaction: no subsystem");
    return -1;
  }

  // sanity and safety checks
  if ((sig <= 0) || (sig >= 32) || (sig == SIGKILL || sig == SIGSTOP)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  sig %= 32;

  uintptr_t newHandler = 0;
  int handlerType = 1;
  if (act) {
    newHandler = reinterpret_cast<uintptr_t>(act->sa_handler);
    if (newHandler == 0) {
      SG_NOTICE(" + SIG_DFL");
      newHandler = reinterpret_cast<uintptr_t>(default_sig_handlers[sig]);
      handlerType = 1;
    } else if (newHandler == 1) {
      SG_NOTICE(" + SIG_IGN");
      newHandler = reinterpret_cast<uintptr_t>(sigign);
      handlerType = 2;
    } else if (newHandler == static_cast<uintptr_t>(-1)) {
      SG_NOTICE(" + Invalid");
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    } else {
      handlerType = 0;
      if (!PosixSubsystem::checkAddress(newHandler, 1, PosixSubsystem::SafeExecute)) {
        SG_NOTICE(" + Handler is not executable userspace memory");
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
    }
  }

  // Store the old signal handler information only after the replacement has
  // passed validation.
  if (oact) {
    PosixSubsystem::SignalDisposition oldDisposition;
    ByteSet(oact, 0, sizeof(struct sigaction));
    if (pSubsystem->getSignalDisposition(sig, oldDisposition)) {
      oact->sa_flags = oldDisposition.flags;
      oact->sa_restorer = reinterpret_cast<void (*)()>(oldDisposition.restorer);
      MemoryCopy(&oact->sa_mask, &oldDisposition.signalMask, sizeof(oldDisposition.signalMask));
      if (oldDisposition.type == 0)
        oact->sa_handler = reinterpret_cast<void (*)(int)>(oldDisposition.handler);
      else if (oldDisposition.type == 1)
        oact->sa_handler = reinterpret_cast<void (*)(int)>(0);
      else if (oldDisposition.type == 2)
        oact->sa_handler = reinterpret_cast<void (*)(int)>(1);
    }
  }

  // Publish the validated replacement disposition.
  if (act) {
    PosixSubsystem::SignalHandler* sigHandler = new PosixSubsystem::SignalHandler;
    sigHandler->flags = act->sa_flags;
    sigHandler->restorer = reinterpret_cast<uintptr_t>(act->sa_restorer);
    sigHandler->type = handlerType;
    MemoryCopy(&sigHandler->sigMask, &act->sa_mask, sizeof(sigHandler->sigMask));

    sigHandler->pEvent = new SignalEvent(
        newHandler, static_cast<size_t>(sig), ~0UL, sigHandler->sigMask,
        !(sigHandler->flags & SA_NODEFER), false,
        handlerType == 0 ? Event::HandlerPrivilege::User : Event::HandlerPrivilege::Kernel);
    SG_NOTICE("Creating the event (" << reinterpret_cast<uintptr_t>(sigHandler->pEvent) << ").");
    pSubsystem->setSignalHandler(sig, sigHandler);
  } else if (!oact) {
    // no valid arguments!
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  return 0;
}

int posix_sigaction(int sig, const struct sigaction* act, struct sigaction* oact) {
  SG_NOTICE("sigaction(" << Dec << sig << Hex << ", " << reinterpret_cast<uintptr_t>(act) << ", "
                         << reinterpret_cast<uintptr_t>(oact) << ")");
  struct sigaction requested = {};
  if ((act && !PosixSubsystem::copyFromUser(&requested, act, sizeof(requested))) ||
      (oact && !PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(oact), sizeof(*oact),
                                             PosixSubsystem::SafeWrite))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  struct sigaction previous = {};
  int result = posix_sigaction_impl(sig, act ? &requested : nullptr, oact ? &previous : nullptr);
  if ((result == 0) && oact && !PosixSubsystem::copyToUser(oact, &previous, sizeof(previous))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return result;
}

#if BITS_64
struct LinuxAmd64KernelSigaction {
  uint64_t handler;
  uint64_t flags;
  uint64_t restorer;
  uint64_t mask;
};

static_assert(sizeof(LinuxAmd64KernelSigaction) == 32,
              "Linux amd64 kernel sigaction layout must remain 32 bytes");
static_assert(sizeof(stack_t) == 24 && __builtin_offsetof(stack_t, ss_flags) == 8 &&
                  __builtin_offsetof(stack_t, ss_size) == 16,
              "Linux amd64 sigaltstack layout must remain 24 bytes");

int posix_linux_amd64_sigaction(int sig, const LinuxAmd64KernelSigaction* act,
                                LinuxAmd64KernelSigaction* oact) {
  SG_NOTICE("linux-amd64 sigaction(" << Dec << sig << Hex << ", "
                                     << reinterpret_cast<uintptr_t>(act) << ", "
                                     << reinterpret_cast<uintptr_t>(oact) << ")");
  LinuxAmd64KernelSigaction linuxAct = {};
  if ((act && !PosixSubsystem::copyFromUser(&linuxAct, act, sizeof(linuxAct))) ||
      (oact && !PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(oact),
                                             sizeof(LinuxAmd64KernelSigaction),
                                             PosixSubsystem::SafeWrite))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  if ((sig >= 32) && (sig <= 64)) {
    // Pedigree does not deliver Linux real-time signals yet. Reporting
    // their default disposition keeps Linux runtimes from treating the
    // smaller native signal namespace as an exec-time failure.
    if (oact) {
      LinuxAmd64KernelSigaction ignored = {};
      if (!PosixSubsystem::copyToUser(oact, &ignored, sizeof(ignored))) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
    }
    return 0;
  }

  struct sigaction nativeAct = {};
  const struct sigaction* nativeActPtr = nullptr;
  if (act) {
    nativeAct.sa_handler =
        reinterpret_cast<void (*)(int)>(static_cast<uintptr_t>(linuxAct.handler));
    nativeAct.sa_flags = static_cast<int>(linuxAct.flags);
    nativeAct.sa_restorer = reinterpret_cast<void (*)()>(static_cast<uintptr_t>(linuxAct.restorer));
    MemoryCopy(&nativeAct.sa_mask, &linuxAct.mask, sizeof(linuxAct.mask));
    nativeActPtr = &nativeAct;
  }

  struct sigaction nativeOld = {};
  struct sigaction* nativeOldPtr = oact ? &nativeOld : nullptr;
  int result = posix_sigaction_impl(sig, nativeActPtr, nativeOldPtr);
  if ((result == 0) && oact) {
    LinuxAmd64KernelSigaction linuxOld = {};
    linuxOld.handler = reinterpret_cast<uintptr_t>(nativeOld.sa_handler);
    linuxOld.flags = static_cast<uint64_t>(static_cast<uint32_t>(nativeOld.sa_flags));
    linuxOld.restorer = reinterpret_cast<uintptr_t>(nativeOld.sa_restorer);
    MemoryCopy(&linuxOld.mask, &nativeOld.sa_mask, sizeof(linuxOld.mask));
    if (!PosixSubsystem::copyToUser(oact, &linuxOld, sizeof(linuxOld))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
  }

  return result;
}
#endif

uintptr_t posix_signal(int sig, void* func) {
  ERROR("signal called but glue signal should redirect to sigaction");
  return 0;
}

int posix_raise(int sig, SyscallState& State) {
  SG_NOTICE("raise");

  // Create the pending signal and pass it in
  Thread* pThread = Processor::information().getCurrentThread();
  Process* pProcess = pThread->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    ERROR("posix_raise: no subsystem");
    return -1;
  }

  if (sig == SIGCONT) {
    pProcess->resume();
  }

  uint32_t signalFlags = 0;
  PosixSubsystem::SignalDeliveryResult delivery = PosixSubsystem::SignalDeliveryResult::Unavailable;
  const bool bWasInterrupts = Processor::getInterrupts();
  {
    // A scheduler pass with no userspace stack can still select a fallback
    // stack and consume this event. Defer delivery across the IRQ-enabled
    // disposition lock, then close that window before selecting SA_ONSTACK.
    Uninterruptible whileQueueing;
    delivery = pSubsystem->queueSignalDelivery(pThread, sig, &signalFlags);
    Processor::setInterrupts(false);
  }
  const bool signalQueued = delivery == PosixSubsystem::SignalDeliveryResult::Queued;

  uintptr_t stackPointer = State.getStackPointer();
  Thread::AlternateSignalStack& currStack = pThread->getAlternateSignalStack();
  bool useAlternateStack =
      signalQueued && (signalFlags & SA_ONSTACK) && currStack.enabled && !currStack.inUse;
  if (useAlternateStack) {
    stackPointer = (currStack.base + currStack.size) & ~static_cast<uintptr_t>(0xF);
    currStack.inUse = true;
  }

  // Jump to the signal handler
  Processor::information().getScheduler().checkEventState(stackPointer);
  if (useAlternateStack) {
    currStack.inUse = false;
  }
  Processor::setInterrupts(bWasInterrupts);

  // All done
  return 0;
}

int pedigree_sigret() {
  SG_NOTICE("pedigree_sigret");

  if (!SyscallManager::instance().requestEventReturn()) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  return 0;
}

int pedigree_unwind_signal() {
  SG_NOTICE("pedigree_unwind_signal");

  if (!SyscallManager::instance().requestEventStatePop()) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  return 0;
}

static int doThreadKill(Thread* p, int sig) {
  // Are we allowed to do this?
  if (p->getParent()->isSuspended()) {
    if (!(sig == SIGKILL || sig == SIGCONT)) {
      WARNING(
          "kill: can't send anything other than SIGKILL or SIGCONT "
          "to a suspended process.");
      return -1;
    }
  }

  // Build the pending signal and pass it in
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(p->getParent()->getSubsystem());
  if (!pSubsystem) {
    ERROR("posix_kill: no subsystem on process " << p->getParent()->getId());
    return -1;
  }
  // sendSignal applies SIGCONT's unconditional continuation side effect
  // before resolving whether handler delivery is ignored or blocked.
  pSubsystem->sendSignal(p, sig, false);

  return 0;
}

static int doProcessKill(Process* p, int sig) {
  Process::ThreadLease target;
  return p->acquireThread(target, static_cast<size_t>(0)) ? doThreadKill(target.get(), sig) : -1;
}

static bool canSignalProcess(const PosixProcess* caller, const PosixProcess* target, int sig) {
  const int64_t callerReal = caller->getUserId();
  const int64_t callerEffective = caller->getEffectiveUserId();
  if (callerEffective == 0) {
    return true;
  }

  const int64_t targetReal = target->getUserId();
  const int64_t targetSaved = target->getSavedUserId();
  const bool realMatches = callerReal >= 0 && (callerReal == targetReal ||
                                               (targetSaved >= 0 && callerReal == targetSaved));
  const bool effectiveMatches =
      callerEffective >= 0 &&
      (callerEffective == targetReal || (targetSaved >= 0 && callerEffective == targetSaved));
  if (realMatches || effectiveMatches) {
    return true;
  }

  PosixSession* callerSession = caller->getSession();
  return sig == SIGCONT && callerSession && callerSession == target->getSession();
}

int posix_kill(int pid, int sig) {
  SG_NOTICE("kill(" << pid << ", " << sig << ")");

  if (sig < 0 ||
      sig >= static_cast<int>(sizeof(default_sig_handlers) / sizeof(default_sig_handlers[0]))) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  List<size_t> processList;

  // Metadata about the calling process.
  PosixProcess* pThisProcess =
      static_cast<PosixProcess*>(Processor::information().getCurrentThread()->getParent());
  size_t thisGroupId = 0;
  const bool thisHasGroup = pThisProcess->getProcessGroupId(thisGroupId);

  bool bKillingSelf = false;
  bool foundTarget = false;

  // Check for the process(es) we are about to kill.
  size_t i = 0;
  for (; i < Scheduler::instance().getNumProcesses(); ++i) {
    Scheduler::ProcessLease process;
    if (!Scheduler::instance().acquireProcess(process, i)) {
      continue;
    }
    Process* pProcess = process.get();
    Process::ThreadLease primaryThread;
    if (!pProcess->acquireThread(primaryThread, static_cast<size_t>(0))) {
      continue;
    }

    if (primaryThread->getStatus() == Thread::Zombie) {
      // Oops, process already been terminated.
      if (static_cast<int>(pProcess->getId()) == pid) {
        break;
      } else {
        continue;
      }
    }

    if (pProcess->getType() != Process::Posix) {
      continue;
    }

    PosixProcess* pPosixProcess = static_cast<PosixProcess*>(pProcess);
    bool selected = false;
    if (pid > 0) {
      selected = static_cast<int>(pProcess->getId()) == pid;
    } else {
      size_t groupId = 0;
      const bool hasGroup = pPosixProcess->getProcessGroupId(groupId);
      if (pid == 0) {
        // Any process in the same process group as the caller.
        selected = hasGroup && thisHasGroup && groupId == thisGroupId;
        if (selected) {
          SC_NOTICE(" -> selecting process " << pProcess->getId() << " in group [" << groupId
                                             << "]");
        }
      } else if (pid == -1) {
        // Kill all processes we have permission to kill (limit to only
        // direct children for now)
        selected = pProcess->getParent() == pThisProcess;
      } else {
        // Absolute group ID reference
        selected = hasGroup && groupId == static_cast<size_t>(-static_cast<int64_t>(pid));
      }
    }

    if (!selected) {
      continue;
    }

    foundTarget = true;
    if (!canSignalProcess(pThisProcess, pPosixProcess, sig)) {
      continue;
    }

    processList.pushBack(pProcess->getId());
  }

  // No process(es) found?
  if (processList.count() == 0) {
    if (foundTarget) {
      SYSCALL_ERROR(NotEnoughPermissions);
      SG_NOTICE("  -> target exists, but permission was denied");
    } else {
      SYSCALL_ERROR(NoSuchProcess);
      SG_NOTICE("  -> no such process");
    }
    return -1;
  }

  // Signal zero performs the same target and permission checks without
  // allocating, queueing, or dispatching an event.
  if (!sig) {
    return 0;
  }

  // Go ahead and kill each process.
  for (List<size_t>::Iterator it = processList.begin(); it != processList.end(); ++it) {
    if (*it != pThisProcess->getId()) {
      Scheduler::ProcessLease member;
      if (!Scheduler::instance().acquireProcessById(member, *it) ||
          member->getType() != Process::Posix) {
        continue;
      }
      SG_NOTICE(" -> not killing current process, killing " << member->getId());
      NOTICE("sending #" << Dec << member->getId() << " signal #" << sig << " from #"
                         << pThisProcess->getId());
      doProcessKill(member.get(), sig);
    } else {
      SG_NOTICE(" -> killing current process (" << pThisProcess->getId() << ")");
      bKillingSelf = true;
    }
  }

  // Yield to allow the events to be propagated across the process(es)
  Scheduler::instance().yield();

  if (bKillingSelf) {
    SG_NOTICE("performing kill of " << pThisProcess->getId() << "...");
    NOTICE("sending self #" << Dec << pThisProcess->getId() << " signal #" << sig);
    doProcessKill(pThisProcess, sig);

    // If it was us, try to handle the signal *now*, or else we're going to
    // end up who-knows-where on return.
    Processor::information().getScheduler().checkEventState(0);
  }

  return 0;
}

int posix_sigprocmask(int how, const void* set, void* oset, size_t sigsetSize, bool linuxCompat) {
  constexpr size_t KernelSigsetSize = sizeof(uint64_t);
  constexpr uint64_t UnblockableSignals =
      (static_cast<uint64_t>(1) << (SIGKILL - 1)) | (static_cast<uint64_t>(1) << (SIGSTOP - 1));
  static_assert(sizeof(sigset_t) >= KernelSigsetSize,
                "native sigset_t must hold Pedigree's signal mask");

  if (linuxCompat && sigsetSize != KernelSigsetSize) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  const size_t userSigsetSize = linuxCompat ? KernelSigsetSize : sizeof(sigset_t);
  sigset_t requestedSet = {};
  uint64_t requestedMask = 0;
  if (set) {
    if (!PosixSubsystem::copyFromUser(&requestedSet, set, userSigsetSize)) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    if (how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    MemoryCopy(&requestedMask, &requestedSet, KernelSigsetSize);
  }

  Thread* pThread = Processor::information().getCurrentThread();
  uint64_t oldMask = pThread->getSignalMask();
  if (oset) {
    sigset_t previousSet = {};
    MemoryCopy(&previousSet, &oldMask, KernelSigsetSize);
    if (!PosixSubsystem::copyToUser(oset, &previousSet, userSigsetSize)) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
  }

  if (set) {
    uint64_t newMask = oldMask;
    if (how == SIG_BLOCK) {
      newMask |= requestedMask;
    } else if (how == SIG_UNBLOCK) {
      newMask &= ~requestedMask;
    } else {
      newMask = requestedMask;
    }

    // Linux silently leaves SIGKILL and SIGSTOP unblocked.
    pThread->setSignalMask(newMask & ~UnblockableSignals);
  }

  return 0;
}

size_t posix_alarm(uint32_t seconds) {
  SG_NOTICE("alarm(" << seconds << ")");

  Thread* currentThread = Processor::information().getCurrentThread();
  Process* process = currentThread ? currentThread->getParent() : nullptr;
  if (!process || process->getType() != Process::Posix) {
    ERROR("posix_alarm: no POSIX process");
    return 0;
  }

  PosixProcess* posixProcess = static_cast<PosixProcess*>(process);
  Time::Timestamp previousValue = 0;
  posixProcess->getRealIntervalTimer().setIntervalAndValue(
      0, static_cast<Time::Timestamp>(seconds) * Time::Multiplier::Second, nullptr, &previousValue);

  Time::Timestamp previousSeconds = previousValue / Time::Multiplier::Second;
  const Time::Timestamp subsecond = previousValue % Time::Multiplier::Second;
  // A live alarm must not look disarmed, while longer remainders use Linux's
  // historical half-second rounding rule.
  if ((!previousSeconds && subsecond) || subsecond >= (Time::Multiplier::Second / 2)) {
    ++previousSeconds;
  }
  return static_cast<uint32_t>(previousSeconds);
}

int posix_sleep(uint32_t seconds) {
  SG_NOTICE("sleep");

  uint64_t startTick = Machine::instance().getTimer()->getTickCount();
  const bool completed = Time::delay(seconds * Time::Multiplier::Second);
  Thread* thread = Processor::information().getCurrentThread();
  if (!completed && thread->getInterruptionReason() == Thread::InterruptedBySignal) {
    uint64_t endTick = Machine::instance().getTimer()->getTickCount();
    uint64_t elapsed = endTick - startTick;
    uint32_t elapsedSecs = static_cast<uint32_t>(elapsed / 1000);
    thread->clearInterruption();

    if (elapsedSecs >= seconds)
      return 0;
    else
      return seconds - elapsedSecs;
  }

  return 0;
}

int posix_usleep(size_t useconds) {
  SG_NOTICE("usleep");

  const bool completed = Time::delay(useconds * Time::Multiplier::Microsecond);
  Thread* thread = Processor::information().getCurrentThread();
  if (!completed && thread->getInterruptionReason() == Thread::InterruptedBySignal) {
    thread->clearInterruption();
    SYSCALL_ERROR(Interrupted);
    return -1;
  }

  return 0;
}

namespace {
constexpr Time::Timestamp MaximumLinuxSleepNanoseconds = 0x7FFFFFFFFFFFFFFFULL;

bool validRelativeTimespec(const struct timespec& value) {
  return value.tv_sec >= 0 && value.tv_nsec >= 0 && value.tv_nsec < 1000000000;
}

Time::Timestamp relativeTimespecToNanoseconds(const struct timespec& value) {
  const Time::Timestamp seconds = static_cast<Time::Timestamp>(value.tv_sec);
  if (seconds >= (MaximumLinuxSleepNanoseconds / Time::Multiplier::Second)) {
    return MaximumLinuxSleepNanoseconds;
  }

  const Time::Timestamp wholeSeconds = seconds * Time::Multiplier::Second;
  const Time::Timestamp nanoseconds = static_cast<Time::Timestamp>(value.tv_nsec);
  return wholeSeconds + nanoseconds;
}

Time::Timestamp nanosleepAlarmDuration(Time::Timestamp requested) {
  const Time::Timestamp remainder = requested % Time::Multiplier::Microsecond;
  if (!remainder) {
    return requested;
  }

  // The machine timer accepts whole microseconds. Round up so a partial
  // microsecond request cannot expire before its requested duration.
  return requested + (Time::Multiplier::Microsecond - remainder);
}
}  // namespace

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
extern "C" EXPORTED_PUBLIC Time::Timestamp posixNanosleepAlarmDurationForTest(time_t seconds,
                                                                              long nanoseconds) {
  const struct timespec requested = {seconds, nanoseconds};
  return nanosleepAlarmDuration(relativeTimespecToNanoseconds(requested));
}
#endif

int posix_nanosleep(const struct timespec* rqtp, struct timespec* rmtp) {
  struct timespec requested = {};
  if (!PosixSubsystem::copyFromUser(&requested, rqtp, sizeof(requested))) {
    SG_NOTICE("nanosleep -> invalid address");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (!validRelativeTimespec(requested)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  SG_NOTICE("nanosleep(" << Dec << requested.tv_sec << ":" << requested.tv_nsec << Hex << ") - "
                         << Machine::instance().getTimer()->getTickCount() << ".");

  const Time::Timestamp ts = relativeTimespecToNanoseconds(requested);
  if (!ts) {
    return 0;
  }

  const Time::Timestamp start = Time::getTicks();
  const bool completed = Time::delay(nanosleepAlarmDuration(ts));

  Thread* thread = Processor::information().getCurrentThread();
  if (!completed && thread->getInterruptionReason() == Thread::InterruptedBySignal) {
    const Time::Timestamp elapsed = Time::getTicks() - start;
    if (elapsed >= ts) {
      thread->clearInterruption();
      return 0;
    }

    const Time::Timestamp remaining = ts - elapsed;
    const struct timespec result = {static_cast<time_t>(remaining / Time::Multiplier::Second),
                                    static_cast<long>(remaining % Time::Multiplier::Second)};
    thread->clearInterruption();
    if (rmtp && !PosixSubsystem::copyToUser(rmtp, &result, sizeof(result))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    SYSCALL_ERROR(Interrupted);
    return -1;
  }

  return 0;
}

int posix_clock_gettime(clockid_t clock_id, struct timespec* tp) {
  SG_NOTICE("clock_gettime(" << Dec << clock_id << Hex << ")");
  Time::Timestamp nanoseconds = 0;
  switch (clock_id) {
    case CLOCK_REALTIME:
      nanoseconds = Time::getTimeNanoseconds();
      break;
    case CLOCK_MONOTONIC:
      nanoseconds = Time::getTicks();
      break;
    default:
      SYSCALL_ERROR(InvalidArgument);
      return -1;
  }

  const struct timespec result = {static_cast<time_t>(nanoseconds / Time::Multiplier::Second),
                                  static_cast<long>(nanoseconds % Time::Multiplier::Second)};
  if (!PosixSubsystem::copyToUser(tp, &result, sizeof(result))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  return 0;
}

int posix_sigaltstack(const stack_t* stack, stack_t* oldstack) {
  stack_t requested = {};
  if (stack && !PosixSubsystem::copyFromUser(&requested, stack, sizeof(requested))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (stack) {
    if (requested.ss_flags & ~SS_DISABLE) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
  }

  Thread* pThread = Processor::information().getCurrentThread();
  Thread::AlternateSignalStack& currStack = pThread->getAlternateSignalStack();

  if (stack && currStack.inUse) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }

  if (stack && !(requested.ss_flags & SS_DISABLE)) {
    uintptr_t base = reinterpret_cast<uintptr_t>(requested.ss_sp);
    if (requested.ss_size < MINSIGSTKSZ) {
      SYSCALL_ERROR(OutOfMemory);
      return -1;
    }
    if (!base || requested.ss_size > (~static_cast<uintptr_t>(0) - base) ||
        !PosixSubsystem::checkAddress(base, requested.ss_size, PosixSubsystem::SafeWrite)) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
  }

  if (oldstack) {
    stack_t previous = {};
    if (currStack.enabled) {
      previous.ss_sp = reinterpret_cast<void*>(currStack.base);
      previous.ss_size = currStack.size;
      previous.ss_flags = currStack.inUse ? SS_ONSTACK : 0;
    } else {
      previous.ss_flags = SS_DISABLE;
    }
    if (!PosixSubsystem::copyToUser(oldstack, &previous, sizeof(previous))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
  }

  if (stack) {
    if (requested.ss_flags & SS_DISABLE) {
      currStack.base = 0;
      currStack.size = 0;
      currStack.enabled = false;
    } else {
      currStack.base = reinterpret_cast<uintptr_t>(requested.ss_sp);
      currStack.size = requested.ss_size;
      currStack.enabled = true;
    }
  }

  return 0;
}

void pedigree_init_sigret() {
  SG_NOTICE("init_sigret");
  static physical_uintptr_t sigretPhys = 0;

  // Handle allocation if needed.
  if (sigretPhys == 0) {
    sigretPhys = PhysicalMemoryManager::instance().allocatePage();
    PhysicalMemoryManager::instance().pin(sigretPhys);

    // Map trampoline page in and bring across the sigret code.
    Processor::information().getVirtualAddressSpace().map(
        sigretPhys, reinterpret_cast<void*>(Event::getTrampoline()),
        VirtualAddressSpace::Write | VirtualAddressSpace::Shared | VirtualAddressSpace::Execute);

    MemoryCopy(
        reinterpret_cast<void*>(Event::getTrampoline()), reinterpret_cast<void*>(sigret_stub),
        (reinterpret_cast<uintptr_t>(&sigret_stub_end) - reinterpret_cast<uintptr_t>(sigret_stub)));

    // Mark read-only now that we have mapped in the page.
    Processor::information().getVirtualAddressSpace().setFlags(
        reinterpret_cast<void*>(Event::getTrampoline()),
        VirtualAddressSpace::Execute | VirtualAddressSpace::Shared);
  }

  // Map the signal return stub to the correct location
  if (!Processor::information().getVirtualAddressSpace().isMapped(
          reinterpret_cast<void*>(Event::getTrampoline()))) {
    Processor::information().getVirtualAddressSpace().map(
        sigretPhys, reinterpret_cast<void*>(Event::getTrampoline()),
        VirtualAddressSpace::Shared | VirtualAddressSpace::Execute);
  }

  // Install default signal handlers
  Thread* pThread = Processor::information().getCurrentThread();
  pThread->getAlternateSignalStack() = Thread::AlternateSignalStack();
  Process* pProcess = pThread->getParent();
  PosixSubsystem* pSubsystem = static_cast<PosixSubsystem*>(pProcess->getSubsystem());
  if (!pSubsystem) {
    pSubsystem = new PosixSubsystem();
    pProcess->setSubsystem(pSubsystem);
    pSubsystem->setProcess(pProcess);
  }

  for (size_t i = 0; i < 32; i++) {
    // Set all dispositions back to default, except if an ignore
    // disposition was present (SIG_IGN does in fact carry through an exec)
    int signalDisposition = 1;

    PosixSubsystem::SignalDisposition existingDisposition;
    if (pSubsystem->getSignalDisposition(i, existingDisposition)) {
      if (existingDisposition.type == 2) {
        signalDisposition = 2;
      }
    }

    // Constructor zeroes out everything, which is correct for this initial
    // setup of the signal handlers (except, of course, the handler
    // location).
    PosixSubsystem::SignalHandler* sigHandler = new PosixSubsystem::SignalHandler();
    sigHandler->sig = i;
    sigHandler->type = signalDisposition;

    uintptr_t newHandler = signalDisposition == 1
                               ? reinterpret_cast<uintptr_t>(default_sig_handlers[i])
                               : reinterpret_cast<uintptr_t>(sigign);

    sigHandler->pEvent = new SignalEvent(newHandler, i);

    pSubsystem->setSignalHandler(i, sigHandler);
  }

  SG_NOTICE("Creating initial set of signal handlers is complete");
}
