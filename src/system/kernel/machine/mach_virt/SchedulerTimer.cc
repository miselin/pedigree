/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "SchedulerTimer.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/SchedulerTimerDispatchCleanup.h"
#include "pedigree/kernel/machine/SchedulerTimerHandler.h"
#include "pedigree/kernel/processor/Processor.h"

#include "DeviceTree.h"

namespace {
constexpr uint64_t NanosecondsPerSecond = 1000000000ULL;
constexpr uint32_t TicksPerSecond = 100;
}  // namespace

VirtSchedulerTimer VirtSchedulerTimer::m_Instance;

VirtSchedulerTimer& VirtSchedulerTimer::instance() {
  return m_Instance;
}

VirtSchedulerTimer::VirtSchedulerTimer()
    : m_Handler(),
      m_Frequency(0),
      m_LastCount(0),
      m_IntervalTicks(0),
      m_IrqId(0),
      m_Initialised(false) {}

VirtSchedulerTimer::~VirtSchedulerTimer() {
  uninitialise();
}

uint64_t VirtSchedulerTimer::counter() const {
  uint64_t value;
  asm volatile("mrs %0, cntvct_el0" : "=r"(value));
  return value;
}

uint64_t VirtSchedulerTimer::ticksToNanoseconds(uint64_t ticks) const {
  return (ticks / m_Frequency) * NanosecondsPerSecond +
         (ticks % m_Frequency) * NanosecondsPerSecond / m_Frequency;
}

bool VirtSchedulerTimer::initialise() {
  if (m_Initialised) {
    return false;
  }
  asm volatile("mrs %0, cntfrq_el0" : "=r"(m_Frequency));
  if (!m_Frequency) {
    return false;
  }
  m_IntervalTicks = static_cast<uint32_t>(m_Frequency / TicksPerSecond);
  if (!m_IntervalTicks) {
    m_IntervalTicks = 1;
  }
  asm volatile("msr cntv_ctl_el0, %0\n\tisb" : : "r"(uint64_t(0)) : "memory");

  IrqManager& manager = *Machine::instance().getIrqManager();
  m_IrqId = manager.registerSchedulerIrqHandler(VirtDeviceTree::virtualTimerIrq(), this,
                                                IrqPolicy::levelHard());
  if (!m_IrqId) {
    return false;
  }

  m_LastCount = counter();
  asm volatile("msr cntv_tval_el0, %0\n\tmsr cntv_ctl_el0, %1\n\tisb"
               :
               : "r"(uint64_t(m_IntervalTicks)), "r"(uint64_t(1))
               : "memory");
  m_Initialised = true;
  return true;
}

void VirtSchedulerTimer::uninitialise() {
  if (!m_Initialised) {
    return;
  }
  asm volatile("msr cntv_ctl_el0, %0\n\tisb" : : "r"(uint64_t(0)) : "memory");
  IrqManager& manager = *Machine::instance().getIrqManager();
  if (!manager.unregisterSchedulerIrqHandler(m_IrqId, this)) {
    return;
  }
  m_IrqId = 0;
  m_Initialised = false;
}

bool VirtSchedulerTimer::registerHandler(SchedulerTimerHandler* handler) {
  return m_Handler.publish(Processor::id(), handler);
}

bool VirtSchedulerTimer::removeHandler(SchedulerTimerHandler* handler) {
  return canRemoveHandlerInCurrentContext() && m_Handler.unpublish(Processor::id(), handler);
}

uint64_t VirtSchedulerTimer::nominalQuantumNs() const {
  return NanosecondsPerSecond / TicksPerSecond;
}

void VirtSchedulerTimer::schedulerIrq(irq_id_t number, InterruptState& state) {
  if (number != m_IrqId) {
    return;
  }

  // Re-arm before the callback: a context switch may never return here.
  asm volatile("msr cntv_tval_el0, %0\n\tmsr cntv_ctl_el0, %1\n\tisb"
               :
               : "r"(uint64_t(m_IntervalTicks)), "r"(uint64_t(1))
               : "memory");
  const uint64_t now = counter();
  const uint64_t elapsed = ticksToNanoseconds(now - m_LastCount);
  m_LastCount = now;

  SchedulerTimerHandlerSlot::DispatchGuard dispatch;
  if (m_Handler.beginDispatch(Processor::id(), dispatch)) {
    SchedulerTimerDispatchCleanup cleanup(dispatch);
    ExecutionContextGuard context(ExecutionContext::SchedulerIrq);
    dispatch.handler()->timer(elapsed, state);
  }
}
