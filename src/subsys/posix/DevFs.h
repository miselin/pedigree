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

#ifndef DEVFS_H
#define DEVFS_H

#include <vfs/Filesystem.h>
#include <vfs/Directory.h>
#include <vfs/File.h>

class DevFsDirectory;

void createPtyNodes(Filesystem *fs, DevFsDirectory *root, size_t &baseInode);

/** This class provides slightly more flexibility for adding files to a directory. */
class DevFsDirectory : public Directory
{
    public:
        DevFsDirectory(String name, Time::Timestamp accessedTime, Time::Timestamp modifiedTime, Time::Timestamp creationTime,
            uintptr_t inode, class Filesystem *pFs, size_t size, File *pParent) :
            Directory(name, accessedTime, modifiedTime, creationTime, inode,
                pFs, size, pParent)
        {
        }

        virtual ~DevFsDirectory()
        {
        }

        void addEntry(String name, File *pFile)
        {
            getCache().insert(name, pFile);
            m_bCachePopulated = true;
        }
};

/** This class provides /dev */
class DevFs : public Filesystem
{
public:
  DevFs() : m_pRoot(0)
  {
  };

  virtual ~DevFs()
  {
    delete m_pRoot;
  };

  static DevFs &instance()
  {
    return m_Instance;
  }

  virtual bool initialise(Disk *pDisk);

  virtual File* getRoot()
  {
    return m_pRoot;
  }
  virtual String getVolumeLabel()
  {
    return String("dev");
  }

protected:
  virtual bool createFile(File* parent, String filename, uint32_t mask)
  {return false;}
  virtual bool createDirectory(File* parent, String filename)
  {return false;}
  virtual bool createSymlink(File* parent, String filename, String value)
  {return false;}
  virtual bool remove(File* parent, File* file)
  {return false;}

private:

  DevFs(const DevFs &);
  DevFs &operator = (const DevFs &);

  DevFsDirectory *m_pRoot;
  static DevFs m_Instance;
};

#endif
