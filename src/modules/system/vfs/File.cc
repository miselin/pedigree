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

#include "File.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/Pair.h"
#include "pedigree/kernel/utilities/Result.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Filesystem.h"
#include "VFS.h"

void File::writeCallback(CacheConstants::CallbackCause cause, uintptr_t loc, uintptr_t page,
                         void* meta) {
  File* pFile = reinterpret_cast<File*>(meta);

  switch (cause) {
    case CacheConstants::WriteBack: {
      // We are given one dirty page. Blocks can be smaller than a page.
      size_t off = 0;
      for (; off < PhysicalMemoryManager::getPageSize(); off += pFile->getBlockSize()) {
        pFile->writeBlock(loc + off, page + off);
      }
    } break;
    case CacheConstants::Eviction:
      // Remove this page from our data cache.
      /// \todo handle block size < 4K??
      pFile->setCachedPage(loc / pFile->getBlockSize(), FILE_BAD_BLOCK);
      break;
    default:
      WARNING(
          "File: unknown cache callback -- could indicate potential "
          "future I/O issues.");
      break;
  }
}

File::File()
    : m_Name(),
      m_AccessedTime(0),
      m_ModifiedTime(0),
      m_CreationTime(0),
      m_Inode(0),
      m_pFilesystem(0),
      m_Size(0),
      m_pParent(0),
      m_nWriters(0),
      m_nReaders(0),
      m_Uid(0),
      m_Gid(0),
      m_Permissions(0),
      m_DataCache(FILE_BAD_BLOCK),
      m_bDirect(false),
      m_FillCache(),
      m_FillCacheLock(),
      m_Lock(),
      m_MonitorTargets() {}

File::File(const String& name, Time::Timestamp accessedTime, Time::Timestamp modifiedTime,
           Time::Timestamp creationTime, uintptr_t inode, Filesystem* pFs, size_t size,
           File* pParent)
    : m_Name(name),
      m_AccessedTime(accessedTime),
      m_ModifiedTime(modifiedTime),
      m_CreationTime(creationTime),
      m_Inode(inode),
      m_pFilesystem(pFs),
      m_Size(size),
      m_pParent(pParent),
      m_nWriters(0),
      m_nReaders(0),
      m_Uid(0),
      m_Gid(0),
      m_Permissions(0),
      m_DataCache(FILE_BAD_BLOCK),
      m_bDirect(false),
      m_FillCache(),
      m_FillCacheLock(),
      m_Lock(),
      m_MonitorTargets() {
  size_t maxBlock = size / getBlockSize();
  if (size % getBlockSize()) {
    ++maxBlock;
  }

  // Prefill the block cache with bad pages to avoid continuous resizes.
  setCachedPage(maxBlock, FILE_BAD_BLOCK);
}

File::~File() {
  LockGuard<Mutex> guard(m_Lock);
  for (auto target : m_MonitorTargets) {
    delete target;
  }
  m_MonitorTargets.clear();
}

uint64_t File::read(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock) {
  if (isBytewise()) {
    // Have to perform bytewise reads
    /// \todo consider caching this still
    return readBytewise(location, size, buffer, bCanBlock);
  }

  if (!size || location >= m_Size) {
    return 0;
  }

  const uint64_t remaining = m_Size - location;
  if (size > remaining) {
    size = remaining;
  }

  const size_t blockSize = useFillCache() ? PhysicalMemoryManager::getPageSize() : getBlockSize();

  size_t n = 0;
  while (size) {
    if (location >= m_Size)
      return n;

    uintptr_t block = location / blockSize;
    uintptr_t offs = location % blockSize;
    uintptr_t sz = (size + offs > blockSize) ? blockSize - offs : size;

    // Handle a possible early EOF.
    if (sz > (m_Size - location))
      sz = m_Size - location;

    uintptr_t buff = readIntoCache(block);
    if (buff == FILE_BAD_BLOCK) {
      ERROR("File::read - failed to get page from cache, returning early");
      return n;
    }

    if (buffer) {
      ForwardMemoryCopy(reinterpret_cast<void*>(buffer), reinterpret_cast<void*>(buff + offs), sz);
      buffer += sz;
    }
    releaseReadReference(block);

    location += sz;
    size -= sz;
    n += sz;
  }
  return n;
}

uint64_t File::write(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock) {
  if (!size || location > (~static_cast<uint64_t>(0) - size)) {
    return 0;
  }

  const uint64_t endLocation = location + size;
  if (endLocation > static_cast<uint64_t>(~static_cast<size_t>(0))) {
    return 0;
  }

  if (isBytewise()) {
    // Have to perform bytewise reads
    /// \todo consider caching this still
    return writeBytewise(location, size, buffer, bCanBlock);
  }

  const size_t filesystemBlockSize = getBlockSize();
  const size_t blockSize =
      useFillCache() ? PhysicalMemoryManager::getPageSize() : filesystemBlockSize;

  // Extend the file before writing it if needed.
  extend(static_cast<size_t>(endLocation), location, size);

  size_t n = 0;
  while (size) {
    uintptr_t block = location / blockSize;
    uintptr_t offs = location % blockSize;
    uintptr_t sz = (size + offs > blockSize) ? blockSize - offs : size;

    uintptr_t buff = readIntoCache(block);
    if (buff == FILE_BAD_BLOCK) {
      ERROR("File::read - failed to get page from cache, returning early");
      return n;
    }

    ForwardMemoryCopy(reinterpret_cast<void*>(buff + offs), reinterpret_cast<void*>(buffer), sz);

    // Trigger an immediate write-back - write-through cache.
    if (useFillCache()) {
      const uint64_t pageOffset = block * blockSize;
      const size_t firstBlock = offs / filesystemBlockSize;
      const size_t endBlock = (offs + sz + filesystemBlockSize - 1) / filesystemBlockSize;
      for (size_t i = firstBlock; i < endBlock; ++i) {
        const size_t fileBlockOffset = i * filesystemBlockSize;
        writeBlock(pageOffset + fileBlockOffset, buff + fileBlockOffset);
      }
    } else {
      writeBlock(block * blockSize, buff);
    }
    releaseReadReference(block);

    location += sz;
    buffer += sz;
    size -= sz;
    n += sz;
  }

  if (location >= m_Size) {
    m_Size = location;
    fileAttributeChanged();
  }
  return n;
}

physical_uintptr_t File::getPhysicalPage(size_t offset) {
  if (m_bDirect) {
    WARNING("File in direct mode, cannot get backing page.");
    return ~0UL;
  }

  EMIT_IF(VFS_NOMMU) {
    return ~0UL;
  }

  // Sanitise input.
  size_t blockSize = getBlockSize();
  size_t nativeBlockSize = PhysicalMemoryManager::getPageSize();
  if (useFillCache()) {
    blockSize = nativeBlockSize;
  }
  offset &= ~(blockSize - 1);

  // Quick and easy exit.
  if (offset >= m_Size) {
    return ~0UL;
  }

  // Check if we have this page in the cache and acquire the cache reference
  // before translating it. A cache address observed before a successful pin
  // may already be retiring.
  uintptr_t vaddr = FILE_BAD_BLOCK;
  bool pinned = false;
  if (LIKELY(!useFillCache())) {
    // A key can be evicted and replaced between the address snapshot and
    // pinBlock(). Validate that the address still names the pinned page.
    vaddr = getCachedPage(offset / blockSize);
    if ((!vaddr) || (vaddr == FILE_BAD_BLOCK) || !pinBlock(offset)) {
      return ~0UL;
    }
    pinned = true;

    if (getCachedPage(offset / blockSize) != vaddr) {
      unpinBlock(offset);
      return ~0UL;
    }
  } else {
    // Using the fill cache, because the filesystem has a block size
    // smaller than our native page size. lookup() itself acquires the
    // reference; taking a second pin here would leak one on every mmap.
    vaddr = m_FillCache.lookup(offset);
    if (!vaddr) {
      return ~0UL;
    }
    pinned = true;
  }

  // Translate only while the exact published page remains pinned.
  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  if (va.isMapped(reinterpret_cast<void*>(vaddr))) {
    physical_uintptr_t phys = 0;
    size_t flags = 0;
    va.getMapping(reinterpret_cast<void*>(vaddr), phys, flags);
    return phys;
  }

  if (pinned) {
    if (UNLIKELY(useFillCache()))
      m_FillCache.release(offset);
    else
      unpinBlock(offset);
  }
  return ~0UL;
}

void File::returnPhysicalPage(size_t offset) {
  if (m_bDirect) {
    return;
  }

  // Sanitise input.
  size_t blockSize = getBlockSize();
  size_t nativeBlockSize = PhysicalMemoryManager::getPageSize();
  if (useFillCache()) {
    blockSize = nativeBlockSize;
  }
  offset &= ~(blockSize - 1);

  // Release the page. Beware - this could cause a cache evict, which will
  // make the next read/write at this offset do real (slow) I/O.
  if (UNLIKELY(useFillCache())) {
    m_FillCache.release(offset);
  } else {
    unpinBlock(offset);
  }
}

void File::sync() {
  struct SyncPage {
    size_t block;
    uintptr_t buffer;
  };

  size_t snapshotSize = 0;
  {
    LockGuard<Mutex> guard(m_Lock);
    snapshotSize = m_DataCache.count();
  }

  // Reserve outside m_Lock. Allocation can trigger cache pressure, whose
  // eviction callback removes entries under this same File lock.
  Vector<SyncPage> pages(snapshotSize);
  {
    LockGuard<Mutex> guard(m_Lock);
    const size_t count = m_DataCache.count() < snapshotSize ? m_DataCache.count() : snapshotSize;
    for (size_t i = 0; i < count; ++i) {
      auto result = m_DataCache.getNth(i);
      if (result.hasError()) {
        break;
      }

      const uintptr_t buffer = result.value().second();
      if (buffer != FILE_BAD_BLOCK) {
        pages.pushBack({result.value().first().hash(), buffer});
      }
    }
  }

  const size_t blockSize = getBlockSize();
  for (const SyncPage& page : pages) {
    const uint64_t location = page.block * blockSize;
    if (!pinBlock(location)) {
      continue;
    }

    // m_DataCache is a weak identity index. Pin the producer cache, then
    // verify that the snapshot still names the pinned page.
    if (getCachedPage(page.block) == page.buffer) {
      writeBlock(location, page.buffer);
    }
    unpinBlock(location);
  }
}

void File::sync(size_t offset, bool async) {}

Time::Timestamp File::getCreationTime() {
  return m_CreationTime;
}

void File::setCreationTime(Time::Timestamp t) {
  m_CreationTime = t;
  fileAttributeChanged();
}

Time::Timestamp File::getAccessedTime() {
  return m_AccessedTime;
}

void File::setAccessedTime(Time::Timestamp t) {
  m_AccessedTime = t;
  fileAttributeChanged();
}

Time::Timestamp File::getModifiedTime() {
  return m_ModifiedTime;
}

void File::setModifiedTime(Time::Timestamp t) {
  m_ModifiedTime = t;
  fileAttributeChanged();
}

const String& File::getName() const {
  return m_Name;
}

void File::getName(String& s) const {
  s = m_Name;
}

size_t File::getSize() {
  return m_Size;
}

void File::setSize(size_t sz) {
  m_Size = sz;
}

bool File::isSymlink() {
  return false;
}

bool File::isDirectory() {
  return false;
}

bool File::isPipe() const {
  return false;
}

bool File::isFifo() const {
  return false;
}

bool File::isSocket() const {
  return false;
}

uintptr_t File::getInode() const {
  return m_Inode;
}

void File::setInode(uintptr_t inode) {
  m_Inode = inode;
}

Filesystem* File::getFilesystem() const {
  return m_pFilesystem;
}

void File::setFilesystem(Filesystem* pFs) {
  m_pFilesystem = pFs;
}

void File::fileAttributeChanged() {}

void File::increaseRefCount(bool bIsWriter) {
  if (bIsWriter)
    m_nWriters++;
  else
    m_nReaders++;
}

void File::decreaseRefCount(bool bIsWriter) {
  if (bIsWriter)
    m_nWriters--;
  else
    m_nReaders--;
}

void File::setPermissions(uint32_t perms) {
  m_Permissions = perms;
  fileAttributeChanged();
}

uint32_t File::getPermissions() const {
  return m_Permissions;
}

void File::setUid(size_t uid) {
  m_Uid = uid;
  fileAttributeChanged();
}

size_t File::getUid() const {
  return m_Uid;
}

void File::setGid(size_t gid) {
  m_Gid = gid;
  fileAttributeChanged();
}

size_t File::getGid() const {
  return m_Gid;
}

File* File::getParent() const {
  return m_pParent;
}

int File::select(bool bWriting, int timeout) {
  return 1;
}

bool File::supports(const size_t command) const {
  return false;
}

int File::command(const size_t command, void* buffer) {
  return 0;
}

size_t File::getBlockSize() const {
  return PhysicalMemoryManager::getPageSize();
}

void File::enableDirect() {
  m_bDirect = true;
}

void File::disableDirect() {
  m_bDirect = false;
}

void File::preallocate(size_t expectedSize, bool zero) {}

void File::truncate() {}

File* File::open() {
  return this;
}

bool File::isBytewise() const {
  return false;
}

uint64_t File::readBytewise(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock) {
  if (isBytewise()) {
    FATAL("A bytewise File subclass didn't implement readBytewise");
  }
  return 0;
}

uint64_t File::writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock) {
  if (isBytewise()) {
    FATAL("A bytewise File subclass didn't implement writeBytewise");
  }
  return 0;
}

uintptr_t File::readBlock(uint64_t location) {
  String fullPath;
  getFullPath(fullPath);
  ERROR("File: base class readBlock() called for " << fullPath);
  // only truly breaks on debug-enabled builds - in release builds this will
  // just cause an error in the caller
  assert(false);
  return 0;
}

void File::writeBlock(uint64_t location, uintptr_t addr) {}

void File::extend(size_t newSize) {
  if (m_Size < newSize)
    m_Size = newSize;
}

void File::extend(size_t newSize, uint64_t location, uint64_t size) {
  extend(newSize);
}

bool File::pinBlock(uint64_t location) {
  return false;
}

void File::unpinBlock(uint64_t location) {}

void File::evict(uint64_t location) {
  setCachedPage(location / getBlockSize(), FILE_BAD_BLOCK);
}

void File::setPermissionsOnly(uint32_t perms) {
  m_Permissions = perms;
}

void File::setUidOnly(size_t uid) {
  m_Uid = uid;
}

void File::setGidOnly(size_t gid) {
  m_Gid = gid;
}

void File::dataChanged() {
  EMIT_IF(THREADS) {
    bool bAny = false;
    {
      LockGuard<Mutex> guard(m_Lock);

      for (List<MonitorTarget*>::Iterator it = m_MonitorTargets.begin();
           it != m_MonitorTargets.end(); it++) {
        MonitorTarget* pMT = *it;

        pMT->pThread->sendEvent(pMT->pEvent);
        delete pMT;

        bAny = true;
      }

      m_MonitorTargets.clear();
    }

    // If anything was waiting on a change, wake it up now.
    if (bAny) {
      Scheduler::instance().yield();
    }
  }
}

void File::monitor(Thread* pThread, Event* pEvent) {
  assert(pThread);
  assert(pEvent);

  EMIT_IF(THREADS) {
    LockGuard<Mutex> guard(m_Lock);
    Event::SendLease registration;
    if (pEvent->tryAcquireRegistration(registration)) {
      m_MonitorTargets.pushBack(
          new MonitorTarget(pThread, pEvent, pedigree_std::move(registration)));
    }
  }
}

void File::cullMonitorTargets(Thread* pThread) {
  EMIT_IF(THREADS) {
    LockGuard<Mutex> guard(m_Lock);

    for (List<MonitorTarget*>::Iterator it = m_MonitorTargets.begin();
         it != m_MonitorTargets.end();) {
      MonitorTarget* pMT = *it;

      if (pMT->pThread == pThread) {
        delete pMT;
        it = m_MonitorTargets.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void File::cullMonitorTargets(Event* pEvent) {
  EMIT_IF(THREADS) {
    LockGuard<Mutex> guard(m_Lock);

    for (List<MonitorTarget*>::Iterator it = m_MonitorTargets.begin();
         it != m_MonitorTargets.end();) {
      MonitorTarget* pMT = *it;
      if (pMT->pEvent == pEvent) {
        delete pMT;
        it = m_MonitorTargets.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void File::getFilesystemLabel(HugeStaticString& s) {
  s = m_pFilesystem->getVolumeLabel();
}

void File::getFullPath(String& result, bool bWithMount) {
  HugeStaticString str;
  HugeStaticString tmp;
  str.clear();
  tmp.clear();

  if (getParent() != 0)
    str = getName();

  File* f = this;
  while ((f = f->getParent())) {
    // This feels a bit weird considering the while loop's subject...
    if (f->getParent()) {
      tmp = str;
      str = f->getName();
      str += "/";
      str += tmp;
    }
  }

  tmp = str;
  str = "/";
  str += tmp;

  if (bWithMount && m_pFilesystem) {
    String mountPath;
    if (VFS::instance().getMountPath(m_pFilesystem, mountPath) && mountPath != "/") {
      tmp = str;
      str = mountPath;
      if (tmp != "/") {
        str += tmp;
      }
    }
  } else if (bWithMount && !m_pFilesystem) {
    ERROR("File::getFullPath called without a filesystem!");
  }

  result.assign(str, str.length());
}

String File::getFullPath(bool bWithMount) {
  String path;
  getFullPath(path, bWithMount);
  return path;
}

uintptr_t File::getCachedPage(size_t block, bool locked) {
  LockGuard<Mutex> guard(m_Lock, locked);

  DataCacheKey key(block);
  auto result = m_DataCache.lookup(key);
  if (result.hasValue()) {
    return result.value();
  } else {
    return FILE_BAD_BLOCK;
  }
}

void File::setCachedPage(size_t block, uintptr_t value, bool locked) {
  LockGuard<Mutex> guard(m_Lock, locked);

  assert(value);

  DataCacheKey key(block);
  if (m_DataCache.contains(key)) {
    if (value == FILE_BAD_BLOCK) {
      m_DataCache.remove(key);
    } else {
      m_DataCache.update(key, value);
    }
  } else {
    m_DataCache.insert(key, value);
  }
}

bool File::useFillCache() const {
  EMIT_IF(VFS_NOMMU) {
    // No fill cache in NOMMU builds.
    return false;
  }
  else {
    size_t blockSize = getBlockSize();
    size_t nativeBlockSize = PhysicalMemoryManager::getPageSize();
    return blockSize < nativeBlockSize;
  }
}

uintptr_t File::readIntoCache(uintptr_t block) {
  size_t blockSize = getBlockSize();
  size_t nativeBlockSize = PhysicalMemoryManager::getPageSize();
  const bool fillCache = useFillCache();

  const size_t offset = block * (fillCache ? nativeBlockSize : blockSize);

  if (fillCache) {
    LockGuard<Mutex> fillGuard(m_FillCacheLock);

    // Using Cache::insert() here is atomic compared to if we did a
    // lookup() followed by an insert() - means we don't need to lock the
    // File object to do this.
    bool didExist = false;
    uintptr_t vaddr = m_FillCache.insert(offset, nativeBlockSize, &didExist);
    if (!vaddr) {
      return FILE_BAD_BLOCK;
    }

    // If in direct mode we are required to read() again
    bool existingReference = false;
    if (didExist) {
      vaddr = m_FillCache.lookup(offset);
      if (!vaddr) {
        return FILE_BAD_BLOCK;
      }
      if (!m_bDirect) {
        return vaddr;
      }
      existingReference = true;
    }

    // Read the blocks
    ByteSet(reinterpret_cast<void*>(vaddr), 0, nativeBlockSize);
    for (size_t i = 0; i < nativeBlockSize; i += blockSize) {
      if ((offset + i) >= m_Size) {
        break;
      }
      uintptr_t blockAddr = readBlock(offset + i);
      if (!blockAddr || blockAddr == FILE_BAD_BLOCK) {
        if (existingReference) {
          m_FillCache.release(offset);
        }
        if (!didExist && !m_FillCache.discardEditing(offset)) {
          WARNING(
              "File::readIntoCache could not discard a failed fill "
              "for offset "
              << offset);
        }
        return FILE_BAD_BLOCK;
      }
      ForwardMemoryCopy(reinterpret_cast<void*>(vaddr + i), reinterpret_cast<void*>(blockAddr),
                        blockSize);
      unpinBlock(offset + i);
    }

    m_FillCache.markNoLongerEditing(offset, nativeBlockSize);

    if (existingReference) {
      return vaddr;
    }

    vaddr = m_FillCache.lookup(offset);
    return vaddr ? vaddr : FILE_BAD_BLOCK;
  }

  uintptr_t buff = FILE_BAD_BLOCK;
  if (!m_bDirect) {
    while ((buff = getCachedPage(block)) != FILE_BAD_BLOCK) {
      if (!pinBlock(offset)) {
        buff = FILE_BAD_BLOCK;
        break;
      }
      if (getCachedPage(block) == buff) {
        return buff;
      }
      unpinBlock(offset);
    }
  }
  if (buff == FILE_BAD_BLOCK) {
    buff = readBlock(offset);
    if (!buff) {
      ERROR("File::readIntoCache - bad read (" << (block * blockSize) << " - block size is "
                                               << blockSize << ")");
      return FILE_BAD_BLOCK;
    }

    if (!m_bDirect) {
      setCachedPage(block, buff);
    }
  }

  return buff;
}

void File::releaseReadReference(uintptr_t block) {
  if (useFillCache()) {
    m_FillCache.release(block * PhysicalMemoryManager::getPageSize());
  } else {
    unpinBlock(block * getBlockSize());
  }
}
