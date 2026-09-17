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

HostedSyscallManager HostedSyscallManager::m_Instance;

extern void system_reboot(Machine::ShutdownType type);

SyscallManager& SyscallManager::instance() {
  return HostedSyscallManager::instance();
}

bool HostedSyscallManager::registerSyscallHandler(Service_t Service, SyscallHandler* pHandler,
                                                  Registration& registration) {
  return registerHandler(Service, pHandler, registration);
}

void HostedSyscallManager::syscall(SyscallState& syscallState) {
  bool commitThreadExit = false;
  bool exitCurrentProcess = false;
  bool rebootSystem = false;
  Machine::ShutdownType shutdownType = Machine::ShutdownType::Halt;
  bool userReturnTerminal = false;
  int processExitCode = 0;
  Subsystem::ExitCause processExitCause = Subsystem::ExitCause::Normal;
  {
    Thread* entryThread = Processor::information().getCurrentThread();
    const bool fromUserspace =
        entryThread && entryThread->currentTimeAccountingMode() == CpuTimeMode::User;
    TimeTracker tracker(0, fromUserspace);
    if (fromUserspace) {
      Processor::setInterrupts(true);
    }

    const size_t serviceNumber = syscallState.getSyscallService();
#if PEDIGREE_BENCHMARK_SYSCALL_TIMING
    if (fromUserspace && serviceNumber == linuxCompat) {
      tracker.attributeSyscall(syscallState.getSyscallNumber());
    }
#endif
    bool handled = false;
    PostSyscallAction action;
    if (LIKELY(serviceNumber < serviceEnd)) {
      SyscallHandler* handler = m_Instance.loadHandler(static_cast<Service_t>(serviceNumber));
      if (handler) {
        handled = true;
        Thread* thread = Processor::information().getCurrentThread();
        void* previousContext = thread->getSyscallDispatchContext();
        thread->setSyscallDispatchContext(&action);
        syscallState.setSyscallReturnValue(handler->syscall(syscallState));
        thread->setSyscallDispatchContext(previousContext);
        syscallState.setSyscallErrno(thread->getErrno());
        thread->setErrno(0);
      }
    }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    if (handled && action.kind != NoPostSyscallAction &&
        m_Instance.postSyscallHookHandled(action)) {
      return;
    }
#endif
    PerProcessorScheduler& scheduler = Processor::information().getScheduler();
    if (!handled) {
      if (fromUserspace) {
        userReturnTerminal = scheduler.serviceUserReturnWork(syscallState);
      }
    } else {
      const bool directUserTransition =
          action.kind == ReturnFromEvent || action.kind == PopEventState ||
          action.kind == RestoreProcessorState || action.kind == JumpToUserspace;
      if (directUserTransition) {
        userReturnTerminal = scheduler.serviceProcessStopAtUserReturn(
            PerProcessorScheduler::ProcessStopGateMode::DirectUserTransition);
        Thread* current = Processor::information().getCurrentThread();
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
          scheduler.eventHandlerReturned();
          break;
        case PopEventState:
          if (!userReturnTerminal) {
            Processor::information().getCurrentThread()->abandonCurrentState(false);
          }
          break;
        case RestoreProcessorState:
          if (!userReturnTerminal) {
            FATAL("Processor-state restoration requested on hosted.");
          }
          break;
        case JumpToUserspace:
          if (userReturnTerminal) {
            break;
          }
          tracker.finish();
          Processor::setInterrupts(false);
          Processor::information().getCurrentThread()->abandonAllStates();
          Processor::jumpUser(nullptr, action.state.getInstructionPointer(),
                              action.state.getStackPointer());
        case RebootSystem:
          rebootSystem = true;
          shutdownType = static_cast<Machine::ShutdownType>(action.value);
          break;
        case NoPostSyscallAction:
          if (fromUserspace) {
            Thread* current = Processor::information().getCurrentThread();
            if (current && current->canSkipUserReturnWork()) {
              break;
            }
          }
          if (fromUserspace) {
            userReturnTerminal = scheduler.serviceUserReturnWork(syscallState);
          }
          break;
      }
    }

    if (!exitCurrentProcess && !rebootSystem) {
      Thread* pThread = Processor::information().getCurrentThread();
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
  }

  if (rebootSystem) {
    Processor::setInterrupts(false);
    Processor::information().getCurrentThread()->abandonAllStates();
    Processor::setInterrupts(true);
    system_reboot(shutdownType);
    return;
  }
  if (exitCurrentProcess) {
    Processor::information().getCurrentThread()->getParent()->getSubsystem()->exit(
        processExitCode, processExitCause);
  }
  if (commitThreadExit) {
    Processor::information().getScheduler().commitCurrentThreadExit();
  }
}

uintptr_t HostedSyscallManager::syscall(Service_t service, uintptr_t function, uintptr_t p1,
                                        uintptr_t p2, uintptr_t p3, uintptr_t p4, uintptr_t p5) {
  HostedSyscallState state = {};
  state.service = service;
  state.number = function;
  state.p1 = p1;
  state.p2 = p2;
  state.p3 = p3;
  state.p4 = p4;
  state.p5 = p5;
  syscall(state);
  return state.result;
}

//
// Functions only usable in the kernel initialisation phase
//

void HostedSyscallManager::initialiseProcessor() {}

HostedSyscallManager::HostedSyscallManager() {}

HostedSyscallManager::~HostedSyscallManager() {}
