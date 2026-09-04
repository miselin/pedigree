/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_PROCESS_DEFERREDTHREADREAP_H
#define PEDIGREE_KERNEL_PROCESS_DEFERREDTHREADREAP_H

class Thread;

/** Caller-owned storage for allocation-free off-stack Thread retirement. */
struct DeferredThreadReapNode {
  explicit DeferredThreadReapNode(Thread* target = nullptr) : next(nullptr), thread(target) {}

  DeferredThreadReapNode* next;
  Thread* thread;
};

#endif
