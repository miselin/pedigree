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
  SlamBitmap() : /*m_Entries(nullptr), */ m_EntryCount(0), m_PageCount(0) {}
  void useMemory(void* memory, size_t entryCount, size_t pageCount);

  size_t findFreeRun(size_t pageCount) const;
  void reserve(size_t firstPage, size_t pageCount);
  void release(size_t firstPage, size_t pageCount);
  void setMapped(size_t page);
  void clearMappedAndReady(size_t page);
  bool isReserved(size_t page) const;
  bool isMapped(size_t page) const;
  bool isReady(size_t page) const;
  void setReady(size_t page);
  uint64_t reservedBits(size_t entry) const;
  uintptr_t metadataAddress(size_t entry) const;

 private:
  // struct Entry {
  //   uint64_t reserved;
  //   uint64_t mapped;
  //   uint64_t ready;
  // };
  // Entry* m_Entries;

  uint64_t* m_reserved = nullptr;
  uint64_t* m_mapped = nullptr;
  uint64_t* m_ready = nullptr;

  size_t m_EntryCount;
  size_t m_PageCount;
  size_t m_FirstFreeEntry = 0;
};

#endif
