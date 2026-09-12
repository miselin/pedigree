/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/syscallError.h"

#include "FatFilesystem.h"

class FatFilesystem::ShrinkPlan final : public File::PreparedShrink {
 public:
  ShrinkPlan(FatFilesystem& filesystem, FatFile& file, size_t size)
      : m_Filesystem(filesystem), m_File(file), m_Size(size) {}

  void commit() override {
    LockGuard<Mutex> guard(m_Filesystem.m_FileMutationLock);
    m_Filesystem.publishSize(&m_File, m_Size);
    m_File.m_MetadataDirty = true;
    // Reclaim only after the smaller directory size reaches durable storage.
    m_File.m_TrimPending = true;
  }

 private:
  FatFilesystem& m_Filesystem;
  FatFile& m_File;
  size_t m_Size;
};

bool FatFilesystem::prepareFileShrink(FatFile* file, size_t size,
                                      UniquePointer<File::PreparedShrink>& prepared) {
  LockGuard<Mutex> guard(m_FileMutationLock);
  if (m_bReadOnly) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return false;
  }
  uint32_t count = 0, last = 0;
  if (!m_BlockSize || size >= file->getSize() || !chainExtent(file, count, last) ||
      uint64_t(count) * m_BlockSize < file->getSize()) {
    SYSCALL_ERROR(IoError);
    return false;
  }
  if (!isNodeUnlinked(file)) {
    Dir* entry = getDirectoryEntry(file->getDirCluster(), file->getDirOffset());
    if (!entry) {
      SYSCALL_ERROR(IoError);
      return false;
    }
    delete entry;
  }
  auto plan = UniquePointer<File::PreparedShrink>::adopt(new ShrinkPlan(*this, *file, size));
  if (!plan) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  prepared = pedigree_std::move(plan);
  return true;
}

bool FatFilesystem::trimFileAllocation(FatFile* file) {
  if (!file->m_TrimPending)
    return true;
  bool succeeded = true;
  while (file->m_RetiredClusters.count()) {
    const uint32_t cluster = file->m_RetiredClusters[file->m_RetiredClusters.count() - 1];
    const bool staged = setClusterEntry(cluster, 0, true, false);
    if (!staged && getClusterEntry(cluster) != 0) {
      SYSCALL_ERROR(IoError);
      return false;
    }
    // A staged free may be reused after the allocator flushes the FAT. Never
    // keep its number in a journal that could free that future allocation.
    file->m_RetiredClusters.popBack();
    succeeded = staged && succeeded;
  }
  if (!syncFat() || !succeeded) {
    SYSCALL_ERROR(IoError);
    return false;
  }
  uint32_t count = 0, last = 0;
  if (!chainExtent(file, count, last)) {
    SYSCALL_ERROR(IoError);
    return false;
  }
  const size_t size = file->getSize();
  const size_t keep = size / m_BlockSize + (size % m_BlockSize != 0);
  if (count <= keep) {
    file->m_TrimPending = false;
    return true;
  }
  uint32_t retained = file->getInode();
  for (size_t i = 1; i < keep; ++i)
    retained = getClusterEntry(retained);
  const uint32_t retired = keep ? getClusterEntry(retained) : retained;
  if (!file->m_RetiredClusters.tryReserve(count - keep)) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  uint32_t cluster = retired;
  for (size_t i = keep; i < count; ++i) {
    file->m_RetiredClusters.pushBack(cluster);
    cluster = getClusterEntry(cluster);
  }
  if (!keep) {
    // The directory entry already has a zero start cluster. The retirement
    // journal owns its former chain until each free is staged for retry.
    file->setInode(0);
  } else if (!setClusterEntry(retained, eofValue())) {
    // A failed mirror write cannot surrender ownership of the detached suffix.
    setClusterEntry(retained, retired);
    file->m_RetiredClusters.clear();
    SYSCALL_ERROR(IoError);
    return false;
  }
  return trimFileAllocation(file);
}

void FatFilesystem::truncate(File* file) {
  file->resize(0);
}

bool FatFilesystem::truncateFile(File* file) {
  return file->resize(0);
}

void FatFilesystem::extend(File* file, size_t size) {
  LockGuard<Mutex> guard(m_FileMutationLock);
  if (file->getSize() >= size)
    return;
  if (m_bReadOnly) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return;
  }
  if (size > UINT32_MAX) {
    SYSCALL_ERROR(FileTooLarge);
    return;
  }
  if (!syncFat() || !ensureCapacity(file, size) || !zeroRange(file, file->getSize(), size) ||
      !updateFileMetadata(file, size)) {
    SYSCALL_ERROR(IoError);
    return;
  }
  publishSize(file, size);
}
