/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_KERNEL_PROCESS_PREEMPTION_H
#define PEDIGREE_KERNEL_PROCESS_PREEMPTION_H

#include "pedigree/kernel/compiler.h"

namespace Preemption {
/** Pins the current execution to its CPU without changing the caller's IRQ state.
 * Calls nest. Blocking, yielding, and abandoning the stack are forbidden until
 * the matching outer enable. IRQ handlers may still interrupt this scope.
 */
EXPORTED_PUBLIC void disable();

/** Ends one scope and services deferred scheduling at an ordinary thread boundary. */
EXPORTED_PUBLIC void enable();

EXPORTED_PUBLIC bool disabled();
}  // namespace Preemption

#endif
