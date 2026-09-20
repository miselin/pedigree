/* Copyright (c) 2026, Pedigree Developers. */

#ifndef KERNEL_PROCESSOR_HOSTED_MAPPINGINDEX_H
#define KERNEL_PROCESSOR_HOSTED_MAPPINGINDEX_H

#include <stddef.h>
#include <stdint.h>

class HostedMappingIndex {
 public:
  static constexpr size_t Missing = ~size_t(0);

  HostedMappingIndex();
  ~HostedMappingIndex();

  HostedMappingIndex(const HostedMappingIndex&) = delete;
  HostedMappingIndex& operator=(const HostedMappingIndex&) = delete;
  HostedMappingIndex(HostedMappingIndex&&) = delete;
  HostedMappingIndex& operator=(HostedMappingIndex&&) = delete;

  size_t lookup(uintptr_t page) const;
  bool reserveForInsert();
  void insert(uintptr_t page, size_t slot);
  void erase(uintptr_t page);
  void clear();

 private:
  enum State : uint8_t { Empty, Occupied, Deleted };
  struct Entry {
    uintptr_t page;
    size_t slot;
    State state;
  };

  static size_t hash(uintptr_t page);

  Entry* m_Entries;
  size_t m_Capacity;
  size_t m_Live;
  size_t m_Used;
};

#endif
