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
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
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
  uint32_t mode = LITTLE_TO_HOST32(inode->i_mode);
  setPermissionsOnly(modeToPermissions(mode));
  setUidOnly(LITTLE_TO_HOST16(inode->i_uid));
  setGidOnly(LITTLE_TO_HOST16(inode->i_gid));
  if (getBlockSize() < PhysicalMemoryManager::getPageSize()) {
    enableFillCacheWriteback();
  }
}

Ext2File::~Ext2File() {
  if (getBlockSize() < PhysicalMemoryManager::getPageSize()) {
    shutdownFillCacheWriteback();
  }
}

void Ext2File::preallocate(size_t expectedSize, bool zero) {
  // No need to change the actual file size, just allocate the blocks.
  Ext2Node::ensureLargeEnough(expectedSize, 0, 0, true, !zero);
}

void Ext2File::extend(size_t newSize) {
  Ext2Node::extend(newSize, 0, 0);
  m_Size = m_nSize;
}

void Ext2File::extend(size_t newSize, uint64_t location, uint64_t size) {
  Ext2Node::extend(newSize, location, size);
  m_Size = m_nSize;
}

void Ext2File::truncate() {
  // Wipe all our blocks. (Ext2Node).
  Ext2Node::wipe();
  m_Size = m_nSize;
}

void Ext2File::fileAttributeChanged() {
  static_cast<Ext2Node*>(this)->fileAttributeChanged(m_Size, m_AccessedTime, m_ModifiedTime,
                                                     m_CreationTime);
  static_cast<Ext2Node*>(this)->updateMetadata(getUid(), getGid(),
                                               permissionsToMode(getPermissions()));
}

uintptr_t Ext2File::readBlock(uint64_t location) {
  return Ext2Node::readBlock(location);
}

void Ext2File::writeBlock(uint64_t location, uintptr_t addr) {
  if (useFillCache()) {
    writeBlocks(location, addr, getBlockSize());
  } else {
    Ext2Node::writeBlock(location);
  }
}

void Ext2File::writeBlocks(uint64_t location, uintptr_t addr, size_t length) {
  if (!useFillCache()) {
    File::writeBlocks(location, addr, length);
    return;
  }

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
  return Ext2Node::pinBlock(location);
}

void Ext2File::unpinBlock(uint64_t location) {
  Ext2Node::unpinBlock(location);
}

void Ext2File::sync(size_t offset, bool async) {
  if (!useFillCache() || !syncFillCache(offset, async)) {
    Ext2Node::sync(offset, async);
    return;
  }
  if (async) {
    return;
  }

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
