/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef KERNEL_MACHINE_MACH_PC_LOCALAPICTIMERHANDLERSLOTS_H
#define KERNEL_MACHINE_MACH_PC_LOCALAPICTIMERHANDLERSLOTS_H

#include "pedigree/kernel/machine/SchedulerTimerHandlerSlot.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

/** Fixed handler publication slots for the 8-bit xAPIC processor envelope. */
class LocalApicTimerHandlerSlots {
 public:
  static constexpr size_t Capacity = 256;

  LocalApicTimerHandlerSlots() : m_Handlers() {}

  /** Publish a handler only when the processor slot is currently empty. */
  bool registerHandler(ProcessorId processor, SchedulerTimerHandler* handler) {
    if (processor >= Capacity || !handler)
      return false;

    return m_Handlers[processor].publish(processor, handler);
  }

  /**
   * Unpublish only the exact handler currently owned by this processor.
   * A different processor is rejected before it can close this slot.
   */
  bool removeHandler(ProcessorId processor, SchedulerTimerHandler* handler) {
    if (processor >= Capacity || !handler)
      return false;

    return m_Handlers[processor].unpublish(processor, handler);
  }

  /**
   * Admit one callback on this physical APIC processor and bind its lifetime
   * to the caller's guard. No raw handler pointer escapes the slot.
   */
  bool beginDispatch(ProcessorId processor, SchedulerTimerHandlerSlot::DispatchGuard& guard) {
    if (processor >= Capacity)
      return false;
    return m_Handlers[processor].beginDispatch(processor, guard);
  }

  /** Test/diagnostic predicate which never permits callback dispatch. */
  bool isPublished(ProcessorId processor, SchedulerTimerHandler* handler) const {
    return processor < Capacity && m_Handlers[processor].isPublished(processor, handler);
  }

 private:
  LocalApicTimerHandlerSlots(const LocalApicTimerHandlerSlots&) = delete;
  LocalApicTimerHandlerSlots& operator=(const LocalApicTimerHandlerSlots&) = delete;

  SchedulerTimerHandlerSlot m_Handlers[Capacity];
};

#endif
