#include "SyscallManager.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/TimeTracker.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/syscallError.h"

Armv7SyscallManager Armv7SyscallManager::m_Instance;

extern void system_reboot(Machine::ShutdownType type);

namespace {
class SyscallReturnScope {
 public:
  explicit SyscallReturnScope(const SyscallState* original)
      : m_Thread(Processor::information().getCurrentThread()),
        m_StateLevel(m_Thread->getStateLevel()),
        m_Previous(m_Thread->getOriginalSyscallState()),
        m_Discard(&restore, this) {
    m_Thread->setOriginalSyscallState(original);
  }

  ~SyscallReturnScope() {
    restore(this);
  }

 private:
  static void restore(void* context) {
    auto* scope = static_cast<SyscallReturnScope*>(context);
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
}  // namespace

SyscallManager& SyscallManager::instance() {
  return Armv7SyscallManager::instance();
}

bool Armv7SyscallManager::registerSyscallHandler(Service_t service, SyscallHandler* handler,
                                                 Registration& registration, FastEntry entry) {
  return registerHandler(service, handler, registration, entry);
}

void Armv7SyscallManager::handle(SyscallState& state) {
  Armv7SyscallManager& manager = m_Instance;
  Thread* syscallThread = Processor::information().getCurrentThread();
  const SyscallState original = state;
  bool commitThreadExit = false;
  bool exitCurrentProcess = false;
  bool rebootSystem = false;
  bool userReturnTerminal = false;
  bool interruptedWithoutProgress = false;
  int processExitCode = 0;
  Subsystem::ExitCause processExitCause = Subsystem::ExitCause::Normal;
  Machine::ShutdownType shutdownType = Machine::ShutdownType::Halt;

  {
    TimeTracker tracker(nullptr, true, true, syscallThread);
    Processor::setInterrupts(true);
    const size_t service = state.getSyscallService();
#if PEDIGREE_BENCHMARK_SYSCALL_TIMING
    if (service == linuxCompat) {
      tracker.attributeSyscall(state.getSyscallNumber());
    }
#endif
    bool handled = false;
    PostSyscallAction action;
    if (service < serviceEnd) {
      HandlerLease handler;
      if (manager.acquireHandler(static_cast<Service_t>(service), handler, action)) {
        handled = true;
        uintptr_t result = manager.dispatchHandler(handler, state);
        uintptr_t error = syscallThread->getErrno();
        interruptedWithoutProgress =
            service == linuxCompat && result == uintptr_t(-1) && error == Error::Interrupted;
        if (service == linuxCompat) {
          state.setSyscallReturnValue(error ? -error : result);
        } else {
          state.setSyscallReturnValue(result);
          state.setSyscallErrno(error);
        }
        syscallThread->setErrno(0);
      }
    }

    PerProcessorScheduler& scheduler = Processor::information().getScheduler();
    if (!handled) {
      state.setSyscallReturnValue(-static_cast<uintptr_t>(Error::Unimplemented));
      userReturnTerminal = scheduler.serviceUserReturnWork(state);
    } else {
      const bool directTransition =
          action.kind == ReturnFromEvent || action.kind == PopEventState ||
          action.kind == RestoreProcessorState || action.kind == JumpToUserspace;
      if (directTransition) {
        userReturnTerminal = scheduler.serviceProcessStopAtUserReturn(
            PerProcessorScheduler::ProcessStopGateMode::DirectUserTransition);
        if (syscallThread->getUnwindState() != Thread::Continue) {
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
          if (!userReturnTerminal) {
            tracker.finishInKernel();
            scheduler.eventHandlerReturned();
          }
          break;
        case PopEventState:
          if (!userReturnTerminal) {
            syscallThread->abandonCurrentState(false);
          }
          break;
        case RestoreProcessorState:
          if (!userReturnTerminal) {
            for (size_t i = 0; i < 13; ++i) {
              state.r[i] = action.state.r[i];
            }
            state.sp = action.state.sp;
            state.pc = action.state.pc;
            state.cpsr = action.state.cpsr;
            userReturnTerminal =
                scheduler.serviceUserReturnWork(state, UserReturnFrame::Origin::SignalRestore);
          }
          break;
        case JumpToUserspace:
          if (!userReturnTerminal) {
            tracker.finishInKernel();
            syscallThread->abandonAllStates();
            for (size_t i = 0; i < 13; ++i) {
              state.r[i] = 0;
            }
            const uintptr_t entry = action.state.getInstructionPointer();
            state.pc = entry & ~uintptr_t(1);
            state.sp = action.state.getStackPointer();
            state.cpsr = 0x10 | ((entry & 1) ? 0x20 : 0);
            state.faultStatus = 0;
            state.svcNumber = 0;
            userReturnTerminal =
                scheduler.serviceUserReturnWork(state, UserReturnFrame::Origin::NewImage);
          }
          break;
        case RebootSystem:
          rebootSystem = true;
          shutdownType = static_cast<Machine::ShutdownType>(action.value);
          break;
        case NoPostSyscallAction:
          if (!syscallThread->canSkipUserReturnWork() || interruptedWithoutProgress) {
            SyscallReturnScope returnScope(interruptedWithoutProgress ? &original : nullptr);
            userReturnTerminal = scheduler.serviceUserReturnWork(state);
          }
          break;
      }
    }

    if (!exitCurrentProcess && !rebootSystem) {
      const Thread::UnwindType unwind = syscallThread->getUnwindState();
      if (userReturnTerminal || unwind != Thread::Continue) {
        if (unwind == Thread::TerminateThread) {
          commitThreadExit = true;
        } else if (unwind == Thread::Exit) {
          exitCurrentProcess = true;
          const Thread::DeferredProcessExit request = syscallThread->takeDeferredProcessExit();
          processExitCode = request.code;
          processExitCause = request.cause;
        }
      }
    }
    tracker.finishInKernel();
  }

  if (rebootSystem) {
    Processor::setInterrupts(false);
    syscallThread->abandonAllStates();
    Processor::setInterrupts(true);
    system_reboot(shutdownType);
  }
  if (exitCurrentProcess) {
    syscallThread->getParent()->getSubsystem()->exit(processExitCode, processExitCause);
  }
  if (commitThreadExit) {
    Processor::information().getScheduler().commitCurrentThreadExit();
  }

  while (true) {
    bool waited = false;
    if (syscallThread->completeAffinityAtSafePoint(&waited) == AffinityResult::Terminal) {
      Processor::information().getScheduler().commitUserReturnTerminalState();
      FATAL_NOLOCK("Terminal affinity return unexpectedly returned");
    }
    if (!waited) {
      break;
    }
    Processor::setInterrupts(true);
    if (Processor::information().getScheduler().serviceUserReturnWork(state)) {
      Processor::information().getScheduler().commitUserReturnTerminalState();
      FATAL_NOLOCK("Terminal user return unexpectedly returned");
    }
  }
  syscallThread->transitionTimeAtInterruptReturn(CpuTimeMode::Kernel, CpuTimeMode::User);
  Processor::setInterrupts(false);
}

uintptr_t Armv7SyscallManager::syscall(Service_t service, uintptr_t function,
                                       uintptr_t p1, uintptr_t p2, uintptr_t p3,
                                       uintptr_t p4, uintptr_t p5) {
  register uintptr_t r0 asm("r0") = p1;
  register uintptr_t r1 asm("r1") = p2;
  register uintptr_t r2 asm("r2") = p3;
  register uintptr_t r3 asm("r3") = p4;
  register uintptr_t r4 asm("r4") = p5;
  register uintptr_t r7 asm("r7") = (static_cast<uintptr_t>(service) << 16) | function;
  asm volatile("push {lr}\n\tsvc #1\n\tpop {lr}"
               : "+r"(r0)
               : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r7)
               : "memory");
  return r0;
}
