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
#include "modules/subsys/posix/linux-wait-abi.h"
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
constexpr int IgnoredClockNanosleepFlag = 0x40000000;
constexpr clockid_t UnsupportedCoarseClock = 6;
constexpr time_t UntouchedRemainderSeconds = 123;
constexpr long UntouchedRemainderNanoseconds = 456;
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
  const bool allocated =
      process->allocateUserRange(Process::UserRegion::Normal, mappingLength, address);
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
      process->freeUserRange(Process::UserRegion::Normal, address, mappingLength);
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
  LinuxKernelTimespec* linuxInput = reinterpret_cast<LinuxKernelTimespec*>(input);
  LinuxKernelTimespec* linuxOutput = reinterpret_cast<LinuxKernelTimespec*>(output);
  LinuxKernelTimespec* linuxPageEdge = reinterpret_cast<LinuxKernelTimespec*>(pageEdge);
  LinuxKernelTimespec* linuxReadOnly = reinterpret_cast<LinuxKernelTimespec*>(readOnly);
  LinuxKernelTimespec* linuxBad = reinterpret_cast<LinuxKernelTimespec*>(bad);

  passed &= posixNanosleepAlarmDurationForTest(0, 1) == Time::Multiplier::Microsecond &&
            posixNanosleepAlarmDurationForTest(0, 1000) == Time::Multiplier::Microsecond &&
            posixNanosleepAlarmDurationForTest(0, 1001) == (2 * Time::Multiplier::Microsecond) &&
            posixNanosleepAlarmDurationForTest(MaximumLinuxSleepSeconds, 0) ==
                static_cast<Time::Timestamp>(MaximumLinuxSleepSeconds) * Time::Multiplier::Second &&
            posixNanosleepAlarmDurationForTest(MaximumLinuxSleepSeconds, 854775807) ==
                RoundedMaximumLinuxSleepNanoseconds &&
            posixNanosleepAlarmDurationForTest(MaximumLinuxSleepSeconds, 854775808) ==
                RoundedMaximumLinuxSleepNanoseconds &&
            posixNanosleepAlarmDurationForTest(MaximumLinuxSleepSeconds + 1, 0) ==
                RoundedMaximumLinuxSleepNanoseconds;

  struct timespec observed = {};
  thread->setErrno(PreservedErrno);
  passed &= posix_clock_gettime(CLOCK_REALTIME, output) == 0 &&
            thread->getErrno() == PreservedErrno &&
            PosixSubsystem::copyFromUser(&observed, output, sizeof(observed)) &&
            canonicalTimespec(observed);

  struct timespec monotonicBefore = {};
  struct timespec raw = {};
  struct timespec monotonicAfter = {};
  passed &= posix_clock_gettime(CLOCK_MONOTONIC, output) == 0 &&
            PosixSubsystem::copyFromUser(&monotonicBefore, output, sizeof(monotonicBefore)) &&
            posix_clock_gettime(CLOCK_MONOTONIC_RAW, output) == 0 &&
            PosixSubsystem::copyFromUser(&raw, output, sizeof(raw)) &&
            posix_clock_gettime(CLOCK_MONOTONIC, pageEdge) == 0 &&
            PosixSubsystem::copyFromUser(&monotonicAfter, pageEdge, sizeof(monotonicAfter)) &&
            canonicalTimespec(monotonicBefore) && canonicalTimespec(raw) &&
            canonicalTimespec(monotonicAfter) &&
            (raw.tv_sec > monotonicBefore.tv_sec ||
             (raw.tv_sec == monotonicBefore.tv_sec && raw.tv_nsec >= monotonicBefore.tv_nsec)) &&
            (monotonicAfter.tv_sec > raw.tv_sec ||
             (monotonicAfter.tv_sec == raw.tv_sec && monotonicAfter.tv_nsec >= raw.tv_nsec));

  thread->setErrno(0);
  passed &= posix_clock_gettime(-1, bad) == -1 && thread->getErrno() == Error::InvalidArgument;
  thread->setErrno(0);
  passed &=
      posix_clock_gettime(CLOCK_REALTIME, nullptr) == -1 && thread->getErrno() == Error::BadAddress;
  thread->setErrno(0);
  passed &=
      posix_clock_gettime(CLOCK_MONOTONIC, bad) == -1 && thread->getErrno() == Error::BadAddress;
  thread->setErrno(0);
  passed &= posix_clock_gettime(CLOCK_MONOTONIC_RAW, bad) == -1 &&
            thread->getErrno() == Error::BadAddress;

  LinuxKernelTimespec linuxObserved = {};
  thread->setErrno(PreservedErrno);
  passed &= posix_clock_getres_native(CLOCK_REALTIME, output) == 0 &&
            thread->getErrno() == PreservedErrno &&
            PosixSubsystem::copyFromUser(&observed, output, sizeof(observed)) &&
            observed.tv_sec == 0 && observed.tv_nsec == 1;
  thread->setErrno(PreservedErrno);
  passed &= posix_clock_getres(CLOCK_REALTIME, linuxOutput) == 0 &&
            thread->getErrno() == PreservedErrno &&
            PosixSubsystem::copyFromUser(&linuxObserved, linuxOutput, sizeof(linuxObserved)) &&
            linuxObserved.tv_sec == 0 && linuxObserved.tv_nsec == 1;
  thread->setErrno(PreservedErrno);
  passed &=
      posix_clock_getres(CLOCK_MONOTONIC, nullptr) == 0 && thread->getErrno() == PreservedErrno;
  passed &=
      posix_clock_getres_native(CLOCK_MONOTONIC_RAW, output) == 0 &&
      PosixSubsystem::copyFromUser(&observed, output, sizeof(observed)) && observed.tv_sec == 0 &&
      observed.tv_nsec == 1 && posix_clock_getres(CLOCK_MONOTONIC_RAW, linuxPageEdge) == 0 &&
      PosixSubsystem::copyFromUser(&linuxObserved, linuxPageEdge, sizeof(linuxObserved)) &&
      linuxObserved.tv_sec == 0 && linuxObserved.tv_nsec == 1 &&
      posix_clock_getres_native(CLOCK_MONOTONIC_RAW, nullptr) == 0 &&
      posix_clock_getres(CLOCK_MONOTONIC_RAW, nullptr) == 0 && thread->getErrno() == PreservedErrno;
  thread->setErrno(0);
  passed &= posix_clock_getres(UnsupportedCoarseClock, linuxBad) == -1 &&
            thread->getErrno() == Error::InvalidArgument;
  thread->setErrno(0);
  passed &= posix_clock_getres(CLOCK_MONOTONIC, linuxBad) == -1 &&
            thread->getErrno() == Error::BadAddress;

  const struct timespec invalidRequests[] = {{-1, 0}, {0, -1}, {0, 1000000000}};
  for (size_t i = 0; passed && i < sizeof(invalidRequests) / sizeof(invalidRequests[0]); ++i) {
    passed &= PosixSubsystem::copyToUser(input, &invalidRequests[i], sizeof(invalidRequests[i]));
    thread->setErrno(0);
    passed &= posix_nanosleep(input, bad) == -1 && thread->getErrno() == Error::InvalidArgument;
  }

  thread->setErrno(0);
  passed &= posix_nanosleep(bad, nullptr) == -1 && thread->getErrno() == Error::BadAddress;

  const LinuxKernelTimespec invalidLinuxRequests[] = {{-1, 0}, {0, -1}, {0, 1000000000}};
  for (size_t i = 0; passed && i < sizeof(invalidLinuxRequests) / sizeof(invalidLinuxRequests[0]);
       ++i) {
    passed &= PosixSubsystem::copyToUser(linuxInput, &invalidLinuxRequests[i],
                                         sizeof(invalidLinuxRequests[i]));
    thread->setErrno(0);
    passed &= posix_clock_nanosleep(CLOCK_MONOTONIC, 0, linuxInput, linuxBad) == -1 &&
              thread->getErrno() == Error::InvalidArgument;
  }
  thread->setErrno(0);
  passed &= posix_clock_nanosleep(UnsupportedCoarseClock, 0, linuxBad, linuxBad) == -1 &&
            thread->getErrno() == Error::InvalidArgument;
  thread->setErrno(0);
  passed &= posix_clock_nanosleep(CLOCK_MONOTONIC_RAW, 0, linuxBad, linuxBad) == -1 &&
            thread->getErrno() == Error::InvalidArgument;
  thread->setErrno(0);
  passed &= posix_clock_settime(CLOCK_MONOTONIC_RAW, linuxBad) == -1 &&
            thread->getErrno() == Error::InvalidArgument;
  thread->setErrno(0);
  passed &= posix_clock_nanosleep(CLOCK_MONOTONIC, 0, linuxBad, nullptr) == -1 &&
            thread->getErrno() == Error::BadAddress;

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

  const LinuxKernelTimespec linuxOneNanosecond = {0, 1};
  const LinuxKernelTimespec untouchedRemainder = {UntouchedRemainderSeconds,
                                                  UntouchedRemainderNanoseconds};
  passed &=
      PosixSubsystem::copyToUser(linuxInput, &linuxOneNanosecond, sizeof(linuxOneNanosecond)) &&
      PosixSubsystem::copyToUser(linuxOutput, &untouchedRemainder, sizeof(untouchedRemainder));
  const size_t alarmsBeforeRelativeClockSleep = Time::getHostedAlarmCreateCount();
  thread->setErrno(PreservedErrno);
  passed &= posix_clock_nanosleep(CLOCK_MONOTONIC, IgnoredClockNanosleepFlag, linuxInput,
                                  linuxOutput) == 0 &&
            thread->getErrno() == PreservedErrno &&
            Time::getHostedAlarmCreateCount() == alarmsBeforeRelativeClockSleep + 1 &&
            PosixSubsystem::copyFromUser(&linuxObserved, linuxOutput, sizeof(linuxObserved)) &&
            linuxObserved.tv_sec == untouchedRemainder.tv_sec &&
            linuxObserved.tv_nsec == untouchedRemainder.tv_nsec;

  const size_t alarmsBeforePastDeadline = Time::getHostedAlarmCreateCount();
  thread->setErrno(PreservedErrno);
  passed &= posix_clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, linuxInput, linuxBad) == 0 &&
            thread->getErrno() == PreservedErrno &&
            Time::getHostedAlarmCreateCount() == alarmsBeforePastDeadline;

  const Time::Timestamp futureDeadline = Time::getTicks() + (2 * Time::Multiplier::Millisecond);
  const LinuxKernelTimespec futureDeadlineValue = {
      static_cast<int64_t>(futureDeadline / Time::Multiplier::Second),
      static_cast<int64_t>(futureDeadline % Time::Multiplier::Second)};
  passed &=
      PosixSubsystem::copyToUser(linuxInput, &futureDeadlineValue, sizeof(futureDeadlineValue)) &&
      PosixSubsystem::copyToUser(linuxOutput, &untouchedRemainder, sizeof(untouchedRemainder));
  const size_t alarmsBeforeFutureDeadline = Time::getHostedAlarmCreateCount();
  thread->setErrno(PreservedErrno);
  passed &= posix_clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, linuxInput, linuxOutput) == 0 &&
            thread->getErrno() == PreservedErrno && Time::getTicks() >= futureDeadline &&
            Time::getHostedAlarmCreateCount() >= alarmsBeforeFutureDeadline + 1 &&
            PosixSubsystem::copyFromUser(&linuxObserved, linuxOutput, sizeof(linuxObserved)) &&
            linuxObserved.tv_sec == untouchedRemainder.tv_sec &&
            linuxObserved.tv_nsec == untouchedRemainder.tv_nsec;

  passed &= PosixSubsystem::copyToUser(linuxInput, &linuxOneNanosecond, sizeof(linuxOneNanosecond));

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

  g_SleepSignalCalls = 0;
  SignalEvent clockExpiryCollision(reinterpret_cast<uintptr_t>(&sleepSignalHandler), SleepSignal);
  const bool clockCollisionQueued = thread->sendEvent(&clockExpiryCollision);
  thread->setErrno(PreservedErrno);
  const int clockCollisionResult = posix_clock_nanosleep(CLOCK_MONOTONIC, 0, linuxInput, linuxBad);
  const bool clockCollisionStillQueued = thread->hasEvent(&clockExpiryCollision);
  if (clockCollisionStillQueued) {
    thread->cullEvent(&clockExpiryCollision);
  }
  passed &= clockCollisionQueued && clockCollisionResult == 0 && !clockCollisionStillQueued &&
            g_SleepSignalCalls == 1 && thread->getErrno() == PreservedErrno &&
            thread->getInterruptionReason() == Thread::NotInterrupted;

  passed &= MemoryMapManager::instance().setPermissions(address + (pageSize * 2), pageSize,
                                                        MemoryMappedObject::Read) == 1;
  thread->setErrno(0);
  passed &= posix_clock_gettime(CLOCK_REALTIME, readOnly) == -1 &&
            thread->getErrno() == Error::BadAddress;
  thread->setErrno(0);
  passed &= posix_clock_getres(CLOCK_REALTIME, linuxReadOnly) == -1 &&
            thread->getErrno() == Error::BadAddress;
  thread->setErrno(0);
  passed &= posix_clock_gettime(CLOCK_MONOTONIC_RAW, readOnly) == -1 &&
            thread->getErrno() == Error::BadAddress;
  thread->setErrno(0);
  passed &= posix_clock_getres(CLOCK_MONOTONIC_RAW, linuxReadOnly) == -1 &&
            thread->getErrno() == Error::BadAddress;

  const bool middleRemoved =
      mapping && MemoryMapManager::instance().remove(address + pageSize, pageSize) == 1;
  passed &= middleRemoved;
  thread->setErrno(0);
  passed &= posix_nanosleep(pageEdge, nullptr) == -1 && thread->getErrno() == Error::BadAddress;
  thread->setErrno(0);
  passed &= posix_clock_gettime(CLOCK_MONOTONIC, pageEdge) == -1 &&
            thread->getErrno() == Error::BadAddress;
  thread->setErrno(0);
  passed &= posix_clock_nanosleep(CLOCK_REALTIME, 0, linuxPageEdge, nullptr) == -1 &&
            thread->getErrno() == Error::BadAddress;

  // A Linux time32 request ends at the page boundary; musl's native timespec does not fit.
  const LinuxKernelTimespec32 oneNanosecond32 = {0, 1};
  passed &= PosixSubsystem::copyToUser(pageEdge, &oneNanosecond32, sizeof(oneNanosecond32));
  thread->setErrno(PreservedErrno);
  passed &= posix_nanosleep(pageEdge, nullptr, true) == 0 && thread->getErrno() == PreservedErrno;

  MemoryMapManager::instance().remove(address, pageSize);
  MemoryMapManager::instance().remove(address + (pageSize * 2), pageSize);
  process->freeUserRange(Process::UserRegion::Normal, address, mappingLength);

  context->passed = passed;
  context->returned += 1;
  return passed ? 0 : 1;
}

enum RemainderTarget { ValidRemainder, InvalidRemainder };
enum SleepCall { NanosleepCall, RelativeClockNanosleepCall, AbsoluteClockNanosleepCall };

struct InterruptedSleepContext {
  InterruptedSleepContext(struct timespec request, RemainderTarget target,
                          SleepCall call = NanosleepCall, clockid_t clockId = CLOCK_MONOTONIC)
      : request(request),
        target(target),
        call(call),
        clockId(clockId),
        ready(0),
        returned(0),
        mapped(0),
        copiedRemainder(0),
        result(0),
        error(0),
        remaining{} {}

  struct timespec request;
  RemainderTarget target;
  SleepCall call;
  clockid_t clockId;
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
  const bool allocated = process->allocateUserRange(Process::UserRegion::Normal, pageSize, address);
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
      process->freeUserRange(Process::UserRegion::Normal, address, pageSize);
    }
    context->returned += 1;
    return 1;
  }

  struct timespec* input = reinterpret_cast<struct timespec*>(address + 64);
  struct timespec* output = reinterpret_cast<struct timespec*>(address + 256);
  const uintptr_t kernelStart = Processor::information().getVirtualAddressSpace().getKernelStart();
  struct timespec* remainder =
      context->target == ValidRemainder ? output : reinterpret_cast<struct timespec*>(kernelStart);
  struct timespec syscallRequest = context->request;
  if (context->call == AbsoluteClockNanosleepCall) {
    const Time::Timestamp duration =
        static_cast<Time::Timestamp>(context->request.tv_sec) * Time::Multiplier::Second +
        static_cast<Time::Timestamp>(context->request.tv_nsec);
    const Time::Timestamp deadline =
        (context->clockId == CLOCK_REALTIME ? Time::getTimeNanoseconds() : Time::getTicks()) +
        duration;
    syscallRequest.tv_sec = static_cast<time_t>(deadline / Time::Multiplier::Second);
    syscallRequest.tv_nsec = static_cast<long>(deadline % Time::Multiplier::Second);
  }

  const struct timespec untouchedRemainder = {UntouchedRemainderSeconds,
                                              UntouchedRemainderNanoseconds};
  if (!PosixSubsystem::copyToUser(input, &syscallRequest, sizeof(syscallRequest)) ||
      (context->target == ValidRemainder &&
       !PosixSubsystem::copyToUser(output, &untouchedRemainder, sizeof(untouchedRemainder)))) {
    MemoryMapManager::instance().remove(address, pageSize);
    process->freeUserRange(Process::UserRegion::Normal, address, pageSize);
    context->returned += 1;
    return 1;
  }

  context->mapped += 1;
  context->ready += 1;
  thread->setErrno(PreservedErrno);
  if (context->call == NanosleepCall) {
    context->result = posix_nanosleep(input, remainder);
  } else {
    context->result = posix_clock_nanosleep(
        context->clockId, context->call == AbsoluteClockNanosleepCall ? TIMER_ABSTIME : 0,
        reinterpret_cast<LinuxKernelTimespec*>(input),
        reinterpret_cast<LinuxKernelTimespec*>(remainder));
  }
  context->error = thread->getErrno();
  if (context->target == ValidRemainder &&
      PosixSubsystem::copyFromUser(&context->remaining, output, sizeof(context->remaining))) {
    context->copiedRemainder += 1;
  }

  MemoryMapManager::instance().remove(address, pageSize);
  process->freeUserRange(Process::UserRegion::Normal, address, pageSize);
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
                      RemainderTarget target, bool expectSaturation, SleepCall call = NanosleepCall,
                      clockid_t clockId = CLOCK_MONOTONIC) {
  PosixProcess* process = new PosixProcess(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  InterruptedSleepContext context(request, target, call, clockId);
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

  bool validResult = false;
  if (call == AbsoluteClockNanosleepCall) {
    validResult =
        context.result == -1 && context.error == Error::Interrupted &&
        (target == InvalidRemainder ||
         (context.copiedRemainder && context.remaining.tv_sec == UntouchedRemainderSeconds &&
          context.remaining.tv_nsec == UntouchedRemainderNanoseconds));
  } else if (target == InvalidRemainder) {
    validResult =
        context.result == -1 && context.error == Error::BadAddress && !context.copiedRemainder;
  } else {
    validResult = context.result == -1 && context.error == Error::Interrupted &&
                  context.copiedRemainder && canonicalTimespec(context.remaining) &&
                  (context.remaining.tv_sec > 0 || context.remaining.tv_nsec > 0) &&
                  (context.remaining.tv_sec < request.tv_sec ||
                   (context.remaining.tv_sec == request.tv_sec &&
                    context.remaining.tv_nsec <= request.tv_nsec));
  }
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
  passed &= interruptedSleep(kernelProcess, ordinaryRequest, ValidRemainder, false,
                             RelativeClockNanosleepCall, CLOCK_MONOTONIC);
  passed &= interruptedSleep(kernelProcess, ordinaryRequest, InvalidRemainder, false,
                             RelativeClockNanosleepCall, CLOCK_REALTIME);
  passed &= interruptedSleep(kernelProcess, hugeRequest, ValidRemainder, true,
                             RelativeClockNanosleepCall, CLOCK_MONOTONIC);
  passed &= interruptedSleep(kernelProcess, ordinaryRequest, ValidRemainder, false,
                             AbsoluteClockNanosleepCall, CLOCK_MONOTONIC);
  passed &= interruptedSleep(kernelProcess, ordinaryRequest, InvalidRemainder, false,
                             AbsoluteClockNanosleepCall, CLOCK_REALTIME);

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL sleep-clock-usercopy: "
        "Linux timespec validation, saturation, interruption, or usercopy behavior regressed");
    return false;
  }
  NOTICE("HOSTED-SYSCALL-TEST: PASS sleep-clock-usercopy");
  return true;
}
