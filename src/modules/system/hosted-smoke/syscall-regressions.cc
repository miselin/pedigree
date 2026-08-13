/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/Uninterruptible.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/SyscallHandler.h"
#include "pedigree/kernel/processor/SyscallManager.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/utility.h"

namespace {
bool check(bool condition, const char* test, const char* detail) {
  if (condition) {
    return true;
  }

  ERROR("HOSTED-WAIT-TEST: FAIL " << test << ": " << detail);
  return false;
}

struct HandlerLifetimeContext;
HandlerLifetimeContext* g_HandlerLifetimeContext = nullptr;

class LifetimeHandler : public SyscallHandler {
 public:
  explicit LifetimeHandler(HandlerLifetimeContext& context) : m_Context(context) {}

  uintptr_t syscall(SyscallState&) override;

 private:
  HandlerLifetimeContext& m_Context;
};

struct HandlerLifetimeContext {
  HandlerLifetimeContext()
      : handler(*this),
        remover(nullptr),
        phase(0),
        hookCalls(0),
        hookObservedDrain(0),
        handlerCalls(0),
        callbacksAfterReturn(0),
        unregisterReturned(0),
        unregisterSucceeded(0),
        failures(0) {}

  LifetimeHandler handler;
  SyscallManager::Registration registration;
  Thread* remover;
  Atomic<size_t> phase;
  Atomic<size_t> hookCalls;
  Atomic<size_t> hookObservedDrain;
  Atomic<size_t> handlerCalls;
  Atomic<size_t> callbacksAfterReturn;
  Atomic<size_t> unregisterReturned;
  Atomic<size_t> unregisterSucceeded;
  Atomic<size_t> failures;
};

uintptr_t LifetimeHandler::syscall(SyscallState&) {
  m_Context.handlerCalls += 1;
  if (m_Context.unregisterReturned) {
    m_Context.callbacksAfterReturn += 1;
  }
  return 0x51;
}

class SelfUnregisteringHandler : public SyscallHandler {
 public:
  explicit SelfUnregisteringHandler(SyscallManager::Registration& registration)
      : m_Registration(registration), calls(0), rejectionSeen(0) {}

  uintptr_t syscall(SyscallState&) override {
    calls += 1;
    if (!m_Registration.reset() && m_Registration) {
      rejectionSeen += 1;
    }
    return 0x52;
  }

  SyscallManager::Registration& m_Registration;
  Atomic<size_t> calls;
  Atomic<size_t> rejectionSeen;
};

struct ReciprocalUnregisterContext;

class ReciprocalUnregisterHandler : public SyscallHandler {
 public:
  ReciprocalUnregisterHandler(ReciprocalUnregisterContext& context, bool first)
      : m_Context(context), m_First(first) {}

  uintptr_t syscall(SyscallState&) override;

 private:
  ReciprocalUnregisterContext& m_Context;
  bool m_First;
};

struct ReciprocalUnregisterContext {
  ReciprocalUnregisterContext()
      : first(*this, true),
        second(*this, false),
        entered(0),
        attempts(0),
        rejections(0),
        resetReturns(0),
        failures(0) {}

  ReciprocalUnregisterHandler first;
  ReciprocalUnregisterHandler second;
  SyscallManager::Registration firstRegistration;
  SyscallManager::Registration secondRegistration;
  Atomic<size_t> entered;
  Atomic<size_t> attempts;
  Atomic<size_t> rejections;
  Atomic<size_t> resetReturns;
  Atomic<size_t> failures;
};

uintptr_t ReciprocalUnregisterHandler::syscall(SyscallState&) {
  m_Context.entered += 1;
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (m_Context.entered != static_cast<size_t>(2) && Time::getTicks() < deadline) {
    Scheduler::instance().yield();
  }

  if (m_Context.entered != static_cast<size_t>(2)) {
    m_Context.failures += 1;
    return 0;
  }

  m_Context.attempts += 1;
  SyscallManager::Registration& peer =
      m_First ? m_Context.secondRegistration : m_Context.firstRegistration;
  if (!peer.reset() && peer) {
    m_Context.rejections += 1;
  } else {
    m_Context.failures += 1;
  }
  m_Context.resetReturns += 1;
  const Time::Timestamp returnDeadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (m_Context.resetReturns != static_cast<size_t>(2) && Time::getTicks() < returnDeadline) {
    Scheduler::instance().yield();
  }
  if (m_Context.resetReturns != static_cast<size_t>(2)) {
    m_Context.failures += 1;
  }
  return m_First ? 0x53 : 0x54;
}

struct ReciprocalSyscallInvocation {
  ReciprocalUnregisterContext* context;
  Service_t service;
  uintptr_t result;
};

int invokeReciprocalSyscall(void* parameter) {
  ReciprocalSyscallInvocation* invocation =
      reinterpret_cast<ReciprocalSyscallInvocation*>(parameter);
  invocation->result = SyscallManager::instance().syscall(invocation->service, 0);
  return 0;
}

bool reciprocalUnregisterRejected() {
  SyscallManager& manager = SyscallManager::instance();
  ReciprocalUnregisterContext context;
  const bool registered =
      manager.registerSyscallHandler(TUI, &context.first, context.firstRegistration) &&
      manager.registerSyscallHandler(native, &context.second, context.secondRegistration);
  if (!registered) {
    if (context.firstRegistration) {
      context.firstRegistration.reset();
    }
    return check(false, "syscall-reciprocal-unregister",
                 "the reciprocal handlers could not register");
  }

  ReciprocalSyscallInvocation first = {&context, TUI, 0};
  ReciprocalSyscallInvocation second = {&context, native, 0};
  Thread* firstThread = new Thread(Scheduler::instance().getKernelProcess(),
                                   invokeReciprocalSyscall, &first, nullptr, false, true);
  Thread* secondThread = new Thread(Scheduler::instance().getKernelProcess(),
                                    invokeReciprocalSyscall, &second, nullptr, false, true);
  firstThread->setName("hosted syscall reciprocal first");
  secondThread->setName("hosted syscall reciprocal second");

  const bool firstJoined = firstThread->join();
  const bool secondJoined = secondThread->join();
  const bool registrationsPreserved = context.firstRegistration && context.secondRegistration;
  const bool firstRetired = context.firstRegistration.reset();
  const bool secondRetired = context.secondRegistration.reset();
  const bool passed =
      check(firstJoined && secondJoined && first.result == 0x53 && second.result == 0x54 &&
                context.entered == 2 && context.attempts == 2 && context.rejections == 2 &&
                context.resetReturns == 2 && context.failures == 0,
            "syscall-reciprocal-unregister", "reciprocal callbacks did not finish cleanly") &&
      check(registrationsPreserved && firstRetired && secondRetired,
            "syscall-reciprocal-unregister",
            "callback-context rejection lost external cleanup ownership");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS syscall-reciprocal-unregister");
  }
  return passed;
}

void handlerPinHook(Service_t service, SyscallHandler* handler) {
  HandlerLifetimeContext* context = g_HandlerLifetimeContext;
  if (!context || service != TUI || handler != &context->handler ||
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
        debugAddress == reinterpret_cast<uintptr_t>(&context->handler)) {
      context->hookObservedDrain += 1;
      context->phase = 3;
      return;
    }
    Scheduler::instance().yield();
  }

  context->failures += 1;
  context->phase = 3;
}

int unregisterPinnedHandler(void* parameter) {
  HandlerLifetimeContext* context = reinterpret_cast<HandlerLifetimeContext*>(parameter);
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (context->phase != static_cast<size_t>(1) && Time::getTicks() < deadline) {
    Scheduler::instance().yield();
  }

  if (context->phase != static_cast<size_t>(1)) {
    context->failures += 1;
    return 1;
  }

  context->phase = 2;
  if (context->registration.reset()) {
    context->unregisterSucceeded += 1;
  }
  context->unregisterReturned += 1;
  return 0;
}

bool handlerLifetimeBarrier() {
  SyscallManager& manager = SyscallManager::instance();
  HandlerLifetimeContext context;
  context.remover = new Thread(Scheduler::instance().getKernelProcess(), unregisterPinnedHandler,
                               &context, nullptr, false, true);
  context.remover->setName("hosted syscall-handler remover");

  g_HandlerLifetimeContext = &context;
  manager.setHandlerPinHook(handlerPinHook);
  const bool registered =
      manager.registerSyscallHandler(TUI, &context.handler, context.registration);
  SyscallManager::Registration duplicate;
  const bool duplicateRejected =
      !manager.registerSyscallHandler(TUI, &context.handler, duplicate) && !duplicate;
  const uintptr_t result = manager.syscall(TUI, 0);
  const bool joined = context.remover->join();
  manager.setHandlerPinHook(nullptr);
  g_HandlerLifetimeContext = nullptr;

  const size_t callsAtUnregisterReturn = context.handlerCalls;
  const uintptr_t lateResult = manager.syscall(TUI, 0);
  const bool lateDispatchRejected = lateResult == 0 &&
                                    context.handlerCalls == callsAtUnregisterReturn &&
                                    context.callbacksAfterReturn == 0;

  SyscallManager::Registration reusable;
  const bool reregistered = manager.registerSyscallHandler(TUI, &context.handler, reusable);
  const uintptr_t redispatchResult = reregistered ? manager.syscall(TUI, 0) : 0;
  SyscallManager::Registration moved(pedigree_std::move(reusable));
  const bool movePreservedOwnership = !reusable && moved && moved.reset();

  SyscallManager::Registration selfRegistration;
  SelfUnregisteringHandler selfUnregistering(selfRegistration);
  const bool selfRegistered =
      manager.registerSyscallHandler(TUI, &selfUnregistering, selfRegistration);
  const uintptr_t selfResult = selfRegistered ? manager.syscall(TUI, 0) : 0;
  const bool selfCleanup = selfRegistered && selfRegistration.reset();

  const bool passed =
      check(registered && duplicateRejected && result == 0x51, "syscall-handler-lifetime",
            "registration, token ownership, or dispatch failed") &&
      check(joined && context.failures == 0, "syscall-handler-lifetime",
            "the unregister worker did not finish cleanly") &&
      check(context.hookCalls == 1 && context.hookObservedDrain == 1 &&
                context.unregisterSucceeded == 1 && context.unregisterReturned == 1,
            "syscall-handler-lifetime", "unregister returned before the admitted handler") &&
      check(lateDispatchRejected, "syscall-handler-lifetime",
            "a handler ran after token reset returned") &&
      check(reregistered && redispatchResult == 0x51 && movePreservedOwnership,
            "syscall-handler-lifetime", "generation-bearing token move or slot reuse failed") &&
      check(selfRegistered && selfResult == 0x52 && selfUnregistering.calls == 1 &&
                selfUnregistering.rejectionSeen == 1 && selfCleanup,
            "syscall-handler-lifetime", "self-reset changed ownership or reported safe teardown");

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS syscall-handler-lifetime");
  }
  return passed;
}

class PostActionHandler : public SyscallHandler {
 public:
  PostActionHandler() : requested(SyscallManager::NoPostSyscallAction), calls(0), failures(0) {}

  uintptr_t syscall(SyscallState&) override {
    bool staged = false;
    switch (requested) {
      case SyscallManager::TerminateCurrentThread:
        staged = SyscallManager::instance().requestThreadExit();
        break;
      case SyscallManager::ExitCurrentProcess:
        staged = SyscallManager::instance().requestProcessExit(37);
        break;
      case SyscallManager::ReturnFromEvent:
        staged = SyscallManager::instance().requestEventReturn();
        break;
      case SyscallManager::PopEventState:
        staged = SyscallManager::instance().requestEventStatePop();
        break;
      case SyscallManager::RestoreProcessorState: {
        ProcessorState state;
        state.setInstructionPointer(0x1234);
        state.setStackPointer(0x5678);
        staged = SyscallManager::instance().requestStateRestore(state);
        break;
      }
      case SyscallManager::JumpToUserspace:
        staged = SyscallManager::instance().requestUserJump(0x1234, 0x5678);
        break;
      case SyscallManager::RebootSystem:
        staged = SyscallManager::instance().requestReboot();
        break;
      case SyscallManager::NoPostSyscallAction:
        break;
    }

    calls += 1;
    if (!staged) {
      failures += 1;
    }
    return 0x61;
  }

  SyscallManager::PostSyscallActionKind requested;
  Atomic<size_t> calls;
  Atomic<size_t> failures;
};

struct PostActionContext {
  SyscallManager::PostSyscallActionKind expected = SyscallManager::NoPostSyscallAction;
  intptr_t expectedValue = 0;
  SyscallManager::Registration* registration = nullptr;
  Atomic<size_t> calls = 0;
  Atomic<size_t> retired = 0;
  Atomic<size_t> failures = 0;
};

PostActionContext* g_PostActionContext = nullptr;

bool postActionHook(SyscallManager::PostSyscallActionKind kind, intptr_t value) {
  PostActionContext* context = g_PostActionContext;
  if (!context) {
    return false;
  }

  context->calls += 1;
  Thread* thread = Processor::information().getCurrentThread();
  if (kind != context->expected || value != context->expectedValue || !thread ||
      thread->isTerminationDeferred()) {
    context->failures += 1;
  }
  if (context->registration && context->registration->reset()) {
    context->retired += 1;
  } else {
    context->failures += 1;
  }
  return true;
}

bool postSyscallActions() {
  static const SyscallManager::PostSyscallActionKind actions[] = {
      SyscallManager::TerminateCurrentThread,
      SyscallManager::ExitCurrentProcess,
      SyscallManager::ReturnFromEvent,
      SyscallManager::PopEventState,
      SyscallManager::RestoreProcessorState,
      SyscallManager::JumpToUserspace,
      SyscallManager::RebootSystem};
  constexpr size_t ActionCount = sizeof(actions) / sizeof(actions[0]);

  SyscallManager& manager = SyscallManager::instance();
  PostActionHandler handler;
  PostActionContext context;
  g_PostActionContext = &context;
  manager.setPostSyscallHook(postActionHook);

  bool loopPassed = true;
  for (size_t i = 0; i < ActionCount; ++i) {
    const bool needsEventState = actions[i] == SyscallManager::ReturnFromEvent ||
                                 actions[i] == SyscallManager::PopEventState;
    Thread* thread = Processor::information().getCurrentThread();
    const bool stateReady = !needsEventState || (thread && thread->pushState() != nullptr);
    SyscallManager::Registration registration;
    context.expected = actions[i];
    context.expectedValue = actions[i] == SyscallManager::ExitCurrentProcess ? 37 : 0;
    context.registration = &registration;
    handler.requested = actions[i];
    if (!stateReady || !manager.registerSyscallHandler(TUI, &handler, registration) ||
        manager.syscall(TUI, 0) != 0x61 || registration) {
      loopPassed = false;
      if (registration) {
        registration.reset();
      }
    }
    if (needsEventState && stateReady) {
      thread->popState();
    }
  }

  manager.setPostSyscallHook(nullptr);
  g_PostActionContext = nullptr;
  const bool passed = check(
      loopPassed && handler.calls == ActionCount && handler.failures == 0 &&
          context.calls == ActionCount && context.retired == ActionCount && context.failures == 0,
      "syscall-post-actions", "a terminal action ran before its handler token retired");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS syscall-post-actions");
  }
  return passed;
}

class InvalidEventActionHandler : public SyscallHandler {
 public:
  InvalidEventActionHandler()
      : requested(SyscallManager::NoPostSyscallAction), calls(0), rejections(0) {}

  uintptr_t syscall(SyscallState&) override {
    bool accepted = false;
    if (requested == SyscallManager::ReturnFromEvent) {
      accepted = SyscallManager::instance().requestEventReturn();
    } else if (requested == SyscallManager::PopEventState) {
      accepted = SyscallManager::instance().requestEventStatePop();
    }

    calls += 1;
    if (!accepted) {
      rejections += 1;
    }
    return 0x62;
  }

  SyscallManager::PostSyscallActionKind requested;
  Atomic<size_t> calls;
  Atomic<size_t> rejections;
};

bool baseStateEventActionsRejected() {
  Thread* thread = Processor::information().getCurrentThread();
  SyscallManager& manager = SyscallManager::instance();
  InvalidEventActionHandler handler;
  SyscallManager::Registration registration;
  const bool registered = thread && !thread->getStateLevel() &&
                          manager.registerSyscallHandler(TUI, &handler, registration);

  handler.requested = SyscallManager::ReturnFromEvent;
  const uintptr_t returnResult = registered ? manager.syscall(TUI, 0) : 0;
  handler.requested = SyscallManager::PopEventState;
  const uintptr_t popResult = registered ? manager.syscall(TUI, 0) : 0;
  const bool retired = registered && registration.reset();

  const bool passed =
      check(registered && returnResult == 0x62 && popResult == 0x62 && handler.calls == 2 &&
                handler.rejections == 2 && retired && !thread->getStateLevel(),
            "syscall-event-action-boundary", "a base-state event return or pop was accepted");
  if (passed) {
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "syscall-event-action-boundary");
  }
  return passed;
}

struct AbandonedStackContext;

class AbandoningHandler : public SyscallHandler {
 public:
  explicit AbandoningHandler(AbandonedStackContext& context) : m_Context(context) {}

  uintptr_t syscall(SyscallState&) override;

 private:
  AbandonedStackContext& m_Context;
};

struct AbandonedStackContext {
  AbandonedStackContext() : handler(*this), entered(0), returned(0), destructed(0) {}

  AbandoningHandler handler;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
  Atomic<size_t> destructed;
};

class AbandonedStackCanary {
 public:
  explicit AbandonedStackCanary(Atomic<size_t>* destructed) : m_Destructed(destructed) {}

  ~AbandonedStackCanary() {
    *m_Destructed += 1;
  }

 private:
  Atomic<size_t>* m_Destructed;
};

uintptr_t AbandoningHandler::syscall(SyscallState&) {
  Thread* thread = Processor::information().getCurrentThread();
  if (!thread->pushState()) {
    return 0;
  }

  Uninterruptible eventAndTerminationDeferral;
  TerminationDeferral nestedTerminationDeferral;
  AbandonedStackCanary stackCanary(&m_Context.destructed);
  m_Context.entered += 1;
  thread->getScheduler()->abandonCurrentThreadStack(
      PerProcessorScheduler::StackDiscardReason::HostedRegression);
}

int invokeAbandoningSyscall(void* parameter) {
  AbandonedStackContext* context = reinterpret_cast<AbandonedStackContext*>(parameter);
  SyscallManager::instance().syscall(TUI, 0);
  context->returned += 1;
  return 1;
}

bool abandonedSyscallStack() {
  SyscallManager& manager = SyscallManager::instance();
  AbandonedStackContext context;
  SyscallManager::Registration registration;
  if (!manager.registerSyscallHandler(TUI, &context.handler, registration)) {
    return check(false, "syscall-abandoned-stack", "the abandoning handler could not register");
  }

  const size_t discardsBefore = PerProcessorScheduler::stackDiscardCount(
      PerProcessorScheduler::StackDiscardReason::HostedRegression);
  Thread* thread = new Thread(Scheduler::instance().getKernelProcess(), invokeAbandoningSyscall,
                              &context, nullptr, false, true);
  thread->setName("hosted abandoning syscall");
  const bool joined = thread->join();
  const bool retired = registration.reset();

  const bool passed = check(
      joined && retired && context.entered == 1 && context.returned == 0 &&
          context.destructed == 0 &&
          PerProcessorScheduler::stackDiscardCount(
              PerProcessorScheduler::StackDiscardReason::HostedRegression) == discardsBefore + 1,
      "syscall-abandoned-stack",
      "forced teardown did not count exactly one intentionally leaked stack");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS syscall-abandoned-stack");
  }
  return passed;
}
}  // namespace

bool runHostedSyscallRegressions() {
  return handlerLifetimeBarrier() && reciprocalUnregisterRejected() && postSyscallActions() &&
         baseStateEventActionsRejected() && abandonedSyscallStack();
}
