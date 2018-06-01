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

#ifndef EXT2_DIRECTORY_H
#define EXT2_DIRECTORY_H

#include "Ext2Node.h"
#include "modules/system/vfs/Directory.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/String.h"

class File;
struct Inode;

/** A File is a file, a directory or a symlink. */
class Ext2Directory : public Directory, public Ext2Node
{
  private:
    /** Copy constructors are hidden - unused! */
    Ext2Directory(const Ext2Directory &file);
    Ext2Directory &operator=(const Ext2Directory &);

  public:
    /** Constructor, should be called only by a Filesystem. */
    Ext2Directory(
        const String &name, uintptr_t inode_num, Inode *inode,
        class Ext2Filesystem *pFs, File *pParent);
    /** Destructor */
    virtual ~Ext2Directory();

    void truncate()
    {
    }

    /** Reads directory contents into File* cache. */
    virtual void cacheDirectoryContents();

    /** Adds a directory entry. */
    virtual bool addEntry(String filename, File *pFile, size_t type);
    /** Removes a directory entry. */
    virtual bool removeEntry(const String &filename, Ext2Node *pFile);

    /** Updates inode attributes. */
    void fileAttributeChanged();

  private:
    virtual File *convertToFile(const DirectoryEntryMetadata &meta);
};

#endif
