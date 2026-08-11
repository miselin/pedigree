/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef ATA_WRITE_CACHE_H
#define ATA_WRITE_CACHE_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Cache.h"

/**
 * Trades the pin transferred with an accepted queued ATA write for the
 * execution pin returned by lookup(). Cache::lookup refuses a still-published
 * page once retirement starts, so the transferred pin must be retired even
 * when no execution pin can be acquired.
 */
MUST_USE_RESULT inline uintptr_t ataTakeQueuedWritePage(Cache& cache, uint64_t location) {
  const uintptr_t buffer = cache.lookup(location);
  cache.release(location);
  return buffer;
}

#endif
