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

#include "Ext2Symlink.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Ext2Filesystem.h"
#include "ext2.h"

class File;
class Filesystem;

Ext2Symlink::Ext2Symlink(const String& name, uintptr_t inode_num, Inode* inode, Ext2Filesystem* pFs,
                         File* pParent)
    : Symlink(name, LITTLE_TO_HOST32(inode->i_atime), LITTLE_TO_HOST32(inode->i_mtime),
              LITTLE_TO_HOST32(inode->i_ctime), inode_num, static_cast<Filesystem*>(pFs),
              LITTLE_TO_HOST32(inode->i_size),  /// \todo Deal with >4GB files here.
              pParent),
      Ext2Node(inode_num, inode, pFs) {
  uint32_t mode = LITTLE_TO_HOST32(inode->i_mode);
  setPermissionsOnly(modeToPermissions(mode));
  setUidOnly(Ext2Owner::uid(*inode));
  setGidOnly(Ext2Owner::gid(*inode));
}

Ext2Symlink::~Ext2Symlink() {}

uint64_t Ext2Symlink::readBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                   bool canBlock) {
  if (!m_State->allocationValid || !size || location >= getSize())
    return 0;
  const uint64_t remaining = getSize() - location;
  if (size > remaining)
    size = remaining;

  if (isInlineSymlink()) {
    MemoryCopy(reinterpret_cast<void*>(buffer), adjust_pointer(m_pInode->i_block, location), size);
    return size;
  }

  if (getSize() > m_pExt2Fs->m_BlockSize) {
    WARNING("Ext2: rather large symlink found, not handled yet");
    return 0;
  }

  uintptr_t block = Ext2Node::readBlock(location);
  if (!block) {
    return 0;
  }
  MemoryCopy(reinterpret_cast<void*>(buffer), reinterpret_cast<void*>(block), size);
  Ext2Node::unpinBlock(location);
  m_Size = m_nSize;
  return size;
}

uint64_t Ext2Symlink::writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                    bool canBlock) {
  LockGuard<Mutex> inode(m_State->writebackLock);
  if (location > m_pExt2Fs->m_BlockSize || size > m_pExt2Fs->m_BlockSize - location) {
    SYSCALL_ERROR(FileTooLarge);
    return 0;
  }
  if (!ensureLargeEnough(location + size, location, size))
    return 0;
  m_Size = m_nSize;

  uintptr_t block = Ext2Node::readBlock(location);
  if (!block) {
    SYSCALL_ERROR(IoError);
    return 0;
  }
  MemoryCopy(reinterpret_cast<void*>(block), reinterpret_cast<void*>(buffer), size);
  Ext2Node::writeBlock(location);
  Ext2Node::unpinBlock(location);
  return size;
}

void Ext2Symlink::truncate() {
  Ext2Node::wipe();
  m_Size = m_nSize;
}

void Ext2Symlink::fileAttributeChanged() {
  LockGuard<Mutex> guard(m_State->writebackLock);
  Ext2Node::fileAttributeChanged(m_Size, LITTLE_TO_HOST32(m_pInode->i_atime),
                                 LITTLE_TO_HOST32(m_pInode->i_mtime),
                                 LITTLE_TO_HOST32(m_pInode->i_ctime));
}

File::Attributes Ext2Symlink::getAttributes() const {
  return inodeAttributes();
}

void Ext2Symlink::updateAttributes(const Attributes& attributes, uint32_t mask) {
  updateInodeAttributes(attributes, mask);
}
