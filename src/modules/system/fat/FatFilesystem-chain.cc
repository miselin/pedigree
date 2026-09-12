/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/syscallError.h"

#include "FatFilesystem.h"

bool FatFilesystem::chainExtent(File* file, uint32_t& count, uint32_t& last) {
  count = 0;
  last = 0;
  FatFile::State* state = nullptr;
  const uint64_t revision = __atomic_load_n(&m_ChainRevision, __ATOMIC_ACQUIRE);
  const uintptr_t inode = file->getInode();
  if (!file->isDirectory() && !file->isSymlink()) {
    state = static_cast<FatFile*>(file)->m_State;
    if (state->chainRevision == revision &&
        (state->clusters.count() ? state->clusters[0] == inode : inode == 0)) {
      count = static_cast<uint32_t>(state->clusters.count());
      last = count ? state->clusters[count - 1] : 0;
      return true;
    }
    state->chainRevision = 0;
    state->clusters.clear();
  }
  if (inode > UINT32_MAX)
    return false;

  uint32_t cluster = static_cast<uint32_t>(inode);
  while (cluster) {
    if (cluster < 2 || uint64_t(cluster) >= uint64_t(m_ClusterCount) + 2 || isEof(cluster) ||
        count >= m_ClusterCount)
      return false;
    const uint32_t next = getClusterEntry(cluster);
    if (!next)
      return false;
    if (state) {
      if (!state->clusters.tryReserve(size_t(count) + 1)) {
        SYSCALL_ERROR(OutOfMemory);
        return false;
      }
      state->clusters.pushBack(cluster);
    }
    ++count;
    last = cluster;
    if (isEof(next))
      break;
    cluster = next;
  }

  // A concurrent allocation must leave this snapshot stale, never stamp it current.
  if (state)
    state->chainRevision = revision;
  return true;
}

uint32_t FatFilesystem::fileClusterAt(File* file, size_t index) {
  uint32_t count = 0, last = 0;
  if (!chainExtent(file, count, last) || index >= count)
    return 0;
  if (!file->isDirectory() && !file->isSymlink())
    return static_cast<FatFile*>(file)->m_State->clusters[index];

  uint32_t cluster = static_cast<uint32_t>(file->getInode());
  for (size_t position = 0; position < index; ++position) {
    cluster = getClusterEntry(cluster);
    if (cluster < 2 || uint64_t(cluster) >= uint64_t(m_ClusterCount) + 2 || isEof(cluster))
      return 0;
  }
  return cluster;
}
