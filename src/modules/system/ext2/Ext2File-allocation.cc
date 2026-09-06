/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/syscallError.h"

#include "Ext2File.h"
#include "Ext2Filesystem.h"

bool Ext2File::allocateFileRange(size_t offset, size_t length) {
  LockGuard<Mutex> guard(m_State->writebackLock);
  if (!m_State->allocationValid) {
    SYSCALL_ERROR(IoError);
    return false;
  }
  {
#if THREADS || defined(STANDALONE_MUTEXES)
    LockGuard<Mutex> allocation(m_pExt2Fs->m_WriteLock);
#endif
    // The allocator's legacy zero result conflates bitmap I/O failure and full
    // storage. Preload the bitmaps so a failed read retains its specific error.
    for (size_t group = 0; group < m_pExt2Fs->m_nGroupDescriptors; ++group) {
      if (!m_pExt2Fs->ensureFreeBlockBitmapLoaded(group))
        return false;
    }
  }
  const size_t blockSize = m_pExt2Fs->m_BlockSize;
  const size_t end = offset + length;
  const size_t count = end / blockSize + (end % blockSize != 0);
  if (count > m_Blocks.count()) {
    if (!m_Blocks.tryReserve(count)) {
      SYSCALL_ERROR(OutOfMemory);
      return false;
    }
    // Preserve holes before the requested range. ensureWritableRange allocates
    // and zeroes data before attaching each direct or indirect mapping.
    while (m_Blocks.count() < count)
      m_Blocks.pushBack(0);
  }
  const bool success = ensureWritableRange(offset, length);
  if (!useFillCache()) {
    // A native-block read may still have indexed the shared sparse zero block.
    LockGuard<Mutex> index(cacheState().indexLock);
    cacheState().data.clear();
  }
  return success;
}
