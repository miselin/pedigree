/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/syscallError.h"

#include "File.h"
#include "Filesystem.h"

bool File::allocateRange(size_t offset, size_t length, bool keepSize) {
  TerminationDeferral lifetime;
  if (!supportsRegularFileOperations()) {
    syscallError(isDirectory() ? Error::IsADirectory : Error::OperationNotSupported);
    return false;
  }
  if (!length) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  const uint64_t maximum = maximumFileSize();
  if (offset > maximum || length > maximum - offset || length > ~size_t(0) - offset) {
    SYSCALL_ERROR(FileTooLarge);
    return false;
  }
  LockGuard<Mutex> writer(writeSerializationLock());
  LockGuard<Mutex> data(dataMutationLock());
  if (m_pFilesystem && m_pFilesystem->isReadOnly()) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return false;
  }
  if (cacheState().executableMappings) {
    SYSCALL_ERROR(TextFileBusy);
    return false;
  }
  const size_t oldSize = getSize();
  const size_t end = offset + length;
  const size_t newSize = keepSize || end < oldSize ? oldSize : end;
  if (!allowResize(oldSize, newSize) || !allocateFileRange(offset, length))
    return false;
  // Reservation precedes EOF publication. A failed reservation may keep blocks,
  // but cannot expose uninitialised bytes or promise storage that was not acquired.
  if (newSize > oldSize) {
    if (!resizeFile(newSize))
      return false;
    if (useFillCache()) {
      const size_t pageSize = PhysicalMemoryManager::getPageSize();
      const size_t within = oldSize % pageSize;
      if (within) {
        const size_t pageOffset = oldSize - within;
        const uintptr_t page = cacheState().fill.lookup(pageOffset);
        if (page) {
          const size_t amount =
              newSize - oldSize < pageSize - within ? newSize - oldSize : pageSize - within;
          ByteSet(reinterpret_cast<void*>(page + within), 0, amount);
          cacheState().fill.markDirty(pageOffset);
          cacheState().fill.release(pageOffset);
        }
      }
    }
  }
  Attributes attributes;
  attributes.modified = attributes.changed = Time::getTime();
  updateAttributes(attributes, ModifyTime | ChangeTime);
  publishEvent(FileEvents::Modify);
  return true;
}

bool File::allocateFileRange(size_t, size_t) {
  SYSCALL_ERROR(OperationNotSupported);
  return false;
}
