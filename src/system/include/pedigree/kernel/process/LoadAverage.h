/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_LOAD_AVERAGE_H
#define PEDIGREE_LOAD_AVERAGE_H

#include "pedigree/kernel/processor/types.h"

/** The scheduler owns serialization and samples eligible non-idle tasks. */
class LoadAverage {
 public:
  static constexpr uint64_t PeriodNanoseconds = 5000000000ULL;
  static constexpr uint64_t Scale = 65536;

  void update(uint64_t now, uint32_t active) {
    if (now < m_LastSample)
      return;
    const uint64_t periods = (now - m_LastSample) / PeriodNanoseconds;
    if (!periods)
      return;
    m_LastSample += periods * PeriodNanoseconds;
    // Rounded exp(-5 / tau) for one, five and fifteen minutes, in 16.16 units.
    const uint32_t decay[] = {60296, 64453, 65173};
    for (size_t i = 0; i < 3; ++i) {
      const uint64_t factor = power(decay[i], periods);
      const uint64_t target = static_cast<uint64_t>(active) * Scale;
      m_Values[i] = (m_Values[i] * factor + target * (Scale - factor) + Scale / 2) / Scale;
    }
  }
  void snapshot(uint64_t values[3]) const {
    for (size_t i = 0; i < 3; ++i)
      values[i] = m_Values[i];
  }

 private:
  static uint64_t power(uint64_t base, uint64_t exponent) {
    uint64_t result = Scale;
    // A delayed worker folds missed periods in at most 64 iterations.
    while (exponent) {
      if (exponent & 1)
        result = (result * base + Scale / 2) / Scale;
      exponent >>= 1;
      if (exponent)
        base = (base * base + Scale / 2) / Scale;
    }
    return result;
  }
  uint64_t m_LastSample = 0;
  uint64_t m_Values[3] = {};
};

#endif
