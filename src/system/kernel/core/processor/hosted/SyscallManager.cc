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
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/SyscallHandler.h"
#include "pedigree/kernel/processor/state.h"

HostedSyscallManager HostedSyscallManager::m_Instance;

extern void system_reboot();

SyscallManager& SyscallManager::instance() {
  return HostedSyscallManager::instance();
}

bool HostedSyscallManager::registerSyscallHandler(Service_t Service, SyscallHandler* pHandler,
                                                  Registration& registration) {
  return registerHandler(Service, pHandler, registration);
}

void HostedSyscallManager::syscall(SyscallState& syscallState) {
  size_t serviceNumber = syscallState.getSyscallService();

  if (UNLIKELY(serviceNumber >= serviceEnd)) {
    // TODO: We should return an error here
    return;
  }

  bool commitThreadExit = false;
  bool exitCurrentProcess = false;
  bool rebootSystem = false;
  int processExitCode = 0;
  {
    bool handled = false;
    PostSyscallAction action;
    {
      // The lease must retire before the deferral allows a pending terminal
      // request to consume this thread's stack.
      TerminationDeferral callbackDeferral;
      HandlerLease handler;
      if (m_Instance.acquireHandler(static_cast<Service_t>(serviceNumber), handler, action)) {
        handled = true;
        syscallState.setSyscallReturnValue(handler.handler()->syscall(syscallState));
        Thread* thread = Processor::information().getCurrentThread();
        syscallState.setSyscallErrno(thread->getErrno());
        thread->setErrno(0);
      }
    }

    if (!handled) {
      return;
    }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    if (action.kind != NoPostSyscallAction && m_Instance.postSyscallHookHandled(action)) {
      return;
    }
#endif
    switch (action.kind) {
      case TerminateCurrentThread:
        commitThreadExit = true;
        break;
      case ExitCurrentProcess:
        exitCurrentProcess = true;
        processExitCode = static_cast<int>(action.value);
        break;
      case ReturnFromEvent:
        Processor::information().getScheduler().eventHandlerReturned();
      case PopEventState:
        Processor::information().getCurrentThread()->abandonCurrentState(false);
        break;
      case RestoreProcessorState:
        FATAL("Processor-state restoration requested on hosted.");
        return;
      case JumpToUserspace:
        Processor::setInterrupts(false);
        Processor::information().getCurrentThread()->abandonAllStates();
        Processor::jumpUser(nullptr, action.state.getInstructionPointer(),
                            action.state.getStackPointer());
      case RebootSystem:
        rebootSystem = true;
        break;
      case NoPostSyscallAction:
        break;
    }

    if (!exitCurrentProcess && !rebootSystem) {
      Thread* pThread = Processor::information().getCurrentThread();
      const Thread::UnwindType unwindState = pThread->getUnwindState();
      if (unwindState == Thread::TerminateThread) {
        commitThreadExit = true;
      }
      if (unwindState == Thread::Exit) {
        NOTICE("Unwind state exit, in interrupt handler");
        exitCurrentProcess = true;
        processExitCode = pThread->takeDeferredProcessExitCode();
      }
    }
  }

  if (rebootSystem) {
    Processor::setInterrupts(false);
    Processor::information().getCurrentThread()->abandonAllStates();
    Processor::setInterrupts(true);
    system_reboot();
    return;
  }
  if (exitCurrentProcess) {
    Processor::information().getCurrentThread()->getParent()->getSubsystem()->exit(processExitCode);
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
