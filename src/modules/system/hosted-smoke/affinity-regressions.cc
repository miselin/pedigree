/* Copyright (c) 2026, Pedigree Developers. */
#include <config.h>

#if PEDIGREE_AFFINITY_TESTS && THREADS
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/time/Time.h"

namespace {
bool check(bool condition, const char* detail) {
  if (!condition)
    ERROR("AFFINITY-CORE: FAIL " << detail);
  return condition;
}

bool sameMask(const CpuAffinityMask& left, const CpuAffinityMask& right) {
  for (size_t i = 0; i < CpuAffinityMask::WordCount; ++i) {
    if (left.words[i] != right.words[i])
      return false;
  }
  return true;
}

struct WaitState {
  WaitQueue queue;
  Semaphore entered{0}, firstDone{0}, secondDone{0};
  Atomic<size_t> returns{0};
  Atomic<size_t> beforeGateCpu{CpuAffinityMask::MaximumCpus};
  Atomic<size_t> firstCpu{CpuAffinityMask::MaximumCpus};
  Atomic<size_t> secondCpu{CpuAffinityMask::MaximumCpus};
  Atomic<size_t> firstReason{static_cast<size_t>(WaitQueue::WakeReason::Waiting)};
  Atomic<size_t> secondReason{static_cast<size_t>(WaitQueue::WakeReason::Waiting)};
  Atomic<bool> gatePassed{false};
  bool repeat = false;
};

int waitEntry(void* parameter) {
  auto& state = *static_cast<WaitState*>(parameter);
  state.entered.release();
  WaitQueue::WakeReason reason;
  {
    auto guard = state.queue.acquire();
    reason = guard.wait(WaitQueue::Channel(&state, 1), Thread::EventWait);
  }
  state.firstReason = static_cast<size_t>(reason);
  state.beforeGateCpu = Processor::index();
  if (reason == WaitQueue::WakeReason::Signalled) {
    // The ordinary wait and its guards have retired. This owned test entry
    // has no CPU-local continuation or cleanup scope across the return gate.
    const auto result = Processor::information().getCurrentThread()->completeAffinityAtSafePoint();
    state.gatePassed = result == AffinityResult::Success && !Processor::getInterrupts();
    state.firstCpu = Processor::index();
    Processor::setInterrupts(true);
  }
  state.returns += 1;
  state.firstDone.release();
  if (!state.repeat || reason != WaitQueue::WakeReason::Signalled || !state.gatePassed)
    return 0;
  {
    auto guard = state.queue.acquire();
    reason = guard.wait(WaitQueue::Channel(&state, 2), Thread::EventWait);
  }
  state.secondReason = static_cast<size_t>(reason);
  state.secondCpu = Processor::index();
  state.returns += 1;
  state.secondDone.release();
  return 0;
}

Thread* startWaiter(WaitState& state, size_t cpu) {
  ThreadPlacement placement;
  placement.allowed.set(cpu);
  placement.migratable = true;
  Thread* current = Processor::information().getCurrentThread();
  auto* peer =
      new Thread(current->getParent(), waitEntry, &state, nullptr, false, false, true, &placement);
  if (!peer)
    return nullptr;
  peer->setPriority(current->getPriority());
  if (!peer->start()) {
    peer->setUnwindState(Thread::TerminateThread);
    if (!peer->joinForCompletion())
      FATAL("AFFINITY-CORE: failed startup could not be joined");
    return nullptr;
  }
  return peer;
}

bool waitForSleeping(Thread& peer, WaitState& state) {
  const Time::Timestamp start = Time::getTicks();
  while (Time::getTicks() - start < 5 * Time::Multiplier::Second) {
    bool sleeping;
    {
      // A successful acquisition also waits for the outgoing-stack release.
      LockGuard<Spinlock> guard(peer.getLock());
      sleeping = peer.getStatus() == Thread::Sleeping;
    }
    if (sleeping && state.queue.waiterCount() == 1)
      return true;
    Scheduler::instance().yield();
  }
  return false;
}

void retireWaiter(Thread* peer, WaitState& state, bool completed) {
  if (!completed) {
    peer->setUnwindState(Thread::TerminateThread);
    state.queue.wakeAll();
  }
  if (!peer->joinForCompletion())
    FATAL("AFFINITY-CORE: owned waiter could not be joined");
}

struct ReadyContext {
  Thread* target = nullptr;
  WaitState* state = nullptr;
  CpuAffinityMask destination;
  PerProcessorScheduler* source = nullptr;
  bool moving = false;
  size_t calls = 0;
  bool passed = true;
};

ReadyContext* g_ReadyContext = nullptr;

void beforeReadyPublication(Thread* peer) {
  auto* context = __atomic_load_n(&g_ReadyContext, __ATOMIC_ACQUIRE);
  if (!context || peer != context->target)
    return;
  ++context->calls;
  if (context->calls != 1) {
    context->passed = false;
    return;
  }
  context->passed &=
      check(Processor::getInterrupts() && !Processor::inDeviceHardIrq(), "ready hook context");
  if (!context->passed)
    return;

  uint64_t generation = 0;
  const AffinityResult requested = peer->requestAffinity(context->destination, generation);
  context->passed &=
      check(requested == AffinityResult::Success, "request while ready publication is held");
  if (requested == AffinityResult::Success) {
    context->passed &= check(peer->waitAffinity(generation) == AffinityResult::Success,
                             "affinity completion while wake is held");
  }
  ThreadPlacement placement;
  peer->snapshotPlacement(placement);
  bool correctOwner;
  {
    LockGuard<Spinlock> guard(peer->getLock());
    correctOwner = peer->getScheduler() == context->source;
  }
  context->passed &= check(correctOwner && sameMask(placement.allowed, context->destination),
                           "mask acknowledged without moving the blocked kernel continuation");

  // This is another real status publisher. It must not bypass the extracted
  // notification, including when no second CPU exists to perform migration.
  Scheduler::instance().threadStatusChanged(peer);
  for (size_t i = 0; i < 8; ++i)
    Scheduler::instance().yield();
  context->passed &= check(context->state->returns == 0 && context->state->queue.waiterCount() == 1,
                           "waiter executed or was reused before wake publication retired");
}

bool heldWake(size_t sourceCpu, size_t destinationCpu) {
  WaitState state;
  state.repeat = true;
  Thread* peer = startWaiter(state, sourceCpu);
  if (!check(peer != nullptr, "wake waiter allocation/start"))
    return false;
  bool passed = check(state.entered.acquireForCompletion(1, 5) && waitForSleeping(*peer, state),
                      "first wait enrollment");
  ReadyContext context;
  context.target = peer;
  context.state = &state;
  context.destination.set(destinationCpu);
  context.moving = sourceCpu != destinationCpu;
  {
    LockGuard<Spinlock> guard(peer->getLock());
    context.source = peer->getScheduler();
  }
  if (passed) {
    __atomic_store_n(&g_ReadyContext, &context, __ATOMIC_RELEASE);
    WaitQueue::setReadyPublicationHookForTest(peer, beforeReadyPublication);
    const bool woke =
        state.queue.wakeOne(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(&state, 1));
    WaitQueue::setReadyPublicationHookForTest(nullptr, nullptr);
    __atomic_store_n(&g_ReadyContext, static_cast<ReadyContext*>(nullptr), __ATOMIC_RELEASE);
    passed &= check(woke && context.calls == 1 && context.passed, "held wake admission");
  }
  bool completed = false;
  if (passed) {
    passed &=
        check(state.firstDone.acquireForCompletion(1, 5) && state.returns == 1 &&
                  state.beforeGateCpu == sourceCpu && state.gatePassed &&
                  state.firstCpu == destinationCpu &&
                  state.firstReason == static_cast<size_t>(WaitQueue::WakeReason::Signalled) &&
                  waitForSleeping(*peer, state),
              "source wait return, clean gate migration and second wait enrollment");
    if (passed) {
      const bool woke =
          state.queue.wakeOne(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(&state, 2));
      completed = woke && state.secondDone.acquireForCompletion(1, 5);
      passed &=
          check(completed && state.returns == 2 && state.secondCpu == destinationCpu &&
                    state.secondReason == static_cast<size_t>(WaitQueue::WakeReason::Signalled) &&
                    state.queue.waiterCount() == 0,
                "waiter reuse, single execution and final wake reason");
    }
  }
  retireWaiter(peer, state, completed);
  if (passed)
    NOTICE("AFFINITY-CORE: PASS extracted wake " << (context.moving ? "migration" : "same CPU"));
  return passed;
}

struct CancelContext {
  Thread* target = nullptr;
  Atomic<size_t> calls{0};
  Atomic<bool> safeContext{true};
};

CancelContext* g_CancelContext = nullptr;

void cancelBeforeCommit(Thread* peer) {
  auto* context = __atomic_load_n(&g_CancelContext, __ATOMIC_ACQUIRE);
  if (!context || peer != context->target)
    return;
  context->calls += 1;
  context->safeContext = Processor::getInterrupts() && !Processor::inDeviceHardIrq();
  peer->setUnwindState(Thread::TerminateThread);
}

bool cancelPending(size_t sourceCpu, size_t destinationCpu) {
  WaitState state;
  Thread* peer = startWaiter(state, sourceCpu);
  if (!check(peer != nullptr, "cancel waiter allocation/start"))
    return false;
  bool passed = check(state.entered.acquireForCompletion(1, 5) && waitForSleeping(*peer, state),
                      "cancel wait enrollment");
  CancelContext context;
  context.target = peer;
  CpuAffinityMask destination;
  destination.set(destinationCpu);
  if (passed) {
    __atomic_store_n(&g_CancelContext, &context, __ATOMIC_RELEASE);
    Thread::setAffinityCommitHookForTest(peer, cancelBeforeCommit);
    uint64_t generation = 0;
    const AffinityResult requested = peer->requestAffinity(destination, generation);
    passed &= check(requested == AffinityResult::Success, "pending request admission");
    if (requested == AffinityResult::Success) {
      passed &= check(peer->waitAffinity(generation) == AffinityResult::Terminal,
                      "pending generation terminal completion");
    }
    Thread::setAffinityCommitHookForTest(nullptr, nullptr);
    __atomic_store_n(&g_CancelContext, static_cast<CancelContext*>(nullptr), __ATOMIC_RELEASE);
    passed &= check(context.calls == 1 && context.safeContext, "terminal precommit hook");
  }
  retireWaiter(peer, state, false);
  if (passed)
    NOTICE("AFFINITY-CORE: PASS pending terminal cancellation");
  return passed;
}

struct PinContext {
  Thread* target = nullptr;
  Atomic<size_t> calls{0};
  Atomic<bool> rejected{false};
};

PinContext* g_PinContext = nullptr;

void pinBeforeCommit(Thread* peer) {
  auto* context = __atomic_load_n(&g_PinContext, __ATOMIC_ACQUIRE);
  if (!context || peer != context->target)
    return;
  context->calls += 1;
  const bool pinned = peer->tryPinLegacyUserCallbacks();
  context->rejected = !pinned;
  if (pinned)
    peer->unpinLegacyUserCallbacks();
}

bool legacyPins(size_t sourceCpu, size_t destinationCpu) {
  WaitState state;
  Thread* peer = startWaiter(state, sourceCpu);
  if (!check(peer != nullptr, "legacy pin waiter allocation/start"))
    return false;
  bool passed = check(state.entered.acquireForCompletion(1, 5) && waitForSleeping(*peer, state),
                      "legacy pin wait enrollment");
  CpuAffinityMask source, destination;
  source.set(sourceCpu);
  destination.set(destinationCpu);
  size_t pins = 0;
  for (size_t i = 0; passed && i < 2; ++i) {
    const bool pinned = peer->tryPinLegacyUserCallbacks();
    pins += pinned;
    passed &= check(pinned, "counted legacy pin admission");
  }
  const bool moving = sourceCpu != destinationCpu;
  auto rejectsExclusion = [&] {
    uint64_t generation = ~uint64_t(0);
    const auto result = peer->requestAffinity(destination, generation);
    if (result == AffinityResult::Success || result == AffinityResult::Busy)
      peer->waitAffinity(generation);
    ThreadPlacement placement;
    peer->snapshotPlacement(placement);
    return check(result == AffinityResult::Unsupported && !generation && placement.migratable &&
                     sameMask(placement.allowed, source),
                 "legacy pin rejected exclusion without changing policy");
  };
  if (passed && moving)
    passed &= rejectsExclusion();
  if (passed) {
    uint64_t generation = 0;
    const auto result = peer->requestAffinity(source, generation);
    passed &= check(result == AffinityResult::Success, "legacy pin compatible mask admission");
    if (result == AffinityResult::Success)
      passed &= check(peer->waitAffinity(generation) == AffinityResult::Success,
                      "legacy pin compatible mask acknowledgement");
  }
  if (pins) {
    peer->unpinLegacyUserCallbacks();
    --pins;
  }
  if (passed && moving)
    passed &= rejectsExclusion();
  while (pins) {
    peer->unpinLegacyUserCallbacks();
    --pins;
  }
  PinContext context;
  context.target = peer;
  if (passed) {
    if (moving) {
      __atomic_store_n(&g_PinContext, &context, __ATOMIC_RELEASE);
      Thread::setAffinityCommitHookForTest(peer, pinBeforeCommit);
    }
    uint64_t generation = 0;
    const auto result = peer->requestAffinity(destination, generation);
    passed &= check(result == AffinityResult::Success, "last legacy pin release permits policy");
    if (result == AffinityResult::Success)
      passed &= check(peer->waitAffinity(generation) == AffinityResult::Success,
                      "unpinned policy acknowledgement");
    if (moving) {
      Thread::setAffinityCommitHookForTest(nullptr, nullptr);
      __atomic_store_n(&g_PinContext, static_cast<PinContext*>(nullptr), __ATOMIC_RELEASE);
      passed &= check(context.calls == 1 && context.rejected,
                      "accepted excluding request rejects legacy pin admission");
      const bool pinned = peer->tryPinLegacyUserCallbacks();
      passed &= check(!pinned, "committed excluding mask rejects legacy pin admission");
      if (pinned)
        peer->unpinLegacyUserCallbacks();
    }
    ThreadPlacement placement;
    peer->snapshotPlacement(placement);
    bool sourceOwned;
    {
      LockGuard<Spinlock> guard(peer->getLock());
      sourceOwned = peer->getScheduler() == Scheduler::schedulerForCpu(sourceCpu);
    }
    passed &= check(sourceOwned && state.returns == 0 && sameMask(placement.allowed, destination),
                    "sleeping target acknowledges policy on original owner");
  }
  bool completed = false;
  if (passed) {
    completed =
        state.queue.wakeOne(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(&state, 1)) &&
        state.firstDone.acquireForCompletion(1, 5);
    passed &= check(completed && state.beforeGateCpu == sourceCpu && state.gatePassed &&
                        state.firstCpu == destinationCpu && state.returns == 1,
                    "unpinned target moves only at clean gate");
  }
  retireWaiter(peer, state, completed);
  if (passed)
    NOTICE("AFFINITY-CORE: PASS legacy callback pins " << (moving ? "migration" : "same CPU"));
  return passed;
}

int forbiddenEntry(void* parameter) {
  *static_cast<bool*>(parameter) = true;
  return 0;
}

bool pinnedAdmission() {
  bool entered = false;
  Thread* current = Processor::information().getCurrentThread();
  auto* peer =
      new Thread(current->getParent(), forbiddenEntry, &entered, nullptr, false, true, true);
  if (!check(peer != nullptr, "pinned waiter allocation"))
    return false;
  ThreadPlacement before, after;
  peer->snapshotPlacement(before);
  uint64_t generation = 0;
  const AffinityResult result = peer->requestAffinity(before.allowed, generation);
  peer->snapshotPlacement(after);
  bool passed = check(!before.migratable && result == AffinityResult::Pinned && !after.migratable &&
                          sameMask(before.allowed, after.allowed),
                      "fixed placement admission changed state");
  peer->setUnwindState(Thread::TerminateThread);
  if (!peer->joinForCompletion())
    FATAL("AFFINITY-CORE: unstarted pinned waiter could not be joined");
  passed &= check(!entered, "unstarted pinned waiter executed");
  if (passed)
    NOTICE("AFFINITY-CORE: PASS pinned admission");
  return passed;
}
}  // namespace

EXPORTED_PUBLIC bool runAffinityRegressions() {
  NOTICE("AFFINITY-CORE: BEGIN");
  if (!check(Processor::getInterrupts() && !Processor::inDeviceHardIrq() &&
                 Processor::information().getCurrentThread(),
             "fixture entry context"))
    return false;
  const size_t source = Processor::index();
  const CpuAffinityMask online = Scheduler::onlineAffinity();
  if (!check(online.contains(source), "source CPU online"))
    return false;
  size_t destination = source;
  for (size_t cpu = 0; cpu < CpuAffinityMask::MaximumCpus; ++cpu) {
    if (cpu != source && online.contains(cpu)) {
      destination = cpu;
      break;
    }
  }
  bool passed = pinnedAdmission() && legacyPins(source, source) && heldWake(source, source) &&
                cancelPending(source, source);
  if (destination != source) {
    passed = passed && legacyPins(source, destination) && heldWake(source, destination) &&
             cancelPending(source, destination);
  } else {
    NOTICE("AFFINITY-CORE: SKIP cross-CPU migration (one online CPU)");
  }
  if (passed)
    NOTICE("AFFINITY-CORE: END PASS");
  return passed;
}
#endif
