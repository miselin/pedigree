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

#ifndef PROCFS_H
#define PROCFS_H

#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Timer.h"

#include "pedigree/kernel/Log.h"

#include "pedigree/kernel/utilities/ExtensibleBitmap.h"
#include <vfs/Directory.h>
#include <vfs/File.h>
#include <vfs/Filesystem.h>

class ProcFs;
class ProcFsDirectory;
class Thread;
class PosixProcess;

class MeminfoFile : public File
{
  public:
    MeminfoFile(size_t inode, Filesystem *pParentFS, File *pParent);
    ~MeminfoFile();

    uint64_t read(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    uint64_t write(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);

    virtual size_t getSize();

    static int run(void *p);

    void updateThread();

  private:
    Thread *m_pUpdateThread;
    bool m_bRunning;
    String m_Contents;
    Mutex m_Lock;
};

class MountFile : public File
{
  public:
    MountFile(size_t inode, Filesystem *pParentFS, File *pParent);
    ~MountFile();

    uint64_t read(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    uint64_t write(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);

    virtual size_t getSize();
};

class UptimeFile : public File
{
  public:
    UptimeFile(size_t inode, Filesystem *pParentFS, File *pParent);
    ~UptimeFile();

    uint64_t read(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    uint64_t write(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);

    virtual size_t getSize();

  private:
    String generateString();
};

class ConstantFile : public File
{
  public:
    ConstantFile(
        String name, String value, size_t inode, Filesystem *pParentFS,
        File *pParent);
    ~ConstantFile();

    uint64_t read(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    uint64_t write(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);

    virtual size_t getSize();

  private:
    String m_Contents;
};

/** This class provides slightly more flexibility for adding files to a
 * directory. */
class ProcFsDirectory : public Directory
{
  public:
    ProcFsDirectory(
        String name, Time::Timestamp accessedTime, Time::Timestamp modifiedTime,
        Time::Timestamp creationTime, uintptr_t inode, class Filesystem *pFs,
        size_t size, File *pParent)
        : Directory(
              name, accessedTime, modifiedTime, creationTime, inode, pFs, size,
              pParent)
    {
    }

    virtual ~ProcFsDirectory()
    {
    }

    void addEntry(String name, File *pFile)
    {
        addDirectoryEntry(name, pFile);
    }
};

/** This class provides /dev */
class ProcFs : public Filesystem
{
  public:
    ProcFs() : m_pRoot(0)
    {
    }

    virtual ~ProcFs();

    virtual bool initialise(Disk *pDisk);

    virtual File *getRoot()
    {
        return m_pRoot;
    }
    virtual String getVolumeLabel()
    {
        return String("proc");
    }

    virtual size_t getNextInode();
    virtual void revertInode();

    void addProcess(PosixProcess *proc);
    void removeProcess(PosixProcess *proc);

  protected:
    virtual bool createFile(File *parent, String filename, uint32_t mask)
    {
        return false;
    }
    virtual bool createDirectory(File *parent, String filename, uint32_t mask)
    {
        return false;
    }
    virtual bool createSymlink(File *parent, String filename, String value)
    {
        return false;
    }
    virtual bool remove(File *parent, File *file)
    {
        return false;
    }

  private:
    ProcFs(const ProcFs &);
    ProcFs &operator=(const ProcFs &);

    ProcFsDirectory *m_pRoot;

    Tree<size_t, ProcFsDirectory *> m_pProcessDirectories;

    size_t m_NextInode;
};

#endif  // PROCFS_H
