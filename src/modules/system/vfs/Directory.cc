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

#include "Directory.h"
#include "Filesystem.h"
#include "VFS.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/Pair.h"
#include "pedigree/kernel/utilities/Result.h"
#include "pedigree/kernel/utilities/StringView.h"
#include "pedigree/kernel/utilities/Vector.h"

template class HashTable<String, Directory::DirectoryEntry *, HashedStringView>;

Directory::Directory() : File(), m_Cache(nullptr), m_bCachePopulated(false)
{
}

Directory::Directory(
    const String &name, Time::Timestamp accessedTime,
    Time::Timestamp modifiedTime, Time::Timestamp creationTime, uintptr_t inode,
    Filesystem *pFs, size_t size, File *pParent)
    : File(
          name, accessedTime, modifiedTime, creationTime, inode, pFs, size,
          pParent),
      m_Cache(nullptr), m_bCachePopulated(false)
{
}

Directory::~Directory()
{
    emptyCache();
}

File *Directory::getChild(size_t n)
{
    if (UNLIKELY(!m_bCachePopulated))
    {
        cacheDirectoryContents();
        m_bCachePopulated = true;
    }

    DirectoryEntryCache::PairLookupResult result = m_Cache.getNth(n);
    if (result.hasError())
    {
        return 0;
    }
    else
    {
        return result.value().second()->get();
    }
}

size_t Directory::getNumChildren()
{
    if (UNLIKELY(!m_bCachePopulated))
    {
        cacheDirectoryContents();
        m_bCachePopulated = true;
    }

    return m_Cache.count();
}

void Directory::cacheDirectoryContents()
{
}

File *Directory::lookup(const HashedStringView &s) const
{
    if (LIKELY(m_bCachePopulated))
    {
        DirectoryEntryCache::LookupResult result = m_Cache.lookup(s);
        if (result.hasValue())
        {
            return result.value()->get();
        }
    }
    return nullptr;
}

void Directory::remove(const HashedStringView &s)
{
    DirectoryEntryCache::LookupResult result = m_Cache.lookup(s);
    if (result.hasValue())
    {
        DirectoryEntry *v = result.value();
        /// \todo add sibling keys for other HashTable functions
        m_Cache.remove(s.toString());
        delete v;
    }
}

void Directory::addDirectoryEntry(const String &name, File *pTarget)
{
    assert(pTarget != nullptr);

    DirectoryEntry *entry = new DirectoryEntry(pTarget);

    if (!m_Cache.insert(name, entry))
    {
        ERROR(
            "can't add directory entry for '" << name
                                              << "' as it already exists.");
        delete entry;
    }
    else
    {
        // Track eagerly added file object
        VFS::instance().trackFile(pTarget);

        m_bCachePopulated = true;
    }
}

void Directory::addDirectoryEntry(
    const String &name, DirectoryEntryMetadata &&meta)
{
    DirectoryEntry *entry = new DirectoryEntry(pedigree_std::move(meta));

    if (!m_Cache.insert(name, entry))
    {
        ERROR(
            "can't add directory entry for '" << name
                                              << "' as it already exists.");
        delete entry;
    }
    else
    {
        m_bCachePopulated = true;
    }
}

Directory *Directory::getReparsePoint() const
{
    return m_ReparseTarget;
}

void Directory::setReparsePoint(Directory *pTarget)
{
    m_ReparseTarget = pTarget;
}

bool Directory::addEphemeralFile(File *pFile)
{
    assert(pFile != nullptr);

    if (UNLIKELY(!m_bCachePopulated))
    {
        cacheDirectoryContents();
        m_bCachePopulated = true;
    }

    if (m_Cache.lookup(pFile->getName()).hasValue())
    {
        // already exists!
        return false;
    }

    /// \todo removal will still want to hit the Filesystem here! not good!
    DirectoryEntry *entry = new DirectoryEntry(pFile);
    m_Cache.insert(pFile->getName(), entry);

    VFS::instance().trackFile(pFile);

    return true;
}

bool Directory::empty()
{
    // Need to make sure we can safely remove all nodes regardless of what
    // happens to the directory cache while we empty
    Vector<DirectoryEntry *> dentries;
    Vector<File *> entries;
    for (auto it = m_Cache.begin(); it != m_Cache.end(); ++it)
    {
        dentries.pushBack(*it);
        entries.pushBack((*it)->get());
    }

    for (auto it : entries)
    {
        if (!getFilesystem()->remove(this, it))
        {
            /// \note partial failure - some entries have been deleted by this
            /// point!
            return false;
        }
    }

    m_Cache.clear();

    for (auto it : dentries)
    {
        delete it;
    }

    return true;
}

void Directory::emptyCache()
{
    Vector<DirectoryEntry *> entries;
    for (auto it : m_Cache)
    {
        entries.pushBack(it);
    }

    m_Cache.clear();
    m_bCachePopulated = false;

    // Now that the hashtable is flattened into this vector, it's safe to
    // delete without worrying about our deletion modifying the table.
    for (auto it : entries)
    {
        delete it;
    }
}

File *Directory::evaluateEntry(const DirectoryEntryMetadata &meta)
{
    if (!meta.pDirectory)
    {
        return nullptr;
    }
    File *newFile = meta.pDirectory->convertToFile(meta);

    // Track this lazy-loaded directory entry.
    VFS::instance().trackFile(newFile);

    return newFile;
}

void Directory::destroyEntry(File *file)
{
    VFS::instance().untrackFile(file);
}

File *Directory::convertToFile(const DirectoryEntryMetadata &meta)
{
    return nullptr;
}

void Directory::preallocateDirectoryEntries(size_t count)
{
    m_Cache.reserve(count);
}

Directory::DirectoryEntryMetadata::DirectoryEntryMetadata()
    : pDirectory(nullptr), filename(), opaque()
{
}
Directory::DirectoryEntryMetadata::DirectoryEntryMetadata(
    Directory::DirectoryEntryMetadata &&other)
    : pDirectory(pedigree_std::move(other.pDirectory)),
      filename(pedigree_std::move(other.filename)),
      opaque(pedigree_std::move(other.opaque))
{
    other.pDirectory = nullptr;
}

Directory::DirectoryEntryMetadata::~DirectoryEntryMetadata()
{
    opaque.reset();
}
