/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/SyscallManager.h"

namespace {
class LifetimeHandler : public SyscallHandler {
 public:
  LifetimeHandler() : entered(0), release(0), own(nullptr), peer(nullptr), abandon(false) {}

  uintptr_t syscall(SyscallState&) override {
    if (own && (own->reset() || !*own)) {
      FATAL("QEMU syscall self-removal lost ownership");
    }
    if (peer && (!peer->reset() || *peer)) {
      FATAL("QEMU syscall idle peer could not retire");
    }
    if (abandon) {
      Thread* thread = Processor::information().getCurrentThread();
      thread->getScheduler()->abandonCurrentThreadStack(
          PerProcessorScheduler::StackDiscardReason::HostedRegression);
    }
    if (own) {
      entered.release();
      if (!release.acquireForCompletion()) {
        FATAL("QEMU syscall lifetime wait interrupted");
      }
    }
    return 0x51;
  }

  Semaphore entered;
  Semaphore release;
  SyscallManager::Registration* own;
  SyscallManager::Registration* peer;
  bool abandon;
};

uintptr_t fastEntry(SyscallHandler* handler, SyscallState& state) {
  return handler->syscall(state) + 0x100;
}

class NestedRetirementHandler : public SyscallHandler {
 public:
  explicit NestedRetirementHandler(bool inner) : m_Inner(inner) {}

  uintptr_t syscall(SyscallState&) override {
    Thread* thread = Processor::information().getCurrentThread();
    SyscallManager& manager = SyscallManager::instance();
    if (m_Inner) {
      // Exec from a nested event abandons ancestor scopes while keeping the
      // current syscall alive long enough to request its userspace transition.
      thread->prepareSignalStateForExec();
      if (!manager.requestUserJump(0x1000, 0x2000)) {
        FATAL("QEMU ancestor retirement lost the nested syscall action");
      }
      return 0x62;
    }
    if (!thread->pushState()) {
      FATAL("QEMU syscall nested state could not be created");
    }
    uintptr_t result = 0;
    if (manager.dispatchHandlerForTest(native, result) || result != 0x62) {
      FATAL("QEMU nested syscall action was not retained");
    }
    thread->popState();
    if (manager.requestThreadExit()) {
      FATAL("QEMU ancestor retirement left a stale syscall context");
    }
    return 0x63;
  }

 private:
  bool m_Inner;
};

struct Invocation {
  uintptr_t result = 0;
  bool returned = false;
};

int invoke(void* parameter) {
  Invocation* invocation = static_cast<Invocation*>(parameter);
  invocation->returned = SyscallManager::instance().dispatchHandlerForTest(TUI, invocation->result);
  return 0;
}

struct Removal {
  explicit Removal(SyscallManager::Registration& token)
      : registration(token), closed(0), retired(0) {}
  SyscallManager::Registration& registration;
  Semaphore closed;
  Atomic<size_t> retired;
};

int remove(void* parameter) {
  Removal* removal = static_cast<Removal*>(parameter);
  if (!removal->registration.closeAdmission()) {
    FATAL("QEMU syscall admission could not close");
  }
  removal->closed.release();
  if (!removal->registration.reset()) {
    FATAL("QEMU syscall admitted callback could not drain");
  }
  removal->retired = 1;
  return 0;
}
}  // namespace

bool runSyscallLifetimeRegression() {
  NOTICE("QEMU-CONCURRENCY-TEST: BEGIN syscall-handler-lifetime");
  SyscallManager& manager = SyscallManager::instance();
  Process* process = Scheduler::instance().getKernelProcess();
  LifetimeHandler handler, peer;
  SyscallManager::Registration registration, peerRegistration;
  if (!manager.registerSyscallHandler(TUI, &handler, registration, fastEntry) ||
      !manager.registerSyscallHandler(native, &peer, peerRegistration)) {
    return false;
  }
  handler.own = &registration;
  handler.peer = &peerRegistration;
  Invocation invocation;
  Thread* callback = new Thread(process, invoke, &invocation);
  if (!handler.entered.acquireForCompletion()) {
    return false;
  }
  Removal removal(registration);
  Thread* remover = new Thread(process, remove, &removal);
  if (!removal.closed.acquireForCompletion()) {
    return false;
  }
  uintptr_t result = 0;
  if (manager.dispatchHandlerForTest(TUI, result)) {
    return false;
  }
  for (size_t i = 0; i < 32; ++i) {
    Scheduler::instance().yield();
    if (removal.retired) {
      return false;
    }
  }
  handler.release.release();
  if (!callback->joinForCompletion() || !remover->joinForCompletion() || !invocation.returned ||
      invocation.result != 0x151 || !removal.retired || registration || peerRegistration) {
    return false;
  }

  // Reuse the service with a different object and the virtual fallback. A stale
  // fast entry from the prior generation would change the return value.
  if (!manager.registerSyscallHandler(TUI, &peer, registration) ||
      !manager.dispatchHandlerForTest(TUI, result) || result != 0x51 || !registration.reset()) {
    return false;
  }

  peer.abandon = true;
  if (!manager.registerSyscallHandler(TUI, &peer, registration)) {
    return false;
  }
  Invocation abandoned;
  Thread* discarded = new Thread(process, invoke, &abandoned);
  if (!discarded->joinForCompletion() || abandoned.returned || !registration.reset()) {
    return false;
  }

  NestedRetirementHandler outer(false), inner(true);
  if (!manager.registerSyscallHandler(TUI, &outer, registration) ||
      !manager.registerSyscallHandler(native, &inner, peerRegistration)) {
    return false;
  }
  Invocation nested;
  Thread* nesting = new Thread(process, invoke, &nested);
  if (!nesting->joinForCompletion() || !nested.returned || nested.result != 0x63 ||
      !registration.reset() || !peerRegistration.reset()) {
    return false;
  }
  NOTICE("QEMU-CONCURRENCY-TEST: PASS syscall-handler-lifetime");
  return true;
}
