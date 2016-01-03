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

#include <Module.h>
#include <process/Process.h>
#include <machine/Device.h>
#include "RamFs.h"

class Disk;

RamFile::RamFile(String name, uintptr_t inode, Filesystem *pParentFS, File *pParent) :
    File(name, 0, 0, 0, inode, pParentFS, 0, pParent), m_FileBlocks(),
    m_nOwnerPid(0)
{
    // Full permissions.
    setPermissions(0777);

    m_nOwnerPid = Processor::information().getCurrentThread()->getParent()->getId();
}

bool RamFile::canWrite()
{
    RamFs *pParent = static_cast<RamFs *>(getFilesystem());
    if(!pParent->getProcessOwnership())
    {
        return true;
    }

    size_t pid = Processor::information().getCurrentThread()->getParent()->getId();
    return pid == m_nOwnerPid;
}

RamDir::RamDir(String name, size_t inode, class Filesystem *pFs, File *pParent) :
            Directory(name, 0, 0, 0, inode, pFs, 0, pParent)
{
    // Full permissions.
    setPermissions(0777);
}

RamDir::~RamDir()
{};

bool RamDir::addEntry(String filename, File *pFile)
{
    getCache().insert(filename, pFile);
    m_bCachePopulated = true;
    return true;
}

bool RamDir::removeEntry(File *pFile)
{
    RamFile *pRamFile = static_cast<RamFile *>(pFile);
    if(!pRamFile->canWrite())
        return false;

    // Remove from cache.
    getCache().remove(pFile->getName());
    return true;
}

RamFs::RamFs() : m_pRoot(0), m_bProcessOwners(false)
{}

RamFs::~RamFs()
{
    if(m_pRoot)
        delete m_pRoot;
}

bool RamFs::initialise(Disk *pDisk)
{
    // Root directory with ./.. entries
    m_pRoot = new RamDir(String(""), 0, this, 0);
    return true;
}

bool RamFs::createFile(File* parent, String filename, uint32_t mask)
{
    if (!parent->isDirectory())
        return false;

    File *f = new RamFile(filename, 0, this, parent);

    RamDir *p = static_cast<RamDir*>(parent);
    return p->addEntry(filename, f);
}

bool RamFs::createDirectory(File* parent, String filename)
{
    if(!parent->isDirectory())
        return false;

    RamDir *pDir = new RamDir(filename, 0, this, parent);

    RamDir *pParent = static_cast<RamDir*>(parent);
    return pParent->addEntry(filename, pDir);
}

bool RamFs::createSymlink(File* parent, String filename, String value)
{
    return false;
}

bool RamFs::remove(File* parent, File* file)
{
    if (file->isDirectory())
        return false;

    RamDir *p = static_cast<RamDir*>(parent);
    return p->removeEntry(file);
}

static bool entry()
{
    return true;
}

static void destroy()
{
}

MODULE_INFO("ramfs", &entry, &destroy, "vfs");
