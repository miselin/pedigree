/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"

#include "modules/system/network-stack/Filter.h"

namespace {
constexpr size_t Attempts = 10000;
constexpr bool PinTestThreads = HOSTED;

struct FilterLifetimeContext {
  FilterLifetimeContext()
      : remover(nullptr),
        callbackId(static_cast<size_t>(-1)),
        phase(0),
        callbackCalls(0),
        callbackAfterRemoval(0),
        removalReturned(0),
        observedDrain(0),
        failures(0) {}

  Thread* remover;
  size_t callbackId;
  Atomic<size_t> phase;
  Atomic<size_t> callbackCalls;
  Atomic<size_t> callbackAfterRemoval;
  Atomic<size_t> removalReturned;
  Atomic<size_t> observedDrain;
  Atomic<size_t> failures;
};

FilterLifetimeContext* g_FilterLifetimeContext = nullptr;

bool lifetimeFilter(uintptr_t, size_t) {
  FilterLifetimeContext* context = g_FilterLifetimeContext;
  context->callbackCalls += 1;
  if (context->removalReturned) {
    context->callbackAfterRemoval += 1;
  }
  return true;
}

void filterPinHook(bool (*callback)(uintptr_t, size_t), size_t id) {
  FilterLifetimeContext* context = g_FilterLifetimeContext;
  if (!context || callback != lifetimeFilter || id != context->callbackId ||
      !context->phase.compareAndSwap(0, 1)) {
    return;
  }

  for (size_t attempt = 0; attempt < Attempts; ++attempt) {
    Thread::WaitDebugInfo info = {};
    uintptr_t debugAddress = 0;
    if (context->remover->getWaitDebugInfo(info) && info.queue && info.channelOwner &&
        info.queued && context->remover->getDebugState(debugAddress) == Thread::CallbackDrain &&
        debugAddress == reinterpret_cast<uintptr_t>(lifetimeFilter)) {
      context->observedDrain += 1;
      break;
    }
    Scheduler::instance().yield();
  }

  if (!context->observedDrain) {
    context->failures += 1;
  }
  context->phase = 2;
}

int removePinnedFilter(void* parameter) {
  FilterLifetimeContext* context = reinterpret_cast<FilterLifetimeContext*>(parameter);
  for (size_t attempt = 0; attempt < Attempts && context->phase != static_cast<size_t>(1);
       ++attempt) {
    Scheduler::instance().yield();
  }
  if (context->phase != static_cast<size_t>(1)) {
    context->failures += 1;
    return 1;
  }

  if (!NetworkFilter::instance().removeCallback(1, context->callbackId)) {
    context->failures += 1;
  }
  context->removalReturned += 1;
  return 0;
}

struct SelfRemovalContext {
  SelfRemovalContext() : callbackId(static_cast<size_t>(-1)), calls(0), deferred(0) {}

  size_t callbackId;
  Atomic<size_t> calls;
  Atomic<size_t> deferred;
};

SelfRemovalContext* g_SelfRemovalContext = nullptr;

bool selfRemovingFilter(uintptr_t, size_t) {
  SelfRemovalContext* context = g_SelfRemovalContext;
  context->calls += 1;
  if (!NetworkFilter::instance().removeCallback(1, context->callbackId)) {
    context->deferred += 1;
  }
  return true;
}

bool check(bool condition, const char* test, const char* detail) {
  if (condition) {
    return true;
  }
#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
  ERROR("QEMU-CONCURRENCY-TEST: FAIL " << test << ": " << detail);
#else
  ERROR("HOSTED-WAIT-TEST: FAIL " << test << ": " << detail);
#endif
  return false;
}

bool filterCallbackLifetimeBarrier() {
  NetworkFilter& filter = NetworkFilter::instance();
  FilterLifetimeContext context;
  g_FilterLifetimeContext = &context;
  context.callbackId = filter.installCallback(1, lifetimeFilter);
  context.remover = new Thread(Scheduler::instance().getKernelProcess(), removePinnedFilter,
                               &context, nullptr, false, PinTestThreads);
  context.remover->setName("hosted network-filter remover");

  NetworkFilter::setCallbackPinHook(filterPinHook);
  filter.filter(1, 0, 0);
  const bool joined = context.remover->join();
  NetworkFilter::setCallbackPinHook(nullptr);

  const size_t callsAfterRemoval = context.callbackCalls;
  filter.filter(1, 0, 0);
  g_FilterLifetimeContext = nullptr;

  SelfRemovalContext selfRemoval;
  g_SelfRemovalContext = &selfRemoval;
  selfRemoval.callbackId = filter.installCallback(1, selfRemovingFilter);
  filter.filter(1, 0, 0);
  const size_t callsAfterSelfRemoval = selfRemoval.calls;
  filter.filter(1, 0, 0);
  g_SelfRemovalContext = nullptr;
  const bool selfRemovalRetired = filter.removeCallback(1, selfRemoval.callbackId);

  bool passed = true;
  passed &= check(context.callbackId != static_cast<size_t>(-1) && joined && !context.failures,
                  "network-filter-callback-lifetime",
                  "the concurrent removal worker did not complete cleanly");
  passed &= check(context.observedDrain == static_cast<size_t>(1) &&
                      context.removalReturned == static_cast<size_t>(1),
                  "network-filter-callback-lifetime",
                  "removeCallback did not wait for the committed filter invocation");
  passed &= check(context.callbackCalls == static_cast<size_t>(1) &&
                      context.callbackCalls == callsAfterRemoval && !context.callbackAfterRemoval,
                  "network-filter-callback-lifetime",
                  "a filter callback began after removeCallback returned");
  passed &= check(selfRemoval.callbackId != static_cast<size_t>(-1) && callsAfterSelfRemoval == 1 &&
                      selfRemoval.calls == static_cast<size_t>(1) &&
                      selfRemoval.deferred == static_cast<size_t>(1) && selfRemovalRetired,
                  "network-filter-callback-lifetime",
                  "self-removal did not close admission for external retirement");

  if (passed) {
#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
    NOTICE("QEMU-CONCURRENCY-TEST: PASS network-filter-callback-lifetime");
#else
    NOTICE("HOSTED-WAIT-TEST: PASS network-filter-callback-lifetime");
#endif
  }
  return passed;
}

constexpr uintptr_t ReciprocalPacketA = 1;
constexpr uintptr_t ReciprocalPacketB = 2;

struct ReciprocalRemovalContext {
  ReciprocalRemovalContext()
      : callbackA(static_cast<size_t>(-1)),
        callbackB(static_cast<size_t>(-1)),
        enteredA(0),
        enteredB(0),
        returnedA(0),
        returnedB(0),
        callsA(0),
        callsB(0),
        processorA(static_cast<size_t>(-1)),
        processorB(static_cast<size_t>(-1)),
        deferredA(0),
        deferredB(0),
        failures(0) {}

  size_t callbackA;
  size_t callbackB;
  Atomic<size_t> enteredA;
  Atomic<size_t> enteredB;
  Atomic<size_t> returnedA;
  Atomic<size_t> returnedB;
  Atomic<size_t> callsA;
  Atomic<size_t> callsB;
  Atomic<size_t> processorA;
  Atomic<size_t> processorB;
  Atomic<size_t> deferredA;
  Atomic<size_t> deferredB;
  Atomic<size_t> failures;
};

ReciprocalRemovalContext* g_ReciprocalRemovalContext = nullptr;

bool reciprocalFilterA(uintptr_t packet, size_t) {
  ReciprocalRemovalContext* context = g_ReciprocalRemovalContext;
  context->callsA += 1;
  if (packet != ReciprocalPacketA) {
    return true;
  }

  context->processorA = Processor::id();
  context->enteredA = 1;
  for (size_t attempt = 0; attempt < Attempts && !context->enteredB; ++attempt) {
    Scheduler::instance().yield();
  }
  if (!context->enteredB) {
    context->failures += 1;
    return false;
  }

  if (!NetworkFilter::instance().removeCallback(1, context->callbackB)) {
    context->deferredA = 1;
  }
  context->returnedA = 1;
  return false;
}

bool reciprocalFilterB(uintptr_t packet, size_t) {
  ReciprocalRemovalContext* context = g_ReciprocalRemovalContext;
  context->processorB = Processor::id();
  context->callsB += 1;
  if (packet != ReciprocalPacketB) {
    return true;
  }

  context->enteredB = 1;
  for (size_t attempt = 0; attempt < Attempts && !context->enteredA; ++attempt) {
    Scheduler::instance().yield();
  }
  if (!context->enteredA) {
    context->failures += 1;
    return false;
  }

  if (!NetworkFilter::instance().removeCallback(1, context->callbackA)) {
    context->deferredB = 1;
  }
  context->returnedB = 1;
  return false;
}

int runReciprocalFilterA(void*) {
  NetworkFilter::instance().filter(1, ReciprocalPacketA, 0);
  return 0;
}

int runReciprocalFilterB(void*) {
  NetworkFilter::instance().filter(1, ReciprocalPacketB, 0);
  return 0;
}

bool reciprocalCallbackRemoval() {
  NetworkFilter& filter = NetworkFilter::instance();
  ReciprocalRemovalContext context;
  g_ReciprocalRemovalContext = &context;
  context.callbackA = filter.installCallback(1, reciprocalFilterA);
  context.callbackB = filter.installCallback(1, reciprocalFilterB);

  Process* process = Scheduler::instance().getKernelProcess();
  Thread* first =
      new Thread(process, runReciprocalFilterA, nullptr, nullptr, false, PinTestThreads);
  Thread* second =
      new Thread(process, runReciprocalFilterB, nullptr, nullptr, false, PinTestThreads);
  first->setName("reciprocal network-filter callback A");
  second->setName("reciprocal network-filter callback B");
  const bool firstJoined = first->joinForCompletion();
  const bool secondJoined = second->joinForCompletion();

  const size_t callsA = context.callsA;
  const size_t callsB = context.callsB;
  filter.filter(1, 0, 0);
  g_ReciprocalRemovalContext = nullptr;
  const bool retiredA = filter.removeCallback(1, context.callbackA);
  const bool retiredB = filter.removeCallback(1, context.callbackB);

#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
  NOTICE("QEMU-CONCURRENCY-TEST: network-filter reciprocal cpus="
         << Dec << static_cast<size_t>(context.processorA) << "/"
         << static_cast<size_t>(context.processorB)
         << " incomplete=" << static_cast<size_t>(context.deferredA) << "/"
         << static_cast<size_t>(context.deferredB));
#endif

  const bool passed =
      check(context.callbackA != static_cast<size_t>(-1) &&
                context.callbackB != static_cast<size_t>(-1) && firstJoined && secondJoined &&
                !context.failures && context.enteredA == static_cast<size_t>(1) &&
                context.enteredB == static_cast<size_t>(1) &&
                context.returnedA == static_cast<size_t>(1) &&
                context.returnedB == static_cast<size_t>(1) &&
                (context.deferredA == static_cast<size_t>(1) ||
                 context.deferredB == static_cast<size_t>(1)) &&
                retiredA && retiredB
#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
                && context.processorA != context.processorB
#endif
            ,
            "network-filter-reciprocal-removal",
            "reciprocal in-callback removals did not return on separate test CPUs") &&
      check(callsA == 2 && callsB == 1 && context.callsA == callsA && context.callsB == callsB,
            "network-filter-reciprocal-removal",
            "a reciprocally removed callback remained published");
  if (passed) {
#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
    NOTICE("QEMU-CONCURRENCY-TEST: PASS network-filter-reciprocal-removal-smp");
#else
    NOTICE("HOSTED-WAIT-TEST: PASS network-filter-reciprocal-removal");
#endif
  }
  return passed;
}
}  // namespace

bool runNetworkFilterConcurrencyRegressions() {
  return filterCallbackLifetimeBarrier() && reciprocalCallbackRemoval();
}
