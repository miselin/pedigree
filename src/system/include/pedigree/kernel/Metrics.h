/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_KERNEL_METRICS_H
#define PEDIGREE_KERNEL_METRICS_H

#include "pedigree/kernel/compiler.h"

#include <config.h>
#include <stddef.h>
#include <stdint.h>

#if PEDIGREE_METRICS
#include "pedigree/kernel/process/CpuAffinity.h"
#include "pedigree/kernel/processor/Processor.h"
#endif

namespace Metrics {
enum Counter : size_t {
  Schedule,
  Yield,
  ContextSwitch,
  SameThread,
  IdleSelection,
  Timer,
  RescheduleService,
  WorkerWake,
  Interrupt,
  Exception,
  Syscall,
  SpinlockAcquire,
  SpinlockContended,
  Count,
};

struct Snapshot {
  uint64_t values[Count] = {};
};

#if PEDIGREE_METRICS
struct alignas(64) CpuCounters {
  uint64_t values[Count];
};
extern EXPORTED_PUBLIC CpuCounters counters[CpuAffinityMask::MaximumCpus];

// A writer can be interrupted or migrate after observing its CPU. Relaxed
// atomic increments preserve counts without masking IRQs or pinning execution.
ALWAYS_INLINE inline void increment(Counter counter) {
  const size_t cpu = Processor::index();
  if (cpu < CpuAffinityMask::MaximumCpus) {
    __atomic_fetch_add(&counters[cpu].values[counter], uint64_t(1), __ATOMIC_RELAXED);
  }
}
EXPORTED_PUBLIC bool snapshot(size_t cpu, Snapshot& result);
#else
inline void increment(Counter) {}
inline bool snapshot(size_t, Snapshot& result) {
  result = {};
  return false;
}
#endif
}  // namespace Metrics
#endif
