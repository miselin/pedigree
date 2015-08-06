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

#ifndef ISO9660DIRECTORY_H
#define ISO9660DIRECTORY_H

#include <vfs/Filesystem.h>
#include <vfs/Directory.h>
#include <utilities/List.h>
#include <utilities/Vector.h>
#include <utilities/Tree.h>
#include <process/Mutex.h>
#include <LockGuard.h>

#include "Iso9660Filesystem.h"
#include "iso9660.h"

class Iso9660Directory : public Directory
{
  friend class Iso9660File;

private:
  Iso9660Directory(const Iso9660Directory &);
  Iso9660Directory& operator =(const Iso9660Directory&);
public:
  Iso9660Directory(String name, size_t inode,
      class Iso9660Filesystem *pFs, File *pParent, Iso9660DirRecord &dirRec,
      Time accessedTime = 0, Time modifiedTime = 0, Time creationTime = 0);
  virtual ~Iso9660Directory() {};

  virtual void cacheDirectoryContents();

  virtual bool addEntry(String filename, File *pFile, size_t type)
  {
    return false;
  }

  virtual bool removeEntry(File *pFile)
  {
    return false;
  }

  void fileAttributeChanged()
  {};

  inline Iso9660DirRecord &getDirRecord()
  {
    return m_Dir;
  }

private:
  // Filesystem object
  Iso9660Filesystem *m_pFs;

  // Our internal directory information (info about *this* directory, not the child)
  Iso9660DirRecord m_Dir;
};

#endif
