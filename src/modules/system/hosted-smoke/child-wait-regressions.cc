/* Copyright (c) 2026, Pedigree Developers. */
#include <config.h>

#if PEDIGREE_CHILD_WAIT_TESTS && THREADS
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/ZombieQueue.h"

namespace {
constexpr size_t TimeoutSeconds = 5;
constexpr int ExitCode = 37;

bool check(bool condition, const char* detail) {
  if (!condition) {
    ERROR("CHILD-WAIT-CORE: FAIL " << detail);
  }
  return condition;
}

bool waitForState(Process& process, Process::ProcessState state) {
  const auto start = Time::getTicks();
  while (Time::getTicks() - start < TimeoutSeconds * Time::Multiplier::Second) {
    if (process.getState() == state) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}

struct StopContext {
  Semaphore ready{0}, go{0}, resumed{0};
  Atomic<bool> abort{false};
  Atomic<size_t> failures{0};
};

int stopEntry(void* parameter) {
  auto& context = *static_cast<StopContext*>(parameter);
  Process& process = *Processor::information().getCurrentThread()->getParent();
  for (size_t i = 0; i < 2; ++i) {
    const size_t epoch = process.getContinuationEpoch();
    context.ready.release();
    if (!context.go.acquire(1, TimeoutSeconds)) {
      context.failures += 1;
      break;
    }
    if (context.abort) {
      break;
    }
    // Cleanup's resume invalidates a stop that has not yet entered its gate.
    process.suspendIfContinuationEpoch(19 + i, epoch);
    context.resumed.release();
  }
  return 0;
}

bool selection(Process& parent, Process& child, bool stopped, bool continued, bool consume,
               Process::ChildTransitionKind expected, int signal = 0) {
  auto guard = parent.acquireChildStateWait();
  Process::ChildTransition transition;
  transition.kind = Process::ChildTransitionKind::Stopped;
  transition.stopSignal = -1;
  const bool selected = child.selectPendingChildTransition(stopped, continued, consume, transition);
  return selected == (expected != Process::ChildTransitionKind::None) &&
         transition.kind == expected && transition.stopSignal == signal;
}

bool transitionSelection(Process& parent) {
  auto* child = new Process(&parent, true);
  if (!check(child != nullptr, "transition process allocation")) {
    return false;
  }
  StopContext context;
  auto* worker = new Thread(child, stopEntry, &context, nullptr, false, true, true);
  if (!worker) {
    delete child;
    return check(false, "transition worker allocation");
  }
  worker->setName("child wait transition");
  const bool started = worker->start();
  using Kind = Process::ChildTransitionKind;
  bool passed = check(started, "transition worker startup");
  if (passed) {
    passed = [&]() {
      if (!check(context.ready.acquire(1, TimeoutSeconds), "first stop entry")) {
        return false;
      }
      context.go.release();
      if (!check(waitForState(*child, Process::Suspended), "first stop publication")) {
        return false;
      }
      bool ok = check(selection(parent, *child, false, false, true, Kind::None) &&
                          selection(parent, *child, false, true, true, Kind::None) &&
                          selection(parent, *child, true, false, false, Kind::Stopped, 19) &&
                          selection(parent, *child, true, true, false, Kind::Stopped, 19) &&
                          selection(parent, *child, true, false, true, Kind::Stopped, 19) &&
                          selection(parent, *child, true, true, true, Kind::None) &&
                          child->getState() == Process::Suspended,
                      "stop peek/consume or unmatched-class preservation");
      child->resume();
      if (!check(context.resumed.acquire(1, TimeoutSeconds) &&
                     context.ready.acquire(1, TimeoutSeconds),
                 "first resume and second stop entry")) {
        return false;
      }
      ok &= check(selection(parent, *child, true, false, true, Kind::None) &&
                      selection(parent, *child, false, true, false, Kind::Continued) &&
                      selection(parent, *child, true, true, false, Kind::Continued) &&
                      selection(parent, *child, false, true, true, Kind::Continued) &&
                      selection(parent, *child, true, true, false, Kind::None),
                  "continue peek/consume or unmatched-class preservation");
      context.go.release();
      if (!check(waitForState(*child, Process::Suspended), "second stop publication")) {
        return false;
      }
      ok &= check(selection(parent, *child, true, false, false, Kind::Stopped, 20),
                  "second stop metadata");
      child->resume();
      if (!check(context.resumed.acquire(1, TimeoutSeconds), "second resume")) {
        return false;
      }
      ok &= check(selection(parent, *child, true, false, true, Kind::None) &&
                      selection(parent, *child, false, true, true, Kind::Continued) &&
                      selection(parent, *child, true, true, false, Kind::None),
                  "resume did not replace the unconsumed stop");
      return ok;
    }();
  }
  context.abort = true;
  child->resume();
  context.go.release();
  if (!started) {
    worker->setUnwindState(Thread::TerminateThread);
  }
  if (!worker->joinForCompletion()) {
    FATAL("CHILD-WAIT-CORE: transition worker could not retire safely");
  }
  passed &= check(context.failures == 0, "transition worker timed out");
  delete child;
  if (passed) {
    NOTICE("CHILD-WAIT-CORE: PASS transition selection");
  }
  return passed;
}

struct TerminalContext {
  Semaphore ready{0}, finish{0};
  Atomic<size_t> destroyed{0};
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  Process* target = nullptr;
  Atomic<Thread*> reaper{nullptr};
  Atomic<size_t> reapableCalls{0};
#endif
};

class ObservedProcess final : public Process {
 public:
  ObservedProcess(Process& parent, TerminalContext& context)
      : Process(DeferredPublication(), &parent, true), m_Context(context) {
    publish();
  }
  ~ObservedProcess() override {
    prepareForDestruction();
    m_Context.destroyed += 1;
  }

 private:
  TerminalContext& m_Context;
};

int exitEntry(void* parameter) {
  auto& context = *static_cast<TerminalContext*>(parameter);
  context.ready.release();
  const bool released = context.finish.acquire(1, TimeoutSeconds);
  Process& process = *Processor::information().getCurrentThread()->getParent();
  if (!process.beginTermination(released ? ExitCode : 1) || !process.quiesceTermination()) {
    FATAL("CHILD-WAIT-CORE: child failed to own its terminal teardown");
  }
  // The subsystem normally supplies the encoded status; this is a core-only child.
  process.setExitStatus((released ? ExitCode : 1) << 8);
  process.finishTermination();
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
TerminalContext* g_TerminalContext = nullptr;

void observeReaper(Process* process, ZombieProcess::ReapPhase phase) {
  auto* context = __atomic_load_n(&g_TerminalContext, __ATOMIC_ACQUIRE);
  if (!context || context->target != process) {
    return;
  }
  if (phase == ZombieProcess::ReapPhase::Reapable) {
    context->reapableCalls += 1;
    context->reaper = Processor::information().getCurrentThread();
  }
}

bool waitForObserverDrain(TerminalContext& context) {
  const auto start = Time::getTicks();
  while (Time::getTicks() - start < TimeoutSeconds * Time::Multiplier::Second) {
    Thread* reaper = context.reaper;
    Thread::WaitDebugInfo info = {};
    if (reaper && reaper->getWaitDebugInfo(info) && info.queued &&
        info.channelOwner == context.target && reaper->getStatus() == Thread::Sleeping) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}
#endif

bool terminalObserver(Process& parent) {
  TerminalContext context;
  auto* child = new ObservedProcess(parent, context);
  if (!check(child != nullptr, "terminal process allocation")) {
    return false;
  }
  auto* worker = new Thread(child, exitEntry, &context, nullptr, false, true, true);
  if (!worker) {
    delete child;
    return check(false, "terminal worker allocation");
  }
  worker->setName("child wait terminal");
  if (!worker->start()) {
    worker->setUnwindState(Thread::TerminateThread);
    if (!worker->joinForCompletion()) {
      FATAL("CHILD-WAIT-CORE: failed terminal startup could not retire safely");
    }
    delete child;
    return check(false, "terminal worker startup");
  }
  bool passed = check(context.ready.acquire(1, TimeoutSeconds), "terminal worker entry");
  context.finish.release();
  if (!waitForState(*child, Process::Terminated)) {
    FATAL("CHILD-WAIT-CORE: terminal publication timed out");
  }

  const size_t pid = child->getId();
  const auto userBefore = parent.getReapedChildrenUserTime();
  const auto kernelBefore = parent.getReapedChildrenKernelTime();
  Scheduler::ProcessLease observer;
  {
    auto guard = parent.acquireChildStateWait();
    passed &= check(child->getState() == Process::Terminated &&
                        Scheduler::instance().acquireProcess(observer, child),
                    "terminal observer admission under the child-state guard");
  }
  if (!observer || !observer->waitUntilTerminationReapable()) {
    FATAL("CHILD-WAIT-CORE: terminal observer could not retain an off-stack child");
  }
  const int status = observer->getExitStatus();
  const auto childUser = observer->getUserTime() + observer->getReapedChildrenUserTime();
  const auto childKernel = observer->getKernelTime() + observer->getReapedChildrenKernelTime();
  passed &= check(status == (ExitCode << 8) && observer->getState() == Process::Terminated &&
                      parent.getReapedChildrenUserTime() == userBefore &&
                      parent.getReapedChildrenKernelTime() == kernelBefore,
                  "terminal observation consumed status or accounted CPU time");

  Process::ReaperClaim claim;
  {
    auto guard = parent.acquireChildStateWait();
    claim = child->tryClaimReaper();
    if (!claim) {
      FATAL("CHILD-WAIT-CORE: terminal observer prevented the sole reaper claim");
    }
    child->reap();
    auto duplicate = child->tryClaimReaper();
    passed &= check(!duplicate, "a competing consumer claimed the child twice");
  }
  Time::Timestamp user = 0, kernel = 0;
  parent.accountReapedChild(child, user, kernel);
  passed &= check(user == childUser && kernel == childKernel &&
                      parent.getReapedChildrenUserTime() == userBefore + childUser &&
                      parent.getReapedChildrenKernelTime() == kernelBefore + childKernel,
                  "sole consuming claim did not account the final CPU totals once");
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  context.target = child;
  __atomic_store_n(&g_TerminalContext, &context, __ATOMIC_RELEASE);
  ZombieProcess::setReapHook(observeReaper);
#endif
  claim.publish();

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  passed &= check(waitForObserverDrain(context) && context.reapableCalls == 1 && !context.destroyed,
                  "published reaper did not wait for the retained observer before destruction");
  Scheduler::ProcessLease late;
  passed &= check(!Scheduler::instance().acquireProcessById(late, pid) && !late,
                  "removed child admitted a new observer");
  late.reset();
#else
  (void)pid;
  NOTICE("CHILD-WAIT-CORE: SKIP exact reaper wait (hosted hook only)");
#endif
  passed &= check(observer->getExitStatus() == status && observer->getState() == Process::Reaped &&
                      !context.destroyed && !observer->tryClaimReaper() &&
                      parent.getReapedChildrenUserTime() == userBefore + childUser &&
                      parent.getReapedChildrenKernelTime() == kernelBefore + childKernel,
                  "retained terminal snapshot or once-only accounting changed after publication");
  // ProcessLease and ReaperClaim remain on their acquiring thread. Release
  // the observer outside both state guards so the real reaper can finish.
  observer.reset();
  const auto releasedAt = Time::getTicks();
  while (!context.destroyed &&
         Time::getTicks() - releasedAt < TimeoutSeconds * Time::Multiplier::Second) {
    Scheduler::instance().yield();
  }
  if (!context.destroyed) {
    FATAL("CHILD-WAIT-CORE: observer release did not unblock destruction");
  }
  const bool drained = ZombieQueue::instance().drain();
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  ZombieProcess::setReapHook(nullptr);
  __atomic_store_n(&g_TerminalContext, static_cast<TerminalContext*>(nullptr), __ATOMIC_RELEASE);
#endif
  if (!drained) {
    FATAL("CHILD-WAIT-CORE: child destruction queue did not drain safely");
  }
  passed &=
      check(context.destroyed == 1 && parent.getReapedChildrenUserTime() == userBefore + user &&
                parent.getReapedChildrenKernelTime() == kernelBefore + kernel,
            "observer release did not complete exactly one destruction");
  if (passed) {
    NOTICE("CHILD-WAIT-CORE: PASS terminal observer and sole reaper");
  }
  return passed;
}
}  // namespace

bool runChildWaitRegressions() {
  NOTICE("CHILD-WAIT-CORE: BEGIN");
  auto* parent = new Process(Scheduler::instance().getKernelProcess(), true);
  if (!check(parent != nullptr, "isolated parent allocation")) {
    return false;
  }
  const bool passed = transitionSelection(*parent) && terminalObserver(*parent);
  delete parent;
  if (passed) {
    NOTICE("CHILD-WAIT-CORE: END PASS");
  }
  return passed;
}
#endif
