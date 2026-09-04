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
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/time/Time.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "modules/subsys/posix/PosixProcess.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/system-syscalls.h"
#include "modules/system/vfs/MemoryMappedFile.h"

extern "C" Time::Timestamp posixNanosleepAlarmDurationForTest(time_t seconds, long nanoseconds);

static_assert(sizeof(time_t) == 8);
static_assert(static_cast<time_t>(-1) < 0);
static_assert(sizeof(struct timespec) == 16);
static_assert(offsetof(struct timespec, tv_sec) == 0);
static_assert(offsetof(struct timespec, tv_nsec) == 8);

namespace {
constexpr int PreservedErrno = 123;
constexpr size_t SleepSignal = 10;
constexpr Time::Timestamp MaximumLinuxSleepNanoseconds = 0x7FFFFFFFFFFFFFFFULL;
constexpr time_t MaximumLinuxSleepSeconds =
    static_cast<time_t>(MaximumLinuxSleepNanoseconds / Time::Multiplier::Second);
constexpr Time::Timestamp RoundedMaximumLinuxSleepNanoseconds =
    MaximumLinuxSleepNanoseconds + (Time::Multiplier::Microsecond -
                                    (MaximumLinuxSleepNanoseconds % Time::Multiplier::Microsecond));

Atomic<size_t> g_SleepSignalCalls(0);

void sleepSignalHandler(size_t) {
  g_SleepSignalCalls += 1;
}

bool canonicalTimespec(const struct timespec& value) {
  return value.tv_sec >= 0 && value.tv_nsec >= 0 && value.tv_nsec < 1000000000;
}

struct ValidationContext {
  ValidationContext() : passed(false), returned(0) {}

  bool passed;
  Atomic<size_t> returned;
};

int exerciseSleepClockValidation(void* parameter) {
  ValidationContext* context = reinterpret_cast<ValidationContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  PosixProcess* process = static_cast<PosixProcess*>(thread->getParent());
  bool passed = process->getType() == Process::Posix;

  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t mappingLength = pageSize * 3;
  uintptr_t address = 0;
  const bool allocated = process->getSpaceAllocator().allocate(mappingLength, address);
  uintptr_t mappedAddress = address;
  MemoryMappedObject* mapping =
      allocated
          ? MemoryMapManager::instance().mapAnon(
                mappedAddress, mappingLength, MemoryMappedObject::Read | MemoryMappedObject::Write)
          : nullptr;
  if (!mapping || mappedAddress != address) {
    if (mapping) {
      MemoryMapManager::instance().remove(mappedAddress, mappingLength);
    }
    if (allocated) {
      process->getSpaceAllocator().free(address, mappingLength);
    }
    context->returned += 1;
    return 1;
  }

  struct timespec* input = reinterpret_cast<struct timespec*>(address + 64);
  struct timespec* output = reinterpret_cast<struct timespec*>(address + 256);
  struct timespec* pageEdge =
      reinterpret_cast<struct timespec*>(address + pageSize - (sizeof(struct timespec) / 2));
  struct timespec* readOnly = reinterpret_cast<struct timespec*>(address + (pageSize * 2) + 64);
  const uintptr_t kernelStart = Processor::information().getVirtualAddressSpace().getKernelStart();
  struct timespec* bad = reinterpret_cast<struct timespec*>(kernelStart);

  passed &= posixNanosleepAlarmDurationForTest(0, 1) == Time::Multiplier::Microsecond &&
            posixNanosleepAlarmDurationForTest(0, 1000) == Time::Multiplier::Microsecond &&
            posixNanosleepAlarmDurationForTest(0, 1001) == (2 * Time::Multiplier::Microsecond) &&
            posixNanosleepAlarmDurationForTest(MaximumLinuxSleepSeconds, 0) ==
                RoundedMaximumLinuxSleepNanoseconds;

  struct timespec observed = {};
  thread->setErrno(PreservedErrno);
  passed &= posix_clock_gettime(CLOCK_REALTIME, output) == 0 &&
            thread->getErrno() == PreservedErrno &&
            PosixSubsystem::copyFromUser(&observed, output, sizeof(observed)) &&
            canonicalTimespec(observed);

  struct timespec monotonicBefore = {};
  struct timespec monotonicAfter = {};
  passed &= posix_clock_gettime(CLOCK_MONOTONIC, output) == 0 &&
            PosixSubsystem::copyFromUser(&monotonicBefore, output, sizeof(monotonicBefore)) &&
            posix_clock_gettime(CLOCK_MONOTONIC, pageEdge) == 0 &&
            PosixSubsystem::copyFromUser(&monotonicAfter, pageEdge, sizeof(monotonicAfter)) &&
            canonicalTimespec(monotonicBefore) && canonicalTimespec(monotonicAfter) &&
            (monotonicAfter.tv_sec > monotonicBefore.tv_sec ||
             (monotonicAfter.tv_sec == monotonicBefore.tv_sec &&
              monotonicAfter.tv_nsec >= monotonicBefore.tv_nsec));

  thread->setErrno(0);
  passed &= posix_clock_gettime(-1, bad) == -1 && thread->getErrno() == Error::InvalidArgument;
  thread->setErrno(0);
  passed &=
      posix_clock_gettime(CLOCK_REALTIME, nullptr) == -1 && thread->getErrno() == Error::BadAddress;
  thread->setErrno(0);
  passed &=
      posix_clock_gettime(CLOCK_MONOTONIC, bad) == -1 && thread->getErrno() == Error::BadAddress;

  const struct timespec invalidRequests[] = {{-1, 0}, {0, -1}, {0, 1000000000}};
  for (size_t i = 0; passed && i < sizeof(invalidRequests) / sizeof(invalidRequests[0]); ++i) {
    passed &= PosixSubsystem::copyToUser(input, &invalidRequests[i], sizeof(invalidRequests[i]));
    thread->setErrno(0);
    passed &= posix_nanosleep(input, bad) == -1 && thread->getErrno() == Error::InvalidArgument;
  }

  thread->setErrno(0);
  passed &= posix_nanosleep(bad, nullptr) == -1 && thread->getErrno() == Error::BadAddress;

  const struct timespec zero = {0, 0};
  passed &= PosixSubsystem::copyToUser(input, &zero, sizeof(zero));
  const size_t alarmsBeforeZero = Time::getHostedAlarmCreateCount();
  thread->setErrno(PreservedErrno);
  passed &= posix_nanosleep(input, bad) == 0 && thread->getErrno() == PreservedErrno &&
            Time::getHostedAlarmCreateCount() == alarmsBeforeZero;

  const struct timespec oneNanosecond = {0, 1};
  passed &= PosixSubsystem::copyToUser(pageEdge, &oneNanosecond, sizeof(oneNanosecond));
  thread->setErrno(PreservedErrno);
  passed &= posix_nanosleep(pageEdge, bad) == 0 && thread->getErrno() == PreservedErrno;

  g_SleepSignalCalls = 0;
  SignalEvent expiryCollision(reinterpret_cast<uintptr_t>(&sleepSignalHandler), SleepSignal);
  const bool collisionQueued = thread->sendEvent(&expiryCollision);
  thread->setErrno(PreservedErrno);
  const int collisionResult = posix_nanosleep(pageEdge, bad);
  const bool collisionStillQueued = thread->hasEvent(&expiryCollision);
  if (collisionStillQueued) {
    thread->cullEvent(&expiryCollision);
  }
  passed &= collisionQueued && collisionResult == 0 && !collisionStillQueued &&
            g_SleepSignalCalls == 1 && thread->getErrno() == PreservedErrno &&
            thread->getInterruptionReason() == Thread::NotInterrupted;

  passed &= MemoryMapManager::instance().setPermissions(address + (pageSize * 2), pageSize,
                                                        MemoryMappedObject::Read) == 1;
  thread->setErrno(0);
  passed &= posix_clock_gettime(CLOCK_REALTIME, readOnly) == -1 &&
            thread->getErrno() == Error::BadAddress;

  const bool middleRemoved =
      mapping && MemoryMapManager::instance().remove(address + pageSize, pageSize) == 1;
  passed &= middleRemoved;
  thread->setErrno(0);
  passed &= posix_nanosleep(pageEdge, nullptr) == -1 && thread->getErrno() == Error::BadAddress;
  thread->setErrno(0);
  passed &= posix_clock_gettime(CLOCK_MONOTONIC, pageEdge) == -1 &&
            thread->getErrno() == Error::BadAddress;

  MemoryMapManager::instance().remove(address, pageSize);
  MemoryMapManager::instance().remove(address + (pageSize * 2), pageSize);
  process->getSpaceAllocator().free(address, mappingLength);

  context->passed = passed;
  context->returned += 1;
  return passed ? 0 : 1;
}

enum RemainderTarget { ValidRemainder, InvalidRemainder };

struct InterruptedSleepContext {
  InterruptedSleepContext(struct timespec request, RemainderTarget target)
      : request(request),
        target(target),
        ready(0),
        returned(0),
        mapped(0),
        copiedRemainder(0),
        result(0),
        error(0),
        remaining{} {}

  struct timespec request;
  RemainderTarget target;
  Atomic<size_t> ready;
  Atomic<size_t> returned;
  Atomic<size_t> mapped;
  Atomic<size_t> copiedRemainder;
  int result;
  size_t error;
  struct timespec remaining;
};

int exerciseInterruptedSleep(void* parameter) {
  InterruptedSleepContext* context = reinterpret_cast<InterruptedSleepContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  PosixProcess* process = static_cast<PosixProcess*>(thread->getParent());

  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  uintptr_t address = 0;
  const bool allocated = process->getSpaceAllocator().allocate(pageSize, address);
  uintptr_t mappedAddress = address;
  MemoryMappedObject* mapping =
      allocated ? MemoryMapManager::instance().mapAnon(
                      mappedAddress, pageSize, MemoryMappedObject::Read | MemoryMappedObject::Write)
                : nullptr;
  if (!mapping || mappedAddress != address) {
    if (mapping) {
      MemoryMapManager::instance().remove(mappedAddress, pageSize);
    }
    if (allocated) {
      process->getSpaceAllocator().free(address, pageSize);
    }
    context->returned += 1;
    return 1;
  }

  struct timespec* input = reinterpret_cast<struct timespec*>(address + 64);
  struct timespec* output = reinterpret_cast<struct timespec*>(address + 256);
  const uintptr_t kernelStart = Processor::information().getVirtualAddressSpace().getKernelStart();
  struct timespec* remainder =
      context->target == ValidRemainder ? output : reinterpret_cast<struct timespec*>(kernelStart);
  if (!PosixSubsystem::copyToUser(input, &context->request, sizeof(context->request))) {
    MemoryMapManager::instance().remove(address, pageSize);
    process->getSpaceAllocator().free(address, pageSize);
    context->returned += 1;
    return 1;
  }

  context->mapped += 1;
  context->ready += 1;
  thread->setErrno(PreservedErrno);
  context->result = posix_nanosleep(input, remainder);
  context->error = thread->getErrno();
  if (context->target == ValidRemainder &&
      PosixSubsystem::copyFromUser(&context->remaining, output, sizeof(context->remaining))) {
    context->copiedRemainder += 1;
  }

  MemoryMapManager::instance().remove(address, pageSize);
  process->getSpaceAllocator().free(address, pageSize);
  context->returned += 1;
  return 0;
}

bool waitForSleepBlock(Thread* thread, InterruptedSleepContext& context) {
  const Time::Timestamp deadline = Time::getTicks() + (2 * Time::Multiplier::Second);
  while (Time::getTicks() < deadline) {
    Thread::WaitDebugInfo info = {};
    uintptr_t debugAddress = 0;
    if (context.ready && thread->getWaitDebugInfo(info) && info.queue && info.queued &&
        thread->getDebugState(debugAddress) == Thread::EventWait) {
      return true;
    }
    if (context.returned) {
      return false;
    }
    Scheduler::instance().yield();
  }
  return false;
}

bool waitForSleepReturn(InterruptedSleepContext& context) {
  const Time::Timestamp deadline = Time::getTicks() + (2 * Time::Multiplier::Second);
  while (Time::getTicks() < deadline) {
    if (context.returned) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}

bool interruptedSleep(Process* kernelProcess, const struct timespec& request,
                      RemainderTarget target, bool expectSaturation) {
  PosixProcess* process = new PosixProcess(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  InterruptedSleepContext context(request, target);
  Thread* worker =
      new Thread(process, exerciseInterruptedSleep, &context, nullptr, false, true, true);
  if (expectSaturation) {
    worker->setName("hosted saturated nanosleep worker");
  } else {
    worker->setName("hosted interrupted nanosleep worker");
  }
  process->publish();

  g_SleepSignalCalls = 0;
  const bool started = worker->start();
  const bool blocked = started && waitForSleepBlock(worker, context);
  SignalEvent* signal = new SignalEvent(reinterpret_cast<uintptr_t>(&sleepSignalHandler),
                                        SleepSignal, ~0UL, 0, true, true);
  const bool signalQueued = started && worker->sendEvent(signal);
  if (!signalQueued) {
    delete signal;
  }

  const bool returned = started && waitForSleepReturn(context);
  if (started && !returned) {
    worker->setUnwindState(Thread::TerminateThread);
  }
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  const bool validResult =
      target == InvalidRemainder
          ? context.result == -1 && context.error == Error::BadAddress && !context.copiedRemainder
          : context.result == -1 && context.error == Error::Interrupted &&
                context.copiedRemainder && canonicalTimespec(context.remaining) &&
                (context.remaining.tv_sec > 0 || context.remaining.tv_nsec > 0) &&
                (context.remaining.tv_sec < request.tv_sec ||
                 (context.remaining.tv_sec == request.tv_sec &&
                  context.remaining.tv_nsec <= request.tv_nsec));
  const bool saturated =
      !expectSaturation || (context.remaining.tv_sec <= MaximumLinuxSleepSeconds &&
                            context.remaining.tv_sec >= MaximumLinuxSleepSeconds - 5);
  const bool passed = started && blocked && signalQueued && returned && joined &&
                      context.mapped == 1 && context.returned == 1 && g_SleepSignalCalls == 1 &&
                      validResult && saturated;
  delete process;
  return passed;
}
}  // namespace

bool runHostedSleepClockSyscallRegressions(Process* kernelProcess) {
  PosixProcess* process = new PosixProcess(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  ValidationContext validation;
  Thread* worker =
      new Thread(process, exerciseSleepClockValidation, &validation, nullptr, false, true, true);
  worker->setName("hosted sleep and clock syscall validation worker");
  process->publish();

  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }
  bool passed = started && joined && validation.returned == 1 && validation.passed;
  delete process;

  const struct timespec ordinaryRequest = {5, 0};
  const struct timespec hugeRequest = {static_cast<time_t>(0x7FFFFFFFFFFFFFFFLL), 999999999};
  passed &= interruptedSleep(kernelProcess, ordinaryRequest, ValidRemainder, false);
  passed &= interruptedSleep(kernelProcess, ordinaryRequest, InvalidRemainder, false);
  passed &= interruptedSleep(kernelProcess, hugeRequest, ValidRemainder, true);

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL sleep-clock-usercopy: "
        "Linux timespec validation, saturation, interruption, or usercopy behavior regressed");
    return false;
  }
  NOTICE("HOSTED-SYSCALL-TEST: PASS sleep-clock-usercopy");
  return true;
}
