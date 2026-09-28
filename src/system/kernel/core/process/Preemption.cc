/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/Metrics.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Preemption.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

void Preemption::disable() {
  Metrics::increment(Metrics::Counter::PreemptionDisable);
  const bool interrupts = Processor::getInterrupts();
  if (interrupts) {
    Processor::setInterrupts(false);
  }
  size_t& depth = Processor::information().m_PreemptionDepth;
  if (__atomic_add_fetch(&depth, size_t(1), __ATOMIC_ACQUIRE) == 0) {
    panic("Preemption disable depth overflowed.");
  }
  if (interrupts) {
    Processor::setInterrupts(true);
  }
}

void Preemption::enable() {
  Metrics::increment(Metrics::Counter::PreemptionEnable);
  const bool interrupts = Processor::getInterrupts();
  if (interrupts) {
    Processor::setInterrupts(false);
  }
  size_t& depth = Processor::information().m_PreemptionDepth;
  const size_t previous = __atomic_fetch_sub(&depth, size_t(1), __ATOMIC_RELEASE);
  if (!previous) {
    panic("Preemption enable has no matching disable.");
  }

#if THREADS
  Thread* current = Processor::information().getCurrentThread();
  bool maySchedule = previous == 1 && interrupts && current &&
                     current->executionContext() == ExecutionContext::WaitableThread &&
                     !Processor::inDeviceHardIrq();
#if HOSTED
  maySchedule &= !current || !current->getHostedSignalDepth();
#endif
  if (maySchedule && current->getScheduler()) {
    // Keep IRQs masked through the pending-edge claim. The scheduler may
    // suspend this stack; restoring the entry IF belongs to this invocation.
    current->getScheduler()->servicePendingScheduling();
  }
#endif
  if (interrupts) {
    Processor::setInterrupts(true);
  }
}

bool Preemption::disabled() {
  Metrics::increment(Metrics::Counter::PreemptionCheck);
  const bool interrupts = Processor::getInterrupts();
  if (interrupts) {
    Processor::setInterrupts(false);
  }
  const bool disabled =
      __atomic_load_n(&Processor::information().m_PreemptionDepth, __ATOMIC_RELAXED) != 0;
  if (interrupts) {
    Processor::setInterrupts(true);
  }
  return disabled;
}
