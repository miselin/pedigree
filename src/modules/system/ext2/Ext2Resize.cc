/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/new"
#include "pedigree/kernel/utilities/utility.h"

#include "Ext2File.h"
#include "Ext2Filesystem.h"
#include "Ext2Node.h"
#include "ext2.h"

namespace {
template <typename T>
bool appendPrepared(Vector<T>& values, const T& value) {
  if (values.count() == values.size() &&
      !values.tryReserve(values.size() ? values.size() * 2 : 16)) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  values.pushBack(value);
  return true;
}
}  // namespace

Ext2Node::TrimPlan::TrimPlan(Ext2Filesystem& filesystem) : filesystem(filesystem) {}

Ext2Node::TrimPlan::~TrimPlan() {
  for (const MappingPage& page : pages) {
    if (page.buffer)
      filesystem.unpinBlock(page.block);
  }
}

bool Ext2Node::collectMappingPages(uint32_t block, unsigned depth, size_t first, size_t span,
                                   Vector<MappingPage>& pages) {
  if (!block)
    return true;
  const uintptr_t buffer = m_pExt2Fs->readBlock(block);
  if (!buffer) {
    SYSCALL_ERROR(IoError);
    return false;
  }
  if (!appendPrepared(pages, MappingPage{block, buffer, first, span, depth})) {
    m_pExt2Fs->unpinBlock(block);
    return false;
  }
  if (depth == 1)
    return true;
  const size_t entries = m_pExt2Fs->m_BlockSize / sizeof(uint32_t);
  const size_t childSpan = span / entries;
  const uint32_t* children = reinterpret_cast<const uint32_t*>(buffer);
  for (size_t i = 0; i < entries; ++i) {
    if (!collectMappingPages(LITTLE_TO_HOST32(children[i]), depth - 1, first + i * childSpan,
                             childSpan, pages))
      return false;
  }
  return true;
}

bool Ext2Node::prepareTrim(size_t keep, TrimPlan& plan, bool allocationLockHeld) {
  if (!m_State->allocationValid) {
    SYSCALL_ERROR(IoError);
    return false;
  }
  plan.keep = keep;
  const size_t entries = m_pExt2Fs->m_BlockSize / sizeof(uint32_t);
  if (!collectMappingPages(LITTLE_TO_HOST32(m_pInode->i_block[12]), 1, 12, entries, plan.pages) ||
      !collectMappingPages(LITTLE_TO_HOST32(m_pInode->i_block[13]), 2, 12 + entries,
                           entries * entries, plan.pages) ||
      !collectMappingPages(LITTLE_TO_HOST32(m_pInode->i_block[14]), 3,
                           12 + entries + entries * entries, entries * entries * entries,
                           plan.pages))
    return false;
  for (size_t i = 0; i < 12; ++i) {
    const uint32_t block = LITTLE_TO_HOST32(m_pInode->i_block[i]);
    if (block) {
      if (i >= keep) {
        if (!appendPrepared(plan.retiredData, block))
          return false;
      } else {
        ++plan.retainedData;
      }
    }
  }
  for (const MappingPage& page : plan.pages) {
    if (page.depth != 1)
      continue;
    const uint32_t* children = reinterpret_cast<const uint32_t*>(page.buffer);
    for (size_t i = 0; i < entries; ++i) {
      const uint32_t block = LITTLE_TO_HOST32(children[i]);
      if (block) {
        if (page.first + i >= keep) {
          if (!appendPrepared(plan.retiredData, block))
            return false;
        } else {
          ++plan.retainedData;
        }
      }
    }
  }
  LockGuard<Mutex> allocationGuard(m_pExt2Fs->m_WriteLock, !allocationLockHeld);
  if (!m_pExt2Fs->prepareInodeWrite(getInodeNumber()))
    return false;
  for (uint32_t block : plan.retiredData) {
    if (!m_pExt2Fs->prepareBlockReleaseLocked(block))
      return false;
  }
  for (const MappingPage& page : plan.pages) {
    if (page.first >= keep && !m_pExt2Fs->prepareBlockReleaseLocked(page.block))
      return false;
  }
  return true;
}

void Ext2Node::commitTrim(TrimPlan& plan, bool allocationLockHeld) {
  const size_t keep = plan.keep;
  const size_t entries = m_pExt2Fs->m_BlockSize / sizeof(uint32_t);
  LockGuard<Mutex> allocationGuard(m_pExt2Fs->m_WriteLock, !allocationLockHeld);
  // Every read, metadata pin, and retirement journal allocation has succeeded.
  for (size_t i = keep; i < 12; ++i)
    m_pInode->i_block[i] = 0;
  const size_t firstIndices[3] = {12, 12 + entries, 12 + entries + entries * entries};
  for (size_t i = 0; i < 3; ++i) {
    if (firstIndices[i] >= keep)
      m_pInode->i_block[12 + i] = 0;
  }
  size_t retainedMetadata = 0;
  for (const MappingPage& page : plan.pages) {
    uint32_t* children = reinterpret_cast<uint32_t*>(page.buffer);
    const size_t childSpan = page.span / entries;
    for (size_t i = 0; i < entries; ++i) {
      if (page.first + i * childSpan >= keep)
        children[i] = 0;
    }
    if (page.first < keep) {
      ++retainedMetadata;
      m_pExt2Fs->writeBlock(page.block);
    }
  }
  while (m_Blocks.count() > keep)
    m_Blocks.popBack();
  m_nMetadataBlocks = retainedMetadata;
  m_State->allocatedDataBlocks = plan.retainedData;
  updateAllocatedSectorCount();
  m_pExt2Fs->writeInode(getInodeNumber());
  for (uint32_t block : plan.retiredData)
    m_pExt2Fs->releaseBlockLocked(block, m_InodeNumber);
  for (MappingPage& page : plan.pages) {
    m_pExt2Fs->unpinBlock(page.block);
    page.buffer = 0;
    if (page.first >= keep)
      m_pExt2Fs->releaseBlockLocked(page.block, m_InodeNumber);
  }
}

bool Ext2Node::trimToBlocks(size_t keep, bool allocationLockHeld) {
  TrimPlan plan(*m_pExt2Fs);
  if (!prepareTrim(keep, plan, allocationLockHeld))
    return false;
  commitTrim(plan, allocationLockHeld);
  return true;
}

class Ext2Node::DataShrinkPlan : public File::PreparedShrink {
 public:
  DataShrinkPlan(Ext2Node& node, size_t size)
      : node(node), size(size), trim(*node.m_pExt2Fs), tail(0), tailBlock(0) {}
  ~DataShrinkPlan() override {
    if (tail)
      node.m_pExt2Fs->unpinBlock(tailBlock);
  }
  void commit() override {
    LockGuard<Mutex> guard(node.m_State->writebackLock);
    node.commitTrim(trim);
    if (tail) {
      const size_t within = size % node.m_pExt2Fs->m_BlockSize;
      ByteSet(reinterpret_cast<void*>(tail + within), 0, node.m_pExt2Fs->m_BlockSize - within);
      node.m_pExt2Fs->writeBlock(tailBlock);
    }
    node.m_nSize = size;
    node.m_pInode->i_size = HOST_TO_LITTLE32(size);
    node.m_pExt2Fs->writeInode(node.getInodeNumber());
    for (Ext2File* alias : node.m_State->files)
      alias->setSize(size);
  }
  Ext2Node& node;
  size_t size;
  TrimPlan trim;
  uintptr_t tail;
  uint32_t tailBlock;
};

bool Ext2Node::prepareDataShrink(size_t size, UniquePointer<File::PreparedShrink>& prepared) {
  DataShrinkPlan* plan = new DataShrinkPlan(*this, size);
  auto owner = UniquePointer<File::PreparedShrink>::adopt(plan);
  if (!plan) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  const size_t blockSize = m_pExt2Fs->m_BlockSize;
  if (size % blockSize) {
    const size_t index = size / blockSize;
    if (index >= m_Blocks.count() || !ensureBlockLoaded(index)) {
      SYSCALL_ERROR(IoError);
      return false;
    }
    plan->tailBlock = m_Blocks[index];
    if (plan->tailBlock) {
      plan->tail = m_pExt2Fs->readBlock(plan->tailBlock);
      if (!plan->tail) {
        SYSCALL_ERROR(IoError);
        return false;
      }
    }
  }
  if (!prepareTrim(size / blockSize + (size % blockSize != 0), plan->trim))
    return false;
  prepared = pedigree_std::move(owner);
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
  if (size > m_nSize) {
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
