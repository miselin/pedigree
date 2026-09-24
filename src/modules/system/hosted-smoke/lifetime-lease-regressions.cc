/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/process/OwnedThread.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/utilities/UniqueResource.h"

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

  void prepareForDestructionForHostedTest() {
    prepareForDestruction();
  }

 private:
  Atomic<size_t>* m_Destroyed;
};

class ObservedThread : public Thread {
 public:
  ObservedThread(Process* parent, ThreadStartFunc entry, Atomic<size_t>* destroyed,
                 bool delayedStart = false, void* parameter = nullptr)
      : Thread(parent, entry, parameter, nullptr, false, true, delayedStart),
        m_Destroyed(destroyed) {}

  ~ObservedThread() override {
    *m_Destroyed += 1;
  }

 private:
  Atomic<size_t>* m_Destroyed;
};

struct OpenFinalLeaseContext {
  explicit OpenFinalLeaseContext(Process* parent)
      : parent(parent),
        target(nullptr),
        exitGate(0),
        entered(0),
        returned(0),
        hookCalls(0),
        safetyAcquired(0),
        gateReleased(0),
        reapableObserved(0),
        wakeAttempts(0),
        failures(0),
        destroyed(0) {}

  Process* parent;
  Thread* target;
  Process::ThreadLease safetyLease;
  Semaphore exitGate;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
  Atomic<size_t> hookCalls;
  Atomic<size_t> safetyAcquired;
  Atomic<size_t> gateReleased;
  Atomic<size_t> reapableObserved;
  Atomic<size_t> wakeAttempts;
  Atomic<size_t> failures;
  Atomic<size_t> destroyed;
};

struct OpenFinalProcessLeaseContext {
  explicit OpenFinalProcessLeaseContext(Process* process)
      : process(process),
        deleter(nullptr),
        deleteGate(0),
        entered(0),
        finished(0),
        hookCalls(0),
        safetyAcquired(0),
        gateReleased(0),
        drainObserved(0),
        wakeAttempts(0),
        failures(0),
        destroyed(0) {}

  Process* process;
  Thread* deleter;
  Scheduler::ProcessLease safetyLease;
  Semaphore deleteGate;
  Atomic<size_t> entered;
  Atomic<size_t> finished;
  Atomic<size_t> hookCalls;
  Atomic<size_t> safetyAcquired;
  Atomic<size_t> gateReleased;
  Atomic<size_t> drainObserved;
  Atomic<size_t> wakeAttempts;
  Atomic<size_t> failures;
  Atomic<size_t> destroyed;
};

struct ClosedFinalProcessLeaseContext {
  explicit ClosedFinalProcessLeaseContext(ObservedProcess* process)
      : process(process),
        drainer(nullptr),
        beginDrain(0),
        entered(0),
        returned(0),
        hookCalls(0),
        drainWaitObserved(0),
        earlyReturnObserved(0),
        gateReleased(0),
        wakeAttempts(0),
        failures(0),
        destroyed(0) {}

  ObservedProcess* process;
  Thread* drainer;
  Semaphore beginDrain;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
  Atomic<size_t> hookCalls;
  Atomic<size_t> drainWaitObserved;
  Atomic<size_t> earlyReturnObserved;
  Atomic<size_t> gateReleased;
  Atomic<size_t> wakeAttempts;
  Atomic<size_t> failures;
  Atomic<size_t> destroyed;
};

OpenFinalLeaseContext* g_OpenFinalLeaseContext = nullptr;
OpenFinalProcessLeaseContext* g_OpenFinalProcessLeaseContext = nullptr;
ClosedFinalProcessLeaseContext* g_ClosedFinalProcessLeaseContext = nullptr;

bool waitForLeaseDrain(Thread* waiter, const void* owner);

int blockedOpenFinalLeaseTarget(void* parameter) {
  OpenFinalLeaseContext* context = reinterpret_cast<OpenFinalLeaseContext*>(parameter);
  context->entered += 1;
  if (!context->exitGate.acquire()) {
    context->failures += 1;
  }
  context->returned += 1;
  return 0;
}

void observeOpenFinalLeaseRelease(Thread* target, Thread::ExternalLeaseReleasePhase phase) {
  OpenFinalLeaseContext* context = __atomic_load_n(&g_OpenFinalLeaseContext, __ATOMIC_ACQUIRE);
  if (!context || target != context->target) {
    return;
  }

  if (phase == Thread::ExternalLeaseBeforeWaiterWake) {
    context->wakeAttempts += 1;
    return;
  }

  context->hookCalls += 1;
  if (!context->parent->acquireThread(context->safetyLease, target)) {
    context->failures += 1;
    return;
  }

  context->safetyAcquired += 1;
  context->gateReleased += 1;
  context->exitGate.release();
  for (size_t attempt = 0; attempt < Attempts; ++attempt) {
    if (target->isReapableForHostedTest()) {
      context->reapableObserved += 1;
      return;
    }
    Scheduler::instance().yield();
  }
  context->failures += 1;
}

int deleteOpenFinalLeasedProcess(void* parameter) {
  OpenFinalProcessLeaseContext* context =
      reinterpret_cast<OpenFinalProcessLeaseContext*>(parameter);
  TerminationDeferral lifecycle;
  context->entered += 1;
  if (!context->deleteGate.acquire()) {
    context->failures += 1;
  }
  delete context->process;
  context->finished += 1;
  return 0;
}

void observeOpenFinalProcessLeaseRelease(Process* process,
                                         Process::ExternalLeaseReleasePhase phase) {
  OpenFinalProcessLeaseContext* context =
      __atomic_load_n(&g_OpenFinalProcessLeaseContext, __ATOMIC_ACQUIRE);
  if (!context || process != context->process) {
    return;
  }

  if (phase == Process::ExternalLeaseBeforeWaiterWake) {
    context->wakeAttempts += 1;
    return;
  }

  context->hookCalls += 1;
  if (!Scheduler::instance().acquireProcess(context->safetyLease, process)) {
    context->failures += 1;
    return;
  }

  context->safetyAcquired += 1;
  context->gateReleased += 1;
  context->deleteGate.release();
  if (waitForLeaseDrain(context->deleter, process)) {
    context->drainObserved += 1;
  } else {
    context->failures += 1;
  }
}

int drainClosedFinalLeasedProcess(void* parameter) {
  ClosedFinalProcessLeaseContext* context =
      reinterpret_cast<ClosedFinalProcessLeaseContext*>(parameter);
  TerminationDeferral lifecycle;
  context->entered += 1;
  if (!context->beginDrain.acquire()) {
    context->failures += 1;
  }
  context->process->prepareForDestructionForHostedTest();
  context->returned += 1;
  return 0;
}

void observeClosedFinalProcessLeaseRelease(Process* process,
                                           Process::ExternalLeaseReleasePhase phase) {
  ClosedFinalProcessLeaseContext* context =
      __atomic_load_n(&g_ClosedFinalProcessLeaseContext, __ATOMIC_ACQUIRE);
  if (!context || process != context->process) {
    return;
  }

  if (phase == Process::ExternalLeaseBeforeWaiterWake) {
    context->wakeAttempts += 1;
    return;
  }

  context->hookCalls += 1;
  context->gateReleased += 1;
  context->beginDrain.release();
  for (size_t attempt = 0; attempt < Attempts; ++attempt) {
    if (context->returned) {
      context->earlyReturnObserved += 1;
      return;
    }

    Thread::WaitDebugInfo info = {};
    if (context->drainer->getWaitDebugInfo(info) && info.queued && info.channelOwner == process &&
        context->drainer->getStatus() == Thread::Sleeping) {
      context->drainWaitObserved += 1;
      return;
    }
    if (context->drainer->getStatus() == Thread::Zombie) {
      context->earlyReturnObserved += 1;
      return;
    }
    Scheduler::instance().yield();
  }
  context->failures += 1;
}

struct ProcessDeleteContext {
  ProcessDeleteContext(Process* process, Atomic<size_t>* destroyed)
      : process(process), destroyed(destroyed), entered(0), finished(0) {}

  Process* process;
  Atomic<size_t>* destroyed;
  Atomic<size_t> entered;
  Atomic<size_t> finished;
};

struct OwnedWorkerContext {
  OwnedWorkerContext()
      : gate(0),
        worker(nullptr),
        entered(0),
        returnedPastWait(0),
        waitInterrupted(0),
        destructed(0),
        destructedBeforeJoin(0),
        joins(0) {}

  Semaphore gate;
  Thread* worker;
  Atomic<size_t> entered;
  Atomic<size_t> returnedPastWait;
  Atomic<size_t> waitInterrupted;
  Atomic<size_t> destructed;
  Atomic<size_t> destructedBeforeJoin;
  Atomic<size_t> joins;
};

class OwnedWorkerStackCanary {
 public:
  explicit OwnedWorkerStackCanary(OwnedWorkerContext* context) : m_Context(context) {}

  ~OwnedWorkerStackCanary() {
    m_Context->destructed += 1;
  }

 private:
  OwnedWorkerContext* m_Context;
};

OwnedWorkerContext* g_OwnedWorkerContext = nullptr;

void observeOwnedWorkerJoin(Thread* target, Process*) {
  if (g_OwnedWorkerContext && target == g_OwnedWorkerContext->worker) {
    if (g_OwnedWorkerContext->destructed == 1) {
      g_OwnedWorkerContext->destructedBeforeJoin += 1;
    }
    g_OwnedWorkerContext->joins += 1;
  }
}

int blockedOwnedWorker(void* parameter) {
  OwnedWorkerContext* context = reinterpret_cast<OwnedWorkerContext*>(parameter);
  OwnedWorkerStackCanary stackCanary(context);
  context->entered += 1;
  context->waitInterrupted = context->gate.acquire() ? 0 : 1;
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

  const bool passed = check(waiting && context.entered == 1 && context.waitInterrupted == 1 &&
                                context.returnedPastWait == 1 && context.destructed == 1 &&
                                context.destructedBeforeJoin == 1 && context.joins == 1,
                            "OwnedThread did not let its blocked worker unwind and join "
                            "exactly once");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS owned-thread-terminal-join");
  }
  return passed;
}

bool idleOnlyAdmissionClose() {
  OperationBarrier barrier;
  OperationBarrier::Lease first, second;
  bool passed = check(barrier.tryAcquire(first) && !barrier.tryCloseIfIdle() && barrier.isOpen() &&
                          barrier.tryAcquire(second),
                      "busy close changed operation admission");
  first = OperationBarrier::Lease();
  passed &= check(!barrier.tryCloseIfIdle(), "busy close lost a remaining admission");
  second = OperationBarrier::Lease();
  passed &=
      check(barrier.tryCloseIfIdle() && barrier.isClosedAndDrained() && !barrier.tryAcquire(first),
            "idle close did not prevent subsequent admission");
  barrier.closeAndWait();
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS idle-only-admission-close");
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

bool openFinalProcessLeaseRelease(Process* kernelProcess) {
  OpenFinalProcessLeaseContext context(nullptr);
  ObservedProcess* process = new ObservedProcess(kernelProcess, &context.destroyed);
  context.process = process;

  Scheduler::ProcessLease lease;
  const bool leaseAcquired = Scheduler::instance().acquireProcess(lease, process);
  if (!leaseAcquired) {
    delete process;
    return check(false, "could not acquire the open-admission ProcessLease");
  }

  Thread* deleter = new Thread(kernelProcess, deleteOpenFinalLeasedProcess, &context);
  deleter->setName("hosted open final ProcessLease deleter");
  context.deleter = deleter;
  const bool waiting = waitForLeaseDrain(deleter, &context.deleteGate);
  if (!waiting) {
    context.deleteGate.release();
    lease.reset();
    if (!deleter->joinForCompletion()) {
      FATAL("Open final ProcessLease cleanup could not join its deleter");
    }
    return check(false, "could not prepare the open-admission ProcessLease release window");
  }

  __atomic_store_n(&g_OpenFinalProcessLeaseContext, &context, __ATOMIC_RELEASE);
  Process::setExternalLeaseReleaseHookForHostedTest(process, observeOpenFinalProcessLeaseRelease);
  lease.reset();
  Process::setExternalLeaseReleaseHookForHostedTest(nullptr, nullptr);
  __atomic_store_n(&g_OpenFinalProcessLeaseContext,
                   static_cast<OpenFinalProcessLeaseContext*>(nullptr), __ATOMIC_RELEASE);

  if (!context.gateReleased) {
    context.deleteGate.release();
  }
  context.safetyLease.reset();
  const bool joined = deleter->joinForCompletion();

  const bool passed = check(
      joined && context.entered == 1 && context.finished == 1 && context.hookCalls == 1 &&
          context.safetyAcquired == 1 && context.gateReleased == 1 && context.drainObserved == 1 &&
          context.wakeAttempts == 0 && !context.failures && context.destroyed == 1,
      "an open final ProcessLease release touched its target after the predicate unlock");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS process-final-open-lease-release");
  }
  return passed;
}

bool closedFinalProcessLeaseHandoff(Process* kernelProcess) {
  ClosedFinalProcessLeaseContext context(nullptr);
  ObservedProcess* process = new ObservedProcess(kernelProcess, &context.destroyed);
  context.process = process;

  Scheduler::ProcessLease lease;
  if (!Scheduler::instance().acquireProcess(lease, process)) {
    delete process;
    return check(false, "could not acquire the closed-handoff ProcessLease");
  }

  Thread* drainer = new Thread(kernelProcess, drainClosedFinalLeasedProcess, &context);
  drainer->setName("hosted closed final ProcessLease drainer");
  context.drainer = drainer;
  if (!waitForLeaseDrain(drainer, &context.beginDrain)) {
    context.beginDrain.release();
    lease.reset();
    if (!drainer->joinForCompletion()) {
      FATAL("Closed final ProcessLease cleanup could not join its drainer");
    }
    delete process;
    return check(false, "could not prepare the closed final ProcessLease handoff");
  }

  // Close enumeration admission before the final release, but do not let the
  // destruction drain enrol until the post-unlock hook opens its gate.
  Scheduler::instance().removeProcess(process);
  __atomic_store_n(&g_ClosedFinalProcessLeaseContext, &context, __ATOMIC_RELEASE);
  Process::setExternalLeaseReleaseHookForHostedTest(process, observeClosedFinalProcessLeaseRelease);
  lease.reset();
  Process::setExternalLeaseReleaseHookForHostedTest(nullptr, nullptr);
  __atomic_store_n(&g_ClosedFinalProcessLeaseContext,
                   static_cast<ClosedFinalProcessLeaseContext*>(nullptr), __ATOMIC_RELEASE);

  if (!context.gateReleased) {
    context.beginDrain.release();
  }
  bool drainerReapable = false;
  for (size_t attempt = 0; attempt < Attempts; ++attempt) {
    if (drainer->isReapableForHostedTest()) {
      drainerReapable = true;
      break;
    }
    Scheduler::instance().yield();
  }
  if (!drainerReapable || !drainer->joinForCompletion()) {
    FATAL("Closed final ProcessLease drainer did not retire after handoff");
  }

  bool passed = check(context.entered == 1 && context.returned == 1 && context.hookCalls == 1 &&
                          context.drainWaitObserved == 1 && !context.earlyReturnObserved &&
                          context.gateReleased == 1 && context.wakeAttempts == 1 &&
                          !context.failures && context.destroyed == 0,
                      "a closed final ProcessLease did not pin its pre-enrolment waiter handoff");
  delete process;
  passed &= check(context.destroyed == 1,
                  "the closed final ProcessLease fixture did not destroy its process once");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS process-final-closed-lease-handoff");
  }
  return passed;
}

bool threadLeaseIdLookup(Process* kernelProcess) {
  Atomic<size_t> destroyed(0);
  Thread* first = new ObservedThread(kernelProcess, immediateExit, &destroyed, true);
  Thread* second = new ObservedThread(kernelProcess, immediateExit, &destroyed, true);
  first->setName("hosted ThreadLease ID first target");
  second->setName("hosted ThreadLease ID second target");

  Process::ThreadLease lease;
  const bool firstAcquired = kernelProcess->acquireThreadById(lease, first->getId());
  bool passed = check(firstAcquired && lease.get() == first,
                      "exact thread ID lookup did not acquire its target");

  const bool secondAcquired = kernelProcess->acquireThreadById(lease, second->getId());
  passed &= check(secondAcquired && lease.get() == second,
                  "exact thread ID lookup did not replace an active lease");

  const bool missingAcquired = kernelProcess->acquireThreadById(lease, ~static_cast<size_t>(0));
  passed &=
      check(!missingAcquired && !lease, "missing thread ID lookup did not reset an active lease");

  first->start();
  second->start();
  const bool firstJoined = first->joinForCompletion();
  const bool secondJoined = second->joinForCompletion();
  passed &= check(firstJoined && secondJoined && destroyed == 2,
                  "thread ID lookup fixtures did not retire cleanly");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS thread-lease-id-lookup");
  }
  return passed;
}

bool threadLeaseBarrier(Process* kernelProcess) {
  Atomic<size_t> destroyed(0);
  Thread* target = new ObservedThread(kernelProcess, immediateExit, &destroyed, true);
  target->setName("hosted ThreadLease target");

  // Pin the target before making it runnable so the test controls the
  // retirement window instead of racing the immediate-exit trampoline.
  Process::ThreadLease lease;
  const bool leaseAcquired = kernelProcess->acquireThreadById(lease, target->getId());
  bool passed =
      check(leaseAcquired && lease.get() == target, "could not acquire the thread lease by ID");
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
    if (!kernelProcess->acquireThreadById(lateLease, target->getId())) {
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

bool openFinalThreadLeaseRelease(Process* kernelProcess) {
  OpenFinalLeaseContext context(kernelProcess);
  Thread* target = new ObservedThread(kernelProcess, blockedOpenFinalLeaseTarget,
                                      &context.destroyed, true, &context);
  target->setName("hosted open final ThreadLease target");
  context.target = target;

  Process::ThreadLease lease;
  const bool leaseAcquired = kernelProcess->acquireThread(lease, target);
  if (!leaseAcquired) {
    target->setUnwindState(Thread::TerminateThread);
    target->joinForCompletion();
    return check(false, "could not acquire the open-admission ThreadLease");
  }

  const bool started = target->start();
  const bool waiting = started && waitForLeaseDrain(target, &context.exitGate);
  if (!started || !waiting) {
    target->setUnwindState(Thread::TerminateThread);
    lease.reset();
    if (!target->joinForCompletion()) {
      FATAL("Open final ThreadLease cleanup could not join its target");
    }
    return check(false, "could not prepare the open-admission final-release window");
  }

  if (!target->detach()) {
    target->setUnwindState(Thread::TerminateThread);
    lease.reset();
    if (!target->joinForCompletion()) {
      FATAL("Open final ThreadLease cleanup could not join its undetached target");
    }
    return check(false, "could not detach the open-admission final-release target");
  }

  __atomic_store_n(&g_OpenFinalLeaseContext, &context, __ATOMIC_RELEASE);
  Thread::setExternalLeaseReleaseHookForHostedTest(target, observeOpenFinalLeaseRelease);
  lease.reset();
  Thread::setExternalLeaseReleaseHookForHostedTest(nullptr, nullptr);
  __atomic_store_n(&g_OpenFinalLeaseContext, static_cast<OpenFinalLeaseContext*>(nullptr),
                   __ATOMIC_RELEASE);

  if (!context.gateReleased) {
    context.exitGate.release();
  }
  context.safetyLease.reset();
  for (size_t attempt = 0; attempt < Attempts && !context.destroyed; ++attempt) {
    Scheduler::instance().yield();
  }
  if (!context.destroyed) {
    FATAL("Open final ThreadLease target did not complete detached retirement");
  }

  const bool passed =
      check(context.entered == 1 && context.returned == 1 && context.hookCalls == 1 &&
                context.safetyAcquired == 1 && context.gateReleased == 1 &&
                context.reapableObserved == 1 && context.wakeAttempts == 0 && !context.failures &&
                context.destroyed == 1,
            "an open final ThreadLease release touched its target after the predicate unlock");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS thread-final-open-lease-release");
  }
  return passed;
}
}  // namespace

bool runHostedLifetimeLeaseRegressions() {
  Process* kernelProcess = Scheduler::instance().getKernelProcess();
  const bool passed =
      idleOnlyAdmissionClose() && ownedThreadTerminalJoin(kernelProcess) &&
      processLeaseBarrier(kernelProcess) && openFinalProcessLeaseRelease(kernelProcess) &&
      closedFinalProcessLeaseHandoff(kernelProcess) && threadLeaseIdLookup(kernelProcess) &&
      threadLeaseBarrier(kernelProcess) && openFinalThreadLeaseRelease(kernelProcess);
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS lifetime-leases");
  }
  return passed;
}
