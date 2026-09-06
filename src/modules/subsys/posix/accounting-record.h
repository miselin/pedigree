/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_ACCOUNTING_RECORD_H
#define POSIX_ACCOUNTING_RECORD_H

#include <stddef.h>
#include <stdint.h>

namespace PosixAccounting {
constexpr uint64_t TickNanoseconds = 10000000;

struct Record {
  uint8_t flags = 0;
  uint8_t version = 3;
  uint16_t terminal = 0;
  uint32_t exitStatus = 0;
  uint32_t uid = 0, gid = 0, pid = 0, parentPid = 0;
  uint32_t birthSeconds = 0;
  uint32_t elapsedFloat = 0;
  uint16_t userTicks = 0, systemTicks = 0, memory = 0;
  uint16_t io = 0, blocks = 0, minorFaults = 0, majorFaults = 0, swaps = 0;
  char command[16] = {};
};
static_assert(sizeof(Record) == 64 && offsetof(Record, command) == 48,
              "Linux acct_v3 record layout changed");

inline uint16_t compressed(uint64_t value) {
  // Choose the smallest base-eight exponent whose rounded mantissa fits.
  for (unsigned exponent = 0; exponent != 8; ++exponent) {
    const unsigned shift = exponent * 3;
    const uint64_t unit = uint64_t(1) << shift;
    uint64_t mantissa = value >> shift;
    if (shift && (value & (unit - 1)) >= unit / 2)
      ++mantissa;
    if (mantissa <= 8191)
      return static_cast<uint16_t>((exponent << 13) | mantissa);
  }
  return 0xffff;
}

inline uint32_t floatBits(uint64_t value) {
  // Kernel code cannot use floating-point registers. The accounting ABI
  // stores an integral AHZ count as an IEEE-754 binary32 value.
  if (!value)
    return 0;
  unsigned top = 0;
  for (uint64_t bits = value; bits >>= 1;)
    ++top;
  const uint32_t significand = top > 23 ? static_cast<uint32_t>(value >> (top - 23))
                                        : static_cast<uint32_t>(value << (23 - top));
  return ((top + 127) << 23) | (significand & 0x7fffff);
}
}  // namespace PosixAccounting
#endif
