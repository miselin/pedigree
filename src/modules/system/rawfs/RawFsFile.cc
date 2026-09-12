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

#include "RawFsFile.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/utilities/utility.h"

#include "RawFs.h"

class Filesystem;

RawFsFile::RawFsFile(String name, RawFs* pFs, File* pParent, Disk* pDisk)
    : File(name, 0 /* Accessed time */, 0 /* Modified time */, 0 /* Creation time */,
           0 /* Inode number */, static_cast<Filesystem*>(pFs), 0 /* Size */, pParent),
      m_pDisk(pDisk),
      m_PageCache(),
      m_PageCacheLock() {
  m_PageCache.setCallback(cacheCallback, this);

  // Owned by root:root
  setUid(0);
  setGid(0);

  // RW for root, readable only by others.
  uint32_t permissions = FILE_UR | FILE_UW | FILE_GR | FILE_OR;
  setPermissions(permissions);

  // Disk size.
  setSize(m_pDisk->getSize());
}

RawFsFile::~RawFsFile() {
  if (!shutdown())
    panic("rawfs: file cache shutdown failed; unwritten data remains");
}

bool RawFsFile::sync() {
  return m_PageCache.syncAll();
}

bool RawFsFile::sync(size_t offset, bool async) {
  if (offset >= getSize())
    return false;
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  return m_PageCache.sync(offset - offset % pageSize, async);
}

bool RawFsFile::shutdown() {
  return m_PageCache.shutdown();
}

bool RawFsFile::cacheCallback(CacheConstants::CallbackCause cause, uintptr_t location,
                              uintptr_t page, void* context) {
  auto* file = static_cast<RawFsFile*>(context);
  if (cause == CacheConstants::Eviction)
    return File::writeCallback(cause, location, page, context);
  if (cause != CacheConstants::WriteBack || !page || location >= file->getSize())
    return false;
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t length = min(pageSize, static_cast<size_t>(file->getSize() - location));
  const bool written =
      file->m_pDisk->writeFrom(location, reinterpret_cast<const void*>(page), length);
  // Successful partial writes still need a device barrier when another write failed.
  const bool durable = file->m_pDisk->syncData();
  return written && durable;
}

size_t RawFsFile::getBlockSize() const {
  // Disk block sizes describe internal I/O/readahead extents. File::readBlock
  // owns one cache page, so VFS must never expose a larger unpinned range.
  return PhysicalMemoryManager::getPageSize();
}

uintptr_t RawFsFile::readBlock(uint64_t location) {
  LockGuard<Mutex> guard(m_PageCacheLock);
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const uint64_t pageLocation = location - (location % pageSize);
  const size_t pageOffset = location - pageLocation;
  if (location >= getSize() || pageLocation >= getSize()) {
    return 0;
  }
  const size_t validLength =
      pageSize < (getSize() - pageLocation) ? pageSize : (getSize() - pageLocation);

  uintptr_t buffer = m_PageCache.lookup(pageLocation);
  if (buffer) {
    return buffer + pageOffset;
  }

  bool didExist = false;
  buffer = m_PageCache.insert(pageLocation, &didExist);
  if (!buffer) {
    return 0;
  }

  if (!didExist) {
    ByteSet(reinterpret_cast<void*>(buffer), 0, pageSize);
    size_t copied = 0;
    while (copied < validLength) {
      const uint64_t diskLocation = pageLocation + copied;
      const BufferView source = m_pDisk->read(diskLocation);
      if (!source) {
        const bool discarded = m_PageCache.discardEditing(pageLocation);
        (void)discarded;
        return 0;
      }

      const size_t remaining = validLength - copied;
      const size_t chunk = source.size() < remaining ? source.size() : remaining;
      if (!chunk) {
        m_pDisk->unpin(diskLocation);
        const bool discarded = m_PageCache.discardEditing(pageLocation);
        (void)discarded;
        return 0;
      }
      MemoryCopy(reinterpret_cast<void*>(buffer + copied), source.data(), chunk);
      m_pDisk->unpin(diskLocation);
      copied += chunk;
    }
    m_PageCache.markNoLongerEditing(pageLocation);
  }

  buffer = m_PageCache.lookup(pageLocation);
  return buffer ? buffer + pageOffset : 0;
}

void RawFsFile::writeBlock(uint64_t location, uintptr_t address) {
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const uint64_t pageLocation = location - (location % pageSize);
  if (!address || location >= getSize() || pageLocation >= getSize()) {
    return;
  }
  const size_t validLength =
      pageSize < (getSize() - pageLocation) ? pageSize : (getSize() - pageLocation);

  const uintptr_t page = readBlock(pageLocation);
  if (!page)
    return;
  if (page != address)
    MemoryCopy(reinterpret_cast<void*>(page), reinterpret_cast<const void*>(address), validLength);
  m_PageCache.markDirty(pageLocation);
  m_PageCache.release(pageLocation);
}

bool RawFsFile::pinBlock(uint64_t location) {
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  if (location >= getSize()) {
    return false;
  }
  return m_PageCache.pin(location - (location % pageSize));
}

void RawFsFile::unpinBlock(uint64_t location) {
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  if (location >= getSize()) {
    return;
  }
  m_PageCache.release(location - (location % pageSize));
}
