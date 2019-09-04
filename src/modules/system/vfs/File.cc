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

#include "File.h"
#include "Filesystem.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/Pair.h"
#include "pedigree/kernel/utilities/Result.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

void File::writeCallback(
    CacheConstants::CallbackCause cause, uintptr_t loc, uintptr_t page,
    void *meta)
{
    File *pFile = reinterpret_cast<File *>(meta);

    switch (cause)
    {
        case CacheConstants::WriteBack:
        {
            // We are given one dirty page. Blocks can be smaller than a page.
            size_t off = 0;
            for (; off < PhysicalMemoryManager::getPageSize();
                 off += pFile->getBlockSize())
            {
                pFile->writeBlock(loc + off, page + off);
            }
        }
        break;
        case CacheConstants::Eviction:
            // Remove this page from our data cache.
            /// \todo handle block size < 4K??
            pFile->setCachedPage(loc / pFile->getBlockSize(), FILE_BAD_BLOCK);
            break;
        default:
            WARNING("File: unknown cache callback -- could indicate potential "
                    "future I/O issues.");
            break;
    }
}

File::File()
    : m_Name(), m_AccessedTime(0), m_ModifiedTime(0), m_CreationTime(0),
      m_Inode(0), m_pFilesystem(0), m_Size(0), m_pParent(0), m_nWriters(0),
      m_nReaders(0), m_Uid(0), m_Gid(0), m_Permissions(0),
      m_DataCache(FILE_BAD_BLOCK), m_bDirect(false),
      m_FillCache(), m_Lock(), m_MonitorTargets()
{
}

File::File(
    const String &name, Time::Timestamp accessedTime,
    Time::Timestamp modifiedTime, Time::Timestamp creationTime, uintptr_t inode,
    Filesystem *pFs, size_t size, File *pParent)
    : m_Name(name), m_AccessedTime(accessedTime), m_ModifiedTime(modifiedTime),
      m_CreationTime(creationTime), m_Inode(inode), m_pFilesystem(pFs),
      m_Size(size), m_pParent(pParent), m_nWriters(0), m_nReaders(0), m_Uid(0),
      m_Gid(0), m_Permissions(0), m_DataCache(FILE_BAD_BLOCK), m_bDirect(false),
      m_FillCache(), m_Lock(), m_MonitorTargets()
{
    size_t maxBlock = size / getBlockSize();
    if (size % getBlockSize())
    {
        ++maxBlock;
    }

    // Prefill the block cache with bad pages to avoid continuous resizes.
    setCachedPage(maxBlock, FILE_BAD_BLOCK);
}

File::~File()
{
}

uint64_t
File::read(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    if (isBytewise())
    {
        // Have to perform bytewise reads
        /// \todo consider caching this still
        return readBytewise(location, size, buffer, bCanBlock);
    }

    if ((location + size) >= m_Size)
    {
        size_t oldSize = size;
        size = m_Size - location;
        if ((location + size) > m_Size)
        {
            // Completely broken read parameters.
            ERROR(
                "VFS: even after fixup, read at location "
                << location << " is larger than file size (" << m_Size << ")");
            ERROR(
                "VFS:    fixup size: " << size
                                       << ", original size: " << oldSize);
            return 0;
        }
    }

    const size_t blockSize =
        useFillCache() ? PhysicalMemoryManager::getPageSize() : getBlockSize();

    size_t n = 0;
    while (size)
    {
        if (location >= m_Size)
            return n;

        uintptr_t block = location / blockSize;
        uintptr_t offs = location % blockSize;
        uintptr_t sz = (size + offs > blockSize) ? blockSize - offs : size;

        // Handle a possible early EOF.
        if (sz > (m_Size - location))
            sz = m_Size - location;

        uintptr_t buff = readIntoCache(block);
        if (buff == FILE_BAD_BLOCK)
        {
            ERROR(
                "File::read - failed to get page from cache, returning early");
            return n;
        }

        if (buffer)
        {
            ForwardMemoryCopy(
                reinterpret_cast<void *>(buffer),
                reinterpret_cast<void *>(buff + offs), sz);
            buffer += sz;
        }

        location += sz;
        size -= sz;
        n += sz;
    }
    return n;
}

uint64_t
File::write(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    if (isBytewise())
    {
        // Have to perform bytewise reads
        /// \todo consider caching this still
        return writeBytewise(location, size, buffer, bCanBlock);
    }

    const size_t blockSize = getBlockSize();

    bool isEntireBlock = false;
    if ((location % blockSize) == 0)
    {
        if ((size % blockSize) == 0)
        {
            isEntireBlock = true;
        }
    }

    // Extend the file before writing it if needed.
    extend(location + size, location, size);

    size_t n = 0;
    while (size)
    {
        uintptr_t block = location / blockSize;
        uintptr_t offs = location % blockSize;
        uintptr_t sz = (size + offs > blockSize) ? blockSize - offs : size;

        uintptr_t buff = readIntoCache(block);
        if (buff == FILE_BAD_BLOCK)
        {
            ERROR(
                "File::read - failed to get page from cache, returning early");
            return n;
        }

        ForwardMemoryCopy(
            reinterpret_cast<void *>(buff + offs),
            reinterpret_cast<void *>(buffer), sz);

        // Trigger an immediate write-back - write-through cache.
        writeBlock(block * blockSize, buff);

        location += sz;
        buffer += sz;
        size -= sz;
        n += sz;
    }

    if (location >= m_Size)
    {
        m_Size = location;
        fileAttributeChanged();
    }
    return n;
}

physical_uintptr_t File::getPhysicalPage(size_t offset)
{
    if (m_bDirect)
    {
        WARNING("File in direct mode, cannot get backing page.");
        return ~0UL;
    }

    EMIT_IF(VFS_NOMMU)
    {
        return ~0UL;
    }

    // Sanitise input.
    size_t blockSize = getBlockSize();
    size_t nativeBlockSize = PhysicalMemoryManager::getPageSize();
    if (useFillCache())
    {
        blockSize = nativeBlockSize;
    }
    offset &= ~(blockSize - 1);

    // Quick and easy exit.
    if (offset > m_Size)
    {
        return ~0UL;
    }

    // Check if we have this page in the cache.
    uintptr_t vaddr = FILE_BAD_BLOCK;
    if (LIKELY(!useFillCache()))
    {
        // Not using fill cache, this is the easy and common case.
        vaddr = getCachedPage(offset / blockSize);
    }
    else
    {
        // Using the fill cache, because the filesystem has a block size
        // smaller than our native page size.
        vaddr = m_FillCache.lookup(offset);
        if (!vaddr)
        {
            // Wasn't there. No physical page.
            vaddr = FILE_BAD_BLOCK;
        }
    }

    if ((!vaddr) || (vaddr == FILE_BAD_BLOCK))
    {
        return ~0UL;
    }

    // Look up the page now that we've confirmed it is in the cache.
    VirtualAddressSpace &va = Processor::information().getVirtualAddressSpace();
    if (va.isMapped(reinterpret_cast<void *>(vaddr)))
    {
        physical_uintptr_t phys = 0;
        size_t flags = 0;
        va.getMapping(reinterpret_cast<void *>(vaddr), phys, flags);

        // Pin this key in the cache down, so we don't lose it.
        if (UNLIKELY(useFillCache()))
        {
            m_FillCache.pin(offset);
        }
        else
        {
            pinBlock(offset);
        }

        return phys;
    }

    return ~0UL;
}

void File::returnPhysicalPage(size_t offset)
{
    if (m_bDirect)
    {
        return;
    }

    // Sanitise input.
    size_t blockSize = getBlockSize();
    size_t nativeBlockSize = PhysicalMemoryManager::getPageSize();
    if (useFillCache())
    {
        blockSize = nativeBlockSize;
    }
    offset &= ~(blockSize - 1);

    // Quick and easy exit for bad input.
    if (offset > m_Size)
    {
        return;
    }

    // Release the page. Beware - this could cause a cache evict, which will
    // make the next read/write at this offset do real (slow) I/O.
    if (UNLIKELY(useFillCache()))
    {
        m_FillCache.release(offset);
    }
    else
    {
        unpinBlock(offset);
    }
}

void File::sync()
{
    LockGuard<Mutex> guard(m_Lock);

    const size_t blockSize = getBlockSize();
    for (size_t i = 0; i < m_DataCache.count(); ++i)
    {
        auto result = m_DataCache.getNth(i);
        if (result.hasError())
        {
            break;
        }

        uintptr_t buffer = result.value().second();
        if (buffer != FILE_BAD_BLOCK)
        {
            writeBlock(i * blockSize, buffer);
        }
    }
}

void File::sync(size_t offset, bool async)
{
}

Time::Timestamp File::getCreationTime()
{
    return m_CreationTime;
}

void File::setCreationTime(Time::Timestamp t)
{
    m_CreationTime = t;
    fileAttributeChanged();
}

Time::Timestamp File::getAccessedTime()
{
    return m_AccessedTime;
}

void File::setAccessedTime(Time::Timestamp t)
{
    m_AccessedTime = t;
    fileAttributeChanged();
}

Time::Timestamp File::getModifiedTime()
{
    return m_ModifiedTime;
}

void File::setModifiedTime(Time::Timestamp t)
{
    m_ModifiedTime = t;
    fileAttributeChanged();
}

const String &File::getName() const
{
    return m_Name;
}

void File::getName(String &s) const
{
    s = m_Name;
}

size_t File::getSize()
{
    return m_Size;
}

void File::setSize(size_t sz)
{
    m_Size = sz;
}

bool File::isSymlink()
{
    return false;
}

bool File::isDirectory()
{
    return false;
}

bool File::isPipe() const
{
    return false;
}

bool File::isFifo() const
{
    return false;
}

bool File::isSocket() const
{
    return false;
}

uintptr_t File::getInode() const
{
    return m_Inode;
}

void File::setInode(uintptr_t inode)
{
    m_Inode = inode;
}

Filesystem *File::getFilesystem() const
{
    return m_pFilesystem;
}

void File::setFilesystem(Filesystem *pFs)
{
    m_pFilesystem = pFs;
}

void File::fileAttributeChanged()
{
}

void File::increaseRefCount(bool bIsWriter)
{
    if (bIsWriter)
        m_nWriters++;
    else
        m_nReaders++;
}

void File::decreaseRefCount(bool bIsWriter)
{
    if (bIsWriter)
        m_nWriters--;
    else
        m_nReaders--;
}

void File::setPermissions(uint32_t perms)
{
    m_Permissions = perms;
    fileAttributeChanged();
}

uint32_t File::getPermissions() const
{
    return m_Permissions;
}

void File::setUid(size_t uid)
{
    m_Uid = uid;
    fileAttributeChanged();
}

size_t File::getUid() const
{
    return m_Uid;
}

void File::setGid(size_t gid)
{
    m_Gid = gid;
    fileAttributeChanged();
}

size_t File::getGid() const
{
    return m_Gid;
}

File *File::getParent() const
{
    return m_pParent;
}

int File::select(bool bWriting, int timeout)
{
    return 1;
}

bool File::supports(const size_t command) const
{
    return false;
}

int File::command(const size_t command, void *buffer)
{
    return 0;
}

size_t File::getBlockSize() const
{
    return PhysicalMemoryManager::getPageSize();
}

void File::enableDirect()
{
    m_bDirect = true;
}

void File::disableDirect()
{
    m_bDirect = false;
}

void File::preallocate(size_t expectedSize, bool zero)
{
}

void File::truncate()
{
}

File *File::open()
{
    return this;
}

bool File::isBytewise() const
{
    return false;
}

uint64_t File::readBytewise(
    uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    if (isBytewise())
    {
        FATAL("A bytewise File subclass didn't implement readBytewise");
    }
    return 0;
}

uint64_t File::writeBytewise(
    uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    if (isBytewise())
    {
        FATAL("A bytewise File subclass didn't implement writeBytewise");
    }
    return 0;
}

uintptr_t File::readBlock(uint64_t location)
{
    String fullPath;
    getFullPath(fullPath);
    ERROR("File: base class readBlock() called for " << fullPath);
    // only truly breaks on debug-enabled builds - in release builds this will
    // just cause an error in the caller
    assert(false);
    return 0;
}

void File::writeBlock(uint64_t location, uintptr_t addr)
{
}

void File::extend(size_t newSize)
{
    if (m_Size < newSize)
        m_Size = newSize;
}

void File::extend(size_t newSize, uint64_t location, uint64_t size)
{
    extend(newSize);
}

void File::pinBlock(uint64_t location)
{
}

void File::unpinBlock(uint64_t location)
{
}

void File::evict(uint64_t location)
{
    setCachedPage(location / getBlockSize(), FILE_BAD_BLOCK);
}

void File::setPermissionsOnly(uint32_t perms)
{
    m_Permissions = perms;
}

void File::setUidOnly(size_t uid)
{
    m_Uid = uid;
}

void File::setGidOnly(size_t gid)
{
    m_Gid = gid;
}

void File::dataChanged()
{
    EMIT_IF(THREADS)
    {
        bool bAny = false;
        {
            LockGuard<Mutex> guard(m_Lock);

            for (List<MonitorTarget *>::Iterator it = m_MonitorTargets.begin();
                 it != m_MonitorTargets.end(); it++)
            {
                MonitorTarget *pMT = *it;

                pMT->pThread->sendEvent(pMT->pEvent);
                delete pMT;

                bAny = true;
            }

            m_MonitorTargets.clear();
        }

        // If anything was waiting on a change, wake it up now.
        if (bAny)
        {
            Scheduler::instance().yield();
        }
    }
}

void File::monitor(Thread *pThread, Event *pEvent)
{
    EMIT_IF(THREADS)
    {
        LockGuard<Mutex> guard(m_Lock);
        m_MonitorTargets.pushBack(new MonitorTarget(pThread, pEvent));
    }
}

void File::cullMonitorTargets(Thread *pThread)
{
    EMIT_IF(THREADS)
    {
        LockGuard<Mutex> guard(m_Lock);

        for (List<MonitorTarget *>::Iterator it = m_MonitorTargets.begin();
             it != m_MonitorTargets.end(); it++)
        {
            MonitorTarget *pMT = *it;

            if (pMT->pThread == pThread)
            {
                delete pMT;
                m_MonitorTargets.erase(it);
                it = m_MonitorTargets.begin();
                if (it == m_MonitorTargets.end())
                    return;
            }
        }
    }
}

void File::getFilesystemLabel(HugeStaticString &s)
{
    s = m_pFilesystem->getVolumeLabel();
}

void File::getFullPath(String &result, bool bWithLabel)
{
    HugeStaticString str;
    HugeStaticString tmp;
    str.clear();
    tmp.clear();

    if (getParent() != 0)
        str = getName();

    File *f = this;
    while ((f = f->getParent()))
    {
        // This feels a bit weird considering the while loop's subject...
        if (f->getParent())
        {
            tmp = str;
            str = f->getName();
            str += "/";
            str += tmp;
        }
    }

    tmp = str;
    str = "/";
    str += tmp;

    if (bWithLabel && m_pFilesystem)
    {
        tmp = str;
        getFilesystemLabel(str);
        str += "»";
        str += tmp;
    }
    else if (bWithLabel && !m_pFilesystem)
    {
        ERROR("File::getFullPath called without a filesystem!");
    }

    result.assign(str, str.length());
}

uintptr_t File::getCachedPage(size_t block, bool locked)
{
    LockGuard<Mutex> guard(m_Lock, locked);

    DataCacheKey key(block);
    auto result = m_DataCache.lookup(key);
    if (result.hasValue())
    {
        return result.value();
    }
    else
    {
        return FILE_BAD_BLOCK;
    }
}

void File::setCachedPage(size_t block, uintptr_t value, bool locked)
{
    LockGuard<Mutex> guard(m_Lock, locked);

    assert(value);

    DataCacheKey key(block);
    if (m_DataCache.contains(key))
    {
        if (value == FILE_BAD_BLOCK)
        {
            m_DataCache.remove(key);
        }
        else
        {
            m_DataCache.update(key, value);
        }
    }
    else
    {
        m_DataCache.insert(key, value);
    }
}

bool File::useFillCache() const
{
    EMIT_IF(VFS_NOMMU)
    {
        // No fill cache in NOMMU builds.
        return false;
    }
    else
    {
        size_t blockSize = getBlockSize();
        size_t nativeBlockSize = PhysicalMemoryManager::getPageSize();
        return blockSize < nativeBlockSize;
    }
}

uintptr_t File::readIntoCache(uintptr_t block)
{
    size_t blockSize = getBlockSize();
    size_t nativeBlockSize = PhysicalMemoryManager::getPageSize();

    size_t offset = block * blockSize;
    size_t mask = blockSize - 1;
    if (useFillCache())
    {
        mask = nativeBlockSize - 1;
    }

    size_t blockOffset = offset & mask;
    offset &= ~mask;

    if (useFillCache())
    {
        // Using Cache::insert() here is atomic compared to if we did a
        // lookup() followed by an insert() - means we don't need to lock the
        // File object to do this.
        bool didExist = false;
        uintptr_t vaddr =
            m_FillCache.insert(offset, nativeBlockSize, &didExist);

        // If in direct mode we are required to read() again
        if (didExist && !m_bDirect)
        {
            // Already in cache - don't re-read the file.
            return vaddr;
        }

        // Read the blocks
        for (size_t i = 0; i < nativeBlockSize; i += blockSize)
        {
            uintptr_t blockAddr = readBlock(offset + i);
            /// \todo handle readBlock failing here
            ForwardMemoryCopy(
                reinterpret_cast<void *>(vaddr),
                reinterpret_cast<void *>(blockAddr), blockSize);
        }

        m_FillCache.markNoLongerEditing(offset, nativeBlockSize);

        NOTICE("readIntoCache: fillcache blockOffset=" << blockOffset);
        return vaddr + blockOffset;
    }

    uintptr_t buff = FILE_BAD_BLOCK;
    if (!m_bDirect)
    {
        buff = getCachedPage(block);
    }
    if (buff == FILE_BAD_BLOCK)
    {
        buff = readBlock(block * blockSize);
        if (!buff)
        {
            ERROR(
                "File::readIntoCache - bad read ("
                << (block * blockSize) << " - block size is " << blockSize
                << ")");
            return FILE_BAD_BLOCK;
        }

        if (!m_bDirect)
        {
            setCachedPage(block, buff);
        }
    }

    return buff + blockOffset;
}
