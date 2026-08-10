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
#include "pedigree/kernel/time/Time.h"

#include "modules/system/vfs/Pipe.h"

namespace {
struct PipeWaitContext {
  explicit PipeWaitContext(Pipe* pipe) : pipe(pipe), entered(0), returned(0), observedReader(0) {}

  Pipe* pipe;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
  Atomic<size_t> observedReader;
};

int waitForPipeReader(void* parameter) {
  PipeWaitContext* context = reinterpret_cast<PipeWaitContext*>(parameter);
  context->entered += 1;
  if (context->pipe->waitForReader(true)) {
    context->observedReader += 1;
  }
  context->returned += 1;
  return 0;
}

Thread* startPipeWaiter(PipeWaitContext& context, const char* name) {
  Thread* thread = new Thread(Scheduler::instance().getKernelProcess(), waitForPipeReader, &context,
                              nullptr, false, true);
  thread->setName(String(name));
  return thread;
}

bool waitForCount(Atomic<size_t>& value, size_t expected) {
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (value < expected && Time::getTicks() < deadline) {
    Scheduler::instance().yield();
  }
  return value == expected;
}

bool waitUntilBlocked(Thread* thread) {
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (Time::getTicks() < deadline) {
    Thread::WaitDebugInfo wait = {};
    uintptr_t address = 0;
    if (thread->getWaitDebugInfo(wait) && wait.queued &&
        thread->getDebugState(address) == Thread::CondWait) {
      return true;
    }
    if (thread->getStatus() == Thread::AwaitingJoin) {
      return false;
    }
    Scheduler::instance().yield();
  }
  return false;
}

bool staleReaderDoesNotSatisfyOpen() {
  Pipe pipe(String("hosted-stale-reader-fifo"), 0, 0, 0, 1, nullptr, 0, nullptr, false);

  // A reader which has already departed is not the FIFO-open predicate.
  pipe.increaseRefCount(false);
  pipe.decreaseRefCount(false);

  PipeWaitContext context(&pipe);
  Thread* waiter = startPipeWaiter(context, "hosted stale FIFO reader waiter");
  const bool blocked = waitUntilBlocked(waiter) && context.returned == 0;

  pipe.increaseRefCount(false);
  const bool returned = waitForCount(context.returned, 1);
  const bool joined = waiter->join();
  pipe.decreaseRefCount(false);

  return blocked && returned && joined && context.observedReader == 1;
}

bool oneReaderWakesEveryWriter() {
  Pipe pipe(String("hosted-broadcast-reader-fifo"), 0, 0, 0, 2, nullptr, 0, nullptr, false);
  PipeWaitContext firstContext(&pipe);
  PipeWaitContext secondContext(&pipe);
  Thread* first = startPipeWaiter(firstContext, "hosted FIFO writer waiter one");
  Thread* second = startPipeWaiter(secondContext, "hosted FIFO writer waiter two");

  const bool bothBlocked = waitUntilBlocked(first) && waitUntilBlocked(second) &&
                           firstContext.returned == 0 && secondContext.returned == 0;

  pipe.increaseRefCount(false);
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while ((firstContext.returned != 1 || secondContext.returned != 1) &&
         Time::getTicks() < deadline) {
    Scheduler::instance().yield();
  }
  const bool bothReturned = firstContext.returned == 1 && secondContext.returned == 1;

  // Keep cleanup finite even when this regression detects the historical
  // one-token/one-writer behavior.
  if (!bothReturned) {
    pipe.increaseRefCount(false);
  }
  const bool firstJoined = first->join();
  const bool secondJoined = second->join();
  pipe.decreaseRefCount(false);
  if (!bothReturned) {
    pipe.decreaseRefCount(false);
  }

  return bothBlocked && bothReturned && firstJoined && secondJoined &&
         firstContext.observedReader == 1 && secondContext.observedReader == 1;
}
}  // namespace

bool runHostedPipeRegressions() {
  const bool passed = staleReaderDoesNotSatisfyOpen() && oneReaderWakesEveryWriter();
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS fifo-reader-predicate");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL fifo-reader-predicate: FIFO open did not "
        "use one locked reader predicate");
  }
  return passed;
}
