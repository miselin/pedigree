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
#include "pedigree/kernel/process/SignalEvent.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/time/Time.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/linux-wait-abi.h"
#include "modules/subsys/posix/poll-syscalls.h"
#include "modules/system/vfs/Pipe.h"

static_assert(sizeof(LinuxKernelTimespec) == 16);
static_assert(offsetof(LinuxKernelTimespec, tv_sec) == 0);
static_assert(offsetof(LinuxKernelTimespec, tv_nsec) == 8);

namespace {
constexpr size_t TestSignal = 10;
constexpr int PreservedErrno = 123;
constexpr uint64_t TestSignalBit = static_cast<uint64_t>(1) << (TestSignal - 1);
constexpr uint64_t PreservedSignalBit = static_cast<uint64_t>(1) << (12 - 1);
constexpr uint64_t UnblockableSignalBits =
    (static_cast<uint64_t>(1) << (SIGKILL - 1)) | (static_cast<uint64_t>(1) << (SIGSTOP - 1));
constexpr uint64_t OriginalSignalMask = TestSignalBit | PreservedSignalBit;
constexpr uint64_t RequestedSignalMask = PreservedSignalBit | UnblockableSignalBits;
constexpr uint64_t ActiveSignalMask = PreservedSignalBit;

Atomic<size_t> g_PpollSignalHandlerCalls(0);
Atomic<size_t> g_PpollSignalHandlerWrites(0);
FileDescriptor* g_PpollSignalWriter = nullptr;

void ppollSignalHandler(size_t) {
  g_PpollSignalHandlerCalls += 1;
  if (g_PpollSignalWriter) {
    char value = 'r';
    if (g_PpollSignalWriter->write(1, reinterpret_cast<uintptr_t>(&value), true) == 1) {
      g_PpollSignalHandlerWrites += 1;
    }
  }
}

bool closeDescriptor(PosixSubsystem* subsystem, size_t fd) {
  DescriptorLease descriptor;
  if (!subsystem->acquireFileDescriptor(fd, descriptor)) {
    return false;
  }
  const bool closed = subsystem->closeFileDescriptor(fd, descriptor);
  descriptor.reset();
  return closed;
}

bool waitForPpollBlock(Thread* thread) {
  const Time::Timestamp deadline = Time::getTicks() + (2 * Time::Multiplier::Second);
  while (Time::getTicks() < deadline) {
    Thread::WaitDebugInfo info = {};
    uintptr_t debugAddress = 0;
    if (thread->getWaitDebugInfo(info) && info.queue && info.queued &&
        thread->getDebugState(debugAddress) == Thread::SemWait) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}

struct PpollValidationContext {
  explicit PpollValidationContext(size_t readFd) : readFd(readFd), passed(false), returned(0) {}

  size_t readFd;
  bool passed;
  Atomic<size_t> returned;
};

int ppollValidationWorker(void* parameter) {
  PpollValidationContext* context = reinterpret_cast<PpollValidationContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  bool passed = true;

  LinuxKernelTimespec zero = {0, 0};
  thread->setErrno(PreservedErrno);
  const int zeroResult = posix_ppoll(nullptr, 0, &zero, nullptr, 37);
  passed = passed && zeroResult == 0 && thread->getErrno() == PreservedErrno;

  LinuxKernelTimespec oneNanosecond = {0, 1};
  thread->setErrno(PreservedErrno);
  const int oneNanosecondResult = posix_ppoll(nullptr, 0, &oneNanosecond, nullptr, 0);
  passed = passed && oneNanosecondResult == 0 && !oneNanosecond.tv_sec && !oneNanosecond.tv_nsec &&
           thread->getErrno() == PreservedErrno;

  LinuxKernelTimespec negativeSeconds = {-1, 0};
  thread->setErrno(0);
  const int negativeSecondsResult = posix_ppoll(nullptr, 0, &negativeSeconds, nullptr, 0);
  passed = passed && negativeSecondsResult == -1 && thread->getErrno() == Error::InvalidArgument;

  LinuxKernelTimespec negativeNanoseconds = {0, -1};
  thread->setErrno(0);
  const int negativeNanosecondsResult = posix_ppoll(nullptr, 0, &negativeNanoseconds, nullptr, 0);
  passed =
      passed && negativeNanosecondsResult == -1 && thread->getErrno() == Error::InvalidArgument;

  LinuxKernelTimespec excessiveNanoseconds = {0, 1000000000};
  thread->setErrno(0);
  const int excessiveNanosecondsResult = posix_ppoll(nullptr, 0, &excessiveNanoseconds, nullptr, 0);
  passed =
      passed && excessiveNanosecondsResult == -1 && thread->getErrno() == Error::InvalidArgument;

  uint64_t signalMask = 0;
  zero = {0, 0};
  thread->setErrno(0);
  const int wrongMaskSizeResult =
      posix_ppoll(nullptr, 0, &zero, &signalMask, sizeof(signalMask) - 1);
  passed = passed && wrongMaskSizeResult == -1 && thread->getErrno() == Error::InvalidArgument;

  const uintptr_t kernelStart = Processor::information().getVirtualAddressSpace().getKernelStart();
  thread->setErrno(0);
  const int badTimeoutResult =
      posix_ppoll(nullptr, 0, reinterpret_cast<LinuxKernelTimespec*>(kernelStart), nullptr, 0);
  passed = passed && badTimeoutResult == -1 && thread->getErrno() == Error::BadAddress;

  zero = {0, 0};
  thread->setErrno(0);
  const int excessiveDescriptorsResult = posix_ppoll(nullptr, 16385, &zero, nullptr, 0);
  passed =
      passed && excessiveDescriptorsResult == -1 && thread->getErrno() == Error::InvalidArgument;

  struct pollfd ready = {static_cast<int>(context->readFd), POLLIN, 0};
  thread->setErrno(PreservedErrno);
  const int readyResult = posix_ppoll(&ready, 1, nullptr, nullptr, 91);
  passed = passed && readyResult == 1 && (ready.revents & POLLIN) &&
           thread->getErrno() == PreservedErrno;

  LinuxKernelTimespec saturatedTimeout = {INT64_MAX, 999999999};
  struct pollfd saturatedReady = {static_cast<int>(context->readFd), POLLIN, 0};
  thread->setErrno(PreservedErrno);
  const int saturatedResult = posix_ppoll(&saturatedReady, 1, &saturatedTimeout, nullptr, 0);
  passed = passed && saturatedResult == 1 && (saturatedReady.revents & POLLIN) &&
           saturatedTimeout.tv_sec >= 0 && saturatedTimeout.tv_nsec >= 0 &&
           saturatedTimeout.tv_nsec < 1000000000 && thread->getErrno() == PreservedErrno;

  const uint64_t previousMask = thread->getSignalMask();
  const uint64_t blockedMask = previousMask | TestSignalBit;
  const uint64_t temporaryMask = blockedMask & ~TestSignalBit;
  thread->setSignalMask(blockedMask);
  thread->clearInterruption();

  LinuxKernelTimespec badInputTimeout = {10, 0};
  thread->setErrno(0);
  const int badInputResult = posix_ppoll(reinterpret_cast<struct pollfd*>(kernelStart), 1,
                                         &badInputTimeout, &temporaryMask, sizeof(temporaryMask));
  const int badInputError = thread->getErrno();
  const bool badInputMaskRestored = thread->getSignalMask() == blockedMask;
  const bool validErrorRemainder = badInputTimeout.tv_sec >= 0 && badInputTimeout.tv_sec <= 10 &&
                                   badInputTimeout.tv_nsec >= 0 &&
                                   badInputTimeout.tv_nsec < 1000000000 &&
                                   (badInputTimeout.tv_sec < 10 || !badInputTimeout.tv_nsec);

  g_PpollSignalHandlerCalls = 0;
  g_PpollSignalWriter = nullptr;
  SignalEvent pendingSignal(reinterpret_cast<uintptr_t>(&ppollSignalHandler), TestSignal);
  const bool signalQueued = thread->sendEvent(&pendingSignal);

  zero = {0, 0};
  thread->setErrno(0);
  const int badMaskBeforeArmResult = posix_ppoll(
      nullptr, 0, &zero, reinterpret_cast<const uint64_t*>(kernelStart), sizeof(signalMask));
  const bool maskRestoredBeforeReturn = thread->getSignalMask() == blockedMask;
  const bool signalStillPending = thread->hasEvent(&pendingSignal);
  const bool signalStayedBlocked = !g_PpollSignalHandlerCalls;
  if (signalStillPending) {
    thread->cullEvent(&pendingSignal);
  }
  thread->setSignalMask(previousMask);
  thread->clearInterruption();

  passed = passed && signalQueued && badInputResult == -1 && badInputError == Error::BadAddress &&
           badInputMaskRestored && validErrorRemainder && badMaskBeforeArmResult == -1 &&
           thread->getErrno() == Error::BadAddress && maskRestoredBeforeReturn &&
           signalStillPending && signalStayedBlocked;
  context->passed = passed;
  context->returned += 1;
  return passed ? 0 : 1;
}

bool ppollValidationAndImmediateReadiness(Process* kernelProcess) {
  constexpr size_t ReadDescriptor = 89;
  constexpr size_t WriteDescriptor = 90;

  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  Pipe* pipe = new Pipe;
  FileDescriptor* reader = new FileDescriptor(pipe, 0, ReadDescriptor, 0, O_RDONLY);
  FileDescriptor* writer = new FileDescriptor(pipe, 0, WriteDescriptor, 0, O_WRONLY);
  subsystem->addFileDescriptor(ReadDescriptor, reader);
  subsystem->addFileDescriptor(WriteDescriptor, writer);

  char value = 'v';
  const bool madeReady = writer->write(1, reinterpret_cast<uintptr_t>(&value), true) == 1;
  PpollValidationContext context(ReadDescriptor);
  Thread* worker = new Thread(process, ppollValidationWorker, &context, nullptr, false, true, true);
  worker->setName("hosted ppoll validation worker");
  const bool started = madeReady && worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  const bool writerClosed = closeDescriptor(subsystem, WriteDescriptor);
  const bool readerClosed = closeDescriptor(subsystem, ReadDescriptor);
  const bool passed =
      started && joined && context.returned == 1 && context.passed && writerClosed && readerClosed;
  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL ppoll-validation-immediate: "
        "Linux timeout, sigset, input-copy, or immediate-readiness semantics regressed");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS ppoll-validation-immediate");
  return true;
}

struct PpollSignalContext {
  explicit PpollSignalContext(size_t readFd)
      : readFd(readFd), entered(0), returned(0), result(-2), error(0), events(0), restoredMask(0) {}

  size_t readFd;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
  int result;
  int error;
  short events;
  uint64_t restoredMask;
};

int ppollSignalWorker(void* parameter) {
  PpollSignalContext* context = reinterpret_cast<PpollSignalContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  thread->setSignalMask(OriginalSignalMask);
  thread->clearInterruption();

  struct pollfd descriptor = {static_cast<int>(context->readFd), POLLIN, 0};
  context->entered += 1;
  thread->setErrno(PreservedErrno);
  context->result =
      posix_ppoll(&descriptor, 1, nullptr, &RequestedSignalMask, sizeof(RequestedSignalMask));
  context->error = thread->getErrno();
  context->events = descriptor.revents;
  context->restoredMask = thread->getSignalMask();
  thread->setSignalMask(0);
  thread->clearInterruption();
  context->returned += 1;
  return 0;
}

bool ppollSignalRace(Process* kernelProcess, bool readyWins) {
  constexpr size_t ReadDescriptor = 91;
  constexpr size_t WriteDescriptor = 92;

  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);
  Pipe* pipe = new Pipe;
  FileDescriptor* reader = new FileDescriptor(pipe, 0, ReadDescriptor, 0, O_RDONLY);
  FileDescriptor* writer = new FileDescriptor(pipe, 0, WriteDescriptor, 0, O_WRONLY);
  subsystem->addFileDescriptor(ReadDescriptor, reader);
  subsystem->addFileDescriptor(WriteDescriptor, writer);

  g_PpollSignalHandlerCalls = 0;
  g_PpollSignalHandlerWrites = 0;
  g_PpollSignalWriter = readyWins ? writer : nullptr;
  PpollSignalContext context(ReadDescriptor);
  Thread* worker = new Thread(process, ppollSignalWorker, &context, nullptr, false, true, true);
  if (readyWins) {
    worker->setName("hosted ppoll ready-signal worker");
  } else {
    worker->setName("hosted ppoll EINTR worker");
  }
  const bool started = worker->start();
  while (started && !context.entered) {
    Scheduler::instance().yield();
  }
  const bool blocked = started && waitForPpollBlock(worker);
  const bool activeMaskObserved = blocked && worker->getSignalMask() == ActiveSignalMask;

  SignalEvent* signal = new SignalEvent(reinterpret_cast<uintptr_t>(&ppollSignalHandler),
                                        TestSignal, ~0UL, 0, true, true);
  const bool signalQueued = blocked && worker->sendEvent(signal);
  if (!signalQueued) {
    delete signal;
  }

  bool rescueWrite = false;
  if (!blocked && started && !context.returned) {
    char rescue = 'x';
    rescueWrite = writer->write(1, reinterpret_cast<uintptr_t>(&rescue), true) == 1;
  }
  const Time::Timestamp returnDeadline = Time::getTicks() + (2 * Time::Multiplier::Second);
  while (started && !context.returned && Time::getTicks() < returnDeadline) {
    Scheduler::instance().yield();
  }
  if (started && !context.returned) {
    char rescue = 'x';
    rescueWrite = writer->write(1, reinterpret_cast<uintptr_t>(&rescue), true) == 1 || rescueWrite;
  }
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }
  g_PpollSignalWriter = nullptr;

  const bool resultPassed =
      readyWins
          ? context.result == 1 && (context.events & POLLIN) && g_PpollSignalHandlerWrites == 1
          : context.result == -1 && context.error == Error::Interrupted && !context.events &&
                !g_PpollSignalHandlerWrites;
  bool passed = started && blocked && activeMaskObserved && signalQueued && !rescueWrite &&
                joined && context.returned == 1 && resultPassed &&
                context.restoredMask == OriginalSignalMask && g_PpollSignalHandlerCalls == 1;

  const bool writerClosed = closeDescriptor(subsystem, WriteDescriptor);
  const bool readerClosed = closeDescriptor(subsystem, ReadDescriptor);
  passed = passed && writerClosed && readerClosed;
  delete process;

  if (!passed) {
    ERROR("HOSTED-SYSCALL-TEST: FAIL "
          << (readyWins ? "ppoll-ready-beats-signal: " : "ppoll-eintr-mask: ")
          << "temporary masking, restoration, or ready-vs-signal precedence regressed");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS "
         << (readyWins ? "ppoll-ready-beats-signal" : "ppoll-eintr-mask"));
  return true;
}
}  // namespace

bool runHostedPpollRegressions(Process* process) {
  return ppollValidationAndImmediateReadiness(process) && ppollSignalRace(process, false) &&
         ppollSignalRace(process, true);
}
