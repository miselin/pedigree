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
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"

#include <signal.h>

#include "modules/subsys/posix/PosixProcess.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/signal-syscalls.h"
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
                          int previousStatus) {
  ExitStatusContext* context = new ExitStatusContext(code, cause);
  PosixProcess* process = new PosixProcess(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  process->setExitStatus(previousStatus);
  Thread* thread = new Thread(process, deferredPosixExit, context, nullptr, false, true, true);
  thread->setName("hosted POSIX exit-status fixture");
  process->publish();

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
  const bool passed = reapable && context->entered == 1 && statusValid;

  if (reapable) {
    delete process;
    delete context;
  }
  return passed;
}
}  // namespace

bool runHostedPosixExitStatusRegressions(Process* kernelProcess) {
  const bool normalPassed =
      runExitStatusFixture(kernelProcess, NormalExitCode, Subsystem::ExitCause::Normal, 0x7F);
  const bool signalPassed =
      runExitStatusFixture(kernelProcess, SIGUSR1, Subsystem::ExitCause::Signal, 0xFF);

  if (!normalPassed || !signalPassed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL posix-exit-status: "
        "normal or signal termination did not publish a compatible wait status");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS posix-normal-exit-status");
  NOTICE("HOSTED-SYSCALL-TEST: PASS posix-signal-exit-status");
  return true;
}
