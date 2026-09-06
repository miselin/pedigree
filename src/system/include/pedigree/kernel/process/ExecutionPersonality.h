/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_EXECUTION_PERSONALITY_H
#define PEDIGREE_EXECUTION_PERSONALITY_H

#include "pedigree/kernel/processor/types.h"

class ExecutionPersonality {
 public:
  static constexpr uint32_t Native = 0;
  static constexpr uint32_t Linux32 = 8;
  static constexpr uint32_t Query = 0xffffffffU;

  uint32_t value() const {
    return __atomic_load_n(&m_Value, __ATOMIC_ACQUIRE);
  }
  bool select(uint32_t requested, uint32_t& previous) {
    if (requested == Query) {
      previous = value();
      return true;
    }
    if (requested != Native && requested != Linux32)
      return false;
    previous = __atomic_exchange_n(&m_Value, requested, __ATOMIC_ACQ_REL);
    return true;
  }
  void inherit(const ExecutionPersonality& source) {
    __atomic_store_n(&m_Value, source.value(), __ATOMIC_RELEASE);
  }

 private:
  uint32_t m_Value = Native;
};

#endif
