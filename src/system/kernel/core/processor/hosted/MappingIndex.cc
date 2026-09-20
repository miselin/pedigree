/* Copyright (c) 2026, Pedigree Developers. */

#include "MappingIndex.h"
#include "pedigree/kernel/utilities/assert.h"

void* __libc_malloc(size_t);
void __libc_free(void*);

HostedMappingIndex::HostedMappingIndex()
    : m_Entries(nullptr), m_Capacity(0), m_Live(0), m_Used(0) {}

HostedMappingIndex::~HostedMappingIndex() {
  if (m_Entries)
    __libc_free(m_Entries);
}

size_t HostedMappingIndex::hash(uintptr_t page) {
  // Mix the address bits so page alignment does not cluster adjacent mappings.
  uint64_t value = page;
  value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
  return static_cast<size_t>(value ^ (value >> 31));
}

size_t HostedMappingIndex::lookup(uintptr_t page) const {
  if (!m_Capacity)
    return Missing;

  size_t index = hash(page) & (m_Capacity - 1);
  for (size_t probes = 0; probes < m_Capacity; ++probes) {
    const Entry& entry = m_Entries[index];
    if (entry.state == Empty)
      return Missing;
    if (entry.state == Occupied && entry.page == page)
      return entry.slot;
    index = (index + 1) & (m_Capacity - 1);
  }
  return Missing;
}

bool HostedMappingIndex::reserveForInsert() {
  if (m_Used < m_Capacity / 2)
    return true;

  size_t capacity = m_Capacity ? m_Capacity : 16;
  // Rebuild at the same capacity when tombstones, rather than live entries,
  // account for the load. This bounds probe lengths without retaining growth.
  if (m_Live >= capacity / 2) {
    if (capacity > Missing / 2)
      return false;
    capacity *= 2;
  }
  if (capacity > Missing / sizeof(Entry))
    return false;

  // The caller holds the address-space lock; SLAM allocation would re-enter it.
  auto* entries = static_cast<Entry*>(__libc_malloc(capacity * sizeof(Entry)));
  if (!entries)
    return false;
  for (size_t i = 0; i < capacity; ++i)
    entries[i].state = Empty;
  for (size_t i = 0; i < m_Capacity; ++i) {
    if (m_Entries[i].state != Occupied)
      continue;
    size_t index = hash(m_Entries[i].page) & (capacity - 1);
    while (entries[index].state != Empty)
      index = (index + 1) & (capacity - 1);
    entries[index] = m_Entries[i];
  }

  Entry* previous = m_Entries;
  m_Entries = entries;
  m_Capacity = capacity;
  m_Used = m_Live;
  if (previous)
    __libc_free(previous);
  return true;
}

void HostedMappingIndex::insert(uintptr_t page, size_t slot) {
  assert(slot != Missing);
  assert(m_Capacity && m_Used < m_Capacity / 2);
  if (!m_Capacity || slot == Missing)
    return;

  size_t index = hash(page) & (m_Capacity - 1);
  size_t deleted = Missing;
  for (size_t probes = 0; probes < m_Capacity; ++probes) {
    Entry& entry = m_Entries[index];
    if (entry.state == Occupied && entry.page == page) {
      assert(false);
      return;
    }
    if (entry.state == Deleted && deleted == Missing)
      deleted = index;
    if (entry.state == Empty) {
      const size_t target = deleted == Missing ? index : deleted;
      m_Entries[target] = {page, slot, Occupied};
      ++m_Live;
      if (deleted == Missing)
        ++m_Used;
      return;
    }
    index = (index + 1) & (m_Capacity - 1);
  }
  assert(false);
}

void HostedMappingIndex::erase(uintptr_t page) {
  if (!m_Capacity)
    return;

  size_t index = hash(page) & (m_Capacity - 1);
  for (size_t probes = 0; probes < m_Capacity; ++probes) {
    Entry& entry = m_Entries[index];
    if (entry.state == Empty)
      return;
    if (entry.state == Occupied && entry.page == page) {
      entry.state = Deleted;
      --m_Live;
      return;
    }
    index = (index + 1) & (m_Capacity - 1);
  }
}

void HostedMappingIndex::clear() {
  for (size_t i = 0; i < m_Capacity; ++i)
    m_Entries[i].state = Empty;
  m_Live = 0;
  m_Used = 0;
}
