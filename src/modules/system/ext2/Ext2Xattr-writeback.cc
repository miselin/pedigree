/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/assert.h"

#include "Ext2Filesystem.h"
#include "Ext2Node.h"
#include "Ext2Xattr.h"
#include "ext2.h"

Ext2Filesystem::AttributeRetirement::~AttributeRetirement() {
  if (buffer)
    filesystem->unpinBlock(block);
  if (provisional)
    filesystem->releaseBlockLocked(block);
}

XattrStatus Ext2Filesystem::attributeFormatStatus() const {
  if (!m_pSuperblock || !m_BlockSize)
    return XattrStatus::IoError;
  // The external Ext2 layout does not describe Ext3/4 journals, checksums, or EA inodes.
  if (LITTLE_TO_HOST32(m_pSuperblock->s_rev_level) != 1 ||
      (LITTLE_TO_HOST32(m_pSuperblock->s_feature_compat) & 4) ||
      (LITTLE_TO_HOST32(m_pSuperblock->s_feature_incompat) & ~uint32_t(2)) ||
      (LITTLE_TO_HOST32(m_pSuperblock->s_feature_ro_compat) & ~uint32_t(7)))
    return XattrStatus::Unsupported;
  return XattrStatus::Success;
}

XattrStatus Ext2Filesystem::readAttributeBlockLocked(Inode* inode, AttributeRetirement& plan) {
  plan.filesystem = this;
  plan.block = LITTLE_TO_HOST32(inode->i_file_acl);
  if (!plan.block)
    return XattrStatus::Success;
  const uint32_t first = LITTLE_TO_HOST32(m_pSuperblock->s_first_data_block);
  const uint32_t perGroup = LITTLE_TO_HOST32(m_pSuperblock->s_blocks_per_group);
  if (plan.block <= first || plan.block >= LITTLE_TO_HOST32(m_pSuperblock->s_blocks_count) ||
      !perGroup || (plan.block - first) / perGroup >= m_nGroupDescriptors)
    return XattrStatus::IoError;
  // Never interpret filesystem allocation metadata as a writable EA payload.
  const uint32_t descriptorEnd =
      first + 1 + (m_nGroupDescriptors * sizeof(GroupDesc) + m_BlockSize - 1) / m_BlockSize;
  if (plan.block < descriptorEnd)
    return XattrStatus::IoError;
  const uint32_t tableBlocks =
      (static_cast<uint64_t>(LITTLE_TO_HOST32(m_pSuperblock->s_inodes_per_group)) * m_InodeSize +
       m_BlockSize - 1) /
      m_BlockSize;
  for (size_t n = 0; n < m_nGroupDescriptors; ++n) {
    const auto* descriptor = m_pGroupDescriptors[n];
    const uint32_t table = LITTLE_TO_HOST32(descriptor->bg_inode_table);
    if (plan.block == LITTLE_TO_HOST32(descriptor->bg_block_bitmap) ||
        plan.block == LITTLE_TO_HOST32(descriptor->bg_inode_bitmap) ||
        (plan.block >= table && plan.block - table < tableBlocks))
      return XattrStatus::IoError;
  }
  plan.buffer = readBlock(plan.block);
  if (!plan.buffer)
    return XattrStatus::IoError;
  return Ext2Ea::validate(reinterpret_cast<void*>(plan.buffer), m_BlockSize);
}

XattrStatus Ext2Filesystem::prepareAttributeRetirementLocked(Inode* inode,
                                                             AttributeRetirement& plan) {
  auto status = readAttributeBlockLocked(inode, plan);
  if (status != XattrStatus::Success || !plan.block)
    return status;
  if (!prepareBlockReleaseLocked(plan.block))
    return XattrStatus::IoError;
  const uint32_t relative = plan.block - LITTLE_TO_HOST32(m_pSuperblock->s_first_data_block);
  const uint32_t perGroup = LITTLE_TO_HOST32(m_pSuperblock->s_blocks_per_group);
  const uint32_t index = relative % perGroup;
  const auto& bitmap = m_pBlockBitmaps[relative / perGroup];
  const auto* byte = reinterpret_cast<const uint8_t*>(bitmap[(index / 8) / m_BlockSize]);
  if (!(byte[(index / 8) % m_BlockSize] & (1U << (index % 8))))
    return XattrStatus::IoError;
  return XattrStatus::Success;
}

XattrStatus Ext2Filesystem::reserveAttributeWritesLocked(size_t additional) {
  if (additional > MaximumAttributeWrites)
    return XattrStatus::NoSpace;
  if (m_AttributeWriteCount + additional > MaximumAttributeWrites && !flushAttributeWritesLocked())
    return XattrStatus::IoError;
  return XattrStatus::Success;
}

void Ext2Filesystem::recordAttributeWriteLocked(uint64_t location, AttributeWriteKind kind,
                                                bool adoptPin) {
  for (size_t n = 0; n < m_AttributeWriteCount; ++n) {
    if (m_AttributeWrites[n].location != location)
      continue;
    if (adoptPin) {
      if (m_AttributeWrites[n].ownsPin)
        m_pDisk->unpin(location);
      else
        m_AttributeWrites[n].ownsPin = true;
    }
    return;
  }
  assert(m_AttributeWriteCount < MaximumAttributeWrites);
  m_AttributeWrites[m_AttributeWriteCount++] = {location, kind, adoptPin};
}

void Ext2Filesystem::recordAttributeAllocationLocked(uint32_t block) {
  const uint32_t first = LITTLE_TO_HOST32(m_pSuperblock->s_first_data_block);
  const uint32_t perGroup = LITTLE_TO_HOST32(m_pSuperblock->s_blocks_per_group);
  const uint32_t group = (block - first) / perGroup;
  const uint32_t index = (block - first) % perGroup;
  const uint32_t bitmap =
      LITTLE_TO_HOST32(m_pGroupDescriptors[group]->bg_block_bitmap) + (index / 8) / m_BlockSize;
  recordAttributeWriteLocked(static_cast<uint64_t>(bitmap) * m_BlockSize,
                             AttributeWriteKind::Allocation);
  const uint32_t descriptor = first + 1 + (group * sizeof(GroupDesc)) / m_BlockSize;
  recordAttributeWriteLocked(static_cast<uint64_t>(descriptor) * m_BlockSize,
                             AttributeWriteKind::Allocation);
  recordAttributeWriteLocked(1024, AttributeWriteKind::Allocation);
}

void Ext2Filesystem::recordAttributeInodeLocked(uint32_t inode) {
  const uint32_t perGroup = LITTLE_TO_HOST32(m_pSuperblock->s_inodes_per_group);
  const uint32_t group = (inode - 1) / perGroup, index = (inode - 1) % perGroup;
  const uint32_t block = LITTLE_TO_HOST32(m_pGroupDescriptors[group]->bg_inode_table) +
                         (static_cast<uint64_t>(index) * m_InodeSize) / m_BlockSize;
  recordAttributeWriteLocked(static_cast<uint64_t>(block) * m_BlockSize, AttributeWriteKind::Inode);
}

void Ext2Filesystem::forgetAttributeBlockLocked(uint32_t block) {
  const uint64_t location = static_cast<uint64_t>(block) * m_BlockSize;
  for (size_t n = 0; n < m_AttributeWriteCount; ++n) {
    if (m_AttributeWrites[n].location != location)
      continue;
    assert(m_AttributeWrites[n].kind == AttributeWriteKind::Payload);
    if (m_AttributeWrites[n].ownsPin)
      m_pDisk->unpin(location);
    m_AttributeWrites[n] = m_AttributeWrites[--m_AttributeWriteCount];
    return;
  }
}

bool Ext2Filesystem::flushAttributeWritesLocked() {
  const AttributeWriteKind order[] = {AttributeWriteKind::Payload, AttributeWriteKind::Allocation,
                                      AttributeWriteKind::Inode};
  for (auto kind : order) {
    for (size_t n = 0; n < m_AttributeWriteCount; ++n) {
      if (m_AttributeWrites[n].kind == kind && !m_pDisk->sync(m_AttributeWrites[n].location, false))
        return false;
    }
  }
  for (size_t n = 0; n < m_AttributeWriteCount; ++n) {
    if (m_AttributeWrites[n].ownsPin)
      m_pDisk->unpin(m_AttributeWrites[n].location);
  }
  m_AttributeWriteCount = 0;
  return true;
}

void Ext2Filesystem::drainAttributeWrites() {
  LockGuard<Mutex> guard(m_WriteLock);
  if (flushAttributeWritesLocked())
    return;
  ERROR("Ext2: attribute metadata writeback failed at filesystem teardown");
  for (size_t n = 0; n < m_AttributeWriteCount; ++n) {
    if (m_AttributeWrites[n].ownsPin)
      m_pDisk->unpin(m_AttributeWrites[n].location);
  }
  m_AttributeWriteCount = 0;
}

XattrStatus Ext2Filesystem::allocateAttributeBlockLocked(uint32_t inode,
                                                         AttributeRetirement& plan) {
  Vector<uint32_t> block;
  if (!block.tryReserve(1))
    return XattrStatus::NoMemory;
  const uint32_t perGroup = LITTLE_TO_HOST32(m_pSuperblock->s_inodes_per_group);
  if (!inode || !perGroup || !m_nGroupDescriptors)
    return XattrStatus::IoError;
  const size_t start = ((inode - 1) / perGroup) % m_nGroupDescriptors;
  for (size_t n = 0; n < m_nGroupDescriptors; ++n) {
    const size_t group = (start + n) % m_nGroupDescriptors;
    if (!LITTLE_TO_HOST16(m_pGroupDescriptors[group]->bg_free_blocks_count))
      continue;
    if (!ensureFreeBlockBitmapLoaded(group))
      return XattrStatus::IoError;
    if (!findFreeBlocksInGroup(group, 1, block))
      continue;
    plan.filesystem = this;
    plan.block = block[0];
    plan.buffer = readBlock(plan.block);
    if (!plan.buffer) {
      releaseBlockLocked(plan.block);
      plan.block = 0;
      return XattrStatus::IoError;
    }
    plan.provisional = true;
    return XattrStatus::Success;
  }
  return XattrStatus::NoSpace;
}

void Ext2Filesystem::commitAttributeRetirementLocked(AttributeRetirement& plan) {
  if (!plan.block)
    return;
  auto* header = reinterpret_cast<Ext2Ea::Header*>(plan.buffer);
  const uint32_t references = LITTLE_TO_HOST32(header->references);
  assert(references);
  if (references == 1) {
    forgetAttributeBlockLocked(plan.block);
    releaseBlockLocked(plan.block);
    recordAttributeAllocationLocked(plan.block);
  } else {
    header->references = HOST_TO_LITTLE32(references - 1);
    writeBlock(plan.block);
    recordAttributeWriteLocked(static_cast<uint64_t>(plan.block) * m_BlockSize,
                               AttributeWriteKind::Payload, true);
    plan.buffer = 0;
  }
}
