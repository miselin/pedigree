/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_MODULES_POSIX_INTERVALTIMERSTATE_H
#define PEDIGREE_MODULES_POSIX_INTERVALTIMERSTATE_H
#include <config.h>

#include "pedigree/kernel/time/Time.h"

namespace PosixIntervalTimerState {
struct Consumption {
  Time::Timestamp value;
  bool armed;
  bool expired;
};

struct AbsoluteConsumption {
  Consumption timer;
  Time::Timestamp baseline;
};

/**
 * Advances an interval timer by elapsed time while coalescing expirations.
 *
 * POSIX standard signals are not queued, so a batch which crosses several
 * periods produces one signal while retaining the exact phase of the next
 * expiry.
 */
inline Consumption consume(Time::Timestamp value, Time::Timestamp interval, bool armed,
                           Time::Timestamp elapsed) {
  if (!armed || !elapsed) {
    return {value, armed, false};
  }
  if (elapsed < value) {
    return {value - elapsed, true, false};
  }
  if (!interval) {
    return {0, false, true};
  }

  const Time::Timestamp overshoot = elapsed - value;
  const Time::Timestamp phase = overshoot % interval;
  return {phase ? interval - phase : interval, true, true};
}

/**
 * Advances from a monotonic absolute CPU-time snapshot.
 *
 * A snapshot taken before a timer reconfiguration may arrive after the new
 * value is armed. Never regressing the baseline makes that stale callback a
 * no-op instead of debiting the new timer.
 */
inline AbsoluteConsumption consumeAbsolute(Time::Timestamp value, Time::Timestamp interval,
                                           bool armed, Time::Timestamp baseline,
                                           Time::Timestamp current) {
  if (current <= baseline) {
    return {{value, armed, false}, baseline};
  }

  return {consume(value, interval, armed, current - baseline), current};
}
}  // namespace PosixIntervalTimerState

#endif
