/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef KERNEL_MACHINE_SCHEDULERTIMERHANDLER_H
#define KERNEL_MACHINE_SCHEDULERTIMERHANDLER_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"

/** @addtogroup kernelmachine
 * @{ */

/**
 * Hard scheduler-tick callback.
 *
 * Scheduler timers retain the interrupted processor state because scheduling
 * may switch away from that state directly. Ordinary TimerHandler callbacks
 * deliberately do not expose it.
 */
class EXPORTED_PUBLIC SchedulerTimerHandler {
 public:
  /** Handles a scheduler tick in interrupt context. */
  virtual void timer(uint64_t delta, InterruptState& state) = 0;

 protected:
  virtual ~SchedulerTimerHandler();
};

/** @} */

#endif
