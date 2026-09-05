/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Ext2Filesystem.h"
#include "Ext2Node.h"
#include "ext2.h"

bool Ext2Node::collectMappingPages(uint32_t block, unsigned depth, size_t first, size_t span,
                                   Vector<MappingPage>& pages) {
  if (!block) {
    return true;
  }
  const uintptr_t buffer = m_pExt2Fs->readBlock(block);
  if (!buffer) {
    SYSCALL_ERROR(IoError);
    return false;
  }
  pages.pushBack({block, buffer, first, span, depth});
  if (depth == 1) {
    return true;
  }
  const size_t entries = m_pExt2Fs->m_BlockSize / sizeof(uint32_t);
  const size_t childSpan = span / entries;
  const uint32_t* children = reinterpret_cast<const uint32_t*>(buffer);
  for (size_t i = 0; i < entries; ++i) {
    if (!collectMappingPages(LITTLE_TO_HOST32(children[i]), depth - 1, first + i * childSpan,
                             childSpan, pages)) {
      return false;
    }
  }
  return true;
}

bool Ext2Node::trimToBlocks(size_t keep, bool allocationLockHeld) {
  const size_t entries = m_pExt2Fs->m_BlockSize / sizeof(uint32_t);
  Vector<MappingPage> pages;
  Vector<uint32_t> retiredData;
  const bool loaded =
      collectMappingPages(LITTLE_TO_HOST32(m_pInode->i_block[12]), 1, 12, entries, pages) &&
      collectMappingPages(LITTLE_TO_HOST32(m_pInode->i_block[13]), 2, 12 + entries,
                          entries * entries, pages) &&
      collectMappingPages(LITTLE_TO_HOST32(m_pInode->i_block[14]), 3,
                          12 + entries + entries * entries, entries * entries * entries, pages);
  if (!loaded) {
    for (const MappingPage& page : pages) {
      m_pExt2Fs->unpinBlock(page.block);
    }
    return false;
  }

  size_t retainedData = 0;
  for (size_t i = 0; i < 12; ++i) {
    const uint32_t block = LITTLE_TO_HOST32(m_pInode->i_block[i]);
    if (block) {
      if (i >= keep) {
        retiredData.pushBack(block);
      } else {
        ++retainedData;
      }
    }
  }
  for (const MappingPage& page : pages) {
    if (page.depth != 1) {
      continue;
    }
    const uint32_t* children = reinterpret_cast<const uint32_t*>(page.buffer);
    for (size_t i = 0; i < entries; ++i) {
      const uint32_t block = LITTLE_TO_HOST32(children[i]);
      if (block) {
        if (page.first + i >= keep) {
          retiredData.pushBack(block);
        } else {
          ++retainedData;
        }
      }
    }
  }

  // Every mapping page is pinned and all storage for retirement is allocated.
  // Detach pointers before returning their blocks to the allocation bitmap.
  for (size_t i = keep; i < 12; ++i) {
    m_pInode->i_block[i] = 0;
  }
  const size_t firstIndices[3] = {12, 12 + entries, 12 + entries + entries * entries};
  for (size_t i = 0; i < 3; ++i) {
    if (firstIndices[i] >= keep) {
      m_pInode->i_block[12 + i] = 0;
    }
  }
  size_t retainedMetadata = 0;
  for (const MappingPage& page : pages) {
    uint32_t* children = reinterpret_cast<uint32_t*>(page.buffer);
    const size_t childSpan = page.span / entries;
    for (size_t i = 0; i < entries; ++i) {
      if (page.first + i * childSpan >= keep) {
        children[i] = 0;
      }
    }
    if (page.first < keep) {
      ++retainedMetadata;
      m_pExt2Fs->writeBlock(page.block);
    }
  }
  while (m_Blocks.count() > keep) {
    m_Blocks.popBack();
  }
  m_nMetadataBlocks = retainedMetadata;
  m_State->allocatedDataBlocks = retainedData;
  m_pInode->i_blocks =
      HOST_TO_LITTLE32((retainedData + retainedMetadata) * (m_pExt2Fs->m_BlockSize / 512));
  m_pExt2Fs->writeInode(getInodeNumber());

  for (uint32_t block : retiredData) {
    if (allocationLockHeld) {
      m_pExt2Fs->releaseBlockLocked(block);
    } else {
      m_pExt2Fs->releaseBlock(block);
    }
  }
  for (const MappingPage& page : pages) {
    m_pExt2Fs->unpinBlock(page.block);
    if (page.first >= keep) {
      if (allocationLockHeld) {
        m_pExt2Fs->releaseBlockLocked(page.block);
      } else {
        m_pExt2Fs->releaseBlock(page.block);
      }
    }
  }
  return true;
}

bool Ext2Node::zeroRange(size_t start, size_t end) {
  const size_t blockSize = m_pExt2Fs->m_BlockSize;
  while (start < end) {
    const size_t block = start / blockSize;
    const size_t within = start % blockSize;
    const size_t amount = (end - start < blockSize - within) ? end - start : blockSize - within;
    if (block >= m_Blocks.count() || !ensureBlockLoaded(block)) {
      SYSCALL_ERROR(IoError);
      return false;
    }
    if (m_Blocks[block]) {
      const uintptr_t buffer = m_pExt2Fs->readBlock(m_Blocks[block]);
      if (!buffer) {
        SYSCALL_ERROR(IoError);
        return false;
      }
      ByteSet(reinterpret_cast<void*>(buffer + within), 0, amount);
      m_pExt2Fs->writeBlock(m_Blocks[block]);
      m_pExt2Fs->unpinBlock(m_Blocks[block]);
    }
    start += amount;
  }
  return true;
}

bool Ext2Node::resizeData(size_t size) {
  if (size > 0xffffffffULL) {
    SYSCALL_ERROR(FileTooLarge);
    return false;
  }
  if (size >= m_nSize) {
    return ensureLargeEnough(size, 0, 0);
  }
  const size_t blockSize = m_pExt2Fs->m_BlockSize;
  const size_t keep = size / blockSize + (size % blockSize != 0);
  // Preserve the old visible bytes if any backing read fails during preflight.
  uintptr_t tail = 0;
  uint32_t tailBlock = 0;
  if (size % blockSize) {
    if (!ensureBlockLoaded(size / blockSize)) {
      SYSCALL_ERROR(IoError);
      return false;
    }
    tailBlock = m_Blocks[size / blockSize];
    if (tailBlock) {
      tail = m_pExt2Fs->readBlock(tailBlock);
      if (!tail) {
        SYSCALL_ERROR(IoError);
        return false;
      }
    }
  }
  if (!trimToBlocks(keep)) {
    if (tail) {
      m_pExt2Fs->unpinBlock(tailBlock);
    }
    return false;
  }
  if (tail) {
    const size_t within = size % blockSize;
    ByteSet(reinterpret_cast<void*>(tail + within), 0, blockSize - within);
    m_pExt2Fs->writeBlock(tailBlock);
    m_pExt2Fs->unpinBlock(tailBlock);
  }
  m_nSize = size;
  m_pInode->i_size = HOST_TO_LITTLE32(size);
  m_pExt2Fs->writeInode(getInodeNumber());
  return true;
}
