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
#include <processor/Processor.h>
#include <process/Scheduler.h>
#include <Log.h>
#include <LockGuard.h>

void File::writeCallback(Cache::CallbackCause cause, uintptr_t loc, uintptr_t page, void *meta)
{
    File *pFile = reinterpret_cast<File *>(meta);

    switch(cause)
    {
        case Cache::WriteBack:
            {
                pFile->m_Lock.acquire();

                // We are given one dirty page. Blocks can be smaller than a page.
                size_t off = 0;
                for(; off < PhysicalMemoryManager::getPageSize(); off += pFile->getBlockSize())
                {
                    pFile->writeBlock(loc + off, page + off);
                }

                pFile->m_Lock.release();
            }
            break;
        case Cache::Eviction:
            // Remove this page from our data cache.
            // Side-effect: if the block size is larger than the page size, the
            // entire block will be removed. Is this something we care about?
            pFile->m_DataCache.remove(loc);
            break;
        default:
            WARNING("File: unknown cache callback -- could indicate potential future I/O issues.");
            break;
    }
}

File::File() :
    m_Name(""), m_AccessedTime(0), m_ModifiedTime(0),
    m_CreationTime(0), m_Inode(0), m_pFilesystem(0), m_Size(0),
    m_pParent(0), m_nWriters(0), m_nReaders(0), m_Uid(0), m_Gid(0),
    m_Permissions(0), m_DataCache(), m_Lock(), m_MonitorTargets()
{
}

File::File(String name, Time::Timestamp accessedTime, Time::Timestamp modifiedTime, Time::Timestamp creationTime,
           uintptr_t inode, Filesystem *pFs, size_t size, File *pParent) :
    m_Name(name), m_AccessedTime(accessedTime), m_ModifiedTime(modifiedTime),
    m_CreationTime(creationTime), m_Inode(inode), m_pFilesystem(pFs),
    m_Size(size), m_pParent(pParent), m_nWriters(0), m_nReaders(0), m_Uid(0),
    m_Gid(0), m_Permissions(0), m_DataCache(), m_Lock(), m_MonitorTargets()
{
}

File::~File()
{
}

uint64_t File::read(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    if ((location+size) >= m_Size)
    {
        size_t oldSize = size;
        size = m_Size-location;
        if((location + size) > m_Size)
        {
            // Completely broken read parameters.
            ERROR("VFS: even after fixup, read at location " << location << " is larger than file size (" << m_Size << ")");
            ERROR("VFS:    fixup size: " << size << ", original size: " << oldSize);
            return 0;
        }
    }

    size_t blockSize = getBlockSize();
    
    size_t n = 0;
    while (size)
    {
        if (location >= m_Size)
            return n;

        uintptr_t block = location / blockSize;
        uintptr_t offs  = location % blockSize;
        uintptr_t sz    = (size+offs > blockSize) ? blockSize-offs : size;

        // Handle a possible early EOF.
        if(sz > (m_Size - location))
            sz = m_Size - location;

        m_Lock.acquire();
        uintptr_t buff = m_DataCache.lookup(block*blockSize);
        if (!buff)
        {
            buff = readBlock(block*blockSize);
            if (!buff)
            {
                ERROR("File::read - bad read (" << (block * blockSize) << " - block size is " << blockSize << ")");
                return n;
            }
            m_DataCache.insert(block*blockSize, buff);
        }
        m_Lock.release();

        if(buffer)
        {
            memcpy(reinterpret_cast<void*>(buffer),
                   reinterpret_cast<void*>(buff+offs),
                   sz);
            buffer += sz;
        }
        location += sz;
        size -= sz;
        n += sz;
    }
    return n;
}

uint64_t File::write(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    size_t blockSize = getBlockSize();

    // Extend the file before writing it if needed.
    extend(location + size);

    size_t n = 0;
    while (size)
    {
        uintptr_t block = location / blockSize;
        uintptr_t offs  = location % blockSize;
        uintptr_t sz    = (size+offs > blockSize) ? blockSize-offs : size;

        m_Lock.acquire();
        uintptr_t buff = m_DataCache.lookup(block*blockSize);
        if (!buff)
        {
            buff = readBlock(block*blockSize);
            if (!buff)
            {
                ERROR("File::write - bad read (" << (block * blockSize) << " - block size is " << blockSize << ")");
                return n;
            }
            m_DataCache.insert(block*blockSize, buff);
        }
        m_Lock.release();

        memcpy(reinterpret_cast<void*>(buff+offs),
               reinterpret_cast<void*>(buffer),
               sz);

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
    // Sanitise input.
    size_t blockSize = getBlockSize();
    offset &= ~(blockSize - 1);

    // Quick and easy exit.
    if(offset > m_Size)
    {
        return ~0UL;
    }

    // Check if we have this page in the cache.
    m_Lock.acquire();
    uintptr_t vaddr = m_DataCache.lookup(offset);
    m_Lock.release();
    if (!vaddr)
    {
        return ~0UL;
    }

    // Look up the page now that we've confirmed it is in the cache.
    VirtualAddressSpace &va = Processor::information().getVirtualAddressSpace();
    if(va.isMapped(reinterpret_cast<void *>(vaddr)))
    {
        physical_uintptr_t phys = 0;
        size_t flags = 0;
        va.getMapping(reinterpret_cast<void *>(vaddr), phys, flags);

        // Pin this key in the cache down, so we don't lose it.
        pinBlock(offset);

        return phys;
    }

    return ~0UL;
}

void File::returnPhysicalPage(size_t offset)
{
    // Sanitise input.
    size_t blockSize = getBlockSize();
    offset &= ~(blockSize - 1);

    // Quick and easy exit for bad input.
    if(offset > m_Size)
    {
        return;
    }

    // Release the page. Beware - this could cause a cache evict, which will
    // make the next read/write at this offset do real (slow) I/O.
    m_Lock.acquire();
    unpinBlock(offset);
    m_Lock.release();
}

void File::sync()
{
    Tree<uint64_t,size_t>::Iterator it;
    for(it = m_DataCache.begin(); it != m_DataCache.end(); ++it)
    {
        // Write back the block via the File subclass.
        writeBlock(it.key(), it.value());
    }
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

String File::getName()
{
    return m_Name;
}

void File::getName(String &s)
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

void File::truncate()
{
}

void File::dataChanged()
{
    m_Lock.acquire();

    bool bAny = false;
    for (List<MonitorTarget*>::Iterator it = m_MonitorTargets.begin();
         it != m_MonitorTargets.end();
         it++)
    {
        MonitorTarget *pMT = *it;

        pMT->pThread->sendEvent(pMT->pEvent);
        delete pMT;

        bAny = true;
    }

    m_MonitorTargets.clear();
    m_Lock.release();

    // If anything was waiting on a change, wake it up now.
    if(bAny)
    {
        Scheduler::instance().yield();
    }
}

void File::cullMonitorTargets(Thread *pThread)
{
    LockGuard<Mutex> guard(m_Lock);

    for (List<MonitorTarget*>::Iterator it = m_MonitorTargets.begin();
         it != m_MonitorTargets.end();
         it++)
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

void File::getFilesystemLabel(HugeStaticString &s)
{
    s = m_pFilesystem->getVolumeLabel();
}

String File::getFullPath(bool bWithLabel)
{
    HugeStaticString str;
    HugeStaticString tmp;
    str.clear();
    tmp.clear();

    if (getParent() != 0)
        str = getName();

    File* f = this;
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

    return String(str);
}

void File::evict(uint64_t location)
{
    LockGuard<Mutex> guard(m_Lock);
    m_DataCache.remove(location);
}

