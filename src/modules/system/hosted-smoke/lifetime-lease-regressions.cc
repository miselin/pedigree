/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/OwnedThread.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"

namespace {
constexpr size_t Attempts = 10000;

bool check(bool condition, const char* detail) {
  if (condition) {
    return true;
  }
  ERROR("HOSTED-WAIT-TEST: FAIL lifetime-leases: " << detail);
  return false;
}

class ObservedProcess : public Process {
 public:
  ObservedProcess(Process* parent, Atomic<size_t>* destroyed)
      : Process(DeferredPublication(), parent), m_Destroyed(destroyed) {
    publish();
  }

  ~ObservedProcess() override {
    // Derived state must remain intact until external inspectors drain.
    prepareForDestruction();
    *m_Destroyed += 1;
  }

 private:
  Atomic<size_t>* m_Destroyed;
};

class ObservedThread : public Thread {
 public:
  ObservedThread(Process* parent, ThreadStartFunc entry, Atomic<size_t>* destroyed,
                 bool delayedStart = false)
      : Thread(parent, entry, nullptr, nullptr, false, true, delayedStart),
        m_Destroyed(destroyed) {}

  ~ObservedThread() override {
    *m_Destroyed += 1;
  }

 private:
  Atomic<size_t>* m_Destroyed;
};

struct ProcessDeleteContext {
  ProcessDeleteContext(Process* process, Atomic<size_t>* destroyed)
      : process(process), destroyed(destroyed), entered(0), finished(0) {}

  Process* process;
  Atomic<size_t>* destroyed;
  Atomic<size_t> entered;
  Atomic<size_t> finished;
};

struct OwnedWorkerContext {
  OwnedWorkerContext() : gate(0), worker(nullptr), entered(0), returnedPastWait(0), joins(0) {}

  Semaphore gate;
  Thread* worker;
  Atomic<size_t> entered;
  Atomic<size_t> returnedPastWait;
  Atomic<size_t> joins;
};

OwnedWorkerContext* g_OwnedWorkerContext = nullptr;

void observeOwnedWorkerJoin(Thread* target, Process*) {
  if (g_OwnedWorkerContext && target == g_OwnedWorkerContext->worker) {
    g_OwnedWorkerContext->joins += 1;
  }
}

int blockedOwnedWorker(void* parameter) {
  OwnedWorkerContext* context = reinterpret_cast<OwnedWorkerContext*>(parameter);
  context->entered += 1;
  context->gate.acquire();
  context->returnedPastWait += 1;
  return 0;
}

int deleteLeasedProcess(void* parameter) {
  ProcessDeleteContext* context = reinterpret_cast<ProcessDeleteContext*>(parameter);
  // Deletion and its ownership handoff form one lifecycle operation. A
  // terminal request may wake the lease drain but cannot cut that operation
  // between reclaiming the object and publishing completion.
  TerminationDeferral lifecycle;
  context->entered += 1;
  delete context->process;
  context->finished += 1;
  return 0;
}

int immediateExit(void*) {
  return 0;
}

bool waitForLeaseDrain(Thread* waiter, const void* owner) {
  for (size_t attempt = 0; attempt < Attempts; ++attempt) {
    Thread::WaitDebugInfo info = {};
    if (waiter->getWaitDebugInfo(info) && info.queued && info.channelOwner == owner &&
        waiter->getStatus() == Thread::Sleeping) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}

bool ownedThreadTerminalJoin(Process* kernelProcess) {
  OwnedWorkerContext context;
  bool waiting = false;
  {
    OwnedThread worker(
        new Thread(kernelProcess, blockedOwnedWorker, &context, nullptr, false, true));
    worker->setName("hosted owned blocked worker");
    context.worker = worker.get();
    waiting = waitForLeaseDrain(context.worker, &context.gate);

    g_OwnedWorkerContext = &context;
    Thread::setJoinOperationHook(observeOwnedWorkerJoin);
  }
  Thread::setJoinOperationHook(nullptr);
  g_OwnedWorkerContext = nullptr;

  const bool passed =
      check(waiting && context.entered == 1 && context.returnedPastWait == 0 && context.joins == 1,
            "OwnedThread did not terminal-cancel and join its blocked worker "
            "exactly once");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS owned-thread-terminal-join");
  }
  return passed;
}

bool processLeaseBarrier(Process* kernelProcess) {
  Atomic<size_t> destroyed(0);
  Process* process = new ObservedProcess(kernelProcess, &destroyed);
  Scheduler::ProcessLease lease;
  const bool leaseAcquired = Scheduler::instance().acquireProcess(lease, process);
  bool passed =
      check(leaseAcquired && static_cast<bool>(lease), "could not acquire the process lease");

  ProcessDeleteContext context(process, &destroyed);
  Thread* deleter = new Thread(kernelProcess, deleteLeasedProcess, &context, nullptr, false, true);
  deleter->setName("hosted ProcessLease deleter");

  const bool draining = waitForLeaseDrain(deleter, process);
  passed &= check(draining && context.entered == 1 && destroyed == 0,
                  "a held ProcessLease did not stop derived destruction");

  Scheduler::ProcessLease lateLease;
  const bool lateLeaseAcquired = Scheduler::instance().acquireProcess(lateLease, process);
  passed &= check(!lateLeaseAcquired && !lateLease,
                  "Process destruction admitted a lease after scheduler removal");

  deleter->setUnwindState(Thread::TerminateThread);
  for (size_t attempt = 0; attempt < 32; ++attempt) {
    Scheduler::instance().yield();
  }
  passed &= check(context.finished == 0 && destroyed == 0,
                  "terminal wake abandoned the ProcessLease completion drain");

  lease.reset();
  for (size_t attempt = 0; attempt < Attempts && !context.finished; ++attempt) {
    Scheduler::instance().yield();
  }
  passed &= check(context.finished == 1 && destroyed == 1,
                  "Process deletion did not complete after its final lease released");
  passed &= check(deleter->joinForCompletion(), "the ProcessLease deleter did not retire");
  return passed;
}

bool threadLeaseBarrier(Process* kernelProcess) {
  Atomic<size_t> destroyed(0);
  Thread* target = new ObservedThread(kernelProcess, immediateExit, &destroyed, true);
  target->setName("hosted ThreadLease target");

  // Pin the target before making it runnable so the test controls the
  // retirement window instead of racing the immediate-exit trampoline.
  Process::ThreadLease lease;
  const bool leaseAcquired = kernelProcess->acquireThread(lease, target);
  bool passed =
      check(leaseAcquired && static_cast<bool>(lease), "could not acquire the thread lease");
  if (!lease) {
    target->start();
    target->joinForCompletion();
    return false;
  }

  passed &= check(target->detach(), "the ThreadLease target could not detach");
  passed &= check(target->start(), "the delayed ThreadLease target did not start");

  bool retirementClosed = false;
  for (size_t attempt = 0; attempt < Attempts; ++attempt) {
    Process::ThreadLease lateLease;
    if (!kernelProcess->acquireThread(lateLease, target)) {
      retirementClosed = true;
      break;
    }
    lateLease.reset();
    Scheduler::instance().yield();
  }

  passed &= check(retirementClosed, "Thread retirement admitted a lease after closing");
  passed &= check(destroyed == 0, "a held ThreadLease did not stop detached target deletion");

  lease.reset();
  for (size_t attempt = 0; attempt < Attempts && !destroyed; ++attempt) {
    Scheduler::instance().yield();
  }
  passed &=
      check(destroyed == 1, "Thread deletion did not complete after its final lease released");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS thread-final-lease-deletion");
  }
  return passed;
}
}  // namespace

bool runHostedLifetimeLeaseRegressions() {
  Process* kernelProcess = Scheduler::instance().getKernelProcess();
  const bool passed = ownedThreadTerminalJoin(kernelProcess) &&
                      processLeaseBarrier(kernelProcess) && threadLeaseBarrier(kernelProcess);
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS lifetime-leases");
  }
  return passed;
}
