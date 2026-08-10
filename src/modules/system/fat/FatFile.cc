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

#include "FatFilesystem.h"

FatFile::FatFile(String name, Time::Timestamp accessedTime, Time::Timestamp modifiedTime,
                 Time::Timestamp creationTime, uintptr_t inode, class Filesystem* pFs, size_t size,
                 uint32_t dirClus, uint32_t dirOffset, File* pParent)
    : File(name, accessedTime, modifiedTime, creationTime, inode, pFs, size, pParent),
      m_DirClus(dirClus),
      m_DirOffset(dirOffset),
      m_FileBlockCache(),
      m_FileBlockCacheLock() {
  m_FileBlockCache.setCallback(writeCallback, static_cast<File*>(this));

  // No permissions on FAT - set all to RWX.
  setPermissions(FILE_UR | FILE_UW | FILE_UX | FILE_GR | FILE_GW | FILE_GX | FILE_OR | FILE_OW |
                 FILE_OX);
}

FatFile::~FatFile() {
  m_FileBlockCache.shutdown();
}

uintptr_t FatFile::readBlock(uint64_t location) {
  LockGuard<Mutex> guard(m_FileBlockCacheLock);
  FatFilesystem* pFs = static_cast<FatFilesystem*>(m_pFilesystem);

  uintptr_t buffer = m_FileBlockCache.lookup(location);
  if (buffer) {
    return buffer;
  }

  bool didExist = false;
  buffer = m_FileBlockCache.insert(location, &didExist);
  if (!buffer) {
    return 0;
  }

  if (!didExist) {
    if (location >= getSize()) {
      const bool discarded = m_FileBlockCache.discardEditing(location);
      (void)discarded;
      return 0;
    }

    const size_t expected =
        ((getSize() - location) < getBlockSize()) ? (getSize() - location) : getBlockSize();
    ByteSet(reinterpret_cast<void*>(buffer), 0, getBlockSize());
    const uint64_t bytesRead = pFs->read(this, location, getBlockSize(), buffer);
    if (bytesRead != expected) {
      if (!m_FileBlockCache.discardEditing(location)) {
        WARNING("FatFile::readBlock could not discard a failed fill at " << location);
      }
      return 0;
    }
    m_FileBlockCache.markNoLongerEditing(location);
  }

  return m_FileBlockCache.lookup(location);
}

void FatFile::writeBlock(uint64_t location, uintptr_t addr) {
  FatFilesystem* pFs = static_cast<FatFilesystem*>(m_pFilesystem);

  // Don't accidentally extend the file when writing the block.
  size_t sz = getBlockSize();
  uint64_t end = location + sz;
  if (end > getSize())
    sz = getSize() - location;
  pFs->write(this, location, sz, addr);
}

void FatFile::sync(size_t offset, bool async) {
  m_FileBlockCache.sync(offset, async);
}

bool FatFile::pinBlock(uint64_t location) {
  return m_FileBlockCache.pin(location);
}

void FatFile::unpinBlock(uint64_t location) {
  m_FileBlockCache.release(location);
}

void FatFile::extend(size_t newSize) {
  FatFilesystem* pFs = static_cast<FatFilesystem*>(m_pFilesystem);

  if (m_Size < newSize) {
    pFs->extend(this, newSize);
    m_Size = newSize;
  }
}

void FatFile::extend(size_t newSize, uint64_t location, uint64_t size) {
  // not using the hints at all
  extend(newSize);
}
