/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/time/Time.h"

#include "modules/drivers/x86/ps2mouse/Ps2MouseCallbackRegistry.h"

namespace {
bool check(bool condition, const char* detail) {
  if (condition) {
    return true;
  }

  ERROR("HOSTED-WAIT-TEST: FAIL ps2mouse-callback-lifetime: " << detail);
  return false;
}

struct CallbackLifetimeContext {
  CallbackLifetimeContext()
      : registration(),
        remover(nullptr),
        phase(0),
        hookCalls(0),
        hookObservedDrain(0),
        callbackCalls(0),
        callbacksAfterReturn(0),
        unregisterReturned(0),
        failures(0) {}

  Ps2MouseCallbackRegistry::Registration registration;
  Thread* remover;
  Atomic<size_t> phase;
  Atomic<size_t> hookCalls;
  Atomic<size_t> hookObservedDrain;
  Atomic<size_t> callbackCalls;
  Atomic<size_t> callbacksAfterReturn;
  Atomic<size_t> unregisterReturned;
  Atomic<size_t> failures;
};

CallbackLifetimeContext* g_CallbackLifetimeContext = nullptr;

void lifetimeCallback(void* parameter, const void*, size_t) {
  CallbackLifetimeContext* context = reinterpret_cast<CallbackLifetimeContext*>(parameter);
  context->callbackCalls += 1;
  if (context->unregisterReturned) {
    context->callbacksAfterReturn += 1;
  }
}

void callbackPinHook(Ps2MouseCallbackRegistry::Handler handler, void* parameter) {
  CallbackLifetimeContext* context = g_CallbackLifetimeContext;
  if (!context || handler != lifetimeCallback || parameter != context ||
      !context->phase.compareAndSwap(0, 1)) {
    return;
  }

  context->hookCalls += 1;
  for (size_t attempt = 0; attempt < 10000; ++attempt) {
    Thread::WaitDebugInfo info = {};
    uintptr_t debugAddress = 0;
    if (context->phase == static_cast<size_t>(2) && context->remover->getWaitDebugInfo(info) &&
        info.queue && info.channelOwner && info.queued &&
        context->remover->getDebugState(debugAddress) == Thread::CallbackDrain &&
        debugAddress == reinterpret_cast<uintptr_t>(context)) {
      break;
    }
    Scheduler::instance().yield();
  }

  Thread::WaitDebugInfo info = {};
  uintptr_t debugAddress = 0;
  if (context->phase == static_cast<size_t>(2) && !context->unregisterReturned &&
      context->remover->getWaitDebugInfo(info) && info.queue && info.channelOwner && info.queued &&
      context->remover->getDebugState(debugAddress) == Thread::CallbackDrain &&
      debugAddress == reinterpret_cast<uintptr_t>(context)) {
    context->hookObservedDrain += 1;
  } else {
    context->failures += 1;
  }
  context->phase = 3;
}

int unregisterPinnedCallback(void* parameter) {
  CallbackLifetimeContext* context = reinterpret_cast<CallbackLifetimeContext*>(parameter);
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (context->phase != static_cast<size_t>(1) && Time::getTicks() < deadline) {
    Scheduler::instance().yield();
  }

  if (context->phase != static_cast<size_t>(1)) {
    context->failures += 1;
    return 1;
  }

  context->phase = 2;
  if (!context->registration.reset()) {
    context->failures += 1;
  }
  context->unregisterReturned += 1;
  context->phase = 4;
  return 0;
}

struct SelfRemovalContext {
  SelfRemovalContext() : registration(), calls(0) {}

  Ps2MouseCallbackRegistry::Registration registration;
  Atomic<size_t> calls;
};

void selfRemovingCallback(void* parameter, const void*, size_t) {
  SelfRemovalContext* context = reinterpret_cast<SelfRemovalContext*>(parameter);
  context->calls += 1;
  context->registration.reset();
}

bool callbackLifetimeBarrier() {
  Ps2MouseCallbackRegistry registry;
  CallbackLifetimeContext context;
  context.remover = new Thread(Scheduler::instance().getKernelProcess(), unregisterPinnedCallback,
                               &context, nullptr, false, true);
  context.remover->setName("hosted PS/2 callback remover");

  g_CallbackLifetimeContext = &context;
  registry.setCallbackPinHook(callbackPinHook);
  const bool registered = registry.subscribe(lifetimeCallback, &context, context.registration);
  const bool duplicateRejected =
      !registry.subscribe(lifetimeCallback, &context, context.registration);
  const uint8_t byte = 0x7F;
  registry.dispatch(&byte, 1);
  const bool joined = context.remover->join();
  registry.setCallbackPinHook(nullptr);
  g_CallbackLifetimeContext = nullptr;

  const size_t callsAtUnregisterReturn = context.callbackCalls;
  registry.dispatch(&byte, 1);
  const bool lateDispatchRejected =
      context.callbackCalls == callsAtUnregisterReturn && context.callbacksAfterReturn == 0;

  const bool reregistered = registry.subscribe(lifetimeCallback, &context, context.registration);
  if (reregistered) {
    registry.dispatch(&byte, 1);
    context.registration.reset();
  }

  SelfRemovalContext selfRemoval;
  const bool selfRegistered =
      registry.subscribe(selfRemovingCallback, &selfRemoval, selfRemoval.registration);
  if (selfRegistered) {
    registry.dispatch(&byte, 1);
    registry.dispatch(&byte, 1);
  }
  const bool selfReregistered =
      registry.subscribe(selfRemovingCallback, &selfRemoval, selfRemoval.registration);
  selfRemoval.registration.reset();

  bool passed = true;
  passed &= check(registered && duplicateRejected, "registration or duplicate rejection failed");
  passed &= check(joined && context.failures == 0,
                  "the concurrent callback remover did not finish cleanly");
  passed &= check(
      context.hookCalls == 1 && context.hookObservedDrain == 1 && context.unregisterReturned == 1,
      "unregister returned instead of waiting for the admitted callback");
  passed &= check(context.callbackCalls >= 1 && lateDispatchRejected,
                  "a callback began after its registration was reset");
  passed &= check(reregistered && context.callbackCalls == callsAtUnregisterReturn + 1,
                  "the drained callback slot could not be reused");
  passed &= check(
      selfRegistered && selfRemoval.calls == 1 && !selfRemoval.registration && selfReregistered,
      "self-removal did not retire the callback after it returned");

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS ps2mouse-callback-lifetime");
  }
  return passed;
}

struct ReciprocalRemovalContext {
  ReciprocalRemovalContext()
      : registry(nullptr),
        first(),
        second(),
        beginReset(0),
        firstParticipant(0),
        callbacksEntered(0),
        firstCalls(0),
        secondCalls(0),
        resetRejections(0),
        resetsFinished(0),
        invocationsFinished(0),
        failures(0) {}

  Ps2MouseCallbackRegistry* registry;
  Ps2MouseCallbackRegistry::Registration first;
  Ps2MouseCallbackRegistry::Registration second;
  Semaphore beginReset;
  Atomic<size_t> firstParticipant;
  Atomic<size_t> callbacksEntered;
  Atomic<size_t> firstCalls;
  Atomic<size_t> secondCalls;
  Atomic<size_t> resetRejections;
  Atomic<size_t> resetsFinished;
  Atomic<size_t> invocationsFinished;
  Atomic<size_t> failures;
};

bool waitForValue(Atomic<size_t>& value, size_t expected) {
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (value != expected && Time::getTicks() < deadline) {
    Scheduler::instance().yield();
  }
  return value == expected;
}

void firstReciprocalCallback(void* parameter, const void*, size_t) {
  ReciprocalRemovalContext* context = reinterpret_cast<ReciprocalRemovalContext*>(parameter);
  context->firstCalls += 1;
  if (!context->firstParticipant.compareAndSwap(0, 1)) {
    return;
  }

  context->callbacksEntered += 1;
  if (!context->beginReset.acquireForCompletion()) {
    context->failures += 1;
    return;
  }
  if (!context->second.reset() && context->second) {
    context->resetRejections += 1;
  } else {
    context->failures += 1;
  }
  context->resetsFinished += 1;
  if (!waitForValue(context->resetsFinished, 2)) {
    context->failures += 1;
  }
}

void secondReciprocalCallback(void* parameter, const void*, size_t) {
  ReciprocalRemovalContext* context = reinterpret_cast<ReciprocalRemovalContext*>(parameter);
  context->secondCalls += 1;
  context->callbacksEntered += 1;
  if (!context->beginReset.acquireForCompletion()) {
    context->failures += 1;
    return;
  }
  if (!context->first.reset() && context->first) {
    context->resetRejections += 1;
  } else {
    context->failures += 1;
  }
  context->resetsFinished += 1;
  if (!waitForValue(context->resetsFinished, 2)) {
    context->failures += 1;
  }
}

int dispatchReciprocalCallbacks(void* parameter) {
  ReciprocalRemovalContext* context = reinterpret_cast<ReciprocalRemovalContext*>(parameter);
  const uint8_t byte = 0x5A;
  context->registry->dispatch(&byte, 1);
  context->invocationsFinished += 1;
  return 0;
}

bool reciprocalRemovalIsRetryable() {
  Ps2MouseCallbackRegistry registry;
  ReciprocalRemovalContext context;
  context.registry = &registry;

  const bool firstRegistered = registry.subscribe(firstReciprocalCallback, &context, context.first);
  const bool secondRegistered =
      registry.subscribe(secondReciprocalCallback, &context, context.second);

  Process* process = Scheduler::instance().getKernelProcess();
  Thread* firstInvoker = nullptr;
  Thread* secondInvoker = nullptr;
  bool firstEntered = false;
  bool bothEntered = false;
  if (firstRegistered && secondRegistered) {
    firstInvoker = new Thread(process, dispatchReciprocalCallbacks, &context, nullptr, false, true);
    firstInvoker->setName("hosted PS/2 reciprocal callback A");
    firstEntered = waitForValue(context.callbacksEntered, 1);

    if (firstEntered) {
      secondInvoker =
          new Thread(process, dispatchReciprocalCallbacks, &context, nullptr, false, true);
      secondInvoker->setName("hosted PS/2 reciprocal callback B");
      bothEntered = waitForValue(context.callbacksEntered, 2);
    }
  }

  context.beginReset.release(2);
  const bool firstJoined = !firstInvoker || firstInvoker->joinForCompletion();
  const bool secondJoined = !secondInvoker || secondInvoker->joinForCompletion();
  const bool tokensPreserved = context.first && context.second;

  const size_t firstCalls = context.firstCalls;
  const size_t secondCalls = context.secondCalls;
  const uint8_t byte = 0xA5;
  registry.dispatch(&byte, 1);
  const bool admissionClosed =
      context.firstCalls == firstCalls && context.secondCalls == secondCalls;

  const bool firstRetired = context.first && context.first.reset();
  const bool secondRetired = context.second && context.second.reset();
  const bool firstReused = registry.subscribe(firstReciprocalCallback, &context, context.first);
  const bool secondReused = registry.subscribe(secondReciprocalCallback, &context, context.second);
  const bool firstReuseRetired = context.first.reset();
  const bool secondReuseRetired = context.second.reset();

  const bool passed = check(
      firstRegistered && secondRegistered && firstEntered && bothEntered && firstJoined &&
          secondJoined && tokensPreserved && admissionClosed && firstRetired && secondRetired &&
          firstReused && secondReused && firstReuseRetired && secondReuseRetired &&
          !context.first && !context.second && context.firstCalls == static_cast<size_t>(2) &&
          context.secondCalls == static_cast<size_t>(1) &&
          context.resetRejections == static_cast<size_t>(2) &&
          context.resetsFinished == static_cast<size_t>(2) &&
          context.invocationsFinished == static_cast<size_t>(2) && !context.failures,
      "reciprocal callbacks did not preserve retryable registration ownership");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS ps2mouse-reciprocal-removal");
  }
  return passed;
}
}  // namespace

bool runHostedPs2MouseRegressions() {
  return callbackLifetimeBarrier() && reciprocalRemovalIsRetryable();
}
