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

#include "ProcFs.h"

#include <BootstrapInfo.h>
#include <processor/Processor.h>
#include <time/Time.h>
#include <LockGuard.h>
#include <Version.h>

#include "file-syscalls.h"

/// \todo expose this via PhysicalMemoryManager interface
extern size_t g_FreePages;
extern size_t g_AllocedPages;

MeminfoFile::MeminfoFile(size_t inode, Filesystem *pParentFS, File *pParent) :
    File(String("meminfo"), 0, 0, 0, inode, pParentFS, 0, pParent),
    m_pUpdateThread(0), m_bRunning(false), m_Contents(), m_Lock(false)
{
    setPermissionsOnly(FILE_UR | FILE_UW | FILE_GR | FILE_GW | FILE_OR);
    setUidOnly(0);
    setGidOnly(0);

    m_bRunning = true;
    m_pUpdateThread = new Thread(Processor::information().getCurrentThread()->getParent(), run, this);
}

MeminfoFile::~MeminfoFile()
{
    m_bRunning = false;
    m_pUpdateThread->join();
}

size_t MeminfoFile::getSize()
{
    LockGuard<Mutex> guard(m_Lock);
    return m_Contents.length();
}

int MeminfoFile::run(void *p)
{
    MeminfoFile *pFile = reinterpret_cast<MeminfoFile *>(p);
    pFile->updateThread();
    return 0;
}

void MeminfoFile::updateThread()
{
    while (m_bRunning)
    {
        m_Lock.acquire();
        uint64_t freeKb = (g_FreePages * 4096) / 1024;  // each page is 4K
        uint64_t allocKb = (g_AllocedPages * 4096) / 1024;  // each page is 4K
        m_Contents.Format("MemTotal: %ld kB\nMemFree: %ld kB\nMemAvailable: %ld kB\n", freeKb + allocKb, freeKb, freeKb);
        m_Lock.release();

        Time::delay(1 * Time::Multiplier::SECOND);
    }
}

uint64_t MeminfoFile::read(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    LockGuard<Mutex> guard(m_Lock);

    if (location >= m_Contents.length())
    {
        return 0;  // EOF
    }
    else if ((location + size) > m_Contents.length())
    {
        size = m_Contents.length() - location;
    }

    char *destination = reinterpret_cast<char *>(buffer);
    const char *source = static_cast<const char *>(m_Contents);

    StringCopy(destination, source);

    return size;
}

uint64_t MeminfoFile::write(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    return 0;
}

MountFile::MountFile(size_t inode, Filesystem *pParentFS, File *pParent) :
    File(String("mounts"), 0, 0, 0, inode, pParentFS, 0, pParent)
{
    setPermissionsOnly(FILE_UR | FILE_GR | FILE_OR);
    setUidOnly(0);
    setGidOnly(0);
}

MountFile::~MountFile() = default;

uint64_t MountFile::read(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    String mounts;
    generate_mtab(mounts);

    if (location >= mounts.length())
    {
        // "EOF"
        return 0;
    }

    if ((location + size) >= mounts.length())
    {
        size = mounts.length() - location;
    }

    char *destination = reinterpret_cast<char *>(buffer);
    StringCopyN(destination, static_cast<const char *>(mounts) + location, size);

    return size;
}

uint64_t MountFile::write(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    return 0;
}

size_t MountFile::getSize()
{
    String mounts;
    generate_mtab(mounts);
    return mounts.length();
}

ConstantFile::ConstantFile(String name, String value, size_t inode, Filesystem *pParentFS, File *pParent) :
    File(name, 0, 0, 0, inode, pParentFS, 0, pParent), m_Contents(value)
{
    setPermissionsOnly(FILE_UR | FILE_UW | FILE_GR | FILE_GW | FILE_OR);
    setUidOnly(0);
    setGidOnly(0);
}

ConstantFile::~ConstantFile()
{}

uint64_t ConstantFile::read(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    if (location >= m_Contents.length())
    {
        return 0;  // EOF
    }
    else if ((location + size) > m_Contents.length())
    {
        size = m_Contents.length() - location;
    }

    char *destination = reinterpret_cast<char *>(buffer);
    const char *source = static_cast<const char *>(m_Contents);

    StringCopy(destination, source);

    return size;
}

uint64_t ConstantFile::write(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    return 0;
}

size_t ConstantFile::getSize()
{
    return m_Contents.length();
}

ProcFs::~ProcFs()
{
    delete m_pRoot;
}

bool ProcFs::initialise(Disk *pDisk)
{
    // Deterministic inode assignment to each ProcFs node
    m_NextInode = 0;

    if (m_pRoot)
    {
        delete m_pRoot;
    }

    m_pRoot = new ProcFsDirectory(String(""), 0, 0, 0, getNextInode(), this, 0, 0);
    // Allow user/group to read and write, but disallow all others anything
    // other than the ability to list and access files.
    m_pRoot->setPermissions(FILE_UR | FILE_UW | FILE_UX | FILE_GR | FILE_GW | FILE_GX | FILE_OR | FILE_OX);

    MeminfoFile *meminfo = new MeminfoFile(getNextInode(), this, m_pRoot);
    m_pRoot->addEntry(meminfo->getName(), meminfo);

    /// \todo also probably need /etc/mtab...
    MountFile *mounts = new MountFile(getNextInode(), this, m_pRoot);
    m_pRoot->addEntry(mounts->getName(), mounts);

    ConstantFile *pFilesystems = new ConstantFile(String("filesystems"), String("\text2\nnodev\tproc\nnodev\ttmpfs\n"), getNextInode(), this, m_pRoot);
    m_pRoot->addEntry(pFilesystems->getName(), pFilesystems);

    // Kernel command line
    String cmdline(g_pBootstrapInfo->getCommandLine());
    cmdline += " single noswap";  // ensure we get into single user mode in Linux userspaces
    ConstantFile *pCmdline = new ConstantFile(String("cmdline"), cmdline, getNextInode(), this, m_pRoot);
    m_pRoot->addEntry(pCmdline->getName(), pCmdline);

    // /proc/version contains some extra version info (not same as uname)
    String version;
    version.Format("Pedigree version %s (%s@%s) %s", g_pBuildRevision, g_pBuildUser, g_pBuildMachine, g_pBuildTime);
    ConstantFile *pVersion = new ConstantFile(String("version"), version, getNextInode(), this, m_pRoot);
    m_pRoot->addEntry(pVersion->getName(), pVersion);

    return true;
}

size_t ProcFs::getNextInode()
{
    return m_NextInode++;
}

void ProcFs::revertInode()
{
    --m_NextInode;
}
