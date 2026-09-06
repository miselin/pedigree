/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/utilities/Pointers.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Ext2Directory.h"
#include "Ext2File.h"
#include "Ext2Filesystem.h"
#include "Ext2Symlink.h"
#include "Ext2Xattr.h"
#include "ext2.h"

XattrStatus Ext2Node::changeXattr(const StringView& name, const void* value, size_t length,
                                  unsigned flags, bool remove) {
  auto replacement = UniqueArray<uint8_t>::allocate(m_pExt2Fs->m_BlockSize);
  if (!replacement)
    return XattrStatus::NoMemory;
  LockGuard<Mutex> inodeGuard(m_State->writebackLock);
  LockGuard<Mutex> allocationGuard(m_pExt2Fs->m_WriteLock);
  if (m_pExt2Fs->isReadOnly())
    return XattrStatus::ReadOnly;
  if (!m_State->allocationValid)
    return XattrStatus::IoError;
  auto status = m_pExt2Fs->attributeFormatStatus();
  if (status != XattrStatus::Success)
    return status;
  Ext2Filesystem::AttributeRetirement old;
  status = m_pExt2Fs->prepareAttributeRetirementLocked(m_pInode, old);
  if (status != XattrStatus::Success)
    return status;
  bool empty = false;
  status = Ext2Ea::rebuild(reinterpret_cast<const void*>(old.buffer), m_pExt2Fs->m_BlockSize, name,
                           value, length, flags, remove, replacement.get(), empty);
  if (status != XattrStatus::Success)
    return status;
  uint32_t sectors = 0;
  if (!encodeAllocation(m_State->allocatedDataBlocks, m_nMetadataBlocks, !empty,
                        m_pExt2Fs->m_BlockSize, sectors))
    return XattrStatus::NoSpace;
  if (!m_pExt2Fs->prepareInodeWrite(m_InodeNumber))
    return XattrStatus::IoError;
  // At most two payloads, two allocation groups, the superblock and inode change.
  status = m_pExt2Fs->reserveAttributeWritesLocked(12);
  if (status != XattrStatus::Success)
    return status;
  Ext2Filesystem::AttributeRetirement allocated;
  auto* destination = &old;
  if (!empty &&
      (!old.block ||
       LITTLE_TO_HOST32(reinterpret_cast<const Ext2Ea::Header*>(old.buffer)->references) > 1)) {
    status = m_pExt2Fs->allocateAttributeBlockLocked(m_InodeNumber, allocated);
    if (status != XattrStatus::Success)
      return status;
    destination = &allocated;
  }
  const uint32_t newBlock = empty ? 0 : destination->block;
  if (!empty) {
    MemoryCopy(reinterpret_cast<void*>(destination->buffer), replacement.get(),
               m_pExt2Fs->m_BlockSize);
    m_pExt2Fs->writeBlock(newBlock);
    m_pExt2Fs->recordAttributeWriteLocked(static_cast<uint64_t>(newBlock) * m_pExt2Fs->m_BlockSize,
                                          Ext2Filesystem::AttributeWriteKind::Payload, true);
    destination->buffer = 0;
    destination->provisional = false;
    if (destination == &allocated)
      m_pExt2Fs->recordAttributeAllocationLocked(newBlock);
  }
  m_pInode->i_file_acl = HOST_TO_LITTLE32(newBlock);
  m_pInode->i_blocks = HOST_TO_LITTLE32(sectors);
  m_pInode->i_ctime = HOST_TO_LITTLE32(Time::getTime());
  m_pExt2Fs->m_pSuperblock->s_feature_compat |= HOST_TO_LITTLE32(EXT2_FEATURE_COMPAT_EXT_ATTR);
  m_pExt2Fs->getDisk()->write(1024);
  m_pExt2Fs->recordAttributeWriteLocked(1024, Ext2Filesystem::AttributeWriteKind::Allocation);
  m_pExt2Fs->writeInode(m_InodeNumber);
  m_pExt2Fs->recordAttributeInodeLocked(m_InodeNumber);
  if (old.block && old.block != newBlock)
    m_pExt2Fs->commitAttributeRetirementLocked(old);
  return XattrStatus::Success;
}

XattrStatus Ext2Node::setXattr(const StringView& name, const void* value, size_t length,
                               unsigned flags) {
  return changeXattr(name, value, length, flags, false);
}
XattrStatus Ext2Node::removeXattr(const StringView& name) {
  return changeXattr(name, nullptr, 0, 0, true);
}

XattrStatus Ext2File::getExtendedAttribute(const StringView& name, void* output, size_t capacity,
                                           size_t& required) {
  return getXattr(name, output, capacity, required);
}
XattrStatus Ext2File::listExtendedAttributes(void* output, size_t capacity, size_t& required) {
  return listXattrs(output, capacity, required);
}
XattrStatus Ext2File::setExtendedAttribute(const StringView& name, const void* value, size_t length,
                                           unsigned flags) {
  const auto status = setXattr(name, value, length, flags);
  if (status == XattrStatus::Success)
    publishEvent(FileEvents::Attributes);
  return status;
}
XattrStatus Ext2File::removeExtendedAttribute(const StringView& name) {
  const auto status = removeXattr(name);
  if (status == XattrStatus::Success)
    publishEvent(FileEvents::Attributes);
  return status;
}
XattrStatus Ext2Directory::getExtendedAttribute(const StringView& name, void* output,
                                                size_t capacity, size_t& required) {
  return getXattr(name, output, capacity, required);
}
XattrStatus Ext2Directory::listExtendedAttributes(void* output, size_t capacity, size_t& required) {
  return listXattrs(output, capacity, required);
}
XattrStatus Ext2Directory::setExtendedAttribute(const StringView& name, const void* value,
                                                size_t length, unsigned flags) {
  const auto status = setXattr(name, value, length, flags);
  if (status == XattrStatus::Success)
    publishEvent(FileEvents::Attributes);
  return status;
}
XattrStatus Ext2Directory::removeExtendedAttribute(const StringView& name) {
  const auto status = removeXattr(name);
  if (status == XattrStatus::Success)
    publishEvent(FileEvents::Attributes);
  return status;
}
XattrStatus Ext2Symlink::getExtendedAttribute(const StringView& name, void* output, size_t capacity,
                                              size_t& required) {
  return getXattr(name, output, capacity, required);
}
XattrStatus Ext2Symlink::listExtendedAttributes(void* output, size_t capacity, size_t& required) {
  return listXattrs(output, capacity, required);
}
XattrStatus Ext2Symlink::setExtendedAttribute(const StringView& name, const void* value,
                                              size_t length, unsigned flags) {
  const auto status = setXattr(name, value, length, flags);
  if (status == XattrStatus::Success)
    publishEvent(FileEvents::Attributes);
  return status;
}
XattrStatus Ext2Symlink::removeExtendedAttribute(const StringView& name) {
  const auto status = removeXattr(name);
  if (status == XattrStatus::Success)
    publishEvent(FileEvents::Attributes);
  return status;
}
