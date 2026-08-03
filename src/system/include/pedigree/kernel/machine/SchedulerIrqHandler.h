/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_MACHINE_SCHEDULERIRQHANDLER_H
#define PEDIGREE_KERNEL_MACHINE_SCHEDULERIRQHANDLER_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/types.h"
#include "pedigree/kernel/processor/state_forward.h"

/**
 * Dedicated controller callback for a scheduler-timer interrupt source.
 *
 * A scheduler tick can switch away from, and permanently abandon, the current
 * interrupt frame. This is therefore a terminal action for the controller:
 * it must finish acknowledgement and per-occurrence accounting before making
 * this callback, with no controller lifetime state left on the abandoned
 * stack.
 */
class EXPORTED_PUBLIC SchedulerIrqHandler
{
  public:
    virtual void schedulerIrq(irq_id_t number, InterruptState &state) = 0;

  protected:
    virtual ~SchedulerIrqHandler()
    {
    }
};

#endif
