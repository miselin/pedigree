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

#ifndef DIRECTORY_H
#define DIRECTORY_H

#include <time/Time.h>
#include <processor/types.h>
#include <utilities/RadixTree.h>
#include "File.h"

/** A Directory node. */
class Directory : public File
{
public:

    /** Eases the pain of casting, and performs a sanity check. */
    static Directory *fromFile(File *pF)
    {
        if (!pF->isDirectory()) FATAL("Casting non-directory File to Directory!");
        return reinterpret_cast<Directory*> (pF);
    }

    /** Constructor, creates an invalid directory. */
    Directory();

    /** Copy constructors are hidden - unused! */
private:
    Directory(const Directory &file);
    Directory& operator =(const Directory&);

public:
    /** Constructor, should be called only by a Filesystem. */
    Directory(String name, Time::Timestamp accessedTime, Time::Timestamp modifiedTime, Time::Timestamp creationTime,
              uintptr_t inode, class Filesystem *pFs, size_t size, File *pParent);
    /** Destructor - doesn't do anything. */
    virtual ~Directory();

    /** Returns true if the File is actually a directory. */
    virtual bool isDirectory()
    {return true;}

    /** Returns the n'th child of this directory, or an invalid file. */
    File* getChild(size_t n);

    /** Returns the number of children in this directory. */
    size_t getNumChildren();

    /** Load the directory's contents into the cache. */
    virtual void cacheDirectoryContents();

    /** Does this directory have cache? */
    virtual bool isCachePopulated() const
    {
        return m_bCachePopulated;
    }

    /** Look up the given filename in the directory. */
    File *lookup(String &s) const
    {
        return m_Cache.lookup(s);
    }

    /** Remove the given filename in the directory. */
    void remove(String &s)
    {
        m_Cache.remove(s);
    }

private:
    /** Directory contents cache. */
    RadixTree<File*> m_Cache;

protected:
    /** Provides subclasses with direct access to the directory's listing. */
    virtual RadixTree<File *> &getCache()
    {
        return m_Cache;
    }

    /**
     * Whether the directory cache is populated with entries or still needs to
     * be loaded. Directories are lazy-loaded using this.
     */
    bool m_bCachePopulated;
};

#endif
