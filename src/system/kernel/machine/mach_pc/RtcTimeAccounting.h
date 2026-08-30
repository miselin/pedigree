/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_MACHINE_MACH_PC_RTCTIMEACCOUNTING_H
#define PEDIGREE_KERNEL_MACHINE_MACH_PC_RTCTIMEACCOUNTING_H
#include <config.h>

#include "pedigree/kernel/processor/types.h"

namespace RtcTimeAccounting {
constexpr uint64_t NanosecondsPerSecond = 1000000000ULL;

struct CivilTime {
  size_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint64_t nanosecond;
};

inline bool isLeapYear(size_t year) {
  return (year % 4) == 0 && ((year % 100) != 0 || (year % 400) == 0);
}

inline uint8_t daysInMonth(size_t year, uint8_t month) {
  static constexpr uint8_t Days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    return 31;
  }
  return month == 2 && isLeapYear(year) ? 29 : Days[month - 1];
}

/** Returns a forward-only delta and advances the observation cursor. */
inline uint64_t consumeElapsed(uint64_t observed, uint64_t& cursor) {
  if (observed <= cursor) {
    return 0;
  }

  const uint64_t delta = observed - cursor;
  cursor = observed;
  return delta;
}

/** Advances a valid civil-time snapshot and returns whole seconds crossed. */
inline uint64_t advanceCivilTime(CivilTime& time, uint64_t delta) {
  uint64_t elapsedSeconds = delta / NanosecondsPerSecond;
  const uint64_t remainder = delta % NanosecondsPerSecond;
  if (remainder >= (NanosecondsPerSecond - time.nanosecond)) {
    ++elapsedSeconds;
    time.nanosecond = remainder - (NanosecondsPerSecond - time.nanosecond);
  } else {
    time.nanosecond += remainder;
  }

  uint64_t total = static_cast<uint64_t>(time.second) + elapsedSeconds;
  time.second = static_cast<uint8_t>(total % 60);
  total = static_cast<uint64_t>(time.minute) + (total / 60);
  time.minute = static_cast<uint8_t>(total % 60);
  total = static_cast<uint64_t>(time.hour) + (total / 60);
  time.hour = static_cast<uint8_t>(total % 24);
  uint64_t days = total / 24;

  while (days) {
    const uint8_t monthDays = daysInMonth(time.year, time.month);
    const uint64_t remainingInMonth = monthDays - time.day;
    if (days <= remainingInMonth) {
      time.day = static_cast<uint8_t>(time.day + days);
      break;
    }

    days -= remainingInMonth + 1;
    time.day = 1;
    if (++time.month > 12) {
      time.month = 1;
      ++time.year;
    }
  }

  return elapsedSeconds;
}
}  // namespace RtcTimeAccounting

#endif
