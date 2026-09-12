/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/syscallError.h"

#include "FatFilesystem.h"

bool FatFilesystem::writeCachedPages(FatFile::State& state, const Cache::WritebackPage* pages,
                                     size_t count) {
  LockGuard<Mutex> guard(m_FileMutationLock);
  if (state.retiring)
    return true;
  if (m_bReadOnly || !m_pDisk || !m_BlockSize)
    return false;
  // Size is sampled inside the allocation lock: delayed writeback must never
  // grow a file that was truncated after these pages were queued.
  FatFile file(state);
  Disk::WriteBuffer requests[Disk::MaxWriteBuffers];
  size_t pending = 0;
  bool succeeded = true;
  auto drain = [&] {
    if (pending)
      succeeded = m_pDisk->writeFromBatch(requests, pending) && succeeded;
    pending = 0;
  };
  for (size_t i = 0; i < count; ++i) {
    const uint64_t location = pages[i].key;
    if (location >= state.size)
      continue;
    size_t remaining =
        pedigree_std::min(size_t(file.getBlockSize()), size_t(state.size - location));
    uint32_t cluster = fileClusterAt(&file, location / m_BlockSize);
    size_t offset = location % m_BlockSize;
    uintptr_t source = pages[i].location;
    while (remaining) {
      if (cluster < 2 || cluster >= m_ClusterCount + 2 || isEof(cluster)) {
        succeeded = false;
        break;
      }
      const size_t length = pedigree_std::min(size_t(m_BlockSize - offset), remaining);
      requests[pending++] = {
          uint64_t(getSectorNumber(cluster)) * m_Superblock.BPB_BytsPerSec + offset,
          reinterpret_cast<void*>(source), length, false};
      if (pending == Disk::MaxWriteBuffers)
        drain();
      source += length;
      remaining -= length;
      offset = 0;
      if (remaining)
        cluster = getClusterEntry(cluster);
    }
  }
  drain();
  const bool durable = m_pDisk->syncData();
  if (!succeeded || !durable) {
    SYSCALL_ERROR(IoError);
    return false;
  }
  return true;
}
