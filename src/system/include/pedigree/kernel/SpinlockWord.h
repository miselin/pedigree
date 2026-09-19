#ifndef KERNEL_SPINLOCKWORD_H
#define KERNEL_SPINLOCKWORD_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"

/** Atomic exclusion only; IRQ state, recursion and waiting belong to the caller. */
class SpinlockWord {
 public:
  explicit constexpr SpinlockWord(bool locked = false) : m_Value(locked ? 0 : 1) {}

  ALWAYS_INLINE bool tryAcquire() {
    processor_register_t expected = 1;
    return __atomic_compare_exchange_n(&m_Value, &expected, 0, false, __ATOMIC_ACQUIRE,
                                       __ATOMIC_RELAXED);
  }

  ALWAYS_INLINE bool acquired() const {
    return __atomic_load_n(&m_Value, __ATOMIC_RELAXED) == 0;
  }

  ALWAYS_INLINE void release() {
    __atomic_store_n(&m_Value, 1, __ATOMIC_RELEASE);
  }

  ALWAYS_INLINE bool releaseChecked() {
    processor_register_t expected = 0;
    return __atomic_compare_exchange_n(&m_Value, &expected, 1, false, __ATOMIC_RELEASE,
                                       __ATOMIC_RELAXED);
  }

 private:
  friend class Spinlock;
  NOT_COPYABLE_OR_ASSIGNABLE(SpinlockWord);

  // Context-switch assembly publishes 1 through this machine-word pointer.
  volatile processor_register_t m_Value;
};

#endif
