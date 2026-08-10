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

#ifndef MEMORY_PRESSURE_MANAGER_H
#define MEMORY_PRESSURE_MANAGER_H

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/new"

/** Maximum memory pressure handler priority (one list per priority level). */
#define MAX_MEMPRESSURE_PRIORITY 16

/**
 * MemoryPressureHandler: interface for memory pressure handlers.
 */
class EXPORTED_PUBLIC MemoryPressureHandler {
 public:
  MemoryPressureHandler();
  virtual ~MemoryPressureHandler();

  /**
   * Returns static storage so describing a recovery action cannot itself
   * allocate memory.
   */
  virtual const char* getMemoryPressureDescription() = 0;

  /**
   * Called by MemoryPressureManager to request this handler to take
   * action to reduce memory pressure.
   * \return true if pages were released, false otherwise.
   */
  virtual bool compact() = 0;

 private:
  friend class MemoryPressureManager;

  MemoryPressureHandler* m_pPrevious;
  MemoryPressureHandler* m_pNext;
  size_t m_Priority;
  size_t m_RegistrationSequence;
  bool m_bRegistered;
  bool m_bRemoving;

#if THREADS
  /** Callback lifetime state protected by m_CallbackWaiters' guard. */
  size_t m_CallbacksInFlight;
  const void* m_pCallbackOwner;
  WaitQueue m_CallbackWaiters;
#endif
};

/**
 * MemoryPressureManager: global memory pressure handling.
 *
 * This class offers a central interface for the various mechanisms available
 * to alleviate memory pressure to register themselves and be called, without
 * requiring the kernel itself to know about them (eg, loadable modules).
 */
class EXPORTED_PUBLIC MemoryPressureManager {
 public:
  MemoryPressureManager();
  ~MemoryPressureManager();

  const static size_t HighestPriority = 0;
  const static size_t HighPriority = MAX_MEMPRESSURE_PRIORITY / 3;
  const static size_t MediumPriority = MAX_MEMPRESSURE_PRIORITY / 2;
  const static size_t LowPriority = (MAX_MEMPRESSURE_PRIORITY * 2) / 3;
  const static size_t LowestPriority = MAX_MEMPRESSURE_PRIORITY - 1;

  static MemoryPressureManager& instance() {
    return m_Instance;
  }

  static size_t getHighWatermark() {
    // Once the system has only this or less pages free, we begin
    // doing compacts. We do not want to wait until the system is
    // actually out of memory, as some compact mechanisms require
    // allocating memory.
    return 16;
  }

  static size_t getLowWatermark() {
    // Caches can begin voluntarily evicting cache pages at this mark.
    // Should no action be taken, the system will take over forcefully
    // at the high water mark.
    return 32;
  }

  /**
   * Attempt to alleviate memory pressure by requesting registered
   * handlers release pages that can be safely released.
   *
   * Registry state is never locked across a callback. A callback may
   * register or remove another handler. Removing the currently executing
   * handler from its own callback is rejected because a synchronous removal
   * cannot wait for itself to finish.
   */
  bool compact();

  /**
   * True while the current execution context is already inside compact().
   * Physical allocators use this to permit callback allocations without
   * recursively starting another pressure pass.
   */
  bool compactingForCurrentExecution() const;

  /**
   * Register a new handler.
   */
  void registerHandler(size_t prio, MemoryPressureHandler* pHandler);

  /**
   * Remove a handler.
   */
  void removeHandler(MemoryPressureHandler* pHandler);

 private:
  static MemoryPressureManager m_Instance;

  Spinlock m_Lock;
  MemoryPressureHandler* m_Handlers[MAX_MEMPRESSURE_PRIORITY];
  MemoryPressureHandler* m_HandlerTails[MAX_MEMPRESSURE_PRIORITY];
  size_t m_NextRegistrationSequence;
  Atomic<bool> m_bCompacting;
  const void* m_pCompactOwner;
#if THREADS
  WaitQueue m_CompactWaiters;
#endif
};

#endif
