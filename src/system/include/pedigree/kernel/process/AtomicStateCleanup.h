/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_PROCESS_ATOMICSTATECLEANUP_H
#define PEDIGREE_KERNEL_PROCESS_ATOMICSTATECLEANUP_H

#include "pedigree/kernel/process/DeferredScope.h"

/** Interrupt and exception cleanup records share Thread's one ordered stack. */
using AtomicStateCleanupRecord = DeferredScopeRecord;

#endif
