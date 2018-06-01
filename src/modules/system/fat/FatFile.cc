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
#include "FatFilesystem.h"

FatFile::FatFile(
    String name, Time::Timestamp accessedTime, Time::Timestamp modifiedTime,
    Time::Timestamp creationTime, uintptr_t inode, class Filesystem *pFs,
    size_t size, uint32_t dirClus, uint32_t dirOffset, File *pParent)
    : File(
          name, accessedTime, modifiedTime, creationTime, inode, pFs, size,
          pParent),
      m_DirClus(dirClus), m_DirOffset(dirOffset), m_FileBlockCache()
{
    m_FileBlockCache.setCallback(writeCallback, static_cast<File *>(this));

    // No permissions on FAT - set all to RWX.
    setPermissions(
        FILE_UR | FILE_UW | FILE_UX | FILE_GR | FILE_GW | FILE_GX | FILE_OR |
        FILE_OW | FILE_OX);
}

FatFile::~FatFile()
{
}

uintptr_t FatFile::readBlock(uint64_t location)
{
    FatFilesystem *pFs = static_cast<FatFilesystem *>(m_pFilesystem);

    uintptr_t buffer = m_FileBlockCache.insert(location);
    pFs->read(this, location, getBlockSize(), buffer);
    m_FileBlockCache.markNoLongerEditing(location);

    return buffer;
}

void FatFile::writeBlock(uint64_t location, uintptr_t addr)
{
    FatFilesystem *pFs = static_cast<FatFilesystem *>(m_pFilesystem);

    // Don't accidentally extend the file when writing the block.
    size_t sz = getBlockSize();
    uint64_t end = location + sz;
    if (end > getSize())
        sz = getSize() - location;
    pFs->write(this, location, sz, addr);
}

void FatFile::sync(size_t offset, bool async)
{
    m_FileBlockCache.sync(offset, async);
}

void FatFile::pinBlock(uint64_t location)
{
    m_FileBlockCache.pin(location);
}

void FatFile::unpinBlock(uint64_t location)
{
    m_FileBlockCache.release(location);
}

void FatFile::extend(size_t newSize)
{
    FatFilesystem *pFs = static_cast<FatFilesystem *>(m_pFilesystem);

    if (m_Size < newSize)
    {
        pFs->extend(this, newSize);
        m_Size = newSize;
    }
}
