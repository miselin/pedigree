/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_CORE_SLAMBITMAP_H
#define PEDIGREE_KERNEL_CORE_SLAMBITMAP_H

#include <stddef.h>
#include <stdint.h>

/** Bitmap used by SLAM to track virtual heap pages. */
class SlamBitmap {
 public:
  struct Entry {
    uint64_t reserved;
    uint64_t mapped;
    uint64_t ready;
  };

  SlamBitmap(Entry* entries, size_t entryCount, size_t pageCount)
      : m_Entries(entries), m_EntryCount(entryCount), m_PageCount(pageCount) {}

  size_t findFreeRun(size_t pageCount) const;
  void reserve(size_t firstPage, size_t pageCount);
  void release(size_t firstPage, size_t pageCount);
  void setMapped(size_t page);
  void clearMappedAndReady(size_t page);
  bool isReserved(size_t page) const;
  bool isMapped(size_t page) const;
  uint64_t reservedEntry(size_t entry) const { return m_Entries[entry].reserved; }
  Entry& entry(size_t index) { return m_Entries[index]; }
  const Entry& entry(size_t index) const { return m_Entries[index]; }

 private:
  Entry* m_Entries;
  size_t m_EntryCount;
  size_t m_PageCount;
};

#endif
