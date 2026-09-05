/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_CLOCK_SYSCALLS_H
#define POSIX_CLOCK_SYSCALLS_H

#include "pedigree/kernel/time/Time.h"

struct PosixClockSnapshot {
  uint64_t generation;
  Time::Timestamp realtime;
  Time::Timestamp monotonic;
};

PosixClockSnapshot posix_clock_snapshot();
uint64_t posix_clock_change_generation();

#endif
