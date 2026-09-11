/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "FatFile.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/syscallError.h"

#include "FatFilesystem.h"

namespace {
class FatShrinkPlan final : public File::PreparedShrink {
 public:
  void commit() override {}
};
}  // namespace

FatFile::FatFile(String name, Time::Timestamp accessedTime, Time::Timestamp modifiedTime,
                 Time::Timestamp creationTime, uintptr_t inode, class Filesystem* pFs, size_t size,
                 uint32_t dirClus, uint32_t dirOffset, File* pParent)
    : File(name, accessedTime, modifiedTime, creationTime, inode, pFs, size, pParent),
      m_DirClus(dirClus),
      m_DirOffset(dirOffset),
      m_MetadataDirty(false) {
  cacheState().fill.setDirtyTracking(Cache::DirtyTracking::Explicit);
  cacheState().fill.setCallback(checkedWriteCallback, this);

  // No permissions on FAT - set all to RWX.
  setPermissions(FILE_UR | FILE_UW | FILE_UX | FILE_GR | FILE_GW | FILE_GX | FILE_OR | FILE_OW |
                 FILE_OX);
}

FatFile::~FatFile() {
  cacheState().fill.shutdown();
}

uintptr_t FatFile::readBlock(uint64_t location) {
  LockGuard<Mutex> guard(cacheState().fillLock);
  location -= location % getBlockSize();

  uintptr_t buffer = cacheState().fill.lookup(location);
  if (buffer) {
    return buffer;
  }

  bool didExist = false;
  buffer = cacheState().fill.insert(location, &didExist);
  if (!buffer) {
    return 0;
  }

  if (!didExist) {
    if (!readPage(location, buffer)) {
      if (!cacheState().fill.discardEditing(location)) {
        WARNING("FatFile::readBlock could not discard a failed fill at " << location);
      }
      return 0;
    }
    cacheState().fill.markNoLongerEditing(location);
  }

  return cacheState().fill.lookup(location);
}

bool FatFile::useFillCache() const {
  EMIT_IF(VFS_NOMMU) {
#if defined(PEDIGREE_BUILDUTILS)
    return m_bForceFillCache;
#else
    return false;
#endif
  }
  else {
    return true;
  }
}

bool FatFile::readPage(uint64_t location, uintptr_t destination) {
  const size_t pageSize = getBlockSize();
  ByteSet(reinterpret_cast<void*>(destination), 0, pageSize);
  const size_t size = getSize();
  if (location >= size)
    return false;
  const size_t expected = size - location < pageSize ? size - location : pageSize;
  FatFilesystem* filesystem = static_cast<FatFilesystem*>(m_pFilesystem);
  return filesystem->read(this, location, expected, destination) == expected;
}

void FatFile::writeBlock(uint64_t location, uintptr_t addr) {
  // The producer page remains authoritative until checked writeback completes.
  cacheState().fill.markDirty(location - location % getBlockSize());
}

bool FatFile::checkedWriteCallback(CacheConstants::CallbackCause cause, uintptr_t location,
                                   uintptr_t page, void* meta) {
  FatFile* file = static_cast<FatFile*>(meta);
  if (cause != CacheConstants::WriteBack)
    return File::fillCacheCallback(cause, location, page, static_cast<File*>(file));

  const size_t size = file->getSize();
  if (location >= size)
    return true;
  const size_t remaining = size - location;
  const size_t length = remaining < file->getBlockSize() ? remaining : file->getBlockSize();
  FatFilesystem* filesystem = static_cast<FatFilesystem*>(file->m_pFilesystem);
  return filesystem->write(file, location, length, page) == length;
}

bool FatFile::sync() {
  const bool dataSucceeded = File::sync();
  FatFilesystem* filesystem = static_cast<FatFilesystem*>(m_pFilesystem);
  return filesystem->syncFileMetadata(this) && dataSucceeded;
}

bool FatFile::sync(size_t offset, bool async) {
  offset -= offset % getBlockSize();
  const bool dataSucceeded = cacheState().fill.sync(offset, async);
  FatFilesystem* filesystem = static_cast<FatFilesystem*>(m_pFilesystem);
  return filesystem->syncFileMetadata(this) && dataSucceeded;
}

File::Attributes FatFile::getAttributes() const {
  FatFile* file = const_cast<FatFile*>(this);
  LockGuard<Mutex> dataGuard(file->dataMutationLock());
  FatFilesystem* filesystem = static_cast<FatFilesystem*>(m_pFilesystem);
  LockGuard<Mutex> guard(filesystem->m_FileMutationLock);
  Attributes attributes = File::getAttributes();
  attributes.blocks = filesystem->allocatedBlocks(file);
  return attributes;
}

bool FatFile::pinBlock(uint64_t location) {
  return cacheState().fill.pin(location - location % getBlockSize());
}

void FatFile::unpinBlock(uint64_t location) {
  cacheState().fill.release(location - location % getBlockSize());
}

void FatFile::extend(size_t newSize) {
  FatFilesystem* pFs = static_cast<FatFilesystem*>(m_pFilesystem);

  const size_t oldSize = getSize();
  pFs->extend(this, newSize);
  const size_t size = getSize();
  const size_t pageSize = getBlockSize();
  const size_t within = oldSize % pageSize;
  if (size > oldSize && within) {
    const size_t pageOffset = oldSize - within;
    const uintptr_t page = cacheState().fill.lookup(pageOffset);
    if (page) {
      // Writable mappings can have changed padding beyond the previous EOF.
      const size_t amount = size - oldSize < pageSize - within ? size - oldSize : pageSize - within;
      ByteSet(reinterpret_cast<void*>(page + within), 0, amount);
      cacheState().fill.markDirty(pageOffset);
      cacheState().fill.release(pageOffset);
    }
  }
}

void FatFile::extend(size_t newSize, uint64_t location, uint64_t size) {
  // not using the hints at all
  extend(newSize);
}

bool FatFile::prepareShrink(const ShrinkContext& context,
                            UniquePointer<PreparedShrink>& prepared) {
  if (context.newSize != 0) {
    SYSCALL_ERROR(OperationNotSupported);
    return false;
  }

  FatShrinkPlan* plan = new FatShrinkPlan();
  if (!plan) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }

  FatFilesystem* filesystem = static_cast<FatFilesystem*>(m_pFilesystem);
  if (!filesystem->truncateFile(this)) {
    delete plan;
    return false;
  }
  prepared = UniquePointer<PreparedShrink>::adopt(plan);
  return true;
}

bool FatFile::resizeFile(size_t size) {
  if (size == getSize())
    return true;
  if (size < getSize()) {
    SYSCALL_ERROR(OperationNotSupported);
    return false;
  }

  FatFilesystem* filesystem = static_cast<FatFilesystem*>(m_pFilesystem);
  filesystem->extend(this, size);
  return getSize() == size;
}
