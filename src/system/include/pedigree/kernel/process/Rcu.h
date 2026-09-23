/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_KERNEL_PROCESS_RCU_H
#define PEDIGREE_KERNEL_PROCESS_RCU_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/RcuReadState.h"

/**
 * Protects resident, nonblocking lookups after processor discovery.
 *
 * Readers may nest, including in IRQ/NMI context. The guard disables maskable
 * interrupts, preventing preemption and migration. Do not enable interrupts,
 * block, fault on pageable storage, or let a borrowed pointer escape: retain a
 * durable owner before leaving. Context switches with a live guard are fatal.
 */
class EXPORTED_PUBLIC RcuReadGuard {
 public:
  RcuReadGuard();
  ~RcuReadGuard();

 private:
  NOT_COPYABLE_OR_ASSIGNABLE(RcuReadGuard);
  RcuReadState* m_State;
  bool m_Interrupts;
};

template <typename T>
class RcuPointer {
 public:
  RcuPointer() = default;
  NOT_COPYABLE_OR_ASSIGNABLE(RcuPointer);

  T* load(const RcuReadGuard&) const {
    return __atomic_load_n(&m_Pointer, __ATOMIC_SEQ_CST);
  }

  /** The caller holds the writer lock that excludes exchange and reclamation. */
  T* loadForUpdate() const {
    return __atomic_load_n(&m_Pointer, __ATOMIC_SEQ_CST);
  }

  /** Serialize writers separately and retire the returned pointer. */
  T* exchange(T* replacement) {
    return __atomic_exchange_n(&m_Pointer, replacement, __ATOMIC_SEQ_CST);
  }

 private:
  T* m_Pointer = nullptr;
};

namespace Rcu {
/**
 * Waits for readers admitted before publication to finish. New readers do not
 * prolong an observed generation. May run under a writer mutex, but never in a
 * read section. Reader exit is the quiescent point; idle CPUs need no polling.
 */
EXPORTED_PUBLIC void synchronize();
}  // namespace Rcu

/**
 * Bounded deferred reclamation for a writer-owned lifetime domain.
 *
 * A full queue applies synchronous backpressure. Call drain() before releasing
 * callback code or its owner (in particular before module unload); destruction
 * also drains. Callbacks reclaim storage in task context and must not invoke
 * queue operations. Do not enqueue while holding locks needed by a callback.
 */
class EXPORTED_PUBLIC RcuRetireQueue {
 public:
  static constexpr size_t Capacity = 32;
  using Reclaim = void (*)(void*);

  RcuRetireQueue() = default;
  ~RcuRetireQueue();
  void retire(void* object, Reclaim reclaim);
  void drain();

 private:
  NOT_COPYABLE_OR_ASSIGNABLE(RcuRetireQueue);
  void drainLocked();
  Mutex m_Lock;
  struct Entry {
    void* object;
    Reclaim reclaim;
  } m_Entries[Capacity];
  size_t m_Count = 0;
};

#endif
