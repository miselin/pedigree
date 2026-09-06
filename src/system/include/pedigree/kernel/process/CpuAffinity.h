/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_PROCESS_CPUAFFINITY_H
#define PEDIGREE_KERNEL_PROCESS_CPUAFFINITY_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"

class Thread;

struct CpuAffinityMask {
  static constexpr size_t MaximumCpus = 1024;
  static constexpr size_t WordBits = sizeof(unsigned long) * 8;
  static constexpr size_t WordCount = MaximumCpus / WordBits;
  static constexpr size_t ByteCount = MaximumCpus / 8;
  unsigned long words[WordCount] = {};

  void* data() {
    return words;
  }
  const void* data() const {
    return words;
  }
  bool contains(size_t cpu) const {
    return cpu < MaximumCpus && (words[cpu / WordBits] & (1UL << (cpu % WordBits)));
  }
  void set(size_t cpu) {
    if (cpu < MaximumCpus)
      words[cpu / WordBits] |= 1UL << (cpu % WordBits);
  }
  bool empty() const {
    for (size_t i = 0; i < WordCount; ++i)
      if (words[i])
        return false;
    return true;
  }
  void intersect(const CpuAffinityMask& other) {
    for (size_t i = 0; i < WordCount; ++i)
      words[i] &= other.words[i];
  }
};

struct EXPORTED_PUBLIC ThreadPlacement {
  CpuAffinityMask allowed;
  bool migratable = false;

  static ThreadPlacement initialUser();
  static ThreadPlacement inherit(Thread& creator);
};

enum class AffinityResult { Success, Invalid, Terminal, Pinned, Busy, Unsupported };

#endif
