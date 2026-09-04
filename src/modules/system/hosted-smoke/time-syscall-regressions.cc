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
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/time/Time.h"

#include <stddef.h>
#include <stdint.h>

#include "modules/subsys/posix/PosixProcess.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/system-syscalls.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include <sys/time.h>

static_assert(sizeof(time_t) == 8);
static_assert(sizeof(suseconds_t) == 8);
static_assert(static_cast<time_t>(-1) < 0);
static_assert(static_cast<suseconds_t>(-1) < 0);
static_assert(sizeof(struct timeval) == 16);
static_assert(offsetof(struct timeval, tv_sec) == 0);
static_assert(offsetof(struct timeval, tv_usec) == 8);
static_assert(sizeof(struct itimerval) == 32);
static_assert(offsetof(struct itimerval, it_interval) == 0);
static_assert(offsetof(struct itimerval, it_value) == 16);

namespace {
constexpr int PreservedErrno = 123;
constexpr Time::Timestamp MaximumLinuxTimerNanoseconds = 0x7FFFFFFFFFFFFFFFULL;

struct TimeSyscallContext {
  TimeSyscallContext() : passed(false), returned(0) {}

  bool passed;
  Atomic<size_t> returned;
};

struct itimerval timerValue(time_t intervalSeconds, suseconds_t intervalMicroseconds,
                            time_t valueSeconds = 0, suseconds_t valueMicroseconds = 0) {
  struct itimerval result = {};
  result.it_interval.tv_sec = intervalSeconds;
  result.it_interval.tv_usec = intervalMicroseconds;
  result.it_value.tv_sec = valueSeconds;
  result.it_value.tv_usec = valueMicroseconds;
  return result;
}

bool sameTimer(const struct itimerval& left, const struct itimerval& right) {
  return left.it_interval.tv_sec == right.it_interval.tv_sec &&
         left.it_interval.tv_usec == right.it_interval.tv_usec &&
         left.it_value.tv_sec == right.it_value.tv_sec &&
         left.it_value.tv_usec == right.it_value.tv_usec;
}

bool timerIsDisarmed(const struct itimerval& value) {
  const struct itimerval zero = {};
  return sameTimer(value, zero);
}

bool timeNotGreater(const struct timeval& left, const struct timeval& right) {
  return left.tv_sec < right.tv_sec ||
         (left.tv_sec == right.tv_sec && left.tv_usec <= right.tv_usec);
}

bool runningTimerMatches(const struct itimerval& observed, const struct itimerval& requested) {
  const bool positive = observed.it_value.tv_sec > 0 || observed.it_value.tv_usec > 0;
  return observed.it_interval.tv_sec == requested.it_interval.tv_sec &&
         observed.it_interval.tv_usec == requested.it_interval.tv_usec &&
         observed.it_value.tv_sec >= 0 && observed.it_value.tv_usec >= 0 &&
         observed.it_value.tv_usec < 1000000 && positive &&
         timeNotGreater(observed.it_value, requested.it_value);
}

int exerciseTimeSyscalls(void* parameter) {
  TimeSyscallContext* context = reinterpret_cast<TimeSyscallContext*>(parameter);
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
    context->passed = false;
    context->returned += 1;
    return 1;
  }

  struct itimerval* input = reinterpret_cast<struct itimerval*>(address + 64);
  struct itimerval* output = reinterpret_cast<struct itimerval*>(address + 256);
  struct itimerval* alias = reinterpret_cast<struct itimerval*>(address + 512);
  struct itimerval* pageEdge =
      reinterpret_cast<struct itimerval*>(address + pageSize - (sizeof(struct itimerval) / 2));
  struct itimerval* readOnly = reinterpret_cast<struct itimerval*>(address + (pageSize * 2) + 64);
  const uintptr_t kernelStart = Processor::information().getVirtualAddressSpace().getKernelStart();
  struct itimerval* bad = reinterpret_cast<struct itimerval*>(kernelStart);

  const int selectors[] = {ITIMER_REAL, ITIMER_VIRTUAL, ITIMER_PROF};
  for (size_t i = 0; passed && i < sizeof(selectors) / sizeof(selectors[0]); ++i) {
    const struct itimerval disarmed = {};
    const struct itimerval requested =
        timerValue(static_cast<time_t>(i + 1), static_cast<suseconds_t>(123456 + i),
                   static_cast<time_t>(60 + i), static_cast<suseconds_t>(654321 + i));
    struct itimerval observed = {};
    passed &= PosixSubsystem::copyToUser(input, &disarmed, sizeof(disarmed)) &&
              posix_setitimer(selectors[i], input, nullptr) == 0 &&
              posix_getitimer(selectors[i], output) == 0 &&
              PosixSubsystem::copyFromUser(&observed, output, sizeof(observed)) &&
              timerIsDisarmed(observed) &&
              PosixSubsystem::copyToUser(input, &requested, sizeof(requested));
    thread->setErrno(PreservedErrno);
    passed &=
        posix_setitimer(selectors[i], input, nullptr) == 0 && thread->getErrno() == PreservedErrno;
    thread->setErrno(PreservedErrno);
    passed &= posix_getitimer(selectors[i], output) == 0 && thread->getErrno() == PreservedErrno &&
              PosixSubsystem::copyFromUser(&observed, output, sizeof(observed)) &&
              runningTimerMatches(observed, requested);
  }

  const struct itimerval spanning = timerValue(4, 654321, 64, 456789);
  struct itimerval observed = {};
  passed &= PosixSubsystem::copyToUser(pageEdge, &spanning, sizeof(spanning)) &&
            posix_setitimer(ITIMER_REAL, pageEdge, nullptr) == 0 &&
            posix_getitimer(ITIMER_REAL, pageEdge) == 0 &&
            PosixSubsystem::copyFromUser(&observed, pageEdge, sizeof(observed)) &&
            runningTimerMatches(observed, spanning);

  const struct itimerval baseline = timerValue(7, 700007, 67, 700007);
  const struct itimerval aliasedRequest = timerValue(9, 900009, 69, 900009);
  struct itimerval aliasResult = {};
  passed &= PosixSubsystem::copyToUser(input, &baseline, sizeof(baseline)) &&
            posix_setitimer(ITIMER_REAL, input, nullptr) == 0 &&
            PosixSubsystem::copyToUser(alias, &aliasedRequest, sizeof(aliasedRequest)) &&
            posix_setitimer(ITIMER_REAL, alias, alias) == 0 &&
            PosixSubsystem::copyFromUser(&aliasResult, alias, sizeof(aliasResult)) &&
            runningTimerMatches(aliasResult, baseline) &&
            posix_getitimer(ITIMER_REAL, output) == 0 &&
            PosixSubsystem::copyFromUser(&observed, output, sizeof(observed)) &&
            runningTimerMatches(observed, aliasedRequest);

  struct itimerval invalidValues[] = {
      timerValue(-1, 0),       timerValue(0, -1),       timerValue(0, 1000000),
      timerValue(0, 0, -1, 0), timerValue(0, 0, 0, -1), timerValue(0, 0, 0, 1000000),
  };
  for (size_t i = 0; passed && i < sizeof(invalidValues) / sizeof(invalidValues[0]); ++i) {
    passed &= PosixSubsystem::copyToUser(input, &invalidValues[i], sizeof(invalidValues[i]));
    thread->setErrno(0);
    passed &= posix_setitimer(ITIMER_REAL, input, nullptr) == -1 &&
              thread->getErrno() == Error::InvalidArgument &&
              posix_getitimer(ITIMER_REAL, output) == 0 &&
              PosixSubsystem::copyFromUser(&observed, output, sizeof(observed)) &&
              runningTimerMatches(observed, aliasedRequest);
  }

  const time_t saturationSeconds =
      static_cast<time_t>(MaximumLinuxTimerNanoseconds / Time::Multiplier::Second);
  const struct itimerval huge = timerValue(saturationSeconds, 0, saturationSeconds, 0);
  passed &= PosixSubsystem::copyToUser(input, &huge, sizeof(huge)) &&
            posix_setitimer(ITIMER_VIRTUAL, input, nullptr) == 0 &&
            process->getVirtualIntervalTimer().getInterval() == MaximumLinuxTimerNanoseconds &&
            process->getVirtualIntervalTimer().getValue() == MaximumLinuxTimerNanoseconds;
  thread->setErrno(PreservedErrno);
  passed &= posix_setitimer(ITIMER_VIRTUAL, nullptr, output) == 0 &&
            thread->getErrno() == PreservedErrno &&
            process->getVirtualIntervalTimer().getInterval() == 0 &&
            process->getVirtualIntervalTimer().getValue() == 0 &&
            PosixSubsystem::copyFromUser(&observed, output, sizeof(observed)) &&
            observed.it_interval.tv_sec ==
                static_cast<time_t>(MaximumLinuxTimerNanoseconds / Time::Multiplier::Second) &&
            observed.it_interval.tv_usec ==
                static_cast<suseconds_t>((MaximumLinuxTimerNanoseconds % Time::Multiplier::Second) /
                                         Time::Multiplier::Microsecond) &&
            posix_getitimer(ITIMER_VIRTUAL, output) == 0 &&
            PosixSubsystem::copyFromUser(&observed, output, sizeof(observed)) &&
            timerIsDisarmed(observed);

  thread->setErrno(0);
  passed &= posix_getitimer(-1, bad) == -1 && thread->getErrno() == Error::InvalidArgument;
  thread->setErrno(0);
  passed &= posix_setitimer(-1, bad, nullptr) == -1 && thread->getErrno() == Error::BadAddress;
  const struct itimerval malformed = timerValue(0, 1000000);
  passed &= PosixSubsystem::copyToUser(input, &malformed, sizeof(malformed));
  thread->setErrno(0);
  passed &=
      posix_setitimer(-1, input, nullptr) == -1 && thread->getErrno() == Error::InvalidArgument;
  passed &= PosixSubsystem::copyToUser(input, &baseline, sizeof(baseline));
  thread->setErrno(0);
  passed &= posix_setitimer(-1, input, bad) == -1 && thread->getErrno() == Error::InvalidArgument;
  thread->setErrno(0);
  passed &= posix_setitimer(-1, nullptr, bad) == -1 && thread->getErrno() == Error::InvalidArgument;

  const struct itimerval beforeFault = timerValue(11, 111111, 71, 111111);
  passed &= PosixSubsystem::copyToUser(input, &beforeFault, sizeof(beforeFault)) &&
            posix_setitimer(ITIMER_REAL, input, nullptr) == 0;
  thread->setErrno(0);
  passed &= posix_getitimer(ITIMER_REAL, nullptr) == -1 && thread->getErrno() == Error::BadAddress;
  thread->setErrno(0);
  passed &= posix_getitimer(ITIMER_REAL, bad) == -1 && thread->getErrno() == Error::BadAddress;
  thread->setErrno(0);
  passed &= posix_setitimer(ITIMER_REAL, bad, nullptr) == -1 &&
            thread->getErrno() == Error::BadAddress && posix_getitimer(ITIMER_REAL, output) == 0 &&
            PosixSubsystem::copyFromUser(&observed, output, sizeof(observed)) &&
            runningTimerMatches(observed, beforeFault);

  const struct itimerval readOnlyRequest = timerValue(12, 121212, 72, 121212);
  passed &= PosixSubsystem::copyToUser(readOnly, &readOnlyRequest, sizeof(readOnlyRequest)) &&
            MemoryMapManager::instance().setPermissions(address + (pageSize * 2), pageSize,
                                                        MemoryMappedObject::Read) == 1 &&
            posix_setitimer(ITIMER_REAL, readOnly, nullptr) == 0;
  thread->setErrno(0);
  passed &= posix_getitimer(ITIMER_REAL, readOnly) == -1 && thread->getErrno() == Error::BadAddress;

  const struct itimerval lateFaultRequest = timerValue(13, 131313, 73, 131313);
  passed &= PosixSubsystem::copyToUser(input, &lateFaultRequest, sizeof(lateFaultRequest));
  thread->setErrno(0);
  passed &= posix_setitimer(ITIMER_REAL, input, readOnly) == -1 &&
            thread->getErrno() == Error::BadAddress && posix_getitimer(ITIMER_REAL, output) == 0 &&
            PosixSubsystem::copyFromUser(&observed, output, sizeof(observed)) &&
            runningTimerMatches(observed, lateFaultRequest);

  const bool middleRemoved =
      mapping && MemoryMapManager::instance().remove(address + pageSize, pageSize) == 1;
  passed &= middleRemoved;
  thread->setErrno(0);
  passed &= posix_setitimer(ITIMER_REAL, pageEdge, nullptr) == -1 &&
            thread->getErrno() == Error::BadAddress;
  thread->setErrno(0);
  passed &= posix_getitimer(ITIMER_REAL, pageEdge) == -1 && thread->getErrno() == Error::BadAddress;
  thread->setErrno(0);
  passed &= posix_setitimer(ITIMER_REAL, reinterpret_cast<struct itimerval*>(address + pageSize),
                            nullptr) == -1 &&
            thread->getErrno() == Error::BadAddress && posix_getitimer(ITIMER_REAL, output) == 0 &&
            PosixSubsystem::copyFromUser(&observed, output, sizeof(observed)) &&
            runningTimerMatches(observed, lateFaultRequest);

  for (size_t i = 0; i < sizeof(selectors) / sizeof(selectors[0]); ++i) {
    passed &= posix_setitimer(selectors[i], nullptr, nullptr) == 0;
  }
  Time::Timestamp finalInterval = 1;
  Time::Timestamp finalValue = 1;
  process->getRealIntervalTimer().getIntervalAndValue(finalInterval, finalValue);
  passed &= !finalInterval && !finalValue;
  process->getVirtualIntervalTimer().getIntervalAndValue(finalInterval, finalValue);
  passed &= !finalInterval && !finalValue;
  process->getProfileIntervalTimer().getIntervalAndValue(finalInterval, finalValue);
  passed &= !finalInterval && !finalValue;

  if (mapping) {
    MemoryMapManager::instance().remove(address, pageSize);
    MemoryMapManager::instance().remove(address + (pageSize * 2), pageSize);
  }
  if (allocated) {
    process->getSpaceAllocator().free(address, mappingLength);
  }

  context->passed = passed;
  context->returned += 1;
  return passed ? 0 : 1;
}
}  // namespace

bool runHostedTimeSyscallRegressions(Process* kernelProcess) {
  PosixProcess* process = new PosixProcess(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  TimeSyscallContext context;
  Thread* worker = new Thread(process, exerciseTimeSyscalls, &context, nullptr, false, true, true);
  worker->setName("hosted interval timer syscall worker");
  process->publish();

  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }
  const bool passed = started && joined && context.returned == 1 && context.passed;
  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL interval-timer-usercopy: "
        "Linux ordering, canonical timeval, saturation, or usercopy behavior regressed");
    return false;
  }
  NOTICE("HOSTED-SYSCALL-TEST: PASS interval-timer-usercopy");
  return true;
}
