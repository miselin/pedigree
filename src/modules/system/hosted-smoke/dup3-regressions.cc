/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
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

namespace {
constexpr size_t Attempts = 10000;
constexpr size_t ReplacementIterations = 256;
constexpr size_t EventFdRaceIterations = 32;
constexpr int PreservedErrno = 173;

struct PublicationStressContext {
  PublicationStressContext(int source, int target)
      : source(source),
        target(target),
        ready(0),
        start(0),
        finished(0),
        observations(0),
        failures(0) {}

  int source;
  int target;
  Atomic<size_t> ready;
  Atomic<size_t> start;
  Atomic<size_t> finished;
  Atomic<size_t> observations;
  Atomic<size_t> failures;
};

bool waitFor(const Atomic<size_t>& value, size_t expected) {
  for (size_t attempt = 0; attempt < Attempts; ++attempt) {
    if (value.value() == expected) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}

int replaceWithCloexec(void* parameter) {
  PublicationStressContext* context = reinterpret_cast<PublicationStressContext*>(parameter);
  context->ready += 1;
  if (!waitFor(context->start, 1)) {
    context->failures += 1;
    context->finished += 1;
    return 1;
  }

  for (size_t iteration = 0; iteration < ReplacementIterations; ++iteration) {
    if (posix_dup3(context->source, context->target, O_CLOEXEC) != context->target) {
      context->failures += 1;
      break;
    }
    Scheduler::instance().yield();
  }
  context->finished += 1;
  return 0;
}

int observeCloexecPublication(void* parameter) {
  PublicationStressContext* context = reinterpret_cast<PublicationStressContext*>(parameter);
  context->ready += 1;
  if (!waitFor(context->start, 1)) {
    context->failures += 1;
    return 1;
  }

  do {
    if (posix_fcntl(context->target, F_GETFD, nullptr) != FD_CLOEXEC) {
      context->failures += 1;
      return 1;
    }
    context->observations += 1;
    Scheduler::instance().yield();
  } while (!context->finished.value());
  return 0;
}

bool cloexecPublicationIsAtomic(Process* process, int source, int target) {
  PublicationStressContext context(source, target);
  Thread* replacer = new Thread(process, replaceWithCloexec, &context, nullptr, false, true, true);
  Thread* observer =
      new Thread(process, observeCloexecPublication, &context, nullptr, false, true, true);
  replacer->setName("hosted dup3 CLOEXEC replacer");
  observer->setName("hosted dup3 CLOEXEC observer");
  const bool replacerStarted = replacer->start();
  const bool observerStarted = observer->start();
  const bool ready = replacerStarted && observerStarted && waitFor(context.ready, 2);
  if (!replacerStarted) {
    context.finished += 1;
  }
  context.start += 1;
  const bool replacerJoined = replacerStarted && replacer->joinForCompletion();
  const bool observerJoined = observerStarted && observer->joinForCompletion();
  if (!replacerStarted) {
    delete replacer;
  }
  if (!observerStarted) {
    delete observer;
  }

  return ready && replacerJoined && observerJoined && context.finished == 1 &&
         context.observations.value() && !context.failures.value();
}

struct EventFdCloseRaceContext {
  EventFdCloseRaceContext(int source, int target)
      : source(source),
        target(target),
        ready(0),
        start(0),
        duplicateResult(-2),
        duplicateError(0),
        closeResult(-2),
        failures(0) {}

  int source;
  int target;
  Atomic<size_t> ready;
  Atomic<size_t> start;
  int duplicateResult;
  int duplicateError;
  int closeResult;
  Atomic<size_t> failures;
};

int duplicateAgainstFinalClose(void* parameter) {
  EventFdCloseRaceContext* context = reinterpret_cast<EventFdCloseRaceContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  context->ready += 1;
  if (!waitFor(context->start, 1)) {
    context->failures += 1;
    return 1;
  }
  thread->setErrno(0);
  context->duplicateResult = posix_dup3(context->source, context->target, O_CLOEXEC);
  context->duplicateError = thread->getErrno();
  return 0;
}

int closeEventFdSource(void* parameter) {
  EventFdCloseRaceContext* context = reinterpret_cast<EventFdCloseRaceContext*>(parameter);
  context->ready += 1;
  if (!waitFor(context->start, 1)) {
    context->failures += 1;
    return 1;
  }
  context->closeResult = posix_close(context->source);
  return 0;
}

bool eventFdFinalCloseRace(Process* process, PosixSubsystem* subsystem) {
  for (size_t iteration = 0; iteration < EventFdRaceIterations; ++iteration) {
    const int source = posix_eventfd2(1, LinuxEventFd::NonBlock);
    const int target = posix_eventfd2(2, LinuxEventFd::NonBlock);
    DescriptorLease sourceDescriptor;
    DescriptorLease targetDescriptor;
    if (source < 0 || target < 0 || !subsystem->acquireFileDescriptor(source, sourceDescriptor) ||
        !subsystem->acquireFileDescriptor(target, targetDescriptor)) {
      return false;
    }
    FileDescriptor::OpenFileDescriptionLease sourceDescription =
        sourceDescriptor->acquireOpenFileDescription();
    FileDescriptor::OpenFileDescriptionLease targetDescription =
        targetDescriptor->acquireOpenFileDescription();
    sourceDescriptor.reset();
    targetDescriptor.reset();

    EventFdCloseRaceContext context(source, target);
    Thread* duplicator =
        new Thread(process, duplicateAgainstFinalClose, &context, nullptr, false, true, true);
    Thread* closer = new Thread(process, closeEventFdSource, &context, nullptr, false, true, true);
    duplicator->setName("hosted dup3 eventfd duplicator");
    closer->setName("hosted dup3 eventfd closer");
    const bool duplicatorStarted = duplicator->start();
    const bool closerStarted = closer->start();
    const bool ready = duplicatorStarted && closerStarted && waitFor(context.ready, 2);
    context.start += 1;
    const bool duplicatorJoined = duplicatorStarted && duplicator->joinForCompletion();
    const bool closerJoined = closerStarted && closer->joinForCompletion();
    if (!duplicatorStarted) {
      delete duplicator;
    }
    if (!closerStarted) {
      delete closer;
    }

    DescriptorLease publishedTarget;
    const bool targetPresent = subsystem->acquireFileDescriptor(target, publishedTarget);
    const bool duplicateWon = context.duplicateResult == target && context.duplicateError == 0;
    const bool closeWon =
        context.duplicateResult == -1 && context.duplicateError == Error::BadFileDescriptor;
    const bool expectedDescription =
        targetPresent && publishedTarget->acquireOpenFileDescription().get() ==
                             (duplicateWon ? sourceDescription.get() : targetDescription.get());
    publishedTarget.reset();
    const bool targetClosed = posix_close(target) == 0;

    if (!ready || !duplicatorJoined || !closerJoined || context.closeResult ||
        context.failures.value() || (!duplicateWon && !closeWon) || !expectedDescription ||
        !targetClosed || sourceDescription->descriptorOwnerCount() ||
        targetDescription->descriptorOwnerCount()) {
      return false;
    }
  }
  return true;
}

struct Dup3Context {
  explicit Dup3Context(Process* process)
      : process(process), completed(false), passed(false), returned(0) {}

  Process* process;
  bool completed;
  bool passed;
  Atomic<size_t> returned;
};

int exerciseDup3(void* parameter) {
  Dup3Context* context = reinterpret_cast<Dup3Context*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  PosixSubsystem* subsystem = static_cast<PosixSubsystem*>(thread->getParent()->getSubsystem());
  bool passed = true;

  const int source = posix_eventfd2(2, LinuxEventFd::NonBlock | LinuxEventFd::CloseOnExec);
  const int target = posix_eventfd2(7, LinuxEventFd::NonBlock);
  DescriptorLease sourceDescriptor;
  DescriptorLease oldTargetDescriptor;
  passed &= source >= 0 && target >= 0 &&
            subsystem->acquireFileDescriptor(source, sourceDescriptor) &&
            subsystem->acquireFileDescriptor(target, oldTargetDescriptor);
  FileDescriptor::OpenFileDescriptionLease sourceDescription;
  FileDescriptor::OpenFileDescriptionLease oldTargetDescription;
  if (sourceDescriptor && oldTargetDescriptor) {
    sourceDescription = sourceDescriptor->acquireOpenFileDescription();
    oldTargetDescription = oldTargetDescriptor->acquireOpenFileDescription();
  }
  sourceDescriptor.reset();
  oldTargetDescriptor.reset();
  if (!sourceDescription || !oldTargetDescription) {
    if (source >= 0) {
      posix_close(source);
    }
    if (target >= 0) {
      posix_close(target);
    }
    context->completed = true;
    context->returned += 1;
    return 1;
  }

  thread->setErrno(PreservedErrno);
  passed &= posix_dup3(source, target, 0) == target && thread->getErrno() == PreservedErrno &&
            sourceDescription->descriptorOwnerCount() == 2 &&
            oldTargetDescription->descriptorOwnerCount() == 0;

  DescriptorLease duplicate;
  passed &= subsystem->acquireFileDescriptor(target, duplicate) &&
            duplicate->acquireOpenFileDescription() == sourceDescription &&
            duplicate->getFlags() == 0;
  duplicate.reset();

  uint64_t value = 0;
  passed &= posix_read(target, reinterpret_cast<char*>(&value), sizeof(value)) == 8 && value == 2;
  value = 3;
  passed &= posix_write(source, reinterpret_cast<char*>(&value), sizeof(value), false) == 8;
  value = 0;
  passed &= posix_read(target, reinterpret_cast<char*>(&value), sizeof(value)) == 8 && value == 3;

  passed &= posix_dup3(source, target, O_CLOEXEC) == target &&
            posix_fcntl(target, F_GETFD, nullptr) == FD_CLOEXEC;
  DescriptorLease retainedTarget;
  passed &= subsystem->acquireFileDescriptor(target, retainedTarget);
  FileDescriptor::OpenFileDescriptionLease retainedDescription;
  if (retainedTarget) {
    retainedDescription = retainedTarget->acquireOpenFileDescription();
  }
  retainedTarget.reset();

  thread->setErrno(0);
  passed &= posix_dup3(source, source, 0) == -1 && thread->getErrno() == Error::InvalidArgument;
  thread->setErrno(0);
  passed &= posix_dup3(-1, -1, 0) == -1 && thread->getErrno() == Error::InvalidArgument;
  thread->setErrno(0);
  passed &=
      posix_dup3(source, target, O_NONBLOCK) == -1 && thread->getErrno() == Error::InvalidArgument;
  thread->setErrno(0);
  passed &= posix_dup3(-1, target, 0) == -1 && thread->getErrno() == Error::BadFileDescriptor;
  thread->setErrno(0);
  passed &= posix_dup3(source, -1, 0) == -1 && thread->getErrno() == Error::BadFileDescriptor;
  thread->setErrno(0);
  passed &= posix_dup3(source, 16384, 0) == -1 && thread->getErrno() == Error::BadFileDescriptor;

  DescriptorLease unchangedTarget;
  passed &= subsystem->acquireFileDescriptor(target, unchangedTarget) &&
            unchangedTarget->acquireOpenFileDescription() == retainedDescription &&
            unchangedTarget->getFlags() == FD_CLOEXEC;
  unchangedTarget.reset();

  const size_t reserved = subsystem->getFd(100);
  thread->setErrno(0);
  passed &= reserved == 100 && posix_dup3(-1, static_cast<int>(reserved), 0) == -1 &&
            thread->getErrno() == Error::BadFileDescriptor;
  thread->setErrno(0);
  passed &= posix_dup3(source, static_cast<int>(reserved), O_CLOEXEC) == -1 &&
            thread->getErrno() == Error::DeviceBusy;
  DescriptorLease unpublishedReservation;
  passed &= !subsystem->acquireFileDescriptor(reserved, unpublishedReservation);
  subsystem->freeFd(reserved);
  passed &=
      posix_dup3(source, static_cast<int>(reserved), O_CLOEXEC) == static_cast<int>(reserved) &&
      posix_fcntl(static_cast<int>(reserved), F_GETFD, nullptr) == FD_CLOEXEC &&
      posix_close(static_cast<int>(reserved)) == 0;

  passed &= cloexecPublicationIsAtomic(context->process, source, target);
  passed &= posix_close(source) == 0;
  value = 5;
  passed &= posix_write(target, reinterpret_cast<char*>(&value), sizeof(value), false) == 8;
  value = 0;
  passed &= posix_read(target, reinterpret_cast<char*>(&value), sizeof(value)) == 8 && value == 5 &&
            posix_close(target) == 0 && sourceDescription->descriptorOwnerCount() == 0;

  passed &= eventFdFinalCloseRace(context->process, subsystem);
  context->passed = passed;
  context->completed = true;
  context->returned += 1;
  return passed ? 0 : 1;
}
}  // namespace

bool runHostedDup3Regressions(Process* kernelProcess) {
  Process* process = new Process(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  Dup3Context context(process);
  Thread* worker = new Thread(process, exerciseDup3, &context, nullptr, false, true, true);
  worker->setName("hosted dup3 regression worker");
  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  const bool passed =
      started && joined && context.returned == 1 && context.completed && context.passed;
  delete process;
  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL dup3-atomic-replacement: "
        "validation, OFD sharing, CLOEXEC publication, replacement, or eventfd close race "
        "regressed");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS dup3-atomic-replacement");
  return true;
}
