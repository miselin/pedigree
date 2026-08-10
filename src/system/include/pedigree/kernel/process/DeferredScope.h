/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_PROCESS_DEFERREDSCOPE_H
#define PEDIGREE_KERNEL_PROCESS_DEFERREDSCOPE_H

#include "pedigree/kernel/processor/types.h"

/**
 * Intrusive record for a deferral scope living on a Thread kernel stack.
 *
 * Thread teardown unlinks these records before abandoning their stack. The
 * record itself needs no destructor in that case.
 */
struct DeferredScopeRecord {
  using Cleanup = void (*)(void*);

  DeferredScopeRecord()
      : next(nullptr),
        stateLevel(0),
        sequence(0),
        defersTermination(false),
        defersEvents(false),
        cleanup(nullptr),
        context(nullptr),
        armed(false) {}

  DeferredScopeRecord* next;
  size_t stateLevel;
  size_t sequence;
  bool defersTermination;
  bool defersEvents;
  Cleanup cleanup;
  void* context;
  bool armed;
};

#endif
