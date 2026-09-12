/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/syscallError.h"

#include "FatFile.h"
#include "FatFilesystem.h"

void FatFile::truncate() {
  resize(0);
}

void FatFile::extend(size_t newSize) {
  FatFilesystem* filesystem = static_cast<FatFilesystem*>(m_pFilesystem);
  const size_t oldSize = getSize();
  filesystem->extend(this, newSize);
  const size_t size = getSize();
  const size_t pageSize = getBlockSize();
  const size_t within = oldSize % pageSize;
  if (size > oldSize && within) {
    const size_t pageOffset = oldSize - within;
    const uintptr_t page = cacheState().fill.lookup(pageOffset);
    if (page) {
      // Shared mappings may have dirtied padding beyond the previous EOF.
      const size_t amount = size - oldSize < pageSize - within ? size - oldSize : pageSize - within;
      ByteSet(reinterpret_cast<void*>(page + within), 0, amount);
      cacheState().fill.markDirty(pageOffset);
      cacheState().fill.release(pageOffset);
    }
  }
}

void FatFile::extend(size_t newSize, uint64_t, uint64_t) {
  extend(newSize);
}

bool FatFile::prepareShrink(const ShrinkContext& context, UniquePointer<PreparedShrink>& prepared) {
  return static_cast<FatFilesystem*>(m_pFilesystem)
      ->prepareFileShrink(this, context.newSize, prepared);
}

bool FatFile::resizeFile(size_t size) {
  if (size > UINT32_MAX) {
    SYSCALL_ERROR(FileTooLarge);
    return false;
  }
  if (size == getSize())
    return true;
  if (size < getSize()) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  static_cast<FatFilesystem*>(m_pFilesystem)->extend(this, size);
  return getSize() == size;
}
