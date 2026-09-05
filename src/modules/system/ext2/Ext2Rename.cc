/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/utility.h"

#include <stddef.h>

#include "Ext2Directory.h"
#include "Ext2File.h"
#include "Ext2Filesystem.h"
#include "Ext2Symlink.h"
#include "ext2.h"

Ext2Directory::RenameRecord::RenameRecord()
    : owner(nullptr),
      block(0),
      buffer(0),
      offset(0),
      entry(nullptr),
      split(nullptr),
      splitLength(0),
      length(0) {}

Ext2Directory::RenameRecord::~RenameRecord() {
  if (buffer) {
    owner->m_pExt2Fs->unpinBlock(block);
  }
}

bool Ext2Directory::prepareRenameRecord(const String& name, uint32_t inode, RenameRecord& record) {
  for (size_t offset = 0; offset < m_nSize;) {
    ParsedEntry entry;
    if (readEntry(offset, entry) != ReadStatus::Complete) {
      SYSCALL_ERROR(IoError);
      return false;
    }
    if (entry.inode == inode && name.length() == entry.nameLength &&
        !StringCompareN(name.cstr(), entry.name, entry.nameLength)) {
      const size_t blockIndex = offset / m_pExt2Fs->m_BlockSize;
      if (!ensureBlockLoaded(blockIndex)) {
        SYSCALL_ERROR(IoError);
        return false;
      }
      record.owner = this;
      record.block = m_Blocks[blockIndex];
      record.buffer = m_pExt2Fs->readBlock(record.block);
      if (!record.buffer) {
        SYSCALL_ERROR(IoError);
        return false;
      }
      record.offset = offset;
      record.entry = reinterpret_cast<Dir*>(record.buffer + offset % m_pExt2Fs->m_BlockSize);
      record.length = entry.recordLength;
      return true;
    }
    offset += entry.recordLength;
  }
  SYSCALL_ERROR(DoesNotExist);
  return false;
}

bool Ext2Directory::prepareRenameSpace(const String& name, RenameRecord& record,
                                       const RenameRecord* source) {
  const size_t required = (offsetof(Dir, d_name) + name.length() + 3) & ~static_cast<size_t>(3);
  if (source && source->length >= required) {
    record.owner = this;
    record.block = source->block;
    record.buffer = m_pExt2Fs->readBlock(record.block);
    if (!record.buffer) {
      SYSCALL_ERROR(IoError);
      return false;
    }
    record.offset = source->offset;
    record.entry = reinterpret_cast<Dir*>(record.buffer + source->offset % m_pExt2Fs->m_BlockSize);
    record.length = source->length;
    return true;
  }
  for (size_t offset = 0; offset < m_nSize;) {
    ParsedEntry entry;
    if (readEntry(offset, entry) != ReadStatus::Complete) {
      SYSCALL_ERROR(IoError);
      return false;
    }
    const size_t minimum = (offsetof(Dir, d_name) + entry.nameLength + 3) & ~static_cast<size_t>(3);
    const bool usable =
        !entry.inode ? entry.recordLength >= required : entry.recordLength - minimum >= required;
    if (usable && (!source || source->offset != offset)) {
      const size_t blockIndex = offset / m_pExt2Fs->m_BlockSize;
      if (!ensureBlockLoaded(blockIndex)) {
        SYSCALL_ERROR(IoError);
        return false;
      }
      record.owner = this;
      record.block = m_Blocks[blockIndex];
      record.buffer = m_pExt2Fs->readBlock(record.block);
      if (!record.buffer) {
        SYSCALL_ERROR(IoError);
        return false;
      }
      record.offset = offset;
      record.entry = reinterpret_cast<Dir*>(record.buffer + offset % m_pExt2Fs->m_BlockSize);
      record.length = entry.recordLength;
      if (entry.inode) {
        record.split = record.entry;
        record.splitLength = minimum;
        record.length -= minimum;
        record.entry = reinterpret_cast<Dir*>(reinterpret_cast<uintptr_t>(record.entry) + minimum);
      }
      return true;
    }
    offset += entry.recordLength;
  }

  const size_t oldBlocks = m_Blocks.count();
  const size_t entries = m_pExt2Fs->m_BlockSize / sizeof(uint32_t);
  if (oldBlocks >= 12 + entries + entries * entries) {
    SYSCALL_ERROR(FileTooLarge);
    return false;
  }
  record.owner = this;
  record.block = m_pExt2Fs->findFreeBlock(getInodeNumber());
  if (!record.block) {
    SYSCALL_ERROR(NoSpaceLeftOnDevice);
    return false;
  }
  record.buffer = m_pExt2Fs->readBlock(record.block);
  if (!record.buffer) {
    m_pExt2Fs->releaseBlock(record.block);
    SYSCALL_ERROR(IoError);
    return false;
  }
  if (!addBlock(record.block)) {
    m_pExt2Fs->releaseBlock(record.block);
    trimToBlocks(oldBlocks);
    return false;
  }
  ByteSet(reinterpret_cast<void*>(record.buffer), 0, m_pExt2Fs->m_BlockSize);
  record.entry = reinterpret_cast<Dir*>(record.buffer);
  record.length = m_pExt2Fs->m_BlockSize;
  record.entry->d_reclen = HOST_TO_LITTLE16(record.length);
  record.offset = oldBlocks * m_pExt2Fs->m_BlockSize;
  m_nSize = m_Blocks.count() * m_pExt2Fs->m_BlockSize;
  setSize(m_nSize);
  fileAttributeChanged();
  m_pExt2Fs->writeBlock(record.block);
  return true;
}

bool Ext2Filesystem::renameNode(Directory* oldParent, const String& oldName, File* source,
                                Directory* newParent, const String& newName, File* replaced) {
  if (oldName.length() > 255 || newName.length() > 255) {
    SYSCALL_ERROR(NameTooLong);
    return false;
  }
  Ext2Directory* oldDirectory = static_cast<Ext2Directory*>(oldParent);
  Ext2Directory* newDirectory = static_cast<Ext2Directory*>(newParent);
  Ext2Directory* movedDirectory =
      source->isDirectory() ? static_cast<Ext2Directory*>(source) : nullptr;
  Ext2Directory* replacedDirectory =
      replaced && replaced->isDirectory() ? static_cast<Ext2Directory*>(replaced) : nullptr;
  const bool oldFirst =
      reinterpret_cast<uintptr_t>(oldDirectory) < reinterpret_cast<uintptr_t>(newDirectory);
  Ext2Directory* first = oldFirst ? oldDirectory : newDirectory;
  Ext2Directory* second = oldFirst ? newDirectory : oldDirectory;
  LockGuard<Mutex> firstGuard(first->m_DirectoryLock);
  LockGuard<Mutex> secondGuard(second->m_DirectoryLock, second != first);
  LockGuard<Mutex> movedGuard(
      movedDirectory ? movedDirectory->m_DirectoryLock : first->m_DirectoryLock,
      movedDirectory != nullptr);
  LockGuard<Mutex> replacedGuard(
      replacedDirectory ? replacedDirectory->m_DirectoryLock : first->m_DirectoryLock,
      replacedDirectory != nullptr);
  if (oldDirectory->m_Removed || newDirectory->m_Removed ||
      (movedDirectory && movedDirectory->m_Removed) ||
      (replacedDirectory && replacedDirectory->m_Removed)) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  if (movedDirectory && oldDirectory != newDirectory && !replacedDirectory &&
      LITTLE_TO_HOST16(newDirectory->Ext2Node::getInode()->i_links_count) == 0xffff) {
    SYSCALL_ERROR(TooManyLinks);
    return false;
  }

  Ext2Directory::RenameRecord oldRecord;
  Ext2Directory::RenameRecord newRecord;
  Ext2Directory::RenameRecord parentRecord;
  Ext2Directory::RenameRecord replacedDot;
  Ext2Directory::RenameRecord replacedParent;
  if (!oldDirectory->prepareRenameRecord(oldName, source->getInode(), oldRecord)) {
    return false;
  }
  if (movedDirectory && oldDirectory != newDirectory &&
      !movedDirectory->prepareRenameRecord(String(".."), oldDirectory->getInodeNumber(),
                                           parentRecord)) {
    return false;
  }
  if (replacedDirectory && (!replacedDirectory->prepareRenameRecord(
                                String("."), replacedDirectory->getInodeNumber(), replacedDot) ||
                            !replacedDirectory->prepareRenameRecord(
                                String(".."), newDirectory->getInodeNumber(), replacedParent))) {
    return false;
  }
  if (replaced) {
    if (!newDirectory->prepareRenameRecord(newName, replaced->getInode(), newRecord)) {
      return false;
    }
  } else if (!newDirectory->prepareRenameSpace(
                 newName, newRecord, oldDirectory == newDirectory ? &oldRecord : nullptr)) {
    return false;
  }

  if (oldRecord.entry != newRecord.entry) {
    ByteSet(oldRecord.entry, 0, oldRecord.length);
    oldRecord.entry->d_reclen = HOST_TO_LITTLE16(oldRecord.length);
  }
  if (newRecord.split) {
    newRecord.split->d_reclen = HOST_TO_LITTLE16(newRecord.splitLength);
  }
  ByteSet(newRecord.entry, 0, newRecord.length);
  newRecord.entry->d_inode = HOST_TO_LITTLE32(source->getInode());
  newRecord.entry->d_reclen = HOST_TO_LITTLE16(newRecord.length);
  newRecord.entry->d_namelen = newName.length();
  if (checkRequiredFeature(2)) {
    newRecord.entry->d_file_type = source->isDirectory() ? EXT2_DIRECTORY
                                   : source->isSymlink() ? EXT2_SYMLINK
                                                         : EXT2_FILE;
  }
  MemoryCopy(newRecord.entry->d_name, newName.cstr(), newName.length());
  if (parentRecord.entry) {
    parentRecord.entry->d_inode = HOST_TO_LITTLE32(newDirectory->getInodeNumber());
    writeBlock(parentRecord.block);
  }
  writeBlock(oldRecord.block);
  if (newRecord.block != oldRecord.block) {
    writeBlock(newRecord.block);
  }
  if (replacedDirectory) {
    replacedDirectory->m_Removed = true;
    ByteSet(replacedDot.entry, 0, replacedDot.length);
    replacedDot.entry->d_reclen = HOST_TO_LITTLE16(replacedDot.length);
    ByteSet(replacedParent.entry, 0, replacedParent.length);
    replacedParent.entry->d_reclen = HOST_TO_LITTLE16(replacedParent.length);
    writeBlock(replacedDot.block);
    writeBlock(replacedParent.block);
    releaseInode(newDirectory->getInodeNumber(), newDirectory);
    releaseInode(replacedDirectory->getInodeNumber(), replacedDirectory);
    releaseInode(replacedDirectory->getInodeNumber(), replacedDirectory);
    {
      LockGuard<Mutex> guard(m_WriteLock);
      const uint32_t group = (replacedDirectory->getInodeNumber() - 1) /
                             LITTLE_TO_HOST32(m_pSuperblock->s_inodes_per_group);
      GroupDesc* descriptor = m_pGroupDescriptors[group];
      descriptor->bg_used_dirs_count =
          HOST_TO_LITTLE16(LITTLE_TO_HOST16(descriptor->bg_used_dirs_count) - 1);
      const uint32_t descriptorBlock = LITTLE_TO_HOST32(m_pSuperblock->s_first_data_block) + 1;
      writeBlock(descriptorBlock + (group * sizeof(GroupDesc)) / m_BlockSize);
    }
  } else if (replaced) {
    Ext2Node* node = replaced->isSymlink()
                         ? static_cast<Ext2Node*>(static_cast<Ext2Symlink*>(replaced))
                         : static_cast<Ext2Node*>(static_cast<Ext2File*>(replaced));
    releaseInode(node->getInodeNumber(), node);
  }
  if (parentRecord.entry) {
    increaseInodeRefcount(newDirectory->getInodeNumber());
    releaseInode(oldDirectory->getInodeNumber(), oldDirectory);
  }
  return true;
}
