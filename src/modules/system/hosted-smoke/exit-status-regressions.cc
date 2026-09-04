/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/SignalEvent.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/utilities/ZombieQueue.h"

#include <signal.h>

#include "modules/subsys/posix/PosixProcess.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/signal-syscalls.h"
#include "modules/subsys/posix/system-syscalls.h"
#include <sys/wait.h>

namespace {
constexpr size_t HostedAttempts = 10000;
constexpr int NormalExitCode = 0xA5;

struct ExitStatusContext {
  ExitStatusContext(int code, Subsystem::ExitCause cause) : code(code), cause(cause), entered(0) {}

  int code;
  Subsystem::ExitCause cause;
  Atomic<size_t> entered;
};

struct TransitionSeederContext {
  explicit TransitionSeederContext(Process* process)
      : process(process), ready(0), go(0), done(0), resumes(0) {}

  Process* process;
  Atomic<size_t> ready;
  Atomic<size_t> go;
  Atomic<size_t> done;
  Atomic<size_t> resumes;
};

struct SigchldVisibilityContext {
  SigchldVisibilityContext()
      : child(nullptr),
        childId(0),
        parentReady(0),
        releaseParent(0),
        handlerCalls(0),
        handlerFailures(0),
        handlerSawTerminated(0),
        handlerSawOwnerOnStack(0),
        waitResult(0),
        waitStatus(0) {}

  PosixProcess* child;
  size_t childId;
  Atomic<size_t> parentReady;
  Atomic<size_t> releaseParent;
  Atomic<size_t> handlerCalls;
  Atomic<size_t> handlerFailures;
  Atomic<size_t> handlerSawTerminated;
  Atomic<size_t> handlerSawOwnerOnStack;
  Atomic<size_t> waitResult;
  Atomic<size_t> waitStatus;
};

SigchldVisibilityContext* g_SigchldVisibilityContext = nullptr;

void hostedSigchldHandler(size_t argument) {
  SigchldVisibilityContext* context =
      __atomic_load_n(&g_SigchldVisibilityContext, __ATOMIC_ACQUIRE);
  if (!context) {
    return;
  }

  const uint8_t* serializedEvent = reinterpret_cast<const uint8_t*>(argument);
  if (!serializedEvent || serializedEvent[0] != SIGCHLD || !context->child) {
    context->handlerFailures += 1;
    context->handlerCalls += 1;
    return;
  }

  context->handlerSawTerminated = context->child->getState() == Process::Terminated ? 1 : 0;
  context->handlerSawOwnerOnStack = context->child->isTerminationReapableForHostedTest() ? 0 : 1;

  const int status = context->child->getExitStatus();
  const int result = posix_waitpid(static_cast<int>(context->childId), nullptr, WNOHANG, nullptr);
  context->waitResult = static_cast<size_t>(result);
  context->waitStatus = static_cast<size_t>(status);
  context->handlerCalls += 1;
}

int sigchldParentTarget(void* parameter) {
  SigchldVisibilityContext* context = reinterpret_cast<SigchldVisibilityContext*>(parameter);
  context->parentReady += 1;
  while (!context->handlerCalls && !context->releaseParent) {
    Scheduler::instance().yield();
  }
  return 0;
}

int resumeSuspendedProcess(void* parameter) {
  TransitionSeederContext* context = reinterpret_cast<TransitionSeederContext*>(parameter);
  context->ready += 1;
  while (!context->go) {
    Scheduler::instance().yield();
  }

  while (!context->done) {
    if (context->process->isSuspended()) {
      context->process->resume();
      context->resumes += 1;
      return 0;
    }
    Scheduler::instance().yield();
  }
  return 1;
}

int deferredPosixExit(void* parameter) {
  ExitStatusContext* context = reinterpret_cast<ExitStatusContext*>(parameter);
  context->entered += 1;

  Thread* current = Processor::information().getCurrentThread();
  if (context->cause == Subsystem::ExitCause::Signal) {
    pedigree_init_sigret();
    PosixSubsystem* subsystem = static_cast<PosixSubsystem*>(current->getParent()->getSubsystem());
    subsystem->sendSignal(current, context->code, false);
    current->getScheduler()->checkEventState(0);
    if (current->getUnwindState() != Thread::Exit) {
      current->deferProcessExit(127);
    }
  } else {
    current->deferProcessExit(context->code);
  }
  Thread::threadExited();
}

bool waitForTermination(Process* process) {
  for (size_t attempt = 0; attempt < HostedAttempts; ++attempt) {
    if (process->isTerminationReapableForHostedTest()) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}

bool runExitStatusFixture(Process* kernelProcess, int code, Subsystem::ExitCause cause,
                          int previousStatus, bool seedTransientStatus) {
  ExitStatusContext* context = new ExitStatusContext(code, cause);
  PosixProcess* process = new PosixProcess(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  process->setExitStatus(previousStatus);
  process->publish();

  bool transientSeeded = !seedTransientStatus;
  if (seedTransientStatus) {
    TransitionSeederContext transitionContext(process);
    Thread* resumer = new Thread(kernelProcess, resumeSuspendedProcess, &transitionContext, nullptr,
                                 false, true, true);
    resumer->setName("hosted POSIX exit-status transition seeder");
    if (resumer->start()) {
      for (size_t attempt = 0; attempt < HostedAttempts && !transitionContext.ready; ++attempt) {
        Scheduler::instance().yield();
      }
      if (transitionContext.ready) {
        transitionContext.go += 1;
        process->suspend(SIGTSTP);
      }
      transitionContext.done += 1;
      transitionContext.go += 1;
      transientSeeded = resumer->joinForCompletion() && transitionContext.ready == 1 &&
                        transitionContext.resumes == 1;
    } else {
      delete resumer;
    }
  }

  Thread* thread = new Thread(process, deferredPosixExit, context, nullptr, false, true, true);
  thread->setName("hosted POSIX exit-status fixture");

  const bool started = thread->start();
  if (!started) {
    delete thread;
    delete process;
    delete context;
    return false;
  }
  const bool reapable = waitForTermination(process);
  const int status = process->getExitStatus();
  const bool normal = cause == Subsystem::ExitCause::Normal;
  const int expectedStatus = normal ? ((code & 0xFF) << 8) : (code & 0x7F);
  const bool statusValid =
      status == expectedStatus && (normal ? (WIFEXITED(status) && WEXITSTATUS(status) == code)
                                          : (WIFSIGNALED(status) && WTERMSIG(status) == code));
  Process::ChildTransition transition;
  bool transientCleared = false;
  {
    auto guard = kernelProcess->acquireChildStateWait();
    transientCleared = !process->takePendingChildTransition(true, true, transition);
  }
  const bool passed =
      reapable && context->entered == 1 && transientSeeded && transientCleared && statusValid;

  if (reapable) {
    delete process;
    delete context;
  }
  return passed;
}

bool sigchldWaitStatusVisible(Process* kernelProcess) {
  constexpr int ExitCode = 37;
  SigchldVisibilityContext context;

  PosixProcess* parent = new PosixProcess(kernelProcess);
  PosixSubsystem* parentSubsystem = new PosixSubsystem;
  parent->setSubsystem(parentSubsystem);

  PosixSubsystem::SignalHandler* handler = new PosixSubsystem::SignalHandler;
  handler->type = 0;
  handler->pEvent = new SignalEvent(reinterpret_cast<uintptr_t>(&hostedSigchldHandler), SIGCHLD);
  parentSubsystem->setSignalHandler(SIGCHLD, handler);

  Thread* parentThread =
      new Thread(parent, sigchldParentTarget, &context, nullptr, false, true, true);
  parentThread->setName("hosted SIGCHLD wait-status target");
  parent->publish();

  __atomic_store_n(&g_SigchldVisibilityContext, &context, __ATOMIC_RELEASE);
  bool parentStarted = parentThread->start();
  for (size_t attempt = 0; parentStarted && attempt < HostedAttempts && !context.parentReady;
       ++attempt) {
    Scheduler::instance().yield();
  }
  if (!parentStarted || context.parentReady != 1) {
    context.releaseParent += 1;
    if (parentStarted) {
      parentThread->joinForCompletion();
    } else {
      delete parentThread;
    }
    __atomic_store_n(&g_SigchldVisibilityContext, static_cast<SigchldVisibilityContext*>(nullptr),
                     __ATOMIC_RELEASE);
    delete parent;
    return false;
  }

  ExitStatusContext childExit(ExitCode, Subsystem::ExitCause::Normal);
  PosixProcess* child = new PosixProcess(parent);
  child->setSubsystem(new PosixSubsystem);
  Thread* childThread =
      new Thread(child, deferredPosixExit, &childExit, nullptr, false, true, true);
  childThread->setName("hosted SIGCHLD wait-status child");
  child->publish();
  context.child = child;
  context.childId = child->getId();

  Scheduler::ProcessLease childLease;
  const bool childLeased = Scheduler::instance().acquireProcess(childLease, child);
  const bool childStarted = childLeased && childThread->start();
  if (!childStarted) {
    delete childThread;
    childLease.reset();
    delete child;
    context.child = nullptr;
    context.releaseParent += 1;
    parentThread->joinForCompletion();
    __atomic_store_n(&g_SigchldVisibilityContext, static_cast<SigchldVisibilityContext*>(nullptr),
                     __ATOMIC_RELEASE);
    delete parent;
    return false;
  }

  for (size_t attempt = 0; childStarted && attempt < HostedAttempts && !context.handlerCalls;
       ++attempt) {
    Scheduler::instance().yield();
  }

  bool reapable = false;
  for (size_t attempt = 0; attempt < HostedAttempts; ++attempt) {
    if (child->isTerminationReapableForHostedTest()) {
      reapable = true;
      break;
    }
    Scheduler::instance().yield();
  }
  if (!reapable) {
    childThread->joinForCompletion();
    reapable = child->isTerminationReapableForHostedTest();
  }
  if (!reapable) {
    FATAL("SIGCHLD wait-status fixture retained an on-stack child");
  }

  const int waitStatus = static_cast<int>(static_cast<size_t>(context.waitStatus));
  bool passed = context.handlerCalls == 1 && context.handlerFailures == 0 &&
                context.handlerSawTerminated == 1 && context.handlerSawOwnerOnStack == 1 &&
                context.waitResult == context.childId && WIFEXITED(waitStatus) &&
                WEXITSTATUS(waitStatus) == ExitCode && reapable &&
                child->getState() == Process::Reaped;

  Process::ReaperClaim rescueReaper;
  bool reaperPublished = child->getState() == Process::Reaped;
  if (!reaperPublished && child->getState() == Process::Terminated) {
    auto guard = parent->acquireChildStateWait();
    if (child->getState() == Process::Terminated) {
      child->reap();
      rescueReaper = child->tryClaimReaper();
    }
  }
  if (rescueReaper) {
    rescueReaper.publish();
    reaperPublished = true;
  }
  if (!reaperPublished) {
    FATAL("SIGCHLD wait-status fixture could not publish child destruction");
  }

  context.releaseParent += 1;
  const bool parentJoined = parentStarted && parentThread->joinForCompletion();
  __atomic_store_n(&g_SigchldVisibilityContext, static_cast<SigchldVisibilityContext*>(nullptr),
                   __ATOMIC_RELEASE);

  Process* childIdentity = child;
  childLease.reset();
  Scheduler::instance().waitUntilProcessRemoved(childIdentity);
  passed &= ZombieQueue::instance().drain();
  passed &= parentJoined;
  delete parent;

  if (passed) {
    NOTICE("HOSTED-SYSCALL-TEST: PASS sigchld-wait-status-visible");
  }
  return passed;
}
}  // namespace

bool runHostedPosixExitStatusRegressions(Process* kernelProcess) {
  const bool normalPassed =
      runExitStatusFixture(kernelProcess, NormalExitCode, Subsystem::ExitCause::Normal, 0x7F, true);
  const bool signalPassed =
      runExitStatusFixture(kernelProcess, SIGUSR1, Subsystem::ExitCause::Signal, 0xFF, false);
  const bool sigchldPassed = sigchldWaitStatusVisible(kernelProcess);

  if (!normalPassed || !signalPassed || !sigchldPassed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL posix-exit-status: "
        "termination status or SIGCHLD publication was not wait-compatible");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS posix-normal-exit-status");
  NOTICE("HOSTED-SYSCALL-TEST: PASS posix-signal-exit-status");
  return true;
}
