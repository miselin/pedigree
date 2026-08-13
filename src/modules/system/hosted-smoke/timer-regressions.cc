/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/machine/TimerHandler.h"
#include "pedigree/kernel/machine/TimerHandlerRegistry.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/RelayEvent.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/eventNumbers.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/time/Time.h"

#include "system/kernel/machine/hosted/Timer.h"

namespace {
constexpr size_t TimerTestEventNumber = EventNumbers::UserStart + 1;
constexpr size_t CullTestEventNumber = EventNumbers::UserStart + 2;
constexpr size_t RelayTestEventNumber = EventNumbers::UserStart + 3;
constexpr size_t DispositionAEventNumber = EventNumbers::UserStart + 4;
constexpr size_t DispositionBEventNumber = EventNumbers::UserStart + 5;
constexpr size_t AlarmLinearizationEventNumber = EventNumbers::UserStart + 6;

Atomic<size_t> g_CullEventDestructions(0);
Atomic<size_t> g_RelayDisposition(0);
Atomic<size_t> g_RelayCallbacks(0);
Atomic<size_t> g_DispositionACalls(0);
Atomic<size_t> g_DispositionBCalls(0);
Event* g_DispositionAEvent = nullptr;
Event* g_DispositionBEvent = nullptr;

bool check(bool condition, const char* test, const char* detail);

bool hasTimerCallbackDrainWait(Thread* thread, TimerHandler* handler) {
  Thread::WaitDebugInfo wait = {};
  uintptr_t debugAddress = 0;
  return thread && thread->getWaitDebugInfo(wait) && wait.queue && wait.channelOwner &&
         wait.channelValue && wait.queued && wait.reason == WaitQueue::WakeReason::Waiting &&
         thread->getDebugState(debugAddress) == Thread::CallbackDrain &&
         debugAddress == reinterpret_cast<uintptr_t>(handler);
}

struct RegistryDispatchContext;
RegistryDispatchContext* g_TimerRegistryDispatchContext = nullptr;
void dispatchTimerWhileWriterLocked();

class RegistryDispatchTimerHandler : public TimerHandler {
 public:
  explicit RegistryDispatchTimerHandler(RegistryDispatchContext& context) : m_Context(context) {}

  void timer(uint64_t delta) override;

 private:
  RegistryDispatchContext& m_Context;
};

struct RegistryDispatchContext {
  explicit RegistryDispatchContext(Timer* timer)
      : timer(timer),
        handler(*this),
        calls(0),
        hookCalls(0),
        admitted(0),
        unregisterSucceeded(0),
        mutationRequested(0),
        delta(0) {}

  Timer* timer;
  RegistryDispatchTimerHandler handler;
  Atomic<size_t> calls;
  Atomic<size_t> hookCalls;
  Atomic<size_t> admitted;
  Atomic<size_t> unregisterSucceeded;
  Atomic<size_t> mutationRequested;
  uint64_t delta;
};

void RegistryDispatchTimerHandler::timer(uint64_t delta) {
  m_Context.calls += 1;
  if (m_Context.mutationRequested.compareAndSwap(1, 2)) {
    m_Context.delta = delta;
    HostedTimer::withHandlerMutationLockForTest(dispatchTimerWhileWriterLocked);
    m_Context.mutationRequested = 3;
  }
}

void dispatchTimerWhileWriterLocked() {
  RegistryDispatchContext* context = g_TimerRegistryDispatchContext;
  if (!context) {
    return;
  }

  context->hookCalls += 1;
  if (HostedTimer::dispatchHandlerForTest(&context->handler, context->delta)) {
    context->admitted += 1;
  }
}

struct AtomicDrainRaceContext;
AtomicDrainRaceContext* g_AtomicDrainRaceContext = nullptr;

class AtomicDrainRaceHandler : public TimerHandler {
 public:
  explicit AtomicDrainRaceHandler(AtomicDrainRaceContext& context) : m_Context(context) {}

  void timer(uint64_t) override;

 private:
  AtomicDrainRaceContext& m_Context;
};

struct AtomicDrainRaceContext {
  explicit AtomicDrainRaceContext(Timer* timer)
      : timer(timer),
        handler(*this),
        removerReady(0, false),
        beginDrain(0, false),
        phase(0),
        pinHookCalls(0),
        drainHookCalls(0),
        handlerCalls(0),
        selfRemovalRejected(0),
        revivalSucceeded(0),
        atomicRemovalRejected(0),
        beginDrainTimedOut(0),
        pinTransitionTimedOut(0),
        drainCompletionTimedOut(0),
        failures(0) {}

  Timer* timer;
  AtomicDrainRaceHandler handler;
  Semaphore removerReady;
  Semaphore beginDrain;
  Atomic<size_t> phase;
  Atomic<size_t> pinHookCalls;
  Atomic<size_t> drainHookCalls;
  Atomic<size_t> handlerCalls;
  Atomic<size_t> selfRemovalRejected;
  Atomic<size_t> revivalSucceeded;
  Atomic<size_t> atomicRemovalRejected;
  Atomic<size_t> beginDrainTimedOut;
  Atomic<size_t> pinTransitionTimedOut;
  Atomic<size_t> drainCompletionTimedOut;
  Atomic<size_t> failures;
};

void AtomicDrainRaceHandler::timer(uint64_t) {
  m_Context.handlerCalls += 1;
  if (m_Context.phase != static_cast<size_t>(2)) {
    return;
  }

  if (!m_Context.timer->unregisterHandler(this)) {
    m_Context.selfRemovalRejected += 1;
  }
  if (m_Context.timer->registerHandler(this)) {
    m_Context.revivalSucceeded += 1;
  }
  m_Context.phase = 3;
}

void atomicDrainPinHook(TimerHandler* handler) {
  constexpr size_t YieldLimit = 10000;
  AtomicDrainRaceContext* context = g_AtomicDrainRaceContext;
  if (!context || handler != &context->handler || !context->phase.compareAndSwap(0, 1)) {
    return;
  }

  context->pinHookCalls += 1;
  context->beginDrain.release();
  for (size_t i = 0; context->phase != static_cast<size_t>(2) && i < YieldLimit; ++i) {
    Scheduler::instance().yield();
  }
  if (context->phase != static_cast<size_t>(2)) {
    context->pinTransitionTimedOut += 1;
    context->failures += 1;
    context->phase = 3;
  }
}

void atomicDrainTransitionHook(TimerHandler* handler) {
  constexpr size_t YieldLimit = 10000;
  AtomicDrainRaceContext* context = g_AtomicDrainRaceContext;
  if (!context || handler != &context->handler || !context->phase.compareAndSwap(1, 2)) {
    if (context && handler == &context->handler) {
      context->failures += 1;
    }
    return;
  }

  context->drainHookCalls += 1;
  for (size_t i = 0; context->phase != static_cast<size_t>(3) && i < YieldLimit; ++i) {
    Scheduler::instance().yield();
  }
  if (context->phase != static_cast<size_t>(3)) {
    context->drainCompletionTimedOut += 1;
    context->failures += 1;
    context->phase = 3;
  }
}

int removeTimerAtomically(void* parameter) {
  AtomicDrainRaceContext* context = reinterpret_cast<AtomicDrainRaceContext*>(parameter);
  context->removerReady.release();
  if (!context->beginDrain.acquireForCompletion(1, 2)) {
    context->beginDrainTimedOut += 1;
    context->failures += 1;
    return 1;
  }
  if (context->phase != static_cast<size_t>(1)) {
    context->failures += 1;
    return 1;
  }

  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);
  const bool removed = context->timer->unregisterHandler(&context->handler);
  Processor::setInterrupts(interruptsWereEnabled);
  if (!removed) {
    context->atomicRemovalRejected += 1;
  } else {
    context->failures += 1;
  }
  return 0;
}

enum class TimerAbandonPoint {
  PartialHazard,
  CommittedHazard,
  DeferredSelfRemoval,
};

struct TimerAbandonContext;
TimerAbandonContext* g_TimerAbandonContext = nullptr;

class AbandoningTimerHandler : public TimerHandler {
 public:
  explicit AbandoningTimerHandler(TimerAbandonContext& context) : m_Context(context) {}

  void timer(uint64_t) override;

 private:
  TimerAbandonContext& m_Context;
};

struct TimerAbandonContext {
  TimerAbandonContext(Timer* timer, TimerHandlerRegistry* registry, TimerAbandonPoint abandonPoint)
      : timer(timer),
        registry(registry),
        handler(*this),
        dispatcher(nullptr),
        remover(nullptr),
        point(abandonPoint),
        phase(0),
        hookCalls(0),
        hookObservedDrain(0),
        handlerCalls(0),
        dispatchReturned(0),
        unregisterReturned(0),
        unregisterSucceeded(0),
        selfRemovalRejected(0),
        failures(0) {}

  Timer* timer;
  TimerHandlerRegistry* registry;
  AbandoningTimerHandler handler;
  Thread* dispatcher;
  Thread* remover;
  TimerAbandonPoint point;
  Atomic<size_t> phase;
  Atomic<size_t> hookCalls;
  Atomic<size_t> hookObservedDrain;
  Atomic<size_t> handlerCalls;
  Atomic<size_t> dispatchReturned;
  Atomic<size_t> unregisterReturned;
  Atomic<size_t> unregisterSucceeded;
  Atomic<size_t> selfRemovalRejected;
  Atomic<size_t> failures;
};

void abandonCurrentTimerDispatcher() {
  Processor::information().getCurrentThread()->getScheduler()->abandonCurrentThreadStack(
      PerProcessorScheduler::StackDiscardReason::HostedRegression);
}

void AbandoningTimerHandler::timer(uint64_t) {
  if (Processor::information().getCurrentThread() != m_Context.dispatcher) {
    return;
  }

  m_Context.handlerCalls += 1;
  if (m_Context.point != TimerAbandonPoint::DeferredSelfRemoval) {
    m_Context.failures += 1;
    return;
  }

  if (!m_Context.registry->unregisterHandler(this)) {
    m_Context.selfRemovalRejected += 1;
  } else {
    m_Context.failures += 1;
  }
  abandonCurrentTimerDispatcher();
}

void abandonPartialTimerHazard(TimerHandler* handler) {
  TimerAbandonContext* context = g_TimerAbandonContext;
  if (!context || handler != &context->handler ||
      context->point != TimerAbandonPoint::PartialHazard ||
      Processor::information().getCurrentThread() != context->dispatcher) {
    return;
  }

  context->hookCalls += 1;
  abandonCurrentTimerDispatcher();
}

void abandonCommittedTimerHazard(TimerHandler* handler) {
  TimerAbandonContext* context = g_TimerAbandonContext;
  if (!context || handler != &context->handler ||
      context->point != TimerAbandonPoint::CommittedHazard ||
      Processor::information().getCurrentThread() != context->dispatcher ||
      !context->phase.compareAndSwap(0, 1)) {
    return;
  }

  context->hookCalls += 1;
  const uint64_t deadline =
      context->timer->getTickCountNano() + (500 * Time::Multiplier::Millisecond);
  while (context->timer->getTickCountNano() < deadline) {
    if (context->phase == static_cast<size_t>(2) && context->remover &&
        hasTimerCallbackDrainWait(context->remover, &context->handler)) {
      context->hookObservedDrain += 1;
      abandonCurrentTimerDispatcher();
    }
    Scheduler::instance().yield();
  }

  context->failures += 1;
  abandonCurrentTimerDispatcher();
}

int awaitAbandoningTimer(void* parameter) {
  TimerAbandonContext* context = reinterpret_cast<TimerAbandonContext*>(parameter);

  // These tests deliberately abandon this exact dispatcher stack from a
  // registry hook or callback. Drive the registry directly so the lifetime
  // test does not depend on which thread services the hardware timer.
  context->registry->dispatch(Time::Multiplier::Millisecond, &context->handler);
  context->failures += 1;
  context->dispatchReturned += 1;
  return 1;
}

int unregisterAbandoningTimer(void* parameter) {
  TimerAbandonContext* context = reinterpret_cast<TimerAbandonContext*>(parameter);
  const uint64_t deadline =
      context->timer->getTickCountNano() + (500 * Time::Multiplier::Millisecond);
  while (context->phase != static_cast<size_t>(1) &&
         context->timer->getTickCountNano() < deadline) {
    Scheduler::instance().yield();
  }
  if (context->phase != static_cast<size_t>(1)) {
    context->failures += 1;
    return 1;
  }

  context->phase = 2;
  if (context->registry->unregisterHandler(&context->handler)) {
    context->unregisterSucceeded += 1;
  }
  context->unregisterReturned += 1;
  return 0;
}

bool timerPartialHazardAbandonment() {
  constexpr const char* Test = "timer-partial-hazard-abandonment";
  Timer* timer = Machine::instance().getTimer();
  TimerHandlerRegistry registry;
  TimerAbandonContext context(timer, &registry, TimerAbandonPoint::PartialHazard);
  context.dispatcher = new Thread(Scheduler::instance().getKernelProcess(), awaitAbandoningTimer,
                                  &context, nullptr, false, true, true);
  context.dispatcher->setName("hosted partial timer hazard");

  g_TimerAbandonContext = &context;
  registry.setHandlerHazardClaimHook(abandonPartialTimerHazard);
  const bool registered = registry.registerHandler(&context.handler);
  const bool started = registered && context.dispatcher->start();
  const bool joined = started && context.dispatcher->join();
  registry.setHandlerHazardClaimHook(nullptr);
  g_TimerAbandonContext = nullptr;

  const size_t claimed = registry.claimedDispatchCountForTest();
  const size_t active = registry.activeDispatchCountForTest(&context.handler);
  const bool removed = registered && registry.unregisterHandler(&context.handler);
  const bool reused = removed && registry.registerHandler(&context.handler);
  const bool cleaned = reused && registry.unregisterHandler(&context.handler);

  bool passed = true;
  passed &= check(registered && started && joined, Test,
                  "the partial-publication worker did not complete");
  passed &= check(context.hookCalls == 1 && context.handlerCalls == 0 &&
                      context.dispatchReturned == 0 && context.failures == 0,
                  Test, "dispatch escaped the forced partial-publication window");
  passed &= check(claimed == 0 && active == 0, Test,
                  "stack abandonment leaked a partial callback hazard");
  passed &= check(removed && reused && cleaned, Test,
                  "the handler slot could not be removed and reused after cleanup");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS timer-partial-hazard-abandonment");
  }
  return passed;
}

bool timerCommittedHazardAbandonment() {
  constexpr const char* Test = "timer-abandoned-dispatch-cleanup";
  Timer* timer = Machine::instance().getTimer();
  TimerHandlerRegistry registry;
  TimerAbandonContext context(timer, &registry, TimerAbandonPoint::CommittedHazard);
  context.remover = new Thread(Scheduler::instance().getKernelProcess(), unregisterAbandoningTimer,
                               &context, nullptr, false, true);
  context.remover->setName("hosted abandoned timer remover");
  context.dispatcher = new Thread(Scheduler::instance().getKernelProcess(), awaitAbandoningTimer,
                                  &context, nullptr, false, true, true);
  context.dispatcher->setName("hosted abandoned timer dispatch");

  g_TimerAbandonContext = &context;
  registry.setHandlerPinHook(abandonCommittedTimerHazard);
  const bool registered = registry.registerHandler(&context.handler);
  if (!registered) {
    context.phase = 1;
  }
  const bool started = registered && context.dispatcher->start();
  const bool dispatcherJoined = started && context.dispatcher->join();
  const bool removerJoined = context.remover->join();
  registry.setHandlerPinHook(nullptr);
  g_TimerAbandonContext = nullptr;

  if (!context.unregisterSucceeded && registered) {
    registry.unregisterHandler(&context.handler);
  }
  const size_t claimed = registry.claimedDispatchCountForTest();
  const size_t active = registry.activeDispatchCountForTest(&context.handler);
  const bool reused = context.unregisterSucceeded && registry.registerHandler(&context.handler);
  const bool cleaned = reused && registry.unregisterHandler(&context.handler);

  bool passed = true;
  passed &= check(registered && started && dispatcherJoined && removerJoined, Test,
                  "the abandoned-dispatch workers did not complete");
  passed &= check(context.failures == 0 && context.hookCalls == 1 && context.hookObservedDrain == 1,
                  Test, "the committed drain-and-abandon window was not reached");
  passed &= check(context.handlerCalls == 0 && context.dispatchReturned == 0, Test,
                  "the abandoned timer stack returned through normal dispatch");
  passed &= check(claimed == 0 && active == 0, Test,
                  "stack abandonment leaked a committed callback hazard");
  passed &= check(
      context.unregisterSucceeded == 1 && context.unregisterReturned == 1 && reused && cleaned,
      Test, "cleanup did not release the synchronous unregister barrier");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS timer-abandoned-dispatch-cleanup");
  }
  return passed;
}

bool timerDeferredSelfRemovalAbandonment() {
  constexpr const char* Test = "timer-abandoned-self-removal";
  Timer* timer = Machine::instance().getTimer();
  TimerHandlerRegistry registry;
  TimerAbandonContext context(timer, &registry, TimerAbandonPoint::DeferredSelfRemoval);
  context.dispatcher = new Thread(Scheduler::instance().getKernelProcess(), awaitAbandoningTimer,
                                  &context, nullptr, false, true, true);
  context.dispatcher->setName("hosted abandoned timer self-removal");

  const bool registered = registry.registerHandler(&context.handler);
  const bool started = registered && context.dispatcher->start();
  const bool joined = started && context.dispatcher->join();
  const size_t claimed = registry.claimedDispatchCountForTest();
  const size_t active = registry.activeDispatchCountForTest(&context.handler);
  const bool reused = registered && registry.registerHandler(&context.handler);
  const bool cleaned = reused && registry.unregisterHandler(&context.handler);

  bool passed = true;
  passed &=
      check(registered && started && joined, Test, "the self-removing worker did not complete");
  passed &= check(context.handlerCalls == 1 && context.selfRemovalRejected == 1 &&
                      context.dispatchReturned == 0 && context.failures == 0,
                  Test, "the callback did not defer removal before abandoning its stack");
  passed &=
      check(claimed == 0 && active == 0, Test, "deferred self-removal leaked a callback hazard");
  passed &= check(reused && cleaned, Test,
                  "final abandoned-stack cleanup did not retire and reuse the slot");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS timer-abandoned-self-removal");
  }
  return passed;
}

struct HandlerLifetimeContext;
HandlerLifetimeContext* g_HandlerLifetimeContext = nullptr;

class LifetimeHandler : public TimerHandler {
 public:
  explicit LifetimeHandler(HandlerLifetimeContext& context) : m_Context(context) {}

  void timer(uint64_t) override;

 private:
  HandlerLifetimeContext& m_Context;
};

struct HandlerLifetimeContext {
  explicit HandlerLifetimeContext(Timer* timer)
      : timer(timer),
        handler(*this),
        remover(nullptr),
        phase(0),
        hookCalls(0),
        hookObservedDrain(0),
        handlerCalls(0),
        callbacksAfterReturn(0),
        unregisterReturned(0),
        unregisterSucceeded(0),
        selfRemovalPending(1),
        selfRemovalRejected(0),
        revivalBlocked(0),
        waitHookCalls(0),
        waitHookFailures(0),
        drainWaitPublished(0),
        wakeBeforeBlock(0),
        failures(0) {}

  Timer* timer;
  LifetimeHandler handler;
  Thread* remover;
  Atomic<size_t> phase;
  Atomic<size_t> hookCalls;
  Atomic<size_t> hookObservedDrain;
  Atomic<size_t> handlerCalls;
  Atomic<size_t> callbacksAfterReturn;
  Atomic<size_t> unregisterReturned;
  Atomic<size_t> unregisterSucceeded;
  Atomic<size_t> selfRemovalPending;
  Atomic<size_t> selfRemovalRejected;
  Atomic<size_t> revivalBlocked;
  Atomic<size_t> waitHookCalls;
  Atomic<size_t> waitHookFailures;
  Atomic<size_t> drainWaitPublished;
  Atomic<size_t> wakeBeforeBlock;
  Atomic<size_t> failures;
};

void LifetimeHandler::timer(uint64_t) {
  m_Context.handlerCalls += 1;
  if (m_Context.unregisterReturned) {
    m_Context.callbacksAfterReturn += 1;
  }
  if (m_Context.selfRemovalPending.compareAndSwap(1, 0) &&
      !m_Context.timer->unregisterHandler(this)) {
    m_Context.selfRemovalRejected += 1;
    if (!m_Context.timer->registerHandler(this)) {
      m_Context.revivalBlocked += 1;
    }
  }
}

class SelfRemovingHandler : public TimerHandler {
 public:
  explicit SelfRemovingHandler(Timer* timer) : m_Timer(timer), calls(0), rejectionSeen(0) {}

  void timer(uint64_t) override {
    calls += 1;
    if (!m_Timer->unregisterHandler(this)) {
      rejectionSeen += 1;
    }
  }

  Timer* m_Timer;
  Atomic<size_t> calls;
  Atomic<size_t> rejectionSeen;
};

void timerDrainBeforeBlockHook(WaitQueue* queue, Thread* thread, const WaitQueue::Channel& channel,
                               size_t debugState) {
  constexpr size_t YieldLimit = 10000;
  HandlerLifetimeContext* context = g_HandlerLifetimeContext;
  if (!context || thread != context->remover) {
    return;
  }

  context->waitHookCalls += 1;
  Thread::WaitDebugInfo wait = {};
  if (!queue || !channel.owner || !channel.value || debugState != Thread::CallbackDrain ||
      !thread->getWaitDebugInfo(wait) || wait.queue != queue ||
      wait.channelOwner != channel.owner || wait.channelValue != channel.value || !wait.queued ||
      wait.reason != WaitQueue::WakeReason::Waiting ||
      !hasTimerCallbackDrainWait(thread, &context->handler)) {
    context->waitHookFailures += 1;
    return;
  }

  context->drainWaitPublished += 1;
  const uint64_t deadline =
      context->timer->getTickCountNano() + (500 * Time::Multiplier::Millisecond);
  for (size_t i = 0; context->timer->getTickCountNano() < deadline && i < YieldLimit; ++i) {
    Thread::WaitDebugInfo currentWait = {};
    if (thread->getWaitDebugInfo(currentWait) && currentWait.queue == queue &&
        currentWait.channelOwner == channel.owner && currentWait.channelValue == channel.value &&
        currentWait.reason == WaitQueue::WakeReason::Signalled) {
      context->wakeBeforeBlock += 1;
      return;
    }
    Scheduler::instance().yield();
  }

  context->waitHookFailures += 1;
}

void handlerPinHook(TimerHandler* handler) {
  constexpr size_t YieldLimit = 10000;
  HandlerLifetimeContext* context = g_HandlerLifetimeContext;
  if (!context || handler != &context->handler || !context->phase.compareAndSwap(0, 1)) {
    return;
  }

  context->hookCalls += 1;
  const uint64_t deadline =
      context->timer->getTickCountNano() + (500 * Time::Multiplier::Millisecond);
  bool observedDrain = false;
  for (size_t i = 0; context->timer->getTickCountNano() < deadline && i < YieldLimit; ++i) {
    if (context->phase == static_cast<size_t>(2) && context->drainWaitPublished &&
        !context->unregisterReturned &&
        hasTimerCallbackDrainWait(context->remover, &context->handler)) {
      observedDrain = true;
      break;
    }
    Scheduler::instance().yield();
  }

  if (observedDrain) {
    // This is the debugger-visible ownership barrier, not merely evidence
    // that the remover happened to lose a timeslice while polling.
    context->hookObservedDrain += 1;
  } else {
    context->failures += 1;
  }
  context->phase = 3;
}

int unregisterPinnedHandler(void* parameter) {
  HandlerLifetimeContext* context = reinterpret_cast<HandlerLifetimeContext*>(parameter);
  const uint64_t deadline =
      context->timer->getTickCountNano() + (500 * Time::Multiplier::Millisecond);
  while (context->phase != static_cast<size_t>(1) &&
         context->timer->getTickCountNano() < deadline) {
    Scheduler::instance().yield();
  }

  if (context->phase != static_cast<size_t>(1)) {
    context->failures += 1;
    return 1;
  }

  context->phase = 2;
  if (context->timer->unregisterHandler(&context->handler)) {
    context->unregisterSucceeded += 1;
  }
  context->unregisterReturned += 1;
  context->phase = 4;
  return 0;
}

void ignoredTimerEvent(size_t) {}

void dispositionAHandler(size_t) {
  g_DispositionACalls += 1;
}

void dispositionBHandler(size_t) {
  g_DispositionBCalls += 1;
}

void relayDisposition(Thread* thread) {
  g_RelayCallbacks += 1;
  Event* event = g_RelayDisposition == 1 ? g_DispositionAEvent : g_DispositionBEvent;
  if (event) {
    thread->sendEvent(event);
  }
}

class TimerTestEvent : public Event {
 public:
  TimerTestEvent(size_t number, bool deletable,
                 uintptr_t handler = reinterpret_cast<uintptr_t>(&ignoredTimerEvent))
      : Event(handler, deletable), m_Number(number) {}

  ~TimerTestEvent() override {
    if (m_Number == CullTestEventNumber) {
      g_CullEventDestructions += 1;
    }
  }

  size_t serialize(uint8_t*) override {
    return 0;
  }

  size_t getNumber() override {
    return m_Number;
  }

 private:
  size_t m_Number;
};

struct AlarmLinearizationContext;

class AlarmLinearizationEvent : public TimerTestEvent {
 public:
  explicit AlarmLinearizationEvent(AlarmLinearizationContext& context);
  ~AlarmLinearizationEvent() override;

 private:
  AlarmLinearizationContext& m_Context;
};

struct AlarmLinearizationContext {
  AlarmLinearizationContext(Timer* timer, Thread* owner)
      : timer(timer),
        owner(owner),
        event(nullptr),
        removerReady(0, false),
        phase(0),
        hookCalls(0),
        lockBoundaryObserved(0),
        removerReturned(0),
        destructions(0),
        failures(0) {}

  Timer* timer;
  Thread* owner;
  AlarmLinearizationEvent* event;
  Semaphore removerReady;
  Atomic<size_t> phase;
  Atomic<size_t> hookCalls;
  Atomic<size_t> lockBoundaryObserved;
  Atomic<size_t> removerReturned;
  Atomic<size_t> destructions;
  Atomic<size_t> failures;
};

AlarmLinearizationContext* g_AlarmLinearizationContext = nullptr;

AlarmLinearizationEvent::AlarmLinearizationEvent(AlarmLinearizationContext& context)
    : TimerTestEvent(AlarmLinearizationEventNumber, false), m_Context(context) {}

AlarmLinearizationEvent::~AlarmLinearizationEvent() {
  m_Context.destructions += 1;
}

void observeAlarmSendAdmission(Event* event) {
  AlarmLinearizationContext* context = g_AlarmLinearizationContext;
  if (!context || event != context->event) {
    return;
  }

  context->hookCalls += 1;
  const bool lockHeld = HostedTimer::alarmLockHeldForTest();
  const bool interruptsDisabled = !Processor::getInterrupts();
  if (lockHeld && interruptsDisabled && !context->removerReturned && !context->destructions) {
    context->lockBoundaryObserved += 1;
  } else {
    context->failures += 1;
  }
  context->phase = 1;

  // If a future change reopens the claim-to-send gap, let the remover win
  // that gap so ASan observes the stale Event use deterministically.
  if (!lockHeld && Processor::getInterrupts()) {
    const uint64_t deadline =
        context->timer->getTickCountNano() + (500 * Time::Multiplier::Millisecond);
    while (!context->destructions && context->timer->getTickCountNano() < deadline) {
      Scheduler::instance().yield();
    }
  }
}

int removeLinearizingAlarm(void* parameter) {
  AlarmLinearizationContext* context = reinterpret_cast<AlarmLinearizationContext*>(parameter);
  context->removerReady.release();

  const uint64_t deadline =
      context->timer->getTickCountNano() + (500 * Time::Multiplier::Millisecond);
  while (!context->phase && context->timer->getTickCountNano() < deadline) {
    Scheduler::instance().yield();
  }
  if (!context->phase) {
    context->failures += 1;
    return 1;
  }

  context->timer->removeAlarm(context->event);
  context->owner->cullEvent(context->event);
  delete context->event;
  context->event = nullptr;
  context->removerReturned = 1;
  return 0;
}

bool check(bool condition, const char* test, const char* detail) {
  if (condition) {
    return true;
  }

  ERROR("HOSTED-WAIT-TEST: FAIL " << test << ": " << detail);
  return false;
}

bool timerWriterLockIndependentDispatch() {
  constexpr const char* Test = "timer-dispatch-writer-lock-independent";
  Timer* timer = Machine::instance().getTimer();
  RegistryDispatchContext context(timer);
  const bool registered = timer->registerHandler(&context.handler);

  if (registered) {
    g_TimerRegistryDispatchContext = &context;
    context.mutationRequested = 1;
    const uint64_t deadline = timer->getTickCountNano() + (250 * Time::Multiplier::Millisecond);
    while (context.mutationRequested != static_cast<size_t>(3) &&
           timer->getTickCountNano() < deadline) {
      Scheduler::instance().yield();
    }
    g_TimerRegistryDispatchContext = nullptr;
  }

  const bool cleaned = registered && timer->unregisterHandler(&context.handler);
  bool passed = true;
  passed &= check(registered, Test, "the test handler could not be registered");
  passed &= check(context.mutationRequested == 3 && context.hookCalls == 1 &&
                      context.admitted == 1 && context.calls >= 2,
                  Test, "dispatch or callback completion waited for the writer lock");
  passed &= check(cleaned, Test, "the test handler could not be removed");
  if (passed) {
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "timer-dispatch-writer-lock-independent");
  }
  return passed;
}

void unregisterTimerBeforePin(TimerHandler* handler) {
  RegistryDispatchContext* context = g_TimerRegistryDispatchContext;
  if (!context || handler != &context->handler || !context->hookCalls.compareAndSwap(0, 1)) {
    return;
  }

  if (context->timer->unregisterHandler(&context->handler)) {
    context->unregisterSucceeded += 1;
  }
  context->hookCalls = 2;
}

bool timerPrePinUnregisterRevalidation() {
  constexpr const char* Test = "timer-pre-pin-unregister-revalidation";
  Timer* timer = Machine::instance().getTimer();
  RegistryDispatchContext context(timer);

  g_TimerRegistryDispatchContext = &context;
  HostedTimer::setHandlerPrePinHook(unregisterTimerBeforePin);
  const bool registered = timer->registerHandler(&context.handler);
  if (registered) {
    const uint64_t deadline = timer->getTickCountNano() + (250 * Time::Multiplier::Millisecond);
    while (context.hookCalls != static_cast<size_t>(2) && timer->getTickCountNano() < deadline) {
      Scheduler::instance().yield();
    }
  }
  HostedTimer::setHandlerPrePinHook(nullptr);
  g_TimerRegistryDispatchContext = nullptr;

  const bool reused = context.unregisterSucceeded && timer->registerHandler(&context.handler);
  bool cleaned = true;
  if (reused) {
    cleaned = timer->unregisterHandler(&context.handler);
  } else if (registered && !context.unregisterSucceeded) {
    cleaned = timer->unregisterHandler(&context.handler);
  }

  bool passed = true;
  passed &= check(registered, Test, "the test handler could not be registered");
  passed &= check(context.hookCalls == 2 && context.unregisterSucceeded == 1, Test,
                  "unregister did not retire the pre-pin publication");
  passed &=
      check(context.calls == 0, Test, "a stale pre-pin snapshot entered the retired callback");
  passed &= check(reused && cleaned, Test, "the revalidated slot could not be reused and removed");
  if (passed) {
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "timer-pre-pin-unregister-revalidation");
  }
  return passed;
}

bool timerAtomicDrainSelfRevival() {
  constexpr const char* Test = "timer-atomic-drain-self-revival";
  Timer* timer = Machine::instance().getTimer();
  AtomicDrainRaceContext context(timer);

  g_AtomicDrainRaceContext = &context;
  HostedTimer::setHandlerPinHook(atomicDrainPinHook);
  HostedTimer::setHandlerAtomicDrainHook(atomicDrainTransitionHook);
  Thread* remover = new Thread(Scheduler::instance().getKernelProcess(), removeTimerAtomically,
                               &context, nullptr, false, true);
  remover->setName("hosted atomic timer-handler remover");

  const bool removerWasReady = context.removerReady.acquireForCompletion(1, 2);
  const bool registered = removerWasReady && timer->registerHandler(&context.handler);
  if (!registered) {
    context.phase = 1;
    context.beginDrain.release();
  }
  const bool joined = remover->join();
  HostedTimer::setHandlerAtomicDrainHook(nullptr);
  HostedTimer::setHandlerPinHook(nullptr);
  g_AtomicDrainRaceContext = nullptr;

  const size_t callsAtAtomicReturn = context.handlerCalls;
  const uint64_t liveDeadline = timer->getTickCountNano() + (250 * Time::Multiplier::Millisecond);
  while (context.handlerCalls == callsAtAtomicReturn && timer->getTickCountNano() < liveDeadline) {
    Scheduler::instance().yield();
  }
  const bool remainedLive = context.handlerCalls > callsAtAtomicReturn;
  const bool cleaned = registered && timer->unregisterHandler(&context.handler);

  bool passed = true;
  passed &=
      check(removerWasReady, Test, "the atomic remover did not become ready before registration");
  passed &= check(registered, Test, "the test handler could not be registered");
  passed &= check(context.beginDrainTimedOut == 0, Test,
                  "the atomic remover timed out waiting for the pin hook");
  passed &= check(context.pinTransitionTimedOut == 0, Test,
                  "the pin hook timed out waiting for the atomic drain transition");
  passed &= check(context.drainCompletionTimedOut == 0, Test,
                  "the atomic drain hook timed out waiting for callback revival");
  passed &= check(
      joined && context.failures == 0 && context.pinHookCalls == 1 && context.drainHookCalls == 1,
      Test, "the atomic drainer interleaving did not complete cleanly");
  passed &= check(context.selfRemovalRejected == 1 && context.atomicRemovalRejected == 1 &&
                      context.revivalSucceeded == 1,
                  Test, "the self-removal request was lost or could not be revived");
  passed &= check(remainedLive && cleaned, Test,
                  "the revived handler was not live or could not be removed");
  if (passed) {
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "timer-atomic-drain-self-revival");
  }
  return passed;
}

class ThreadContextTimerHandler : public TimerHandler {
 public:
  explicit ThreadContextTimerHandler(Thread* registeringThread)
      : registeringThread(registeringThread),
        calls(0),
        failures(0),
        failureMask(0),
        workerAddress(0) {}

  void timer(uint64_t delta) override {
    Thread* current = Processor::information().getCurrentThread();
    const size_t currentAddress = reinterpret_cast<size_t>(current);
    size_t expectedWorker = workerAddress.value();
    if (!expectedWorker && current) {
      workerAddress.compareAndSwap(0, currentAddress);
      expectedWorker = workerAddress.value();
    }

    size_t failed = 0;
    failed |= !current ? 1 : 0;
    failed |= current == registeringThread ? 2 : 0;
    failed |= expectedWorker != currentAddress ? 4 : 0;
    failed |= !Processor::getInterrupts() ? 8 : 0;
    failed |= current && current->getHostedSignalDepth() ? 16 : 0;
    failed |= !delta || (delta % Time::Multiplier::Millisecond) ? 32 : 0;
    if (failed) {
      failures += 1;
      failureMask |= failed;
    }
    calls += 1;
  }

  Thread* registeringThread;
  Atomic<size_t> calls;
  Atomic<size_t> failures;
  Atomic<size_t> failureMask;
  Atomic<size_t> workerAddress;
};

bool hostedTimerThreadContext(Thread* registeringThread) {
  constexpr const char* Test = "hosted-timer-thread-context";
  Timer* timer = Machine::instance().getTimer();
  ThreadContextTimerHandler handler(registeringThread);
  const bool registered = timer->registerHandler(&handler);

  const uint64_t deadline = timer->getTickCountNano() + (250 * Time::Multiplier::Millisecond);
  while (registered && handler.calls < static_cast<size_t>(2) &&
         timer->getTickCountNano() < deadline) {
    Scheduler::instance().yield();
  }

  const bool removed = registered && timer->unregisterHandler(&handler);
  const bool passed =
      check(registered && removed && handler.calls >= static_cast<size_t>(2) &&
                handler.failures == 0 && handler.workerAddress != static_cast<size_t>(0),
            Test, "timer callbacks did not remain on one ordinary worker context");
  if (!passed) {
    ERROR("HOSTED-WAIT-TEST: INFO "
          << Test << ": calls=" << handler.calls.value()
          << ", failures=" << handler.failures.value() << ", mask=" << Hex
          << handler.failureMask.value() << ", worker=" << handler.workerAddress.value()
          << ", registrar=" << reinterpret_cast<uintptr_t>(registeringThread));
  }
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS hosted-timer-thread-context");
  }
  return passed;
}

class HostedTimerOverrunHandler : public TimerHandler {
 public:
  HostedTimerOverrunHandler() : calls(0), maximumDelta(0) {}

  void timer(uint64_t delta) override {
    uint64_t maximum = maximumDelta.value();
    while (delta > maximum && !maximumDelta.compareAndSwap(maximum, delta)) {
      maximum = maximumDelta.value();
    }
    calls += 1;
  }

  Atomic<size_t> calls;
  Atomic<uint64_t> maximumDelta;
};

bool hostedTimerOverrunAccounting() {
  constexpr const char* Test = "hosted-timer-overrun-accounting";
  Timer* timer = Machine::instance().getTimer();
  HostedTimerOverrunHandler handler;
  const bool registered = timer->registerHandler(&handler);

  const uint64_t baselineDeadline =
      timer->getTickCountNano() + (250 * Time::Multiplier::Millisecond);
  while (registered && !handler.calls && timer->getTickCountNano() < baselineDeadline) {
    Scheduler::instance().yield();
  }

  const bool baselineObserved = handler.calls != static_cast<size_t>(0);
  handler.maximumDelta = 0;
  const bool interruptsWereEnabled = Processor::getInterrupts();
  if (baselineObserved && interruptsWereEnabled) {
    Processor::setInterrupts(false);
    const uint64_t blockedUntil = timer->getTickCountNano() + (30 * Time::Multiplier::Millisecond);
    while (timer->getTickCountNano() < blockedUntil) {
      Processor::pause();
    }
    Processor::setInterrupts(true);
  }

  const uint64_t overrunDeadline =
      timer->getTickCountNano() + (250 * Time::Multiplier::Millisecond);
  while (handler.maximumDelta <= Time::Multiplier::Millisecond &&
         timer->getTickCountNano() < overrunDeadline) {
    Scheduler::instance().yield();
  }

  const bool removed = registered && timer->unregisterHandler(&handler);
  const uint64_t maximumDelta = handler.maximumDelta.value();
  const bool passed =
      check(registered && baselineObserved && interruptsWereEnabled && removed &&
                maximumDelta > Time::Multiplier::Millisecond &&
                !(maximumDelta % Time::Multiplier::Millisecond),
            Test, "deferred POSIX expirations were not preserved in callback delta");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS hosted-timer-overrun-accounting");
  }
  return passed;
}

bool timerClockAndDeadline(Thread* thread) {
  Timer* timer = Machine::instance().getTimer();
  bool passed = true;

  const uint64_t nanosBefore = timer->getTickCountNano();
  const uint64_t millis = timer->getTickCount();
  const uint64_t nanosAfter = timer->getTickCountNano();
  passed &= check(millis >= nanosBefore / Time::Multiplier::Millisecond &&
                      millis <= nanosAfter / Time::Multiplier::Millisecond,
                  "timer-clock-deadline",
                  "millisecond and nanosecond tick APIs use different clocks or units");

  TimerTestEvent* remainingEvent = new TimerTestEvent(TimerTestEventNumber, false);
  timer->addAlarm(remainingEvent, 2);
  const size_t remaining = timer->removeAlarm(remainingEvent, false);
  delete remainingEvent;
  passed &= check(remaining == 2, "timer-clock-deadline",
                  "alarm removal did not return whole remaining seconds");

  thread->inhibitEvent(TimerTestEventNumber, true);
  TimerTestEvent* overflowEvent = new TimerTestEvent(TimerTestEventNumber, false);
  const uint64_t overflowStart = timer->getTickCountNano();
  timer->addAlarm(overflowEvent, ~static_cast<size_t>(0), ~static_cast<size_t>(0));
  while (timer->getTickCountNano() < overflowStart + (5 * Time::Multiplier::Millisecond)) {
    Scheduler::instance().yield();
  }
  const bool overflowFired = thread->hasEvent(overflowEvent);
  timer->removeAlarm(overflowEvent);
  thread->cullEvent(overflowEvent);
  delete overflowEvent;
  passed &= check(!overflowFired, "timer-clock-deadline",
                  "an overflowing alarm duration wrapped into an immediate deadline");

  TimerTestEvent* deadlineEvent = new TimerTestEvent(TimerTestEventNumber, false);
  const uint64_t start = timer->getTickCountNano();
  timer->addAlarm(deadlineEvent, 0, 20000);

  const uint64_t earlyDeadline = start + (5 * Time::Multiplier::Millisecond);
  while (timer->getTickCountNano() < earlyDeadline) {
    Scheduler::instance().yield();
  }
  const bool firedEarly = thread->hasEvent(deadlineEvent);

  const uint64_t lateDeadline = start + (250 * Time::Multiplier::Millisecond);
  while (!thread->hasEvent(deadlineEvent) && timer->getTickCountNano() < lateDeadline) {
    Scheduler::instance().yield();
  }
  const bool fired = thread->hasEvent(deadlineEvent);

  timer->removeAlarm(deadlineEvent);
  thread->cullEvent(deadlineEvent);
  const bool removed = !thread->hasEvent(deadlineEvent);
  delete deadlineEvent;
  thread->inhibitEvent(TimerTestEventNumber, false);

  passed &= check(!firedEarly, "timer-clock-deadline", "a 20ms alarm fired during its first 5ms");
  passed &= check(fired, "timer-clock-deadline", "a 20ms alarm did not publish within 250ms");
  passed &= check(removed, "timer-clock-deadline", "a cancelled alarm event remained queued");

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS timer-clock-deadline");
  }
  return passed;
}

bool timerAlarmSendLinearization() {
  constexpr const char* Test = "timer-alarm-send-linearization";
  Timer* timer = Machine::instance().getTimer();
  Thread* owner = Processor::information().getCurrentThread();
  AlarmLinearizationContext context(timer, owner);
  context.event = new AlarmLinearizationEvent(context);
  owner->inhibitEvent(AlarmLinearizationEventNumber, true);

  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);
  g_AlarmLinearizationContext = &context;
  HostedTimer::setAlarmSendAdmissionHookForTest(observeAlarmSendAdmission);
  Processor::setInterrupts(interruptsWereEnabled);

  Thread* remover = new Thread(Scheduler::instance().getKernelProcess(), removeLinearizingAlarm,
                               &context, nullptr, false, true);
  remover->setName("hosted alarm linearization remover");
  const bool removerReady = context.removerReady.acquireForCompletion(1, 2);
  if (removerReady) {
    timer->addAlarm(context.event, 0, 1000);
  }
  const bool removerJoined = remover->join();

  Processor::setInterrupts(false);
  HostedTimer::setAlarmSendAdmissionHookForTest(nullptr);
  g_AlarmLinearizationContext = nullptr;
  Processor::setInterrupts(interruptsWereEnabled);

  if (context.event) {
    timer->removeAlarm(context.event);
    owner->cullEvent(context.event);
    delete context.event;
    context.event = nullptr;
  }
  owner->inhibitEvent(AlarmLinearizationEventNumber, false);

  const bool passed =
      check(interruptsWereEnabled && removerReady && removerJoined && context.hookCalls == 1 &&
                context.lockBoundaryObserved == 1 && context.removerReturned == 1 &&
                context.destructions == 1 && context.failures == 0,
            Test, "alarm cancellation crossed the Event send-admission boundary");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS timer-alarm-send-linearization");
  }
  return passed;
}

bool timerAlarmRemovalLifetime() {
  constexpr size_t AlarmBatchSize = 17;
  constexpr size_t AlarmDelaySeconds = ~static_cast<size_t>(0);
  Timer* timer = Machine::instance().getTimer();
  TimerTestEvent* events[AlarmBatchSize] = {};
  bool passed = true;

  // The alarm list retains sixteen erased nodes. The seventeenth removal
  // forces a node free, making an erase-then-dereference bug visible to ASan.
  for (size_t i = 0; i < AlarmBatchSize; ++i) {
    events[i] = new TimerTestEvent(TimerTestEventNumber, false);
    timer->addAlarm(events[i], AlarmDelaySeconds);
  }
  for (size_t i = 0; i < AlarmBatchSize; ++i) {
    timer->removeAlarm(events[i]);
    passed &= timer->removeAlarm(events[i], false) == 0;
    delete events[i];
  }

  for (size_t i = 0; i < AlarmBatchSize; ++i) {
    events[i] = new TimerTestEvent(TimerTestEventNumber, false);
    timer->addAlarm(events[i], AlarmDelaySeconds);
  }
  for (size_t i = 0; i < AlarmBatchSize; ++i) {
    passed &= timer->removeAlarm(events[i], false) > 0;
    passed &= timer->removeAlarm(events[i], false) == 0;
    delete events[i];
  }

  passed &= check(passed, "timer-alarm-removal-lifetime",
                  "an alarm removal overload left its alarm registered");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS timer-alarm-removal-lifetime");
  }
  return passed;
}

bool timerHandlerLifetimeBarrier() {
  Timer* timer = Machine::instance().getTimer();
  HandlerLifetimeContext context(timer);
  Thread* remover = new Thread(Scheduler::instance().getKernelProcess(), unregisterPinnedHandler,
                               &context, nullptr, false, true);
  context.remover = remover;
  remover->setName("hosted timer-handler remover");

  g_HandlerLifetimeContext = &context;
  HostedTimer::setHandlerPinHook(handlerPinHook);
  WaitQueue::setBeforeBlockHook(timerDrainBeforeBlockHook);
  const bool registered = timer->registerHandler(&context.handler);
  const bool joined = remover->join();
  WaitQueue::setBeforeBlockHook(nullptr);
  HostedTimer::setHandlerPinHook(nullptr);
  g_HandlerLifetimeContext = nullptr;

  if (!context.unregisterSucceeded) {
    timer->unregisterHandler(&context.handler);
  }

  const size_t callsAtUnregisterReturn = context.handlerCalls;
  const uint64_t postReturnDeadline =
      timer->getTickCountNano() + (10 * Time::Multiplier::Millisecond);
  while (timer->getTickCountNano() < postReturnDeadline) {
    Scheduler::instance().yield();
  }

  SelfRemovingHandler selfRemoving(timer);
  const bool selfRegistered = timer->registerHandler(&selfRemoving);
  const uint64_t selfDeadline = timer->getTickCountNano() + (250 * Time::Multiplier::Millisecond);
  while (!selfRemoving.calls && timer->getTickCountNano() < selfDeadline) {
    Scheduler::instance().yield();
  }
  const size_t selfCallsAfterRetirement = selfRemoving.calls;
  const uint64_t selfQuietDeadline =
      timer->getTickCountNano() + (10 * Time::Multiplier::Millisecond);
  while (timer->getTickCountNano() < selfQuietDeadline) {
    Scheduler::instance().yield();
  }

  // A callback cannot synchronously wait for its own pin. The rejected
  // call is retired when the callback unwinds, making its slot reusable.
  const bool selfReregistered = timer->registerHandler(&selfRemoving);
  const bool selfCleanup = selfReregistered && timer->unregisterHandler(&selfRemoving);

  bool passed = true;
  passed &= check(registered, "timer-handler-lifetime", "the test handler could not be registered");
  passed &= check(joined && context.failures == 0, "timer-handler-lifetime",
                  "the concurrent unregister worker did not complete cleanly");
  passed &=
      check(context.hookCalls == 1 && context.hookObservedDrain == 1, "timer-handler-lifetime",
            "unregister returned instead of waiting for the pinned callback");
  const bool waitQueueDrainPassed =
      registered && joined && context.waitHookCalls == 1 && context.waitHookFailures == 0 &&
      context.drainWaitPublished == 1 && context.wakeBeforeBlock == 1 &&
      context.hookObservedDrain == 1 && context.unregisterSucceeded == 1 &&
      context.unregisterReturned == 1;
  passed &= check(waitQueueDrainPassed, "timer-handler-waitqueue-drain",
                  "callback drain did not publish and consume a pre-block wake");
  if (waitQueueDrainPassed) {
    NOTICE("HOSTED-WAIT-TEST: PASS timer-handler-waitqueue-drain");
  }
  passed &= check(context.unregisterSucceeded == 1 && context.unregisterReturned == 1,
                  "timer-handler-lifetime", "the pinned handler did not unregister successfully");
  const bool selfDrainRacePassed =
      registered && joined && context.failures == 0 && context.hookObservedDrain == 1 &&
      context.selfRemovalPending == 0 && context.selfRemovalRejected == 1 &&
      context.unregisterSucceeded == 1 && context.unregisterReturned == 1 &&
      context.revivalBlocked == 1;
  passed &= check(selfDrainRacePassed, "timer-self-unregister-vs-drainer",
                  "self-removal escaped or revived a synchronous drain");
  if (selfDrainRacePassed) {
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "timer-self-unregister-vs-drainer");
  }
  passed &=
      check(context.handlerCalls >= 1 && context.handlerCalls == callsAtUnregisterReturn &&
                context.callbacksAfterReturn == 0,
            "timer-handler-lifetime", "a timer callback began after unregisterHandler returned");
  passed &=
      check(selfRegistered && selfRemoving.rejectionSeen == 1 && selfCallsAfterRetirement == 1 &&
                selfRemoving.calls == selfCallsAfterRetirement,
            "timer-handler-lifetime",
            "self-unregister was not rejected and retired after callback return");
  passed &= check(selfReregistered && selfCleanup, "timer-handler-lifetime",
                  "deferred self-removal did not release its registry slot");

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS timer-handler-lifetime");
  }
  return passed;
}

struct SemaphoreTimeoutContext {
  SemaphoreTimeoutContext(Semaphore* semaphore, Thread* waiter)
      : semaphore(semaphore), waiter(waiter), sawTimeoutEvent(0), failures(0) {}

  Semaphore* semaphore;
  Thread* waiter;
  Atomic<size_t> sawTimeoutEvent;
  Atomic<size_t> failures;
};

int releaseAfterTimeoutQueued(void* parameter) {
  SemaphoreTimeoutContext* context = reinterpret_cast<SemaphoreTimeoutContext*>(parameter);
  Timer* timer = Machine::instance().getTimer();
  const uint64_t deadline = timer->getTickCountNano() + (500 * Time::Multiplier::Millisecond);

  while (!context->waiter->hasEvent(EventNumbers::Interrupt) &&
         timer->getTickCountNano() < deadline) {
    Scheduler::instance().yield();
  }

  if (!context->waiter->hasEvent(EventNumbers::Interrupt)) {
    context->failures += 1;
    return 1;
  }

  context->sawTimeoutEvent += 1;
  context->semaphore->release();
  return 0;
}

bool semaphoreQueuedTimeoutCancellation(Thread* thread) {
  Semaphore semaphore(0);
  SemaphoreTimeoutContext context(&semaphore, thread);
  Thread* releaser = new Thread(Scheduler::instance().getKernelProcess(), releaseAfterTimeoutQueued,
                                &context, nullptr, false, true);
  releaser->setName("hosted queued-timeout releaser");

  thread->inhibitEvent(EventNumbers::Interrupt, true);
  Semaphore::SemaphoreError error = Semaphore::NoError;
  const bool acquired = semaphore.acquireWithError(1, 0, 20000, error);
  const bool joined = releaser->join();
  const bool eventRemoved = !thread->hasEvent(EventNumbers::Interrupt);
  thread->inhibitEvent(EventNumbers::Interrupt, false);

  bool passed = true;
  passed &= check(acquired && error == Semaphore::NoError, "semaphore-timeout-cancel",
                  "a signalled waiter reported the queued timeout instead");
  passed &=
      check(joined && context.failures == 0 && context.sawTimeoutEvent == 1,
            "semaphore-timeout-cancel", "the releaser did not observe exactly one queued timeout");
  passed &=
      check(eventRemoved, "semaphore-timeout-cancel", "the losing timeout event remained queued");
  passed &= check(semaphore.getValue() == 0, "semaphore-timeout-cancel",
                  "the released semaphore unit was not consumed");

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS fresh-thread-timer-progress");
    NOTICE("HOSTED-WAIT-TEST: PASS semaphore-timeout-cancel");
  }
  return passed;
}

bool relayUsesLatestDisposition(Thread* thread) {
  Timer* timer = Machine::instance().getTimer();
  TimerTestEvent dispositionA(DispositionAEventNumber, false,
                              reinterpret_cast<uintptr_t>(&dispositionAHandler));
  TimerTestEvent dispositionB(DispositionBEventNumber, false,
                              reinterpret_cast<uintptr_t>(&dispositionBHandler));
  RelayEvent relay(&relayDisposition, RelayTestEventNumber);

  g_RelayCallbacks = 0;
  g_DispositionACalls = 0;
  g_DispositionBCalls = 0;
  g_DispositionAEvent = &dispositionA;
  g_DispositionBEvent = &dispositionB;

  g_RelayDisposition = 1;
  timer->addAlarm(&relay, 0, 20000);
  g_RelayDisposition = 2;

  const uint64_t deadline = timer->getTickCountNano() + (250 * Time::Multiplier::Millisecond);
  while (!g_DispositionBCalls && timer->getTickCountNano() < deadline) {
    Scheduler::instance().yield();
  }

  timer->removeAlarm(&relay);
  thread->cullEvent(&relay);
  thread->cullEvent(&dispositionA);
  thread->cullEvent(&dispositionB);
  g_DispositionAEvent = nullptr;
  g_DispositionBEvent = nullptr;

  const bool passed =
      check(g_RelayCallbacks == 1 && g_DispositionACalls == 0 && g_DispositionBCalls == 1,
            "relay-latest-disposition", "timer expiry retained the disposition from arm time");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS relay-latest-disposition");
  }
  return passed;
}

bool exactCullRetainsOwnership(Thread* thread) {
  const size_t destructionsBefore = g_CullEventDestructions;
  TimerTestEvent* event = new TimerTestEvent(CullTestEventNumber, true);

  thread->inhibitEvent(CullTestEventNumber, true);
  const bool queued = thread->sendEvent(event);
  thread->cullEvent(event);
  const bool removed = !thread->hasEvent(event);
  const bool retained = g_CullEventDestructions == destructionsBefore;

  if (retained) {
    delete event;
  }
  thread->inhibitEvent(CullTestEventNumber, false);

  bool passed = true;
  passed &= check(queued && removed, "timeoutguard-cancel-ownership",
                  "the exact queued event was not culled");
  passed &= check(retained, "timeoutguard-cancel-ownership",
                  "exact-event culling deleted caller-owned storage");
  passed &=
      check(g_CullEventDestructions == destructionsBefore + 1, "timeoutguard-cancel-ownership",
            "caller-owned event destruction did not occur exactly once");

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS timeoutguard-cancel-ownership");
  }
  return passed;
}
}  // namespace

bool runHostedTimerRegressions(Thread* thread) {
  return hostedTimerThreadContext(Processor::information().getCurrentThread()) &&
         hostedTimerOverrunAccounting() && timerWriterLockIndependentDispatch() &&
         timerPrePinUnregisterRevalidation() && timerAtomicDrainSelfRevival() &&
         timerPartialHazardAbandonment() && timerCommittedHazardAbandonment() &&
         timerDeferredSelfRemovalAbandonment() && timerClockAndDeadline(thread) &&
         timerAlarmSendLinearization() && timerAlarmRemovalLifetime() &&
         timerHandlerLifetimeBarrier() && semaphoreQueuedTimeoutCancellation(thread) &&
         relayUsesLatestDisposition(thread) && exactCullRetainsOwnership(thread);
}
