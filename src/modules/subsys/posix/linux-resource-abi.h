/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_POSIX_LINUX_RESOURCE_ABI_H
#define PEDIGREE_POSIX_LINUX_RESOURCE_ABI_H

#include <stddef.h>
#include <stdint.h>

struct LinuxRlimit64 {
  uint64_t current;
  uint64_t maximum;
};

struct LinuxRusage64 {
  int64_t userSeconds;
  int64_t userMicroseconds;
  int64_t systemSeconds;
  int64_t systemMicroseconds;
  int64_t maximumResidentSetSize;
  int64_t integralSharedMemorySize;
  int64_t integralUnsharedDataSize;
  int64_t integralUnsharedStackSize;
  int64_t pageReclaims;
  int64_t pageFaults;
  int64_t swaps;
  int64_t blockInputs;
  int64_t blockOutputs;
  int64_t messagesSent;
  int64_t messagesReceived;
  int64_t signalsReceived;
  int64_t voluntaryContextSwitches;
  int64_t involuntaryContextSwitches;
};

static_assert(sizeof(LinuxRlimit64) == 16, "Linux amd64 rlimit64 must remain 16 bytes");
static_assert(sizeof(LinuxRusage64) == 144, "Linux amd64 rusage must remain 144 bytes");
static_assert(offsetof(LinuxRusage64, userSeconds) == 0,
              "Linux amd64 rusage user time offset changed");
static_assert(offsetof(LinuxRusage64, systemSeconds) == 16,
              "Linux amd64 rusage system time offset changed");
static_assert(offsetof(LinuxRusage64, maximumResidentSetSize) == 32,
              "Linux amd64 rusage maxrss offset changed");
static_assert(offsetof(LinuxRusage64, involuntaryContextSwitches) == 136,
              "Linux amd64 rusage nivcsw offset changed");

#endif
