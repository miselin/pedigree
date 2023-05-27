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

#include "RamFs.h"
#include "modules/Module.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/new"

String RamFs::m_VolumeLabel("ramfs");

RamFile::RamFile(
    const String &name, uintptr_t inode, Filesystem *pParentFS, File *pParent)
    : File(name, 0, 0, 0, inode, pParentFS, 0, pParent), m_FileBlocks(),
      m_nOwnerPid(0)
{
    // Full permissions.
    setPermissions(0777);

#if THREADS
    m_nOwnerPid =
        Processor::information().getCurrentThread()->getParent()->getId();
#else
    m_nOwnerPid = 0;
#endif
}

RamFile::~RamFile()
{
    truncate();
}

void RamFile::truncate()
{
    if (canWrite())
    {
        // Empty the cache.
        m_FileBlocks.empty();
        setSize(0);
    }
}

bool RamFile::canWrite()
{
    RamFs *pParent = static_cast<RamFs *>(getFilesystem());
    if (!pParent->getProcessOwnership())
    {
        return true;
    }

#if THREADS
    size_t pid =
        Processor::information().getCurrentThread()->getParent()->getId();
    return pid == m_nOwnerPid;
#else
    return true;
#endif
}

uintptr_t RamFile::readBlock(uint64_t location)
{
    uintptr_t buffer = m_FileBlocks.lookup(location);
    if (!buffer)
    {
        // Super trivial. But we are a ram filesystem... can't compact.
        buffer = m_FileBlocks.insert(location);
        pinBlock(location);
        /// \todo Kind of irrelevant here?
        m_FileBlocks.markNoLongerEditing(location);
    }
    return buffer;
}

void RamFile::pinBlock(uint64_t location)
{
    m_FileBlocks.pin(location);
}

void RamFile::unpinBlock(uint64_t location)
{
    m_FileBlocks.release(location);
}

RamDir::RamDir(
    const String &name, size_t inode, class Filesystem *pFs, File *pParent)
    : Directory(name, 0, 0, 0, inode, pFs, 0, pParent)
{
    // Full permissions.
    setPermissions(0777);
}

RamDir::~RamDir(){};

bool RamDir::addEntry(String filename, File *pFile)
{
    addDirectoryEntry(filename, pFile);
    return true;
}

bool RamDir::removeEntry(File *pFile)
{
    RamFile *pRamFile = static_cast<RamFile *>(pFile);
    if (!pRamFile->canWrite())
        return false;

    // Remove from cache.
    remove(pFile->getName());
    return true;
}

RamFs::RamFs() : m_pRoot(0), m_bProcessOwners(false)
{
}

RamFs::~RamFs()
{
    if (m_pRoot)
        delete m_pRoot;
}

bool RamFs::initialise(Disk *pDisk)
{
    // Root directory with ./.. entries
    m_pRoot = new RamDir(String(""), 0, this, 0);
    return true;
}

bool RamFs::createFile(File *parent, const String &filename, uint32_t mask)
{
    if (!parent->isDirectory())
        return false;

    File *f = new RamFile(filename, 0, this, parent);

    RamDir *p = static_cast<RamDir *>(parent);
    return p->addEntry(filename, f);
}

bool RamFs::createDirectory(File *parent, const String &filename, uint32_t mask)
{
    if (!parent->isDirectory())
        return false;

    RamDir *pDir = new RamDir(filename, 0, this, parent);

    RamDir *pParent = static_cast<RamDir *>(parent);
    return pParent->addEntry(filename, pDir);
}

bool RamFs::createSymlink(
    File *parent, const String &filename, const String &value)
{
    return false;
}

bool RamFs::remove(File *parent, File *file)
{
    if (file->isDirectory())
        return false;

    RamDir *p = static_cast<RamDir *>(parent);
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
