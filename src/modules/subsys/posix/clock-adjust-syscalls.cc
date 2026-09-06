/* Copyright (c) 2026, Pedigree Developers. */
#include "clock-adjust-syscalls.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include <stddef.h>
#include <time.h>

#include "PosixSubsystem.h"
#include "clock-syscalls.h"

namespace {
struct LinuxTimex {
  uint32_t modes, modesPadding;
  int64_t offset, frequency, maximumError, estimatedError;
  int32_t status, statusPadding;
  int64_t constant, precision, tolerance;
  int64_t seconds, fraction;
  int64_t tick, ppsFrequency, jitter;
  int32_t shift, shiftPadding;
  int64_t stability, jitterCount, calibrationCount, errorCount, stabilityCount;
  int32_t tai, padding[11];
};
static_assert(sizeof(LinuxTimex) == 208, "Linux amd64 timex layout");
static_assert(offsetof(LinuxTimex, offset) == 8, "Linux timex offset field");
static_assert(offsetof(LinuxTimex, status) == 40, "Linux timex status field");
static_assert(offsetof(LinuxTimex, seconds) == 72, "Linux timex timeval field");
static_assert(offsetof(LinuxTimex, tai) == 160, "Linux timex TAI field");

constexpr uint32_t AdjustSetOffset = 0x0100;
constexpr uint32_t AdjustMicro = 0x1000;
constexpr uint32_t AdjustNano = 0x2000;
constexpr uint32_t AdjustSingleShot = 0x8001;
constexpr uint32_t AdjustSingleShotRead = 0xA001;
constexpr uint32_t KnownModes = 0xF1BF;
constexpr uint32_t SupportedModes = AdjustSetOffset | AdjustMicro | AdjustNano;
constexpr int32_t StatusUnsynchronized = 0x0040;
constexpr int32_t StatusNano = 0x2000;
constexpr int TimeError = 5;
constexpr int64_t MaximumAdjustment = 0x7FFFFFFFFFFFFFFFLL;

Mutex adjustmentLock;
bool nanosecondUnits = false;

bool decodeAdjustment(const LinuxTimex& value, int64_t& nanoseconds) {
  const int64_t units = value.modes & AdjustNano ? 1000000000 : 1000000;
  if (value.fraction < 0 || value.fraction >= units) {
    return false;
  }
  const int64_t fraction = value.fraction * (1000000000 / units);
  if (value.seconds >= 0) {
    if (value.seconds > (MaximumAdjustment - fraction) / 1000000000) {
      return false;
    }
    nanoseconds = value.seconds * 1000000000 + fraction;
    return true;
  }
  // Normalize a negative seconds field before multiplying: {-1, 500000}
  // denotes minus half a second, and the input may contain INT64_MIN.
  const uint64_t seconds = 0 - static_cast<uint64_t>(value.seconds + 1);
  const uint64_t tail = 1000000000 - fraction;
  if (seconds > (static_cast<uint64_t>(MaximumAdjustment) - tail) / 1000000000) {
    return false;
  }
  nanoseconds = -static_cast<int64_t>(seconds * 1000000000 + tail);
  return true;
}
}  // namespace

int posix_clock_adjtime(int clockId, void* value) {
  LinuxTimex requested = {};
  if (!PosixSubsystem::copyFromUser(&requested, value, sizeof(requested))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (clockId != CLOCK_REALTIME) {
    if (clockId == CLOCK_MONOTONIC) {
      SYSCALL_ERROR(OperationNotSupported);
    } else {
      SYSCALL_ERROR(InvalidArgument);
    }
    return -1;
  }
  const bool readOnly = requested.modes == 0 || requested.modes == AdjustSingleShotRead;
  if (!readOnly &&
      Processor::information().getCurrentThread()->getParent()->getEffectiveUserId() != 0) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }
  if ((requested.modes & ~KnownModes) ||
      ((requested.modes & 0x8000) && requested.modes != AdjustSingleShot &&
       requested.modes != AdjustSingleShotRead)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (!readOnly && (requested.modes & ~SupportedModes)) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }
  int64_t adjustment = 0;
  if ((requested.modes & AdjustSetOffset) && !decodeAdjustment(requested, adjustment)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  LinuxTimex result = {};
  {
    LockGuard<Mutex> guard(adjustmentLock);
    if ((requested.modes & AdjustSetOffset) && !posix_clock_step(adjustment)) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    if (!readOnly) {
      if (requested.modes & AdjustNano)
        nanosecondUnits = true;
      if (requested.modes & AdjustMicro)
        nanosecondUnits = false;
    }
    const Time::Timestamp now = Time::getTimeNanoseconds();
    result.modes = requested.modes;
    result.status = StatusUnsynchronized | (nanosecondUnits ? StatusNano : 0);
    // There is no oscillator discipline or measured error bound. Do not report
    // synchronization, a fictitious tuning range, or zero clock uncertainty.
    result.maximumError = result.estimatedError = MaximumAdjustment;
    result.precision = 1;
    result.seconds = now / Time::Multiplier::Second;
    result.fraction =
        (now % Time::Multiplier::Second) / (nanosecondUnits ? 1 : Time::Multiplier::Microsecond);
  }
  if (!PosixSubsystem::copyToUser(value, &result, sizeof(result))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return TimeError;
}

int posix_adjtimex(void* value) {
  return posix_clock_adjtime(CLOCK_REALTIME, value);
}
