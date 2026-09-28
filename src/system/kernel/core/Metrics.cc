/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/Metrics.h"

#if PEDIGREE_METRICS
namespace Metrics {
static_assert(__atomic_always_lock_free(sizeof(uint64_t), nullptr),
              "Kernel metrics require lock-free 64-bit counters");
CpuCounters counters[CpuAffinityMask::MaximumCpus] = {};

bool snapshot(size_t cpu, Snapshot& result) {
  if (cpu >= CpuAffinityMask::MaximumCpus) {
    return false;
  }
  // Individual counters are atomic; collecting CPUs is not a global barrier.
  for (size_t field = 0; field < Count; ++field) {
    result.values[field] = __atomic_load_n(&counters[cpu].values[field], __ATOMIC_RELAXED);
  }
  return true;
}
}  // namespace Metrics
#endif
