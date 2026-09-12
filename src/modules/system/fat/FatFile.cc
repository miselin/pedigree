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

FatFile::FatFile(String name, Time::Timestamp accessedTime, Time::Timestamp modifiedTime,
                 Time::Timestamp creationTime, uintptr_t inode, class Filesystem* pFs, size_t size,
                 uint32_t dirClus, uint32_t dirOffset, File* pParent)
    : File(name, accessedTime, modifiedTime, creationTime, inode, pFs, size, pParent),
      m_State(static_cast<FatFilesystem*>(pFs)->acquireFileState(
          this, inode, size, dirClus, dirOffset, accessedTime, modifiedTime, creationTime)),
      m_DirClus(m_State->directoryCluster),
      m_DirOffset(m_State->directoryOffset),
      m_MetadataDirty(m_State->metadataDirty),
      m_TrimPending(m_State->trimPending),
      m_RetiredClusters(m_State->retiredClusters) {
  {
    LockGuard<Mutex> registry(static_cast<FatFilesystem*>(pFs)->m_StateLock);
    m_NextAlias = m_State->aliases;
    m_State->aliases = this;
  }
  copyStateAttributes();
  File::setInode(m_State->inode);
  File::setSize(m_State->size);
  setPermissionsOnly(FILE_UR | FILE_UW | FILE_UX | FILE_GR | FILE_GW | FILE_GX | FILE_OR | FILE_OW |
                     FILE_OX);
}

FatFile::FatFile(State& state)
    : File(String(), state.accessed, state.modified, state.changed, state.inode, state.filesystem,
           state.size, nullptr),
      m_State(&state),
      m_Proxy(true),
      m_DirClus(state.directoryCluster),
      m_DirOffset(state.directoryOffset),
      m_MetadataDirty(state.metadataDirty),
      m_TrimPending(state.trimPending),
      m_RetiredClusters(state.retiredClusters) {}

FatFile::~FatFile() {
  if (!m_Proxy)
    m_State->filesystem->releaseFileState(this);
}

File::CacheState& FatFile::cacheState() {
  return m_State->cache;
}
Mutex& FatFile::dataMutationLock() {
  return m_State->dataLock;
}
Mutex& FatFile::writeSerializationLock() {
  return m_State->writeLock;
}
size_t& FatFile::physicalPageLoans() {
  return m_State->pageLoans;
}
size_t FatFile::getSize() {
  return m_State->size;
}
uintptr_t FatFile::futexIdentity() {
  return reinterpret_cast<uintptr_t>(m_State);
}

void FatFile::setInode(uintptr_t inode) {
  LockGuard<Mutex> registry(m_State->filesystem->m_StateLock);
  m_State->inode = inode;
  m_State->chainRevision = 0;
  File::setInode(inode);
  for (FatFile* alias = m_State->aliases; alias; alias = alias->m_NextAlias)
    alias->File::setInode(inode);
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
  State* state = static_cast<State*>(meta);
  if (cause == CacheConstants::Eviction) {
    FatFile file(*state);
    return File::fillCacheCallback(cause, location, page, &file);
  }
  if (cause != CacheConstants::WriteBack)
    return true;
  Cache::WritebackPage entry{location, page};
  return state->filesystem->writeCachedPages(*state, &entry, 1);
}

bool FatFile::checkedBatchCallback(const Cache::WritebackPage* pages, size_t count, void* meta) {
  State* state = static_cast<State*>(meta);
  return state->filesystem->writeCachedPages(*state, pages, count);
}

bool FatFile::sync() {
  LockGuard<Mutex> data(m_State->dataLock);
  const bool succeeded = cacheState().fill.syncAll(checkedBatchCallback, m_State);
  return m_State->filesystem->syncFileMetadata(this) && succeeded;
}

bool FatFile::sync(size_t offset, bool async) {
  offset -= offset % getBlockSize();
  if (async)
    return cacheState().fill.sync(offset, true);
  const uintptr_t key = offset;
  const bool succeeded = cacheState().fill.syncBatch(&key, 1, checkedBatchCallback, m_State);
  return m_State->filesystem->syncFileMetadata(this) && succeeded;
}

bool FatFile::syncPages(const uint64_t* offsets, size_t count) {
  if (count > Cache::MaxWritebackPages || (count && !offsets))
    return false;
  uintptr_t keys[Cache::MaxWritebackPages];
  for (size_t i = 0; i < count; ++i)
    keys[i] = offsets[i] - offsets[i] % getBlockSize();
  const bool succeeded = cacheState().fill.syncBatch(keys, count, checkedBatchCallback, m_State);
  return m_State->filesystem->syncFileMetadata(this) && succeeded;
}

File::Attributes FatFile::getAttributes() const {
  FatFile* file = const_cast<FatFile*>(this);
  LockGuard<Mutex> dataGuard(file->dataMutationLock());
  FatFilesystem* filesystem = static_cast<FatFilesystem*>(m_pFilesystem);
  LockGuard<Mutex> guard(filesystem->m_FileMutationLock);
  Attributes attributes = File::getAttributes();
  attributes.accessed = m_State->accessed;
  attributes.modified = m_State->modified;
  attributes.changed = m_State->changed;
  attributes.inode = m_State->identifier;
  attributes.blocks = filesystem->allocatedBlocks(file);
  return attributes;
}

bool FatFile::pinBlock(uint64_t location) {
  return cacheState().fill.pin(location - location % getBlockSize());
}

void FatFile::unpinBlock(uint64_t location) {
  cacheState().fill.release(location - location % getBlockSize());
}

void FatFile::copyStateAttributes() {
  LockGuard<Mutex> metadata(m_MetadataLock);
  m_AccessedTime = m_State->accessed;
  m_ModifiedTime = m_State->modified;
  m_CreationTime = m_State->changed;
}
