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

#include "modules/system/network-stack/Filter.h"

namespace {
constexpr size_t Attempts = 10000;

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

  NetworkFilter::instance().removeCallback(1, context->callbackId);
  context->removalReturned += 1;
  return 0;
}

struct SelfRemovalContext {
  SelfRemovalContext() : callbackId(static_cast<size_t>(-1)), calls(0) {}

  size_t callbackId;
  Atomic<size_t> calls;
};

SelfRemovalContext* g_SelfRemovalContext = nullptr;

bool selfRemovingFilter(uintptr_t, size_t) {
  SelfRemovalContext* context = g_SelfRemovalContext;
  context->calls += 1;
  NetworkFilter::instance().removeCallback(1, context->callbackId);
  return true;
}

bool check(bool condition, const char* detail) {
  if (condition) {
    return true;
  }
  ERROR("HOSTED-WAIT-TEST: FAIL network-filter-callback-lifetime: " << detail);
  return false;
}

bool filterCallbackLifetimeBarrier() {
  NetworkFilter& filter = NetworkFilter::instance();
  FilterLifetimeContext context;
  g_FilterLifetimeContext = &context;
  context.callbackId = filter.installCallback(1, lifetimeFilter);
  context.remover = new Thread(Scheduler::instance().getKernelProcess(), removePinnedFilter,
                               &context, nullptr, false, true);
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
  filter.removeCallback(1, selfRemoval.callbackId);

  bool passed = true;
  passed &= check(context.callbackId != static_cast<size_t>(-1) && joined && !context.failures,
                  "the concurrent removal worker did not complete cleanly");
  passed &= check(context.observedDrain == 1 && context.removalReturned == 1,
                  "removeCallback did not wait for the committed filter invocation");
  passed &= check(context.callbackCalls == 1 && context.callbackCalls == callsAfterRemoval &&
                      !context.callbackAfterRemoval,
                  "a filter callback began after removeCallback returned");
  passed &= check(selfRemoval.callbackId != static_cast<size_t>(-1) && callsAfterSelfRemoval == 1 &&
                      selfRemoval.calls == 1,
                  "self-removal did not retire the active filter callback");

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS network-filter-callback-lifetime");
  }
  return passed;
}
}  // namespace

bool runHostedNetworkFilterRegressions() {
  return filterCallbackLifetimeBarrier();
}
