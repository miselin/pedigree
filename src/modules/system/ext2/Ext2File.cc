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
#include "pedigree/kernel/machine/Disk.h"
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
    if (!m_State->cache) {
      return;
    }
    m_State->cache->fill.setDirtyTracking(Cache::DirtyTracking::Explicit);
    m_State->cache->fill.setCallback(sharedFillCallback, m_State);
  }
  {
    LockGuard<Mutex> writebackGuard(m_State->writebackLock);
    if (!m_State->files.tryReserve(m_State->files.count() + 1)) {
      return;
    }
    m_State->files.pushBack(this);
  }
  if (!m_State->futexIdentity) {
    m_State->futexIdentity = File::futexIdentity();
  }
  uint32_t mode = LITTLE_TO_HOST32(inode->i_mode);
  setPermissionsOnly(modeToPermissions(mode));
  setUidOnly(Ext2Owner::uid(*inode));
  setGidOnly(Ext2Owner::gid(*inode));
  m_Initialized = true;
}

Ext2File::~Ext2File() {
  LockGuard<Mutex> guard(m_State->dataLock);
  {
    LockGuard<Mutex> writebackGuard(m_State->writebackLock);
    for (size_t i = 0; i < m_State->files.count(); ++i) {
      if (m_State->files[i] == this) {
        m_State->files.erase(i);
        break;
      }
    }
  }
  // Linked inode state owns the cache after the last alias closes, so failed
  // writebacks can retry without retaining an object in its destructor.
  if (!m_State->files.count() && m_State->cache && m_State->cache->fill.empty()) {
    delete m_State->cache;
    m_State->cache = nullptr;
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
  if (m_State->quotaFile) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return false;
  }
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
  if (m_State->quotaFile && !m_State->quotaInternalWrite) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return false;
  }
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
        cacheState().fill.markDirty(pageOffset);
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

uint64_t Ext2File::maximumFileSize() const {
  return Ext2Node::maximumFileSize();
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
  if (m_State->quotaFile) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return;
  }
  // No need to change the actual file size, just allocate the blocks.
  Ext2Node::ensureLargeEnough(expectedSize, 0, 0, true, !zero);
}

void Ext2File::extend(size_t newSize) {
  LockGuard<Mutex> guard(m_State->writebackLock);
  if (m_State->quotaFile) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return;
  }
  Ext2Node::extend(newSize, 0, 0);
  m_Size = m_nSize;
}

void Ext2File::extend(size_t newSize, uint64_t location, uint64_t size) {
  LockGuard<Mutex> guard(m_State->writebackLock);
  if (m_State->quotaFile) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return;
  }
  Ext2Node::extend(newSize, location, size);
  m_Size = m_nSize;
}

void Ext2File::truncate() {
  resize(0);
}

bool Ext2File::prepareShrink(const ShrinkContext& context,
                             UniquePointer<PreparedShrink>& prepared) {
  // The generic fill cache is already drained. Do not retain this lock across
  // the mapping journal's returnPhysicalPage calls, which acquire it too.
  LockGuard<Mutex> guard(m_State->writebackLock);
  return prepareDataShrink(context.newSize, prepared);
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

bool Ext2File::readPage(uint64_t location, uintptr_t destination) {
  LockGuard<Mutex> guard(m_State->writebackLock);
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t blockSize = m_pExt2Fs->m_BlockSize;
  if (!destination || !blockSize || pageSize % blockSize || location % pageSize ||
      !m_State->allocationValid || location >= m_nSize) {
    SYSCALL_ERROR(IoError);
    return false;
  }
  ByteSet(reinterpret_cast<void*>(destination), 0, pageSize);
  const size_t remaining = m_nSize - location;
  const size_t length = remaining < pageSize ? remaining : pageSize;
  for (size_t offset = 0; offset < length; offset += blockSize) {
    const size_t block = (location + offset) / blockSize;
    if (block >= m_Blocks.count() || !ensureBlockLoaded(block)) {
      SYSCALL_ERROR(IoError);
      return false;
    }
  }
  return transferBlocksLocked(m_State, location, destination, length, false);
}

bool Ext2File::readPages(ReadPage* pages, size_t count) {
  if (count > MaxReadPages || (count && !pages)) {
    SYSCALL_ERROR(IoError);
    return false;
  }
  for (size_t i = 0; i < count; ++i)
    pages[i].complete = false;
  if (!count)
    return true;
  LockGuard<Mutex> guard(m_State->writebackLock);
  Disk* disk = m_pExt2Fs->m_pDisk;
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t blockSize = m_pExt2Fs->m_BlockSize;
  if (!disk || !blockSize || pageSize % blockSize || !m_State->allocationValid) {
    SYSCALL_ERROR(IoError);
    return false;
  }

  // Resolve mapping blocks before data transport. In particular, a later page's
  // mapping failure must not prevent the demanded page from being populated.
  for (size_t i = 0; i < count; ++i) {
    if (!pages[i].buffer || pages[i].offset % pageSize || pages[i].offset >= m_nSize)
      continue;
    ByteSet(reinterpret_cast<void*>(pages[i].buffer), 0, pageSize);
    const size_t remaining = m_nSize - pages[i].offset;
    const size_t length = remaining < pageSize ? remaining : pageSize;
    bool mapped = true;
    for (size_t offset = 0; offset < length; offset += blockSize) {
      const size_t block = (pages[i].offset + offset) / blockSize;
      if (block >= m_Blocks.count() || !ensureBlockLoaded(block)) {
        mapped = false;
        break;
      }
    }
    pages[i].complete = mapped;
  }

  Disk::ReadBuffer requests[Disk::MaxReadBuffers] = {};
  size_t owners[Disk::MaxReadBuffers] = {};
  size_t requestCount = 0;
  auto transfer = [&] {
    if (!requestCount)
      return;
    const bool allRead = disk->readIntoBatch(requests, requestCount);
    (void)allRead;
    for (size_t i = 0; i < requestCount; ++i)
      pages[owners[i]].complete = requests[i].complete && pages[owners[i]].complete;
    requestCount = 0;
  };
  for (size_t i = 0; i < count; ++i) {
    if (!pages[i].complete)
      continue;
    const size_t remaining = m_nSize - pages[i].offset;
    const size_t length = remaining < pageSize ? remaining : pageSize;
    for (size_t offset = 0; offset < length;) {
      const size_t block = (pages[i].offset + offset) / blockSize;
      const uint32_t physical = m_Blocks[block];
      size_t amount = length - offset < blockSize ? length - offset : blockSize;
      if (!physical) {
        offset += amount;
        continue;
      }
      while (amount % blockSize == 0 && amount < length - offset) {
        const size_t next = block + amount / blockSize;
        if (next >= m_Blocks.count() || m_Blocks[next] == ~uint32_t{0} ||
            static_cast<uint64_t>(m_Blocks[next]) !=
                static_cast<uint64_t>(physical) + amount / blockSize)
          break;
        const size_t available = length - offset - amount;
        amount += available < blockSize ? available : blockSize;
      }
      requests[requestCount] = {static_cast<uint64_t>(physical) * blockSize,
                                reinterpret_cast<void*>(pages[i].buffer + offset), amount, false};
      owners[requestCount++] = i;
      if (requestCount == Disk::MaxReadBuffers)
        transfer();
      offset += amount;
    }
  }
  transfer();
  bool succeeded = true;
  for (size_t i = 0; i < count; ++i)
    succeeded = pages[i].complete && succeeded;
  if (!succeeded)
    SYSCALL_ERROR(IoError);
  return succeeded;
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

  (void)addr;
  if (!length || location > ~uint64_t{0} - (length - 1))
    return;
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const uint64_t last = location + length - 1;
  uint64_t page = location - location % pageSize;
  while (true) {
    cacheState().fill.markDirty(page);
    if (last - page < pageSize)
      break;
    page += pageSize;
  }
}

bool Ext2File::sharedFillCallback(CacheConstants::CallbackCause cause, uintptr_t location,
                                  uintptr_t page, void* metadata) {
  Ext2InodeState* state = static_cast<Ext2InodeState*>(metadata);
  if (cause == CacheConstants::Eviction) {
    LockGuard<Mutex> guard(state->cache->indexLock);
    state->cache->data.remove(DataCacheKey(location / PhysicalMemoryManager::getPageSize()));
    return true;
  }
  if (cause != CacheConstants::WriteBack) {
    return false;
  }
  LockGuard<Mutex> guard(state->writebackLock);
  if (state->orphan && !state->files.count() &&
      !__atomic_load_n(&state->syncReferences, __ATOMIC_ACQUIRE)) {
    return true;
  }
  Disk* disk = state->filesystem->m_pDisk;
  if (!disk)
    return false;
  const bool written =
      transferBlocksLocked(state, location, page, PhysicalMemoryManager::getPageSize(), true);
  const bool durable = disk->syncData();
  return written && durable;
}

bool Ext2File::sharedFillBatchCallback(const Cache::WritebackPage* pages, size_t count,
                                       void* context) {
  auto* state = static_cast<Ext2InodeState*>(context);
  LockGuard<Mutex> guard(state->writebackLock);
  if (!count || (state->orphan && !state->files.count() &&
                 !__atomic_load_n(&state->syncReferences, __ATOMIC_ACQUIRE)))
    return true;
  Disk* disk = state->filesystem->m_pDisk;
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  if (!disk || !state->allocationValid)
    return false;
  bool succeeded = true;
  Disk::WriteBuffer buffers[Disk::MaxWriteBuffers];
  size_t pending = 0;
  auto drain = [&] {
    if (pending)
      succeeded = disk->writeFromBatch(buffers, pending) && succeeded;
    pending = 0;
  };
  for (size_t i = 0; i < count; ++i) {
    const auto& page = pages[i];
    if (state->filesystem->m_BlockSize != pageSize || page.key % pageSize ||
        page.key >= state->size || state->size - page.key < pageSize) {
      drain();
      succeeded = transferBlocksLocked(state, page.key, page.location, pageSize, true) && succeeded;
      continue;
    }
    const size_t block = page.key / pageSize;
    if (block >= state->blocks.count() || state->blocks[block] == ~uint32_t{0}) {
      succeeded = false;
      continue;
    }
    if (!state->blocks[block])
      continue;
    buffers[pending++] = {static_cast<uint64_t>(state->blocks[block]) * pageSize,
                          reinterpret_cast<const void*>(page.location), pageSize, false};
    if (pending == Disk::MaxWriteBuffers)
      drain();
  }
  drain();
  const bool durable = disk->syncData();
  return succeeded && durable;
}

bool Ext2File::transferBlocksLocked(Ext2InodeState* state, uint64_t location, uintptr_t addr,
                                    size_t length, bool write) {
  Ext2Filesystem* filesystem = state->filesystem;
  Disk* disk = filesystem->m_pDisk;
  const size_t blockSize = filesystem->m_BlockSize;
  if (!disk || !blockSize || !state->allocationValid || location % blockSize ||
      location > ~uint64_t{0} - length)
    return false;
  if (location >= state->size)
    return true;
  const size_t remaining = state->size - location;
  if (length > remaining)
    length = remaining;

  for (size_t offset = 0; offset < length;) {
    const uint64_t blockLocation = location + offset;
    const size_t block = blockLocation / blockSize;
    if (block >= state->blocks.count() || state->blocks[block] == ~uint32_t(0))
      return false;
    const uint32_t physicalBlock = state->blocks[block];
    size_t amount = length - offset < blockSize ? length - offset : blockSize;
    if (!physicalBlock) {
      offset += amount;
      continue;
    }

    while (amount % blockSize == 0 && amount < length - offset) {
      const size_t next = block + amount / blockSize;
      if (next >= state->blocks.count() || state->blocks[next] == ~uint32_t(0) ||
          static_cast<uint64_t>(state->blocks[next]) !=
              static_cast<uint64_t>(physicalBlock) + amount / blockSize)
        break;
      const size_t available = length - offset - amount;
      amount += available < blockSize ? available : blockSize;
    }

    const uint64_t diskOffset = static_cast<uint64_t>(physicalBlock) * blockSize;
    const bool transferred =
        write ? disk->writeFrom(diskOffset, reinterpret_cast<const void*>(addr + offset), amount)
              : disk->readInto(diskOffset, reinterpret_cast<void*>(addr + offset), amount);
    if (!transferred)
      return false;
    offset += amount;
  }
  return true;
}

bool Ext2File::pinBlock(uint64_t location) {
  LockGuard<Mutex> guard(m_State->writebackLock);
  return Ext2Node::pinBlock(location);
}

void Ext2File::unpinBlock(uint64_t location) {
  LockGuard<Mutex> guard(m_State->writebackLock);
  Ext2Node::unpinBlock(location);
}

bool Ext2File::sync() {
  bool succeeded = File::sync();
  LockGuard<Mutex> guard(m_State->writebackLock);
  return m_pExt2Fs->syncInode(getInodeNumber(), *this) && succeeded;
}

bool Ext2File::sync(size_t offset, bool async) {
  bool present = false;
  if (useFillCache()) {
    const bool succeeded = syncFillCache(offset, async, present);
    if (present) {
      return succeeded;
    }
  }
  LockGuard<Mutex> guard(m_State->writebackLock);
  return Ext2Node::sync(offset, async);
}

bool Ext2File::syncPages(const uint64_t* offsets, size_t count) {
  if (count > Disk::MaxSyncPages || (count && !offsets))
    return false;
  if (!count)
    return true;
  if (useFillCache()) {
    const size_t pageSize = PhysicalMemoryManager::getPageSize();
    uintptr_t keys[Disk::MaxSyncPages];
    for (size_t i = 0; i < count; ++i) {
      if (offsets[i] >= getSize() || offsets[i] % pageSize || offsets[i] > ~uintptr_t{0})
        return false;
      keys[i] = offsets[i];
    }
    // Claim the authoritative file pages before taking the inode lock: a
    // callback already copying one of these pages may need that same lock.
    return cacheState().fill.syncBatch(keys, count, sharedFillBatchCallback, m_State);
  }

  LockGuard<Mutex> guard(m_State->writebackLock);
  Disk* disk = m_pExt2Fs->m_pDisk;
  const size_t blockSize = m_pExt2Fs->m_BlockSize;
  if (!disk || !blockSize)
    return false;
  for (size_t i = 0; i < count; ++i) {
    if (offsets[i] >= m_nSize || (offsets[i] % blockSize) ||
        offsets[i] / blockSize >= m_Blocks.count())
      return false;
  }
  uint64_t locations[Disk::MaxSyncPages];
  size_t locationCount = 0;
  for (size_t i = 0; i < count; ++i) {
    const size_t block = offsets[i] / blockSize;
    if (!ensureBlockLoaded(block))
      return false;
    if (!m_Blocks[block])
      continue;
    const uint64_t location = static_cast<uint64_t>(m_Blocks[block]) * blockSize;
    if (location >= disk->getSize())
      return false;
    locations[locationCount++] = location;
  }
  return disk->syncPages(locations, locationCount);
}

size_t Ext2File::getBlockSize() const {
  return m_pExt2Fs->m_BlockSize;
}
