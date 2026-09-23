/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_KERNEL_PROCESS_RCUREADSTATE_H
#define PEDIGREE_KERNEL_PROCESS_RCUREADSTATE_H

#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/types.h"

/** One execution CPU's readers; atomic nesting also covers NMI interruption. */
class alignas(64) RcuReadState {
 public:
  using Snapshot = uint64_t;

  void enter() {
    Snapshot state = __atomic_load_n(&m_State, __ATOMIC_RELAXED);
    do {
      if ((state & DepthMask) == DepthMask) {
        panic("RCU reader nesting overflow.");
      }
    } while (!__atomic_compare_exchange_n(&m_State, &state, state + 1, true, __ATOMIC_SEQ_CST,
                                          __ATOMIC_RELAXED));
  }

  void leave() {
    Snapshot state = __atomic_load_n(&m_State, __ATOMIC_RELAXED);
    Snapshot next;
    do {
      const Snapshot depth = state & DepthMask;
      if (!depth) {
        panic("RCU reader nesting underflow.");
      }
      // A generation advances only after the outermost reader has finished.
      next = depth == 1 ? state + Generation - 1 : state - 1;
    } while (!__atomic_compare_exchange_n(&m_State, &state, next, true, __ATOMIC_RELEASE,
                                          __ATOMIC_RELAXED));
  }

  Snapshot snapshot() const {
    return __atomic_load_n(&m_State, __ATOMIC_SEQ_CST);
  }

  bool passed(Snapshot before) const {
    if (!(before & DepthMask)) {
      return true;
    }
    const Snapshot now = __atomic_load_n(&m_State, __ATOMIC_ACQUIRE);
    return !(now & DepthMask) || (now & ~DepthMask) != (before & ~DepthMask);
  }

  bool active() const {
    return (__atomic_load_n(&m_State, __ATOMIC_RELAXED) & DepthMask) != 0;
  }

 private:
  static constexpr Snapshot Generation = Snapshot{1} << 32;
  static constexpr Snapshot DepthMask = Generation - 1;
  static_assert(__atomic_always_lock_free(sizeof(Snapshot), nullptr));
  alignas(sizeof(Snapshot)) Snapshot m_State = 0;
};

#endif
