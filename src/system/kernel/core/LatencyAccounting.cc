/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LatencyAccounting.h"

#if PEDIGREE_LATENCY_ACCOUNTING
#include "pedigree/kernel/process/CpuAffinity.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/time/Time.h"

namespace LatencyAccounting {
namespace {
struct alignas(64) CpuCounters {
  uint64_t sequence = 0;
  Snapshot data;
};
CpuCounters counters[CpuAffinityMask::MaximumCpus];
bool clockReady = false;

CpuCounters* local(bool includeNmi = false) {
  if (!__atomic_load_n(&clockReady, __ATOMIC_ACQUIRE)) {
    return nullptr;
  }
  const size_t cpu = Processor::index();
  if (cpu >= CpuAffinityMask::MaximumCpus ||
      !__atomic_load_n(&counters[cpu].data.values[OnlineSince], __ATOMIC_RELAXED) ||
      (!includeNmi && Processor::information().kernelGsAnchor()->latencyNmiDepth)) {
    return nullptr;
  }
  return &counters[cpu];
}

void begin(CpuCounters& cpu) {
  // x64 store ordering and the local IRQ-off writer need no locked RMW.
  __atomic_store_n(&cpu.sequence, cpu.sequence + 1, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_RELEASE);
}
void end(CpuCounters& cpu) {
  __atomic_store_n(&cpu.sequence, cpu.sequence + 1, __ATOMIC_RELEASE);
}
uint64_t get(CpuCounters& cpu, Field field) {
  return __atomic_load_n(&cpu.data.values[field], __ATOMIC_RELAXED);
}
void set(CpuCounters& cpu, Field field, uint64_t value) {
  __atomic_store_n(&cpu.data.values[field], value, __ATOMIC_RELAXED);
}
void add(CpuCounters& cpu, Field field, uint64_t value) {
  set(cpu, field, get(cpu, field) + value);
}
}  // namespace

void armCpu() {
  const size_t index = Processor::index();
  if (index >= CpuAffinityMask::MaximumCpus) {
    return;
  }
  CpuCounters& cpu = counters[index];
  const uint64_t now = Time::sampleCpuTime().timestamp;
  begin(cpu);
  set(cpu, OnlineSince, now ? now : 1);
  set(cpu, IrqOffOpenSince, now ? now : 1);
  end(cpu);
  __atomic_store_n(&clockReady, true, __ATOMIC_RELEASE);
}

bool active() {
  return local() != nullptr;
}

void recordIrqState(bool enabled, uintptr_t site) {
  CpuCounters* cpu = local();
  if (!cpu) {
    return;
  }
  const uint64_t start = get(*cpu, IrqOffOpenSince);
  if (enabled != (start != 0)) {
    return;
  }
  const uint64_t now = Time::sampleCpuTime().timestamp;
  begin(*cpu);
  if (enabled) {
    const uint64_t elapsed = now >= start ? now - start : 0;
    add(*cpu, IrqOffNanoseconds, elapsed);
    add(*cpu, IrqOffCount, 1);
    if (elapsed > get(*cpu, IrqOffMax)) {
      set(*cpu, IrqOffMax, elapsed);
      set(*cpu, IrqOffMaxEnd, now);
      set(*cpu, IrqOffMaxSite, get(*cpu, IrqOffOpenSite));
    }
    if (elapsed >= Time::Multiplier::Millisecond) {
      add(*cpu, IrqOffOver1ms, 1);
    }
    if (elapsed >= 10 * Time::Multiplier::Millisecond) {
      add(*cpu, IrqOffOver10ms, 1);
    }
    set(*cpu, IrqOffOpenSince, 0);
    set(*cpu, IrqOffOpenSite, 0);
  } else {
    set(*cpu, IrqOffOpenSince, now ? now : 1);
    set(*cpu, IrqOffOpenSite, site);
  }
  end(*cpu);
}

void recordDeferredCpu(uint64_t elapsed) {
  if (CpuCounters* cpu = local(true)) {
    // Interrupt accounting publishes the preceding CPU slice after NMI entry.
    // This independent counter must accept that slice without nesting a writer
    // inside an interrupted IRQ-state transaction.
    __atomic_fetch_add(&cpu->data.values[DeferredCpuNanoseconds], elapsed, __ATOMIC_RELAXED);
  }
}

void recordDeferredWall(uint64_t elapsed) {
  if (CpuCounters* cpu = local()) {
    begin(*cpu);
    add(*cpu, DeferredWallNanoseconds, elapsed);
    add(*cpu, DeferredCount, 1);
    if (elapsed > get(*cpu, DeferredMax)) {
      set(*cpu, DeferredMax, elapsed);
    }
    end(*cpu);
  }
}

bool snapshot(size_t index, Snapshot& result) {
  if (index >= CpuAffinityMask::MaximumCpus) {
    return false;
  }
  CpuCounters& cpu = counters[index];
  // A busy remote writer must not keep a diagnostic reader spinning forever.
  for (size_t attempt = 0; attempt < 32; ++attempt) {
    const uint64_t before = __atomic_load_n(&cpu.sequence, __ATOMIC_ACQUIRE);
    if (before & 1) {
      continue;
    }
    for (size_t field = 0; field < Count; ++field) {
      result.values[field] = __atomic_load_n(&cpu.data.values[field], __ATOMIC_RELAXED);
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (before == __atomic_load_n(&cpu.sequence, __ATOMIC_RELAXED)) {
      return result.values[OnlineSince] != 0;
    }
  }
  return false;
}
}  // namespace LatencyAccounting

extern "C" void pedigree_irq_time_state(bool enabled) {
  LatencyAccounting::recordIrqState(enabled,
                                    reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
}
#endif
