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

#ifndef RAMFS_H
#define RAMFS_H

/**\file  RamFs.h
 *\author Matthew Iselin
 *\date   Sun May 17 10:00:00 2009
 *\brief  An in-RAM filesystem. */

#include "modules/system/vfs/Directory.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/Filesystem.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Cache.h"
#include "pedigree/kernel/utilities/String.h"

class Disk;

class EXPORTED_PUBLIC RamFile : public File
{
  public:
    RamFile(
        const String &name, uintptr_t inode, Filesystem *pParentFS,
        File *pParent);

    virtual ~RamFile();

    virtual void truncate();

    bool canWrite();

  protected:
    virtual uintptr_t readBlock(uint64_t location);

    virtual void pinBlock(uint64_t location);

    virtual void unpinBlock(uint64_t location);

  private:
    Cache m_FileBlocks;

    size_t m_nOwnerPid;
};

/** Defines a directory in the RamFS */
class EXPORTED_PUBLIC RamDir : public Directory
{
  private:
    RamDir(const RamDir &);
    RamDir &operator=(const RamDir &);

  public:
    RamDir(
        const String &name, size_t inode, class Filesystem *pFs, File *pParent);
    virtual ~RamDir();

    virtual void cacheDirectoryContents()
    {
    }

    virtual bool addEntry(String filename, File *pFile);

    virtual bool removeEntry(File *pFile);
};

/** Defines a filesystem that is completely in RAM. */
class EXPORTED_PUBLIC RamFs : public Filesystem
{
  public:
    RamFs();
    virtual ~RamFs();

    virtual bool initialise(Disk *pDisk);

    void setProcessOwnership(bool bEnable)
    {
        m_bProcessOwners = bEnable;
    }

    bool getProcessOwnership() const
    {
        return m_bProcessOwners;
    }

    virtual File *getRoot() const
    {
        return m_pRoot;
    }
    virtual const String &getVolumeLabel() const
    {
        return m_VolumeLabel;
    }

  protected:
    virtual bool
    createFile(File *parent, const String &filename, uint32_t mask);
    virtual bool
    createDirectory(File *parent, const String &filename, uint32_t mask);
    virtual bool
    createSymlink(File *parent, const String &filename, const String &value);
    virtual bool remove(File *parent, File *file);

    RamFs(const RamFs &);
    void operator=(const RamFs &);

    static String m_VolumeLabel;

    /** Root filesystem node. */
    File *m_pRoot;

    bool m_bProcessOwners;
};

#endif
