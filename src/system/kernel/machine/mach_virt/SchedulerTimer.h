/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef KERNEL_MACHINE_VIRT_SCHEDULERTIMER_H
#define KERNEL_MACHINE_VIRT_SCHEDULERTIMER_H

#include "pedigree/kernel/machine/SchedulerIrqHandler.h"
#include "pedigree/kernel/machine/SchedulerTimer.h"
#include "pedigree/kernel/machine/SchedulerTimerHandlerSlot.h"

class VirtSchedulerTimer : public SchedulerTimer, private SchedulerIrqHandler {
 public:
  static VirtSchedulerTimer& instance();

  bool initialise();
  void uninitialise();
  bool registerHandler(SchedulerTimerHandler* handler) override;
  bool removeHandler(SchedulerTimerHandler* handler) override;
  uint64_t nominalQuantumNs() const override;

 private:
  VirtSchedulerTimer();
  ~VirtSchedulerTimer() override;
  void schedulerIrq(irq_id_t number, InterruptState& state) override;
  uint64_t counter() const;
  uint64_t ticksToNanoseconds(uint64_t ticks) const;

  SchedulerTimerHandlerSlot m_Handler;
  uint64_t m_Frequency;
  uint64_t m_LastCount;
  uint32_t m_IntervalTicks;
  irq_id_t m_IrqId;
  bool m_Initialised;

  static VirtSchedulerTimer m_Instance;
};

#endif
