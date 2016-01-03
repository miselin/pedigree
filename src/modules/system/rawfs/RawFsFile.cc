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
#include "RawFs.h"
#include <machine/Disk.h>

RawFsFile::RawFsFile(String name, RawFs *pFs, File *pParent, Disk *pDisk) :
    File(name,
	      0 /* Accessed time */,
	      0 /* Modified time */,
	      0 /* Creation time */,
	      0 /* Inode number */,
	      static_cast<Filesystem*>(pFs),
	      0 /* Size */,
	      pParent),
    m_pDisk(pDisk)
{
    // Owned by root:root
    setUid(0);
    setGid(0);

    // RW for root, readable only by others.
    uint32_t permissions = FILE_UR | FILE_UW | FILE_GR | FILE_OR;
    setPermissions(permissions);

    // Disk size.
    setSize(m_pDisk->getSize());
}

size_t RawFsFile::getBlockSize() const
{
    return m_pDisk->getBlockSize();
}

uintptr_t RawFsFile::readBlock(uint64_t location)
{
    return m_pDisk->read(location);
}

