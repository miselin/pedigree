/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/time/Time.h"

#include <limits.h>
#include <time.h>

#include "PosixSubsystem.h"
#include "clock-syscalls.h"
#include "linux-wait-abi.h"
#include "mqueue-syscalls.h"
#include "pthread-syscalls.h"
#include "system-syscalls.h"
#include "timerfd-syscalls.h"

#define SG_NOTICE(x)

namespace {
constexpr Time::Timestamp MaximumLinuxSleepNanoseconds = 0x7FFFFFFFFFFFFFFFULL;
Mutex clockChangeLock;
ConditionVariable clockChanged;
uint64_t clockChangeGeneration = 0;

bool supportedSleepClock(clockid_t clockId) {
  return clockId == CLOCK_REALTIME || clockId == CLOCK_MONOTONIC;
}

bool supportedReadClock(clockid_t clockId) {
  return supportedSleepClock(clockId) || clockId == CLOCK_MONOTONIC_RAW;
}

Time::Timestamp sleepClockNanoseconds(clockid_t clockId) {
  return clockId == CLOCK_REALTIME ? Time::getTimeNanoseconds() : Time::getTicks();
}

bool validTimespec(int64_t seconds, int64_t nanoseconds) {
  return seconds >= 0 && nanoseconds >= 0 && nanoseconds < 1000000000;
}

Time::Timestamp timespecToNanoseconds(int64_t secondsValue, int64_t nanosecondsValue) {
  const Time::Timestamp seconds = static_cast<Time::Timestamp>(secondsValue);
  const Time::Timestamp nanoseconds = static_cast<Time::Timestamp>(nanosecondsValue);
  if (seconds > (MaximumLinuxSleepNanoseconds - nanoseconds) / Time::Multiplier::Second) {
    return MaximumLinuxSleepNanoseconds;
  }

  const Time::Timestamp wholeSeconds = seconds * Time::Multiplier::Second;
  return wholeSeconds + nanoseconds;
}

Time::Timestamp nanosleepAlarmDuration(Time::Timestamp requested) {
  const Time::Timestamp remainder = requested % Time::Multiplier::Microsecond;
  if (!remainder) {
    return requested;
  }

  // The machine timer accepts whole microseconds. Round up so a partial
  // microsecond request cannot expire before its requested duration.
  return requested + (Time::Multiplier::Microsecond - remainder);
}

bool waitForRealtimeSleep(Time::Timestamp deadline) {
  LockGuard<Mutex> guard(clockChangeLock);
  for (;;) {
    const Time::Timestamp now = Time::getTimeNanoseconds();
    if (now >= deadline) {
      return true;
    }
    Time::Timestamp duration = nanosleepAlarmDuration(deadline - now);
    ConditionVariable::Error error = ConditionVariable::NoError;
    if (clockChanged.wait(clockChangeLock, duration, error) ||
        error == ConditionVariable::TimedOut) {
      continue;
    }
    Thread* thread = Processor::information().getCurrentThread();
    thread->clearInterruption();
    return Time::getTimeNanoseconds() >= deadline;
  }
}

bool waitForClockSleep(clockid_t clockId, bool absolute, Time::Timestamp requested,
                       Time::Timestamp& remaining) {
  remaining = 0;
  if (absolute && clockId == CLOCK_REALTIME) {
    return waitForRealtimeSleep(requested);
  }
  const Time::Timestamp monotonicStart = Time::getTicks();

  while (true) {
    Time::Timestamp duration = requested;
    if (absolute) {
      const Time::Timestamp now = sleepClockNanoseconds(clockId);
      if (now >= requested) {
        return true;
      }
      duration = requested - now;
    } else if (!duration) {
      return true;
    }

    const bool completed = Time::delay(nanosleepAlarmDuration(duration));
    Thread* thread = Processor::information().getCurrentThread();
    if (completed) {
      if (!absolute || sleepClockNanoseconds(clockId) >= requested) {
        return true;
      }

      // A realtime deadline may move backwards while blocked. Re-arm for the
      // same absolute deadline rather than reporting an early completion.
      continue;
    }

    if (thread->getInterruptionReason() != Thread::InterruptedBySignal) {
      return true;
    }

    if (absolute) {
      const bool deadlineReached = sleepClockNanoseconds(clockId) >= requested;
      thread->clearInterruption();
      return deadlineReached;
    }

    const Time::Timestamp elapsed = Time::getTicks() - monotonicStart;
    thread->clearInterruption();
    if (elapsed >= requested) {
      return true;
    }

    remaining = requested - elapsed;
    return false;
  }
}
}  // namespace

PosixClockSnapshot posix_clock_snapshot() {
  LockGuard<Mutex> guard(clockChangeLock);
  return {clockChangeGeneration, Time::getTimeNanoseconds(), Time::getTicks()};
}

uint64_t posix_clock_change_generation() {
  LockGuard<Mutex> guard(clockChangeLock);
  return clockChangeGeneration;
}

bool posix_clock_step(int64_t nanoseconds) {
  uint64_t generation;
  {
    LockGuard<Mutex> guard(clockChangeLock);
    const Time::Timestamp now = Time::getTimeNanoseconds();
    const uint64_t magnitude = nanoseconds < 0 ? 0 - static_cast<uint64_t>(nanoseconds)
                                               : static_cast<uint64_t>(nanoseconds);
    if (now > MaximumLinuxSleepNanoseconds ||
        (nanoseconds < 0 ? magnitude > now : magnitude > MaximumLinuxSleepNanoseconds - now)) {
      return false;
    }
    const Time::Timestamp target = nanoseconds < 0 ? now - magnitude : now + magnitude;
    if (!Time::setTimeNanoseconds(target)) {
      return false;
    }
    generation = ++clockChangeGeneration;
    clockChanged.broadcast();
  }
  posix_futex_clock_changed();
  posix_mqueue_clock_changed();
  posix_timerfd_clock_changed(generation);
  return true;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
extern "C" EXPORTED_PUBLIC Time::Timestamp posixNanosleepAlarmDurationForTest(time_t seconds,
                                                                              long nanoseconds) {
  const struct timespec requested = {seconds, nanoseconds};
  return nanosleepAlarmDuration(timespecToNanoseconds(requested.tv_sec, requested.tv_nsec));
}
#endif

int posix_nanosleep(const struct timespec* rqtp, struct timespec* rmtp) {
  struct timespec requested = {};
  if (!PosixSubsystem::copyFromUser(&requested, rqtp, sizeof(requested))) {
    SG_NOTICE("nanosleep -> invalid address");
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (!validTimespec(requested.tv_sec, requested.tv_nsec)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  SG_NOTICE("nanosleep(" << Dec << requested.tv_sec << ":" << requested.tv_nsec << Hex << ") - "
                         << Machine::instance().getTimer()->getTickCount() << ".");

  const Time::Timestamp duration = timespecToNanoseconds(requested.tv_sec, requested.tv_nsec);
  Time::Timestamp remaining = 0;
  if (waitForClockSleep(CLOCK_MONOTONIC, false, duration, remaining)) {
    return 0;
  }

  const struct timespec result = {static_cast<time_t>(remaining / Time::Multiplier::Second),
                                  static_cast<long>(remaining % Time::Multiplier::Second)};
  if (rmtp && !PosixSubsystem::copyToUser(rmtp, &result, sizeof(result))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  SYSCALL_ERROR(Interrupted);
  return -1;
}

int posix_clock_gettime(clockid_t clock_id, struct timespec* tp) {
  SG_NOTICE("clock_gettime(" << Dec << clock_id << Hex << ")");
  Time::Timestamp nanoseconds = 0;
  switch (clock_id) {
    case CLOCK_REALTIME:
      nanoseconds = Time::getTimeNanoseconds();
      break;
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
      // getTicks has no oscillator discipline; RAW shares this unadjusted clock.
      nanoseconds = Time::getTicks();
      break;
    default:
      SYSCALL_ERROR(InvalidArgument);
      return -1;
  }

  const struct timespec result = {static_cast<time_t>(nanoseconds / Time::Multiplier::Second),
                                  static_cast<long>(nanoseconds % Time::Multiplier::Second)};
  if (!PosixSubsystem::copyToUser(tp, &result, sizeof(result))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  return 0;
}

int posix_clock_settime(clockid_t clockId, const LinuxKernelTimespec* value) {
  if (clockId != CLOCK_REALTIME) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  LinuxKernelTimespec requested = {};
  if (!PosixSubsystem::copyFromUser(&requested, value, sizeof(requested))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (!validTimespec(requested.tv_sec, requested.tv_nsec) ||
      static_cast<uint64_t>(requested.tv_sec) >
          (MaximumLinuxSleepNanoseconds - requested.tv_nsec) / Time::Multiplier::Second) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  Process* process = Processor::information().getCurrentThread()->getParent();
  if (process->getEffectiveUserId() != 0) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }
  const Time::Timestamp target =
      static_cast<uint64_t>(requested.tv_sec) * Time::Multiplier::Second + requested.tv_nsec;
  uint64_t generation;
  {
    LockGuard<Mutex> guard(clockChangeLock);
    if (!Time::setTimeNanoseconds(target)) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    generation = ++clockChangeGeneration;
    clockChanged.broadcast();
  }
  posix_futex_clock_changed();
  posix_mqueue_clock_changed();
  posix_timerfd_clock_changed(generation);
  return 0;
}

int posix_clock_getres_native(clockid_t clock_id, struct timespec* resolution) {
  if (!supportedReadClock(clock_id)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  if (!resolution) {
    return 0;
  }

  const struct timespec result = {0, 1};
  if (!PosixSubsystem::copyToUser(resolution, &result, sizeof(result))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  return 0;
}

int posix_clock_getres(clockid_t clock_id, LinuxKernelTimespec* resolution) {
  if (!supportedReadClock(clock_id)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  if (!resolution) {
    return 0;
  }

  const LinuxKernelTimespec result = {0, 1};
  if (!PosixSubsystem::copyToUser(resolution, &result, sizeof(result))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  return 0;
}

int posix_clock_nanosleep(clockid_t clock_id, int flags, const LinuxKernelTimespec* request,
                          LinuxKernelTimespec* remainder) {
  if (!supportedSleepClock(clock_id)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  LinuxKernelTimespec requested = {};
  if (!PosixSubsystem::copyFromUser(&requested, request, sizeof(requested))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (!validTimespec(requested.tv_sec, requested.tv_nsec)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  const bool absolute = flags & TIMER_ABSTIME;
  const Time::Timestamp requestedNanoseconds =
      timespecToNanoseconds(requested.tv_sec, requested.tv_nsec);
  Time::Timestamp remainingNanoseconds = 0;
  if (waitForClockSleep(clock_id, absolute, requestedNanoseconds, remainingNanoseconds)) {
    return 0;
  }

  if (!absolute && remainder) {
    const LinuxKernelTimespec result = {
        static_cast<int64_t>(remainingNanoseconds / Time::Multiplier::Second),
        static_cast<int64_t>(remainingNanoseconds % Time::Multiplier::Second)};
    if (!PosixSubsystem::copyToUser(remainder, &result, sizeof(result))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
  }

  SYSCALL_ERROR(Interrupted);
  return -1;
}
