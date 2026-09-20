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

#include "SyscallManager.h"
#include "pedigree/kernel/ActivityDiagnostics.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/TimeTracker.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/SyscallHandler.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/syscallError.h"

X64SyscallManager X64SyscallManager::m_Instance;

#define TIME_SYSCALLS 0

extern void system_reboot(Machine::ShutdownType type);
extern "C" void pedigree_defer_user_entry(X64UserEntryMetadata*);

namespace {
void captureUserEntry(SyscallState& state) {
  pedigree_defer_user_entry(&state.m_UserEntry);
}

class SyscallReturnScope {
 public:
  explicit SyscallReturnScope(const SyscallState* state)
      : m_Thread(Processor::information().getCurrentThread()),
        m_StateLevel(m_Thread->getStateLevel()),
        m_Previous(m_Thread->getOriginalSyscallState()),
        m_Discard(&restore, this) {
    m_Thread->setOriginalSyscallState(state);
  }

  ~SyscallReturnScope() {
    restore(this);
  }

 private:
  static void restore(void* context) {
    SyscallReturnScope* scope = static_cast<SyscallReturnScope*>(context);
    if (scope->m_Thread) {
      scope->m_Thread->restoreDeferredSignalMask(scope->m_StateLevel);
      scope->m_Thread->setOriginalSyscallState(scope->m_Previous);
      scope->m_Thread = nullptr;
    }
  }

  Thread* m_Thread;
  size_t m_StateLevel;
  const SyscallState* m_Previous;
  Thread::StackDiscardScope m_Discard;
};

void recordAffinitySample(bool active, bool waited, uint64_t start) {
  if (!active)
    return;
  ActivityDiagnostics::recordUserReturnStage(ActivityDiagnostics::UserReturnStage::SyscallAffinity,
                                             ActivityDiagnostics::timestamp() - start);
  if (waited)
    ActivityDiagnostics::recordUserReturnAffinityWait(true);
}

void recordAccountingSample(bool active, uint64_t start) {
  if (active) {
    ActivityDiagnostics::recordUserReturnStage(
        ActivityDiagnostics::UserReturnStage::SyscallAccounting,
        ActivityDiagnostics::timestamp() - start);
  }
}

bool finishAffinityReturn(SyscallState& state, const SyscallState* original,
                          bool diagnosticSample) {
  Thread* current = Processor::information().getCurrentThread();
  while (true) {
    bool waited = false;
    const uint64_t affinityStart = diagnosticSample ? ActivityDiagnostics::timestamp() : 0;
    const bool terminal = current->completeAffinityAtSafePoint(&waited) == AffinityResult::Terminal;
    recordAffinitySample(diagnosticSample, waited, affinityStart);
    if (terminal)
      return true;
    if (!waited)
      return false;
    Processor::setInterrupts(true);
    SyscallReturnScope returnScope(original);
    if (Processor::information().getScheduler().serviceUserReturnWork(
            state, UserReturnFrame::Origin::Syscall, diagnosticSample))
      return true;
  }
}

bool finishAffinityReturn(InterruptState& state, UserReturnFrame::Origin origin,
                          bool diagnosticSample) {
  Thread* current = Processor::information().getCurrentThread();
  while (true) {
    bool waited = false;
    const uint64_t affinityStart = diagnosticSample ? ActivityDiagnostics::timestamp() : 0;
    const bool terminal = current->completeAffinityAtSafePoint(&waited) == AffinityResult::Terminal;
    recordAffinitySample(diagnosticSample, waited, affinityStart);
    if (terminal)
      return true;
    if (!waited)
      return false;
    Processor::setInterrupts(true);
    if (Processor::information().getScheduler().serviceUserReturnWork(state, origin,
                                                                      diagnosticSample))
      return true;
  }
}
}  // namespace

SyscallManager& SyscallManager::instance() {
  return X64SyscallManager::instance();
}

bool X64SyscallManager::registerSyscallHandler(Service_t Service, SyscallHandler* pHandler,
                                               Registration& registration, FastEntry entry) {
  return registerHandler(Service, pHandler, registration, entry);
}

void X64SyscallManager::syscall(SyscallState& syscallState) {
  captureUserEntry(syscallState);
  // Restart handling consumes only the entry registers, not deferred FS/GS bases.
  const SyscallState originalState = syscallState;
#if PEDIGREE_ACTIVITY_DIAGNOSTICS
  const bool diagnosticSample =
      Processor::information().getScheduler().sampleUserReturnDiagnostics();
#else
  const bool diagnosticSample = false;
#endif
  Thread* syscallThread = Processor::information().getCurrentThread();
  bool commitThreadExit = false;
  bool exitCurrentProcess = false;
  bool rebootSystem = false;
  Machine::ShutdownType shutdownType = Machine::ShutdownType::Halt;
  bool userReturnTerminal = false;
  bool interruptedWithoutProgress = false;
  bool deferTimeAccountingToUserReturn = false;
  uint64_t returnTailStart = 0;
  int processExitCode = 0;
  Subsystem::ExitCause processExitCause = Subsystem::ExitCause::Normal;
  {
    // SYSCALL entered with IF masked by IA32_FMASK. Let the first accounting
    // sample reuse that architectural state instead of masking and restoring
    // interrupts a second time.
    TimeTracker tracker(0, true, true, syscallThread);
#if TIME_SYSCALLS
    Process* pProcess = syscallThread->getParent();
    Time::Stopwatch syscallTimer(true);
    size_t syscallNumber = syscallState.getSyscallNumber();
#endif

    // Enable IRQs - stack switching and such are done now and it's now safe to
    // start processing interrupts elsewhere.
    Processor::setInterrupts(true);

    size_t serviceNumber = syscallState.getSyscallService();
#if PEDIGREE_BENCHMARK_SYSCALL_TIMING
    if (serviceNumber == linuxCompat) {
      tracker.attributeSyscall(syscallState.getSyscallNumber());
    }
#endif
    bool handled = false;
    PostSyscallAction action;
    if (LIKELY(serviceNumber < serviceEnd)) {
      // Blocking callbacks must finish ownership waits before a terminal
      // request can consume this thread's stack.
      TerminationDeferral callbackDeferral;
      SyscallHandler* handler = m_Instance.loadHandler(static_cast<Service_t>(serviceNumber));
      if (handler) {
        handled = true;
        void* previousContext = syscallThread->getSyscallDispatchContext();
        syscallThread->setSyscallDispatchContext(&action);
        uint64_t result = m_Instance.dispatchHandler(static_cast<Service_t>(serviceNumber), handler,
                                                     syscallState);
        syscallThread->setSyscallDispatchContext(previousContext);
        uint64_t errno = syscallThread->getErrno();
        interruptedWithoutProgress = result == static_cast<uint64_t>(-1) &&
                                     errno == Error::Interrupted && serviceNumber == linuxCompat;
        /// \todo this is an extraordinary hack, this should be done in a
        /// way more abstract way than this!!
        if (serviceNumber == linuxCompat) {
          if (errno != 0) {
            syscallState.setSyscallReturnValue(-errno);
          } else {
            syscallState.setSyscallReturnValue(result);
          }
        } else {
          syscallState.setSyscallReturnValue(result);
          syscallState.setSyscallErrno(errno);
        }
        // Reset error number now that we've extracted it.
        syscallThread->setErrno(0);
      }
    }

    returnTailStart = diagnosticSample ? ActivityDiagnostics::timestamp() : 0;
    if (!handled) {
      // Even an invalid service or a temporarily missing handler came from a
      // real userspace frame and must not bypass pending return work.
      userReturnTerminal = Processor::information().getScheduler().serviceUserReturnWork(
          syscallState, UserReturnFrame::Origin::Syscall, diagnosticSample);
    } else {
      const bool directUserTransition =
          action.kind == ReturnFromEvent || action.kind == PopEventState ||
          action.kind == RestoreProcessorState || action.kind == JumpToUserspace;
      if (directUserTransition) {
        const uint64_t stopStart = diagnosticSample ? ActivityDiagnostics::timestamp() : 0;
        userReturnTerminal = Processor::information().getScheduler().serviceProcessStopAtUserReturn(
            PerProcessorScheduler::ProcessStopGateMode::DirectUserTransition);
        if (diagnosticSample) {
          ActivityDiagnostics::recordUserReturnStage(
              ActivityDiagnostics::UserReturnStage::ProcessStop,
              ActivityDiagnostics::timestamp() - stopStart);
        }
        Thread* current = syscallThread;
        if (current && current->getUnwindState() != Thread::Continue) {
          userReturnTerminal = true;
        }
      }

      switch (action.kind) {
        case TerminateCurrentThread:
          commitThreadExit = true;
          break;
        case ExitCurrentProcess:
          exitCurrentProcess = true;
          processExitCode = static_cast<int>(action.value);
          break;
        case ReturnFromEvent:
          if (userReturnTerminal) {
            break;
          }
          tracker.finishInKernel();
          Processor::information().getScheduler().eventHandlerReturned();
          break;
        case PopEventState:
          if (!userReturnTerminal) {
            syscallThread->abandonCurrentState(false);
          }
          break;
        case RestoreProcessorState: {
          if (userReturnTerminal) {
            break;
          }
          // Linux rt_sigreturn replaces the current user register
          // image; it does not own a Pedigree event state to pop.
          const uintptr_t userStack = action.state.rsp;
          const uint64_t userFlags = action.state.rflags;
          alignas(16) unsigned char interruptStack[sizeof(InterruptState)] = {};
          action.state.setStackPointer(
              reinterpret_cast<uintptr_t>(interruptStack + sizeof(interruptStack)));
          X64UserEntryMetadata metadata = syscallState.getUserEntryMetadata();
          metadata.origRax = ~uint64_t(0);
          InterruptState* returnState = InterruptState::construct(action.state, true, metadata);
          returnState->setStackPointer(userStack);
          returnState->setFlags(userFlags);
          // rt_sigreturn restores the old mask before committing this frame.
          // Service newly unblocked signals against that exact restored image
          // so none escape briefly to userspace or wait for another syscall.
          userReturnTerminal = Processor::information().getScheduler().serviceUserReturnWork(
              *returnState, UserReturnFrame::Origin::SignalRestore, diagnosticSample);
          if (userReturnTerminal) {
            break;
          }
          tracker.finishInKernel();
          Thread* current = syscallThread;
          if (finishAffinityReturn(*returnState, UserReturnFrame::Origin::SignalRestore,
                                   diagnosticSample)) {
            userReturnTerminal = true;
            Processor::setInterrupts(true);
            break;
          }
          const uint64_t accountingStart = diagnosticSample ? ActivityDiagnostics::timestamp() : 0;
          current->transitionTime(CpuTimeMode::Kernel, CpuTimeMode::User);
          recordAccountingSample(diagnosticSample, accountingStart);
          Processor::contextSwitch(returnState);
        }
        case JumpToUserspace: {
          if (userReturnTerminal)
            break;
          tracker.finishInKernel();
          Processor::setInterrupts(false);
          Thread* current = syscallThread;
          current->abandonAllStates();
          SyscallState newImage;
          ByteSet(&newImage, 0, sizeof(newImage));
          newImage.setInstructionPointer(action.state.getInstructionPointer());
          newImage.setStackPointer(action.state.getStackPointer());
          newImage.setFlags(0x202);
          X64UserEntryMetadata metadata = {};
          metadata.ds = metadata.es = 0x23;
          metadata.origRax = 59;
          newImage.setUserEntryMetadata(metadata);
          // invoke reset TLS to the new image's actual scheduler-owned base.
          newImage.refreshUserTlsBase();
          Processor::setInterrupts(true);
          // Materialize the loader-owned stack before the final IRQ-off tail.
          *reinterpret_cast<volatile uint64_t*>(newImage.getStackPointer() - 8) = 0;
          while (true) {
            userReturnTerminal = Processor::information().getScheduler().serviceUserReturnWork(
                newImage, UserReturnFrame::Origin::NewImage, diagnosticSample);
            if (userReturnTerminal)
              break;
            bool waited = false;
            const uint64_t affinityStart = diagnosticSample ? ActivityDiagnostics::timestamp() : 0;
            userReturnTerminal =
                current->completeAffinityAtSafePoint(&waited) == AffinityResult::Terminal;
            recordAffinitySample(diagnosticSample, waited, affinityStart);
            if (userReturnTerminal || !waited)
              break;
            Processor::setInterrupts(true);
          }
          if (userReturnTerminal) {
            Processor::setInterrupts(true);
            break;
          }
          const uint64_t accountingStart = diagnosticSample ? ActivityDiagnostics::timestamp() : 0;
          current->transitionTime(CpuTimeMode::Kernel, CpuTimeMode::User);
          recordAccountingSample(diagnosticSample, accountingStart);
          // This tail restores the same image exposed by the stop and retains
          // jumpUser's CR0.TS lazy-FPU setup.
          Processor::restoreState(newImage, nullptr);
        }
        case RebootSystem:
          rebootSystem = true;
          shutdownType = static_cast<Machine::ShutdownType>(action.value);
          break;
        case NoPostSyscallAction: {
#if PEDIGREE_FAST_USER_RETURN
          Thread* current = syscallThread;
          if (!interruptedWithoutProgress && current && current->canSkipUserReturnWork()) {
            deferTimeAccountingToUserReturn = true;
            break;
          }
#endif
          SyscallReturnScope returnScope(interruptedWithoutProgress ? &originalState : nullptr);
          userReturnTerminal = Processor::information().getScheduler().serviceUserReturnWork(
              syscallState, UserReturnFrame::Origin::Syscall, diagnosticSample);
          break;
        }
      }
    }

    if (!exitCurrentProcess && !rebootSystem) {
      Thread* pThread = syscallThread;
      const Thread::UnwindType unwindState = pThread->getUnwindState();
      if (userReturnTerminal || unwindState != Thread::Continue) {
        if (unwindState == Thread::TerminateThread) {
          commitThreadExit = true;
        }
        if (unwindState == Thread::Exit) {
          NOTICE("Unwind state exit at syscall return");
          exitCurrentProcess = true;
          const Thread::DeferredProcessExit request = pThread->takeDeferredProcessExit();
          processExitCode = request.code;
          processExitCause = request.cause;
        }
      }
    }

    // Make sure we come back out with interrupts enabled at all times.
    if ((syscallState.m_RFlagsR11 & 0x200) != 0x200) {
      syscallState.m_RFlagsR11 |= 0x200;
    }

#if TIME_SYSCALLS
    syscallTimer.stop();
    Time::Timestamp value = syscallTimer.value();
    NOTICE("SYSCALL pid=" << Dec << pProcess->getId() << " service=" << serviceNumber
                          << " num=" << syscallNumber << " ns=" << value << Hex);
#endif
    if (deferTimeAccountingToUserReturn)
      tracker.finishForUserReturn();
    else
      tracker.finishInKernel();
  }

  if (rebootSystem) {
    Processor::setInterrupts(false);
    syscallThread->abandonAllStates();
    Processor::setInterrupts(true);
    system_reboot(shutdownType);
    return;
  }
  if (exitCurrentProcess) {
    syscallThread->getParent()->getSubsystem()->exit(
        processExitCode, processExitCause);
  }
  if (commitThreadExit) {
    Processor::information().getScheduler().commitCurrentThreadExit();
  }

  // No syscall handler, return-action lease, or accounting scope survives
  // this boundary. Keep IRQs disabled from the final mask check through SYSRET.
  Thread* current = syscallThread;
  if (finishAffinityReturn(syscallState, interruptedWithoutProgress ? &originalState : nullptr,
                           diagnosticSample)) {
    Processor::setInterrupts(true);
    Processor::information().getScheduler().commitUserReturnTerminalState();
    FATAL_NOLOCK("Terminal affinity return unexpectedly returned");
  }
  const uint64_t accountingStart = diagnosticSample ? ActivityDiagnostics::timestamp() : 0;
  // completeAffinityAtSafePoint() leaves the ordinary return tail with IRQs
  // masked. Do not sample and restore that state again before SYSRET.
  current->transitionTimeAtInterruptReturn(CpuTimeMode::Kernel, CpuTimeMode::User);
  recordAccountingSample(diagnosticSample, accountingStart);
  if (diagnosticSample) {
    ActivityDiagnostics::recordUserReturnStage(ActivityDiagnostics::UserReturnStage::SyscallTail,
                                               ActivityDiagnostics::timestamp() - returnTailStart);
  }
}

uintptr_t X64SyscallManager::syscall(Service_t service, uintptr_t function, uintptr_t p1,
                                     uintptr_t p2, uintptr_t p3, uintptr_t p4, uintptr_t p5) {
  uint64_t rax = (static_cast<uint64_t>(service) << 16) | function;
  uint64_t ret;
  asm volatile(
      "mov %6, %%r8; \
                  syscall"
      : "=a"(ret)
      : "0"(rax), "b"(p1), "d"(p2), "S"(p3), "D"(p4), "m"(p5)
      : "rcx", "r11");
  return ret;
}

//
// Functions only usable in the kernel initialisation phase
//

extern "C" void syscall_handler();
void X64SyscallManager::initialiseProcessor() {
  // Enable SCE (= System Call Extensions)
  // Set IA32_EFER/EFER.SCE
  Processor::writeMachineSpecificRegister(
      0xC0000080, Processor::readMachineSpecificRegister(0xC0000080) | 0x0000000000000001);

  // Setup SYSCALL/SYSRET
  // Set the IA32_STAR/STAR (CS/SS segment selectors)
  Processor::writeMachineSpecificRegister(0xC0000081, 0x001B000800000000LL);
  // Set the IA32_LSTAR/LSTAR (RIP)
  Processor::writeMachineSpecificRegister(0xC0000082, reinterpret_cast<uint64_t>(syscall_handler));
  // Set the IA32_FMASK/SF_MASK (RFLAGS mask, RFLAGS.IF,TF,DF cleared after
  // syscall)
  Processor::writeMachineSpecificRegister(0xC0000084, 0x0000000000000700LL);
}

X64SyscallManager::X64SyscallManager() {}
X64SyscallManager::~X64SyscallManager() {}
