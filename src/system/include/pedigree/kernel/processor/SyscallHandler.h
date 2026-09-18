/*
 * Copyright (c) 2008-2014, Pedigree Developers
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

#ifndef KERNEL_PROCESSOR_SYSCALLHANDLER_H
#define KERNEL_PROCESSOR_SYSCALLHANDLER_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"

/** @addtogroup kernelprocessor
 * @{ */

/** Abstract base class for all syscall-handlers. All syscall-handlers mustbe
 * derived from this class */
class EXPORTED_PUBLIC SyscallHandler {
 public:
  /** Called when the handler is registered with the syscall manager and a
   *syscall occurred \param[in] State reference to the state before the
   *syscall */
  virtual uintptr_t syscall(SyscallState& State) = 0;

  /**
   * Whether syscall() may run with IRQs masked and no dispatch-action context.
   * This query must be bounded and side-effect-free. A true result guarantees
   * bounded completion without enabling IRQs, allocating, acquiring locks,
   * touching user memory, blocking, dispatching events, requesting post-syscall
   * actions, changing TLS or the saved return frame, or reporting interruption.
   * From the frame, both methods may read only saved general registers and control fields;
   * selector/base metadata may not yet be captured, so do not copy the frame.
   * The caller still owns accounting and return work.
   */
  virtual bool canRunWithInterruptsDisabled(const SyscallState& state) const;

 protected:
  /** Virtual destructor */
  virtual ~SyscallHandler();
};

/** @} */

#endif
