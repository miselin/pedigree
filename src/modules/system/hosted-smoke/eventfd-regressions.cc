/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/errors.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"

#include <fcntl.h>
#include <stdint.h>

#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/eventfd-syscalls.h"
#include "modules/subsys/posix/file-syscalls.h"
#include "modules/subsys/posix/poll-syscalls.h"
#include <sys/uio.h>

namespace {
constexpr size_t HostedAttempts = 10000;

struct EventFdCloseContext {
  explicit EventFdCloseContext(int fd)
      : fd(fd), entered(0), returned(0), result(-2), error(0), value(0) {}

  int fd;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
  int result;
  int error;
  uint64_t value;
};

int blockOnEventFd(void* parameter) {
  EventFdCloseContext* context = reinterpret_cast<EventFdCloseContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  context->entered += 1;
  thread->setErrno(0);
  context->result =
      posix_read(context->fd, reinterpret_cast<char*>(&context->value), sizeof(context->value));
  context->error = thread->getErrno();
  context->returned += 1;
  return 0;
}

struct EventFdRegressionContext {
  EventFdRegressionContext() : passed(false), returned(0) {}

  bool passed;
  Atomic<size_t> returned;
};

int exerciseEventFd(void* parameter) {
  EventFdRegressionContext* context = reinterpret_cast<EventFdRegressionContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  bool passed = true;

  thread->setErrno(0);
  const int invalidFlags = posix_eventfd2(0, 0x40000000);
  passed &= invalidFlags == -1 && thread->getErrno() == Error::InvalidArgument;

  const int fd = posix_eventfd2(0, LinuxEventFd::NonBlock | LinuxEventFd::CloseOnExec);
  DescriptorLease descriptor;
  passed &= fd >= 0 && acquireDescriptor(fd, descriptor) && descriptor->getFlags() == FD_CLOEXEC &&
            descriptor->getStatusFlags() == (O_RDWR | O_NONBLOCK) && descriptor->getEventFdImpl() &&
            descriptor->getEventFdImpl()->queryReady() == ReadyWrite;
  descriptor.reset();

  char shortBuffer[sizeof(uint64_t) - 1] = {};
  uint64_t value = 0;
  thread->setErrno(0);
  const int shortRead = posix_read(fd, shortBuffer, sizeof(shortBuffer));
  passed &= shortRead == -1 && thread->getErrno() == Error::InvalidArgument;
  thread->setErrno(0);
  const int emptyRead = posix_read(fd, reinterpret_cast<char*>(&value), sizeof(value));
  passed &= emptyRead == -1 && thread->getErrno() == Error::NoMoreProcesses;
  thread->setErrno(0);
  const int shortWrite = posix_write(fd, shortBuffer, sizeof(shortBuffer), false);
  passed &= shortWrite == -1 && thread->getErrno() == Error::InvalidArgument;
  value = ~static_cast<uint64_t>(0);
  thread->setErrno(0);
  const int invalidWrite = posix_write(fd, reinterpret_cast<char*>(&value), sizeof(value), false);
  passed &= invalidWrite == -1 && thread->getErrno() == Error::InvalidArgument;

  value = 2;
  const int initialWrite = posix_write(fd, reinterpret_cast<char*>(&value), sizeof(value), false);
  struct pollfd pollState = {fd, POLLIN | POLLOUT, 0};
  const int pollResult = posix_poll_safe(&pollState, 1, 0);
  uint64_t longRead[2] = {};
  const int accumulatedRead = posix_read(fd, reinterpret_cast<char*>(longRead), sizeof(longRead));
  passed &= initialWrite == 8 && pollResult == 1 &&
            (pollState.revents & (POLLIN | POLLOUT)) == (POLLIN | POLLOUT) &&
            accumulatedRead == 8 && longRead[0] == 2 && longRead[1] == 0;

  uint64_t longWrite[2] = {3, 5};
  thread->setErrno(0);
  const int longWriteResult =
      posix_write(fd, reinterpret_cast<char*>(longWrite), sizeof(longWrite), false);
  passed &= longWriteResult == -1 && thread->getErrno() == Error::InvalidArgument;

  uint64_t vectorWriteValue = 7;
  struct iovec writeVectors[2] = {
      {&vectorWriteValue, 3},
      {reinterpret_cast<uint8_t*>(&vectorWriteValue) + 3, 5},
  };
  thread->setErrno(0);
  const int splitVectorWrite = posix_writev(fd, writeVectors, 2);
  const int splitVectorError = thread->getErrno();

  uint64_t vectorWriteValues[2] = {7, 9};
  struct iovec completeWriteVectors[2] = {
      {&vectorWriteValues[0], sizeof(vectorWriteValues[0])},
      {&vectorWriteValues[1], sizeof(vectorWriteValues[1])},
  };
  const int vectorWrite = posix_writev(fd, completeWriteVectors, 2);
  uint64_t vectorReadValue = 0;
  struct iovec readVectors[2] = {
      {&vectorReadValue, 2},
      {reinterpret_cast<uint8_t*>(&vectorReadValue) + 2, 6},
  };
  const int vectorRead = posix_readv(fd, readVectors, 2);
  passed &= splitVectorWrite == -1 && splitVectorError == Error::InvalidArgument &&
            vectorWrite == 16 && vectorRead == 8 && vectorReadValue == 16;

  const int alias = posix_dup(fd);
  const int originalClose = posix_close(fd);
  value = 4;
  const int aliasWrite = posix_write(alias, reinterpret_cast<char*>(&value), sizeof(value), false);
  value = 0;
  const int aliasRead = posix_read(alias, reinterpret_cast<char*>(&value), sizeof(value));
  passed &= alias >= 0 && originalClose == 0 && aliasWrite == 8 && aliasRead == 8 && value == 4;

  value = ~static_cast<uint64_t>(0) - 1;
  const int maximumWrite =
      posix_write(alias, reinterpret_cast<char*>(&value), sizeof(value), false);
  pollState = {alias, POLLIN | POLLOUT, 0};
  const int maximumPoll = posix_poll_safe(&pollState, 1, 0);
  value = 1;
  thread->setErrno(0);
  const int overflowWrite =
      posix_write(alias, reinterpret_cast<char*>(&value), sizeof(value), false);
  const int overflowError = thread->getErrno();
  value = 0;
  const int maximumRead = posix_read(alias, reinterpret_cast<char*>(&value), sizeof(value));
  passed &= maximumWrite == 8 && maximumPoll == 1 && (pollState.revents & POLLIN) &&
            !(pollState.revents & POLLOUT) && overflowWrite == -1 &&
            overflowError == Error::NoMoreProcesses && maximumRead == 8 &&
            value == ~static_cast<uint64_t>(0) - 1 && posix_close(alias) == 0;

  const int semaphoreFd = posix_eventfd2(2, LinuxEventFd::Semaphore | LinuxEventFd::NonBlock);
  uint64_t firstSemaphore = 0;
  uint64_t secondSemaphore = 0;
  const int firstSemaphoreRead =
      posix_read(semaphoreFd, reinterpret_cast<char*>(&firstSemaphore), sizeof(firstSemaphore));
  const int secondSemaphoreRead =
      posix_read(semaphoreFd, reinterpret_cast<char*>(&secondSemaphore), sizeof(secondSemaphore));
  thread->setErrno(0);
  const int emptySemaphoreRead =
      posix_read(semaphoreFd, reinterpret_cast<char*>(&value), sizeof(value));
  const int emptySemaphoreError = thread->getErrno();
  passed &= semaphoreFd >= 0 && firstSemaphoreRead == 8 && secondSemaphoreRead == 8 &&
            firstSemaphore == 1 && secondSemaphore == 1 && emptySemaphoreRead == -1 &&
            emptySemaphoreError == Error::NoMoreProcesses && posix_close(semaphoreFd) == 0;

  const int blockingFd = posix_eventfd(0);
  DescriptorLease blockingDescriptor;
  SharedPointer<EventFd> retainedEventFd;
  if (blockingFd >= 0 && acquireDescriptor(blockingFd, blockingDescriptor)) {
    retainedEventFd = blockingDescriptor->getEventFdImpl();
  }
  blockingDescriptor.reset();
  EventFdCloseContext closeContext(blockingFd);
  Thread* reader =
      new Thread(thread->getParent(), blockOnEventFd, &closeContext, nullptr, false, true, true);
  reader->setName("hosted eventfd close waiter");
  const bool readerStarted = reader->start();
  bool readerBlocked = false;
  for (size_t attempt = 0; attempt < HostedAttempts && readerStarted; ++attempt) {
    Thread::WaitDebugInfo info = {};
    if (closeContext.entered && reader->getWaitDebugInfo(info) && info.queue && info.queued &&
        reader->getStatus() == Thread::Sleeping) {
      readerBlocked = true;
      break;
    }
    Scheduler::instance().yield();
  }
  const int blockingClose = readerBlocked ? posix_close(blockingFd) : -1;
  for (size_t attempt = 0; attempt < 256 && !closeContext.returned; ++attempt) {
    Scheduler::instance().yield();
  }
  const bool remainedBlocked = !closeContext.returned;
  const int retainedWrite =
      retainedEventFd && remainedBlocked ? retainedEventFd->writeValue(1, false) : -1;
  const bool readerJoined = readerStarted && reader->joinForCompletion();
  if (!readerStarted) {
    delete reader;
  }
  passed &= blockingFd >= 0 && readerStarted && readerBlocked && blockingClose == 0 &&
            remainedBlocked && retainedWrite == 8 && readerJoined && closeContext.returned == 1 &&
            closeContext.result == 8 && closeContext.error == 0 && closeContext.value == 1;

  context->passed = passed;
  context->returned += 1;
  return passed ? 0 : 1;
}
}  // namespace

bool runHostedEventFdRegressions(Process* kernelProcess) {
  Process* process = new Process(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  EventFdRegressionContext context;
  Thread* worker = new Thread(process, exerciseEventFd, &context, nullptr, false, true, true);
  worker->setName("hosted eventfd regression worker");
  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  const bool passed = started && joined && context.returned == 1 && context.passed;
  delete process;
  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL eventfd-counter-readiness-lifetime: "
        "flags, counter, vector I/O, polling, alias, or in-flight close semantics failed");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS eventfd-counter-readiness-lifetime");
  return true;
}
