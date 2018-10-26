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
#include "Ext2Filesystem.h"
#include "ext2.h"
#include "pedigree/kernel/utilities/utility.h"

class Filesystem;

Ext2File::Ext2File(
    const String &name, uintptr_t inode_num, Inode *inode, Ext2Filesystem *pFs,
    File *pParent)
    : File(
          name, LITTLE_TO_HOST32(inode->i_atime),
          LITTLE_TO_HOST32(inode->i_mtime), LITTLE_TO_HOST32(inode->i_ctime),
          inode_num, static_cast<Filesystem *>(pFs),
          LITTLE_TO_HOST32(inode->i_size),  /// \todo Deal with >4GB files here.
          pParent),
      Ext2Node(inode_num, inode, pFs)
{
    uint32_t mode = LITTLE_TO_HOST32(inode->i_mode);
    setPermissionsOnly(modeToPermissions(mode));
    setUidOnly(LITTLE_TO_HOST16(inode->i_uid));
    setGidOnly(LITTLE_TO_HOST16(inode->i_gid));
}

Ext2File::~Ext2File()
{
}

void Ext2File::preallocate(size_t expectedSize, bool zero)
{
    // No need to change the actual file size, just allocate the blocks.
    Ext2Node::ensureLargeEnough(expectedSize, 0, 0, true, !zero);
}

void Ext2File::extend(size_t newSize)
{
    Ext2Node::extend(newSize, 0, 0);
    m_Size = m_nSize;
}

void Ext2File::extend(size_t newSize, uint64_t location, uint64_t size)
{
    Ext2Node::extend(newSize, location, size);
    m_Size = m_nSize;
}

void Ext2File::truncate()
{
    // Wipe all our blocks. (Ext2Node).
    Ext2Node::wipe();
    m_Size = m_nSize;
}

void Ext2File::fileAttributeChanged()
{
    static_cast<Ext2Node *>(this)->fileAttributeChanged(
        m_Size, m_AccessedTime, m_ModifiedTime, m_CreationTime);
    static_cast<Ext2Node *>(this)->updateMetadata(
        getUid(), getGid(), permissionsToMode(getPermissions()));
}

uintptr_t Ext2File::readBlock(uint64_t location)
{
    return Ext2Node::readBlock(location);
}

void Ext2File::writeBlock(uint64_t location, uintptr_t addr)
{
    Ext2Node::writeBlock(location);
}

void Ext2File::pinBlock(uint64_t location)
{
    Ext2Node::pinBlock(location);
}

void Ext2File::unpinBlock(uint64_t location)
{
    Ext2Node::unpinBlock(location);
}

void Ext2File::sync(size_t offset, bool async)
{
    Ext2Node::sync(offset, async);
}

size_t Ext2File::getBlockSize() const
{
    return m_pExt2Fs->m_BlockSize;
}
