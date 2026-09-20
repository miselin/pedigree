/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */
#ifndef PEDIGREE_KERNEL_PROCESS_PERCPUTIMEACCOUNTING_H
#define PEDIGREE_KERNEL_PROCESS_PERCPUTIMEACCOUNTING_H

#include "pedigree/kernel/process/DeferredTimeAccounting.h"
#include "pedigree/kernel/utilities/new"

class PerCpuTimeAccounting {
 public:
  explicit PerCpuTimeAccounting(size_t count) : m_Storage(nullptr), m_Slots(nullptr), m_Count(0) {
    if (!count || count > (~size_t(0) - (SlotSize - 1)) / SlotSize) {
      return;
    }
    const size_t bytes = count * SlotSize + SlotSize - 1;
#if UTILITY_LINUX
    m_Storage = new (std::nothrow) uint8_t[bytes];
#else
    m_Storage = new uint8_t[bytes];
#endif
    if (!m_Storage) {
      return;
    }
    // The kernel's over-aligned operator new does not implement alignment.
    const uintptr_t aligned =
        (reinterpret_cast<uintptr_t>(m_Storage) + SlotSize - 1) & ~uintptr_t(SlotSize - 1);
    m_Slots = reinterpret_cast<uint8_t*>(aligned);
    m_Count = count;
    for (size_t cpu = 0; cpu < m_Count; ++cpu) {
      new (slot(cpu)) Slot{};
    }
  }

  ~PerCpuTimeAccounting() {
    delete[] m_Storage;
  }

  /** Kernel callers keep IRQs masked and supply their current dense CPU index. */
  ALWAYS_INLINE bool add(CpuTimeMode mode, Time::Timestamp elapsed, size_t cpu) {
    if (cpu >= m_Count) {
      return false;
    }
    Slot* current = slot(cpu);
    Time::Timestamp* value = mode == CpuTimeMode::User ? &current->user : &current->kernel;
#if X64 && !HOSTED && !UTILITY_LINUX
    // Only this CPU writes its slot. A single instruction also excludes an
    // NMI interleaving the read and write of a split load/add/store sequence.
    asm volatile("addq %1, %0" : "+m"(*value) : "r"(elapsed) : "cc");
#else
    __atomic_fetch_add(value, elapsed, __ATOMIC_RELAXED);
#endif
    return true;
  }

  Time::Timestamp total(CpuTimeMode mode) const {
    Time::Timestamp result = 0;
    for (size_t cpu = 0; cpu < m_Count; ++cpu) {
      const Slot* current = slot(cpu);
      const Time::Timestamp* value = mode == CpuTimeMode::User ? &current->user : &current->kernel;
      result += __atomic_load_n(value, __ATOMIC_ACQUIRE);
    }
    return result;
  }

 private:
  static constexpr size_t SlotSize = 64;
  struct alignas(SlotSize) Slot {
    Time::Timestamp user;
    Time::Timestamp kernel;
    uint8_t padding[SlotSize - 2 * sizeof(Time::Timestamp)];
  };
  static_assert(sizeof(Slot) == SlotSize && alignof(Slot) == SlotSize,
                "CPU accounting slots must occupy separate aligned cache lines");

  ALWAYS_INLINE Slot* slot(size_t cpu) const {
    return reinterpret_cast<Slot*>(m_Slots + cpu * SlotSize);
  }

  uint8_t* m_Storage;
  uint8_t* m_Slots;
  size_t m_Count;

  PerCpuTimeAccounting(const PerCpuTimeAccounting&) = delete;
  PerCpuTimeAccounting& operator=(const PerCpuTimeAccounting&) = delete;
};

#endif
