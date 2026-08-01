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
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/utilities/utility.h"

class Filesystem;

RawFsFile::RawFsFile(String name, RawFs *pFs, File *pParent, Disk *pDisk)
    : File(
          name, 0 /* Accessed time */, 0 /* Modified time */,
          0 /* Creation time */, 0 /* Inode number */,
          static_cast<Filesystem *>(pFs), 0 /* Size */, pParent),
      m_pDisk(pDisk), m_PageCache(), m_PageCacheLock()
{
    m_PageCache.setCallback(writeCallback, this);

    // Owned by root:root
    setUid(0);
    setGid(0);

    // RW for root, readable only by others.
    uint32_t permissions = FILE_UR | FILE_UW | FILE_GR | FILE_OR;
    setPermissions(permissions);

    // Disk size.
    setSize(m_pDisk->getSize());
}

RawFsFile::~RawFsFile()
{
    m_PageCache.shutdown();
}

size_t RawFsFile::getBlockSize() const
{
    // Disk block sizes describe internal I/O/readahead extents. File::readBlock
    // owns one cache page, so VFS must never expose a larger unpinned range.
    return PhysicalMemoryManager::getPageSize();
}

uintptr_t RawFsFile::readBlock(uint64_t location)
{
    LockGuard<Mutex> guard(m_PageCacheLock);
    const size_t pageSize = PhysicalMemoryManager::getPageSize();
    const uint64_t pageLocation =
        location - (location % pageSize);
    const size_t pageOffset = location - pageLocation;
    if (
        pageLocation >= getSize() ||
        pageSize > (getSize() - pageLocation))
    {
        return 0;
    }

    uintptr_t buffer = m_PageCache.lookup(pageLocation);
    if (buffer)
    {
        return buffer + pageOffset;
    }

    bool didExist = false;
    buffer = m_PageCache.insert(pageLocation, &didExist);
    if (!buffer)
    {
        return 0;
    }

    if (!didExist)
    {
        size_t copied = 0;
        while (copied < pageSize)
        {
            const uint64_t diskLocation = pageLocation + copied;
            const uintptr_t source = m_pDisk->read(diskLocation);
            if (!source)
            {
                const bool discarded =
                    m_PageCache.discardEditing(pageLocation);
                (void) discarded;
                return 0;
            }

            const size_t sourceOffset = source % pageSize;
            size_t chunk = pageSize - sourceOffset;
            if (chunk > (pageSize - copied))
            {
                chunk = pageSize - copied;
            }
            MemoryCopy(
                reinterpret_cast<void *>(buffer + copied),
                reinterpret_cast<const void *>(source), chunk);
            m_pDisk->unpin(diskLocation);
            copied += chunk;
        }
        m_PageCache.markNoLongerEditing(pageLocation);
    }

    buffer = m_PageCache.lookup(pageLocation);
    return buffer ? buffer + pageOffset : 0;
}

void RawFsFile::writeBlock(uint64_t location, uintptr_t address)
{
    const size_t pageSize = PhysicalMemoryManager::getPageSize();
    const uint64_t pageLocation =
        location - (location % pageSize);
    if (
        !address || pageLocation >= getSize() ||
        pageSize > (getSize() - pageLocation))
    {
        return;
    }

    size_t copied = 0;
    while (copied < pageSize)
    {
        const uint64_t diskLocation = pageLocation + copied;
        const uintptr_t destination = m_pDisk->read(diskLocation);
        if (!destination)
        {
            return;
        }

        const size_t destinationOffset = destination % pageSize;
        size_t chunk = pageSize - destinationOffset;
        if (chunk > (pageSize - copied))
        {
            chunk = pageSize - copied;
        }
        MemoryCopy(
            reinterpret_cast<void *>(destination),
            reinterpret_cast<const void *>(address + copied), chunk);
        m_pDisk->write(diskLocation);
        m_pDisk->unpin(diskLocation);
        copied += chunk;
    }
}

bool RawFsFile::pinBlock(uint64_t location)
{
    const size_t pageSize = PhysicalMemoryManager::getPageSize();
    return m_PageCache.pin(location - (location % pageSize));
}

void RawFsFile::unpinBlock(uint64_t location)
{
    const size_t pageSize = PhysicalMemoryManager::getPageSize();
    m_PageCache.release(location - (location % pageSize));
}
