/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_SYSTEM_INFORMATION_ABI_H
#define POSIX_SYSTEM_INFORMATION_ABI_H

#include <stdint.h>

namespace PosixSystemInformation {
struct Record {
  int64_t uptime = 0;
  uint64_t loads[3] = {};
  uint64_t totalram = 0, freeram = 0, sharedram = 0, bufferram = 0;
  uint64_t totalswap = 0, freeswap = 0;
  uint16_t procs = 0, pad = 0;
  uint32_t alignment = 0;
  uint64_t totalhigh = 0, freehigh = 0;
  uint32_t mem_unit = 1, tail = 0;
};
static_assert(sizeof(Record) == 112, "Linux amd64 sysinfo ABI");
static_assert(__builtin_offsetof(Record, totalhigh) == 88, "Linux sysinfo high-memory offset");
static_assert(__builtin_offsetof(Record, mem_unit) == 104, "Linux sysinfo units offset");

inline bool setMemory(Record& record, uint64_t total, uint64_t free, uint64_t swapTotal,
                      uint64_t swapUsed, uint64_t pageSize) {
  const uint64_t maximum = ~uint64_t(0);
  if (!pageSize || free > total || swapUsed > swapTotal || total > maximum / pageSize ||
      swapTotal > maximum / pageSize)
    return false;
  record.totalram = total * pageSize;
  record.freeram = free * pageSize;
  record.totalswap = swapTotal * pageSize;
  record.freeswap = (swapTotal - swapUsed) * pageSize;
  return true;
}
}  // namespace PosixSystemInformation

#endif
