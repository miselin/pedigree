/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef POSIX_TIMER_STATE_H
#define POSIX_TIMER_STATE_H

#include "pedigree/kernel/time/Time.h"

namespace PosixTimerState {
constexpr Time::Timestamp MaximumTime = Time::Infinity - 1;

struct Timespec {
  int64_t seconds;
  int64_t nanoseconds;
};

struct Setting {
  Timespec interval;
  Timespec value;
};
static_assert(sizeof(Setting) == 32, "Linux amd64 itimerspec ABI");

struct State {
  Time::Timestamp deadline = 0;
  Time::Timestamp interval = 0;
  bool armed = false;
  bool realtime = false;
};

inline bool decode(const Timespec& source, Time::Timestamp& result) {
  if (source.seconds < 0 || source.nanoseconds < 0 ||
      source.nanoseconds >= static_cast<int64_t>(Time::Multiplier::Second))
    return false;
  const uint64_t seconds = source.seconds;
  result = seconds > (MaximumTime - source.nanoseconds) / Time::Multiplier::Second
               ? MaximumTime
               : seconds * Time::Multiplier::Second + source.nanoseconds;
  return true;
}

inline Timespec encode(Time::Timestamp value) {
  return {static_cast<int64_t>(value / Time::Multiplier::Second),
          static_cast<int64_t>(value % Time::Multiplier::Second)};
}

inline Time::Timestamp add(Time::Timestamp start, Time::Timestamp duration) {
  return start >= MaximumTime || duration > MaximumTime - start ? MaximumTime : start + duration;
}

// Advancing from the previous deadline preserves phase even after many missed
// periods. The remainder avoids overflowing a period-count multiplication.
inline uint64_t advance(State& state, Time::Timestamp now) {
  if (!state.armed || now < state.deadline)
    return 0;
  if (!state.interval) {
    state.armed = false;
    return 1;
  }
  const uint64_t elapsed = now - state.deadline;
  const uint64_t expirations = elapsed / state.interval + 1;
  const uint64_t remaining = state.interval - elapsed % state.interval;
  state.deadline = add(now, remaining);
  state.armed = state.deadline > now;
  return expirations;
}

inline Setting snapshot(const State& state, Time::Timestamp now) {
  return {encode(state.interval),
          encode(state.armed && state.deadline > now ? state.deadline - now : 0)};
}
}  // namespace PosixTimerState

#endif
