/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_MACHINE_MACH_PC_TSCCLOCK_H
#define PEDIGREE_KERNEL_MACHINE_MACH_PC_TSCCLOCK_H
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"

#include <config.h>

static_assert(__atomic_always_lock_free(sizeof(uint64_t), nullptr),
              "the PC monotonic clock requires lock-free 64-bit publication");

namespace PcTscClock {
constexpr uint64_t MaximumTimestamp = ~static_cast<uint64_t>(0);

struct Calibration {
  Calibration(uint64_t cycles = 1, uint64_t nanoseconds = 1)
      : cycles(cycles), nanoseconds(nanoseconds), whole(0), multiplier(0), fractional(0) {
    if (!cycles) {
      return;
    }

    whole = nanoseconds / cycles;
    fractional = nanoseconds % cycles;
    if (!fractional) {
      return;
    }

#if defined(__x86_64__)
    uint64_t remainder;
    // fractional < cycles, so this boot-time division cannot overflow. Avoid
    // requiring a freestanding 128-bit division runtime for calibration.
    asm("divq %4"
        : "=a"(multiplier), "=d"(remainder)
        : "0"(uint64_t(0)), "1"(fractional), "r"(cycles)
        : "cc");
#else
    multiplier = static_cast<uint64_t>((static_cast<unsigned __int128>(fractional) << 64) / cycles);
#endif
  }

  uint64_t cycles;
  uint64_t nanoseconds;
  uint64_t whole;
  uint64_t multiplier;
  uint64_t fractional;
};

ALWAYS_INLINE inline uint64_t scale(uint64_t cycles, const Calibration& calibration) {
  const unsigned __int128 product = static_cast<unsigned __int128>(cycles) * calibration.multiplier;
  uint64_t nanoseconds = static_cast<uint64_t>(product >> 64);
  const uint64_t fraction = static_cast<uint64_t>(product);

  // The rounded-down Q64 multiplier underestimates by less than cycles / 2^64.
  // Without a carry from fraction + cycles, its integer part is already exact.
  // Correct the remaining cases to retain floor(x * numerator / denominator)
  // even at long uptimes, without a division or accumulated clock drift.
  if (fraction > MaximumTimestamp - cycles) {
    // The fractional quotient is below cycles, so adding one cannot overflow.
    if (static_cast<unsigned __int128>(cycles) * calibration.fractional >=
        static_cast<unsigned __int128>(nanoseconds + 1) * calibration.cycles) {
      ++nanoseconds;
    }
  }

  if (!calibration.whole) {
    return nanoseconds;
  }

  const unsigned __int128 result =
      static_cast<unsigned __int128>(cycles) * calibration.whole + nanoseconds;
  return result > MaximumTimestamp ? MaximumTimestamp : static_cast<uint64_t>(result);
}

ALWAYS_INLINE inline uint64_t saturatingAdd(uint64_t first, uint64_t second) {
  return first > (MaximumTimestamp - second) ? MaximumTimestamp : first + second;
}

ALWAYS_INLINE inline uint64_t fromAnchor(uint64_t currentTsc, uint64_t anchorTsc,
                                         uint64_t anchorNanoseconds,
                                         const Calibration& calibration) {
  if (currentTsc < anchorTsc) {
    return anchorNanoseconds;
  }

  return saturatingAdd(anchorNanoseconds, scale(currentTsc - anchorTsc, calibration));
}

constexpr uint64_t publicationResult(uint64_t candidate, uint64_t observed, bool exchanged) {
  return exchanged ? candidate : observed;
}

/**
 * Publishes a global monotonic floor with at most one compare-exchange.
 *
 * A failed exchange returns the value which won the race. The unpublished
 * candidate is sampled again by the next clock read rather than making a hard
 * interrupt spin on a contended cache line.
 */
class MonotonicPublication {
 public:
  explicit MonotonicPublication(uint64_t value = 0) : m_Value(value) {}

  uint64_t publish(uint64_t candidate) {
    uint64_t observed = __atomic_load_n(&m_Value, __ATOMIC_ACQUIRE);
    if (candidate <= observed) {
      return observed;
    }

    const bool exchanged = __atomic_compare_exchange_n(&m_Value, &observed, candidate, false,
                                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    // On failure compare_exchange replaces observed with the winner.
    return publicationResult(candidate, observed, exchanged);
  }

  uint64_t value() const {
    return __atomic_load_n(&m_Value, __ATOMIC_ACQUIRE);
  }

  void reset(uint64_t value = 0) {
    __atomic_store_n(&m_Value, value, __ATOMIC_RELEASE);
  }

 private:
  uint64_t m_Value;

  MonotonicPublication(const MonotonicPublication&) = delete;
  MonotonicPublication& operator=(const MonotonicPublication&) = delete;
};
}  // namespace PcTscClock

#endif
