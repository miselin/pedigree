/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/syscallError.h"

#include "RamFs.h"

bool RamFile::allocateFileRange(size_t offset, size_t length) {
  if (!canWrite()) {
    SYSCALL_ERROR(PermissionDenied);
    return false;
  }
  LockGuard<Mutex> guard(m_FileBlocksLock);
  const size_t blockSize = getBlockSize();
  const size_t last = (offset + length - 1) / blockSize;
  for (size_t index = offset / blockSize; index <= last; ++index) {
    const size_t location = index * blockSize;
    uintptr_t page = m_FileBlocks.lookup(location);
    if (page) {
      m_FileBlocks.release(location);
      continue;
    }
    if (!m_BlockOffsets.tryReserve(m_BlockOffsets.count() + 1)) {
      SYSCALL_ERROR(OutOfMemory);
      return false;
    }
    page = m_FileBlocks.insert(location);
    if (!page) {
      SYSCALL_ERROR(OutOfMemory);
      return false;
    }
    ByteSet(reinterpret_cast<void*>(page), 0, blockSize);
    m_BlockOffsets.pushBack(location);
    m_FileBlocks.markNoLongerEditing(location);
    // Its base reference owns actual memory until truncation or file destruction.
  }
  return true;
}
