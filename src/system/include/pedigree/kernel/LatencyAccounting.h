/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_KERNEL_LATENCYACCOUNTING_H
#define PEDIGREE_KERNEL_LATENCYACCOUNTING_H

#include "pedigree/kernel/compiler.h"

#include <config.h>
#include <stddef.h>
#include <stdint.h>

namespace LatencyAccounting {
enum Field : size_t {
  OnlineSince,
  IrqOffNanoseconds,
  IrqOffCount,
  IrqOffMax,
  IrqOffMaxEnd,
  IrqOffMaxSite,
  IrqOffOver1ms,
  IrqOffOver10ms,
  IrqOffOpenSince,
  IrqOffOpenSite,
  DeferredCpuNanoseconds,
  DeferredWallNanoseconds,
  DeferredCount,
  DeferredMax,
  Count,
};

struct Snapshot {
  uint64_t values[Count] = {};
};

#if PEDIGREE_LATENCY_ACCOUNTING
// Local writers run with IRQs masked. CPU contributions also accept NMI entry.
void armCpu();
bool active();
void recordDeferredCpu(uint64_t elapsed);
void recordDeferredWall(uint64_t elapsed);
EXPORTED_PUBLIC bool snapshot(size_t cpu, Snapshot& result);
#else
inline void armCpu() {}
inline bool snapshot(size_t, Snapshot&) {
  return false;
}
#endif
}  // namespace LatencyAccounting

#if PEDIGREE_LATENCY_ACCOUNTING
// Called before enabling IRQs or after masking them, with kernel GS installed.
extern "C" EXPORTED_PUBLIC void pedigree_irq_time_state(bool enabled);
#endif
#endif
