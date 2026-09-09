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
#include "pedigree/kernel/utilities/Buffer.h"

namespace {
enum class BufferOperation {
  Read,
  Write,
  WriteAvailable,
  CanRead,
  CanWrite,
};

struct BufferWaitContext {
  BufferWaitContext(Buffer<uint8_t>* buffer, BufferOperation operation, uint8_t value)
      : buffer(buffer), operation(operation), entered(0), returned(0), result(1), value(value) {}

  Buffer<uint8_t>* buffer;
  BufferOperation operation;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
  Atomic<size_t> result;
  uint8_t value;
};

int waitOnBuffer(void* parameter) {
  BufferWaitContext* context = reinterpret_cast<BufferWaitContext*>(parameter);
  context->entered += 1;
  switch (context->operation) {
    case BufferOperation::Read:
      context->result = context->buffer->read(&context->value, 1, true);
      break;
    case BufferOperation::Write:
      context->result = context->buffer->write(&context->value, 1, true);
      break;
    case BufferOperation::WriteAvailable:
      context->result = context->buffer->writeAvailable(&context->value, 1);
      break;
    case BufferOperation::CanRead:
      context->result = context->buffer->canRead(true) ? 1 : 0;
      break;
    case BufferOperation::CanWrite:
      context->result = context->buffer->canWrite(true) ? 1 : 0;
      break;
  }
  context->returned += 1;
  return 0;
}

bool waitUntilBlocked(Thread* thread, BufferWaitContext& context,
                      Thread::DebugState state = Thread::CondWait) {
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (Time::getTicks() < deadline) {
    Thread::WaitDebugInfo wait = {};
    uintptr_t address = 0;
    if (context.entered == 1 && context.returned == 0 && thread->getWaitDebugInfo(wait) &&
        wait.queued && thread->getDebugState(address) == state) {
      return true;
    }
    if (thread->getStatus() == Thread::AwaitingJoin) {
      return false;
    }
    Scheduler::instance().yield();
  }
  return false;
}

bool waitUntilReturned(BufferWaitContext& first, BufferWaitContext& second) {
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while ((first.returned == 0 || second.returned == 0) && Time::getTicks() < deadline) {
    Scheduler::instance().yield();
  }
  return first.returned == 1 && second.returned == 1;
}

bool availableWriteWaitsForLockOwnership() {
  Buffer<uint8_t> buffer(1);
  BufferWaitContext context(&buffer, BufferOperation::WriteAvailable, 0x92);
  Thread* writer = new Thread(Scheduler::instance().getKernelProcess(), waitOnBuffer, &context,
                              nullptr, false, true, true);
  writer->setName("hosted Buffer available-write contention");

  buffer.acquireHostedOperationLock();
  const bool started = writer->start();
  const bool blocked = started && waitUntilBlocked(writer, context, Thread::SemWait);
  buffer.releaseHostedOperationLock();
  const bool joined = started && writer->joinForCompletion();
  if (!started) {
    delete writer;
  }

  uint8_t observed = 0;
  const bool delivered = buffer.read(&observed, 1, false) == 1 && observed == context.value;
  return started && blocked && joined && context.returned == 1 && context.result == 1 &&
         delivered && buffer.getHostedActiveOperationCount() == 0;
}

bool blockedReaderReturnsWhenReadsAreDisabled() {
  Buffer<uint8_t> buffer(1);
  BufferWaitContext firstContext(&buffer, BufferOperation::Read, 0);
  BufferWaitContext secondContext(&buffer, BufferOperation::Read, 0);
  Thread* first = new Thread(Scheduler::instance().getKernelProcess(), waitOnBuffer, &firstContext,
                             nullptr, false, true, true);
  Thread* second = new Thread(Scheduler::instance().getKernelProcess(), waitOnBuffer,
                              &secondContext, nullptr, false, true, true);
  first->setName("hosted disabled-buffer reader one");
  second->setName("hosted disabled-buffer reader two");

  const bool firstStarted = first->start();
  const bool secondStarted = second->start();
  const bool started = firstStarted && secondStarted;
  const bool blocked =
      started && waitUntilBlocked(first, firstContext) && waitUntilBlocked(second, secondContext);
  buffer.disableReads();
  const bool returned = blocked && waitUntilReturned(firstContext, secondContext);

  // Keep the fixture finite against the historical one-sided wakeup.
  if ((firstStarted || secondStarted) && !returned) {
    buffer.disableWrites();
  }
  const bool firstJoined = firstStarted && first->joinForCompletion();
  const bool secondJoined = secondStarted && second->joinForCompletion();
  if (!firstStarted) {
    delete first;
  }
  if (!secondStarted) {
    delete second;
  }

  return started && blocked && returned && firstJoined && secondJoined &&
         firstContext.entered == 1 && firstContext.returned == 1 && firstContext.result == 0 &&
         secondContext.entered == 1 && secondContext.returned == 1 && secondContext.result == 0;
}

bool blockedWriterReturnsWhenWritesAreDisabled() {
  Buffer<uint8_t> buffer(1);
  const uint8_t firstValue = 0x51;
  const bool filled = buffer.write(&firstValue, 1, false) == 1;
  BufferWaitContext firstContext(&buffer, BufferOperation::Write, 0x7a);
  BufferWaitContext secondContext(&buffer, BufferOperation::Write, 0x39);
  Thread* first = new Thread(Scheduler::instance().getKernelProcess(), waitOnBuffer, &firstContext,
                             nullptr, false, true, true);
  Thread* second = new Thread(Scheduler::instance().getKernelProcess(), waitOnBuffer,
                              &secondContext, nullptr, false, true, true);
  first->setName("hosted disabled-buffer writer one");
  second->setName("hosted disabled-buffer writer two");

  const bool firstStarted = filled && first->start();
  const bool secondStarted = filled && second->start();
  const bool started = firstStarted && secondStarted;
  const bool blocked =
      started && waitUntilBlocked(first, firstContext) && waitUntilBlocked(second, secondContext);
  buffer.disableWrites();
  const bool returned = blocked && waitUntilReturned(firstContext, secondContext);

  // Keep the fixture finite against the historical one-sided wakeup.
  if ((firstStarted || secondStarted) && !returned) {
    buffer.disableReads();
  }
  const bool firstJoined = firstStarted && first->joinForCompletion();
  const bool secondJoined = secondStarted && second->joinForCompletion();
  if (!firstStarted) {
    delete first;
  }
  if (!secondStarted) {
    delete second;
  }

  uint8_t observed = 0;
  const bool preserved =
      returned && buffer.read(&observed, 1, false) == 1 && observed == firstValue;
  return filled && started && blocked && returned && firstJoined && secondJoined &&
         firstContext.entered == 1 && firstContext.returned == 1 && firstContext.result == 0 &&
         secondContext.entered == 1 && secondContext.returned == 1 && secondContext.result == 0 &&
         preserved;
}

bool blockedCanReadReturnsWhenWritesAreDisabled() {
  Buffer<uint8_t> buffer(1);
  BufferWaitContext firstContext(&buffer, BufferOperation::CanRead, 0);
  BufferWaitContext secondContext(&buffer, BufferOperation::CanRead, 0);
  Thread* first = new Thread(Scheduler::instance().getKernelProcess(), waitOnBuffer, &firstContext,
                             nullptr, false, true, true);
  Thread* second = new Thread(Scheduler::instance().getKernelProcess(), waitOnBuffer,
                              &secondContext, nullptr, false, true, true);
  first->setName("hosted closed-writer readiness waiter one");
  second->setName("hosted closed-writer readiness waiter two");

  const bool firstStarted = first->start();
  const bool secondStarted = second->start();
  const bool started = firstStarted && secondStarted;
  const bool blocked =
      started && waitUntilBlocked(first, firstContext) && waitUntilBlocked(second, secondContext);
  buffer.disableWrites();
  const bool returned = blocked && waitUntilReturned(firstContext, secondContext);

  // Keep the fixture finite if readiness ignores the peer-side terminal state.
  if ((firstStarted || secondStarted) && !returned) {
    buffer.disableReads();
  }
  const bool firstJoined = firstStarted && first->joinForCompletion();
  const bool secondJoined = secondStarted && second->joinForCompletion();
  if (!firstStarted) {
    delete first;
  }
  if (!secondStarted) {
    delete second;
  }

  return started && blocked && returned && firstJoined && secondJoined &&
         firstContext.entered == 1 && firstContext.returned == 1 && firstContext.result == 0 &&
         secondContext.entered == 1 && secondContext.returned == 1 && secondContext.result == 0;
}

bool blockedCanWriteReturnsWhenReadsAreDisabled() {
  Buffer<uint8_t> buffer(1);
  const uint8_t value = 0x24;
  const bool filled = buffer.write(&value, 1, false) == 1;
  BufferWaitContext firstContext(&buffer, BufferOperation::CanWrite, 0);
  BufferWaitContext secondContext(&buffer, BufferOperation::CanWrite, 0);
  Thread* first = new Thread(Scheduler::instance().getKernelProcess(), waitOnBuffer, &firstContext,
                             nullptr, false, true, true);
  Thread* second = new Thread(Scheduler::instance().getKernelProcess(), waitOnBuffer,
                              &secondContext, nullptr, false, true, true);
  first->setName("hosted closed-reader readiness waiter one");
  second->setName("hosted closed-reader readiness waiter two");

  const bool firstStarted = filled && first->start();
  const bool secondStarted = filled && second->start();
  const bool started = firstStarted && secondStarted;
  const bool blocked =
      started && waitUntilBlocked(first, firstContext) && waitUntilBlocked(second, secondContext);
  buffer.disableReads();
  const bool returned = blocked && waitUntilReturned(firstContext, secondContext);

  // Keep the fixture finite if readiness ignores the peer-side terminal state.
  if ((firstStarted || secondStarted) && !returned) {
    buffer.disableWrites();
  }
  const bool firstJoined = firstStarted && first->joinForCompletion();
  const bool secondJoined = secondStarted && second->joinForCompletion();
  if (!firstStarted) {
    delete first;
  }
  if (!secondStarted) {
    delete second;
  }

  return filled && started && blocked && returned && firstJoined && secondJoined &&
         firstContext.entered == 1 && firstContext.returned == 1 && firstContext.result == 0 &&
         secondContext.entered == 1 && secondContext.returned == 1 && secondContext.result == 0 &&
         buffer.getDataSize() == 1;
}
}  // namespace

bool runHostedBufferRegressions() {
  const bool availableWritePassed = availableWriteWaitsForLockOwnership();
  if (availableWritePassed) {
    NOTICE("HOSTED-WAIT-TEST: PASS buffer-write-available-contention");
  } else {
    ERROR("HOSTED-WAIT-TEST: FAIL buffer-write-available-contention: contended byte was lost");
  }

  const bool readerPassed = blockedReaderReturnsWhenReadsAreDisabled();
  if (readerPassed) {
    NOTICE("HOSTED-WAIT-TEST: PASS buffer-disable-reads-wake");
  } else {
    ERROR("HOSTED-WAIT-TEST: FAIL buffer-disable-reads-wake: a blocked reader was not released");
  }

  const bool writerPassed = blockedWriterReturnsWhenWritesAreDisabled();
  if (writerPassed) {
    NOTICE("HOSTED-WAIT-TEST: PASS buffer-disable-writes-wake");
  } else {
    ERROR("HOSTED-WAIT-TEST: FAIL buffer-disable-writes-wake: a blocked writer was not released");
  }

  const bool canReadPassed = blockedCanReadReturnsWhenWritesAreDisabled();
  if (canReadPassed) {
    NOTICE("HOSTED-WAIT-TEST: PASS buffer-can-read-peer-close");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL buffer-can-read-peer-close: empty readiness requeued after "
        "writes closed");
  }

  const bool canWritePassed = blockedCanWriteReturnsWhenReadsAreDisabled();
  if (canWritePassed) {
    NOTICE("HOSTED-WAIT-TEST: PASS buffer-can-write-peer-close");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL buffer-can-write-peer-close: full readiness requeued after reads "
        "closed");
  }

  return availableWritePassed && readerPassed && writerPassed && canReadPassed && canWritePassed;
}
