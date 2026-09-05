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

#include "Ext2File.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Ext2Filesystem.h"
#include "ext2.h"

class Filesystem;

Ext2File::Ext2File(const String& name, uintptr_t inode_num, Inode* inode, Ext2Filesystem* pFs,
                   File* pParent)
    : File(name, LITTLE_TO_HOST32(inode->i_atime), LITTLE_TO_HOST32(inode->i_mtime),
           LITTLE_TO_HOST32(inode->i_ctime), inode_num, static_cast<Filesystem*>(pFs),
           LITTLE_TO_HOST32(inode->i_size),  /// \todo Deal with >4GB files here.
           pParent),
      Ext2Node(inode_num, inode, pFs) {
  LockGuard<Mutex> guard(m_State->dataLock);
  if (!m_State->cache) {
    m_State->cache = new CacheState;
    m_State->cache->fill.setCallback(sharedFillCallback, m_State);
  }
  m_State->files.pushBack(this);
  {
    LockGuard<Mutex> writebackGuard(m_State->writebackLock);
    if (!m_State->writebackOwner) {
      m_State->writebackOwner = this;
    }
  }
  if (!m_State->futexIdentity) {
    m_State->futexIdentity = File::futexIdentity();
  }
  uint32_t mode = LITTLE_TO_HOST32(inode->i_mode);
  setPermissionsOnly(modeToPermissions(mode));
  setUidOnly(LITTLE_TO_HOST16(inode->i_uid));
  setGidOnly(LITTLE_TO_HOST16(inode->i_gid));
}

Ext2File::~Ext2File() {
  LockGuard<Mutex> guard(m_State->dataLock);
  for (size_t i = 0; i < m_State->files.count(); ++i) {
    if (m_State->files[i] == this) {
      m_State->files.erase(i);
      break;
    }
  }
  if (!m_State->files.count()) {
    // Cache shutdown waits for callbacks without holding the writeback lock.
    // This final alias remains a valid callback target until that drain ends.
    delete m_State->cache;
    m_State->cache = nullptr;
  }
  LockGuard<Mutex> writebackGuard(m_State->writebackLock);
  if (m_State->writebackOwner == this) {
    m_State->writebackOwner = m_State->files.count() ? m_State->files[0] : nullptr;
  }
}

File::CacheState& Ext2File::cacheState() {
  return *m_State->cache;
}

bool Ext2File::useFillCache() const {
  EMIT_IF(VFS_NOMMU) {
    return File::useFillCache();
  }
  else {
    return true;
  }
}

File::Attributes Ext2File::getAttributes() const {
  return inodeAttributes();
}

void Ext2File::updateAttributes(const Attributes& attributes, uint32_t mask) {
  updateInodeAttributes(attributes, mask);
}

bool Ext2File::prepareSharedMapping(size_t offset, size_t length) {
  LockGuard<Mutex> dataGuard(m_State->dataLock);
  LockGuard<Mutex> guard(m_State->writebackLock);
  if (m_pExt2Fs->isReadOnly()) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return false;
  }
  if (offset >= m_nSize) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  if (length > m_nSize - offset) {
    length = m_nSize - offset;
  }
  const bool success = ensureWritableRange(offset, length);
  if (!useFillCache()) {
    LockGuard<Mutex> indexGuard(cacheState().indexLock);
    cacheState().data.clear();
  }
  return success;
}

bool Ext2File::prepareWrite(uint64_t location, uint64_t size) {
  LockGuard<Mutex> guard(m_State->writebackLock);
  if (m_pExt2Fs->isReadOnly()) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return false;
  }
  const size_t oldSize = m_nSize;
  const size_t end = static_cast<size_t>(location + size);
  if (!ensureLargeEnough(end, location, size)) {
    return false;
  }
  if (end > oldSize && useFillCache()) {
    const size_t pageSize = PhysicalMemoryManager::getPageSize();
    const size_t within = oldSize % pageSize;
    if (within) {
      const size_t pageOffset = oldSize - within;
      const uintptr_t page = cacheState().fill.lookup(pageOffset);
      if (page) {
        const size_t amount = end - oldSize < pageSize - within ? end - oldSize : pageSize - within;
        ByteSet(reinterpret_cast<void*>(page + within), 0, amount);
        cacheState().fill.release(pageOffset);
      }
    }
  }
  const bool success = ensureWritableRange(location, size);
  if (!useFillCache()) {
    LockGuard<Mutex> indexGuard(cacheState().indexLock);
    cacheState().data.clear();
  }
  return success;
}

size_t Ext2File::getSize() {
  return __atomic_load_n(&m_nSize, __ATOMIC_ACQUIRE);
}

uintptr_t Ext2File::futexIdentity() {
  return m_State->futexIdentity;
}

Mutex& Ext2File::dataMutationLock() {
  return m_State->dataLock;
}

Mutex& Ext2File::writeSerializationLock() {
  return m_State->writeLock;
}

size_t& Ext2File::physicalPageLoans() {
  return m_State->pageLoans;
}

bool Ext2File::tryBeginMappingRelease() {
  if (!m_State->dataLock.tryAcquire()) {
    return false;
  }
  if (!m_State->writebackLock.tryAcquire()) {
    m_State->dataLock.release();
    return false;
  }
  m_State->writebackLock.release();
  return true;
}

void Ext2File::preallocate(size_t expectedSize, bool zero) {
  LockGuard<Mutex> writeGuard(m_State->writeLock);
  LockGuard<Mutex> dataGuard(m_State->dataLock);
  LockGuard<Mutex> guard(m_State->writebackLock);
  // No need to change the actual file size, just allocate the blocks.
  Ext2Node::ensureLargeEnough(expectedSize, 0, 0, true, !zero);
}

void Ext2File::extend(size_t newSize) {
  LockGuard<Mutex> guard(m_State->writebackLock);
  Ext2Node::extend(newSize, 0, 0);
  m_Size = m_nSize;
}

void Ext2File::extend(size_t newSize, uint64_t location, uint64_t size) {
  LockGuard<Mutex> guard(m_State->writebackLock);
  Ext2Node::extend(newSize, location, size);
  m_Size = m_nSize;
}

void Ext2File::truncate() {
  resize(0);
}

bool Ext2File::resizeFile(size_t size) {
  LockGuard<Mutex> guard(m_State->writebackLock);
  if (!resizeData(size)) {
    return false;
  }
  for (Ext2File* alias : m_State->files) {
    alias->setSize(m_nSize);
  }
  return true;
}

void Ext2File::fileAttributeChanged() {
  LockGuard<Mutex> guard(m_State->writebackLock);
  Ext2Node::fileAttributeChanged(getSize(), LITTLE_TO_HOST32(m_pInode->i_atime),
                                 LITTLE_TO_HOST32(m_pInode->i_mtime),
                                 LITTLE_TO_HOST32(m_pInode->i_ctime));
}

uintptr_t Ext2File::readBlock(uint64_t location) {
  LockGuard<Mutex> guard(m_State->writebackLock);
  return Ext2Node::readBlock(location);
}

void Ext2File::writeBlock(uint64_t location, uintptr_t addr) {
  if (useFillCache()) {
    writeBlocks(location, addr, getBlockSize());
  } else {
    LockGuard<Mutex> guard(m_State->writebackLock);
    Ext2Node::writeBlock(location);
  }
}

void Ext2File::writeBlocks(uint64_t location, uintptr_t addr, size_t length) {
  if (!useFillCache()) {
    File::writeBlocks(location, addr, length);
    return;
  }

  LockGuard<Mutex> guard(m_State->writebackLock);
  writeBlocksLocked(location, addr, length);
}

void Ext2File::sharedFillCallback(CacheConstants::CallbackCause cause, uintptr_t location,
                                  uintptr_t page, void* metadata) {
  Ext2InodeState* state = static_cast<Ext2InodeState*>(metadata);
  if (cause == CacheConstants::Eviction) {
    LockGuard<Mutex> guard(state->cache->indexLock);
    state->cache->data.remove(DataCacheKey(location / PhysicalMemoryManager::getPageSize()));
    return;
  }
  if (cause != CacheConstants::WriteBack) {
    return;
  }
  LockGuard<Mutex> guard(state->writebackLock);
  assert(state->writebackOwner);
  state->writebackOwner->writeBlocksLocked(location, page, PhysicalMemoryManager::getPageSize());
}

void Ext2File::writeBlocksLocked(uint64_t location, uintptr_t addr, size_t length) {
  const size_t blockSize = getBlockSize();
  uint32_t pinnedBlocks[TargetInfo::getPageSize() / 1024] = {};
  size_t pinnedCount = 0;

  // ATA queues can coalesce writes by native page, so copy every constituent
  // before publishing the first lower write.
  for (size_t offset = 0; offset < length; offset += blockSize) {
    if (pinnedCount == sizeof(pinnedBlocks) / sizeof(pinnedBlocks[0])) {
      break;
    }
    if (location > ~static_cast<uint64_t>(0) - offset) {
      break;
    }

    const uint64_t blockLocation = location + offset;
    const size_t block = blockLocation / blockSize;
    if (block >= m_Blocks.count() || blockLocation >= m_nSize || !ensureBlockLoaded(block) ||
        !m_Blocks[block]) {
      continue;
    }

    const uint32_t physicalBlock = m_Blocks[block];
    const uintptr_t destination = m_pExt2Fs->readBlock(physicalBlock);
    if (!destination || destination == FILE_BAD_BLOCK) {
      continue;
    }

    size_t copyLength = blockSize;
    const size_t remaining = m_nSize - static_cast<size_t>(blockLocation);
    if (copyLength > remaining) {
      copyLength = remaining;
    }
    ForwardMemoryCopy(reinterpret_cast<void*>(destination), reinterpret_cast<void*>(addr + offset),
                      copyLength);
    pinnedBlocks[pinnedCount++] = physicalBlock;
  }

  for (size_t i = 0; i < pinnedCount; ++i) {
    m_pExt2Fs->writeBlock(pinnedBlocks[i]);
  }
  for (size_t i = 0; i < pinnedCount; ++i) {
    m_pExt2Fs->unpinBlock(pinnedBlocks[i]);
  }
}

bool Ext2File::pinBlock(uint64_t location) {
  LockGuard<Mutex> guard(m_State->writebackLock);
  return Ext2Node::pinBlock(location);
}

void Ext2File::unpinBlock(uint64_t location) {
  LockGuard<Mutex> guard(m_State->writebackLock);
  Ext2Node::unpinBlock(location);
}

void Ext2File::sync(size_t offset, bool async) {
  if (!useFillCache() || !syncFillCache(offset, async)) {
    LockGuard<Mutex> guard(m_State->writebackLock);
    Ext2Node::sync(offset, async);
    return;
  }
  if (async) {
    return;
  }

  LockGuard<Mutex> guard(m_State->writebackLock);

  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t pageOffset = offset - (offset % pageSize);
  const size_t blockSize = getBlockSize();
  for (size_t pageBlock = 0; pageBlock < pageSize; pageBlock += blockSize) {
    if (pageOffset > ~static_cast<size_t>(0) - pageBlock) {
      break;
    }
    const size_t blockOffset = pageOffset + pageBlock;
    if (blockOffset >= m_nSize) {
      break;
    }
    Ext2Node::sync(blockOffset, false);
  }
}

size_t Ext2File::getBlockSize() const {
  return m_pExt2Fs->m_BlockSize;
}
