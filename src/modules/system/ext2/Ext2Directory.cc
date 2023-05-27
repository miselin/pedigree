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

#include "Ext2Directory.h"
#include "Ext2File.h"
#include "Ext2Filesystem.h"
#include "Ext2Symlink.h"
#include "ext2.h"
#include "modules/system/vfs/File.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/stddef.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Pointers.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

class Filesystem;

Ext2Directory::Ext2Directory(
    const String &name, uintptr_t inode_num, Inode *inode, Ext2Filesystem *pFs,
    File *pParent)
    : Directory(
          name, LITTLE_TO_HOST32(inode->i_atime),
          LITTLE_TO_HOST32(inode->i_mtime), LITTLE_TO_HOST32(inode->i_ctime),
          inode_num, static_cast<Filesystem *>(pFs),
          LITTLE_TO_HOST32(inode->i_size),  /// \todo Deal with >4GB files here.
          pParent),
      Ext2Node(inode_num, inode, pFs)
{
    uint32_t mode = LITTLE_TO_HOST32(inode->i_mode);
    setPermissionsOnly(modeToPermissions(mode));
    setUidOnly(LITTLE_TO_HOST16(inode->i_uid));
    setGidOnly(LITTLE_TO_HOST16(inode->i_gid));
}

Ext2Directory::~Ext2Directory()
{
}

bool Ext2Directory::addEntry(const String &filename, File *pFile, size_t type)
{
    // Make sure we're already cached before we add an entry.
    cacheDirectoryContents();

    // Calculate the size of our Dir* entry.
    size_t length =
        4 + /* 32-bit inode number */
        2 + /* 16-bit record length */
        1 + /* 8-bit name length */
        1 + /* 8-bit file type */
        filename
            .length(); /* Don't leave space for NULL-terminator, not needed. */

    bool bFound = false;

    uint32_t i;
    Dir *pDir = 0;
    Dir *pLastDir = 0;
    Dir *pBlockEnd = 0;
    for (i = 0; i < m_Blocks.count(); i++)
    {
        ensureBlockLoaded(i);
        uintptr_t buffer = m_pExt2Fs->readBlock(m_Blocks[i]);
        pLastDir = pDir;
        pDir = reinterpret_cast<Dir *>(buffer);
        pBlockEnd = adjust_pointer(pDir, m_pExt2Fs->m_BlockSize);
        while (pDir < pBlockEnd)
        {
            // What's the minimum length of this directory entry?
            size_t thisReclen = 4 + 2 + 1 + 1 + pDir->d_namelen;
            // Align to 4-byte boundary.
            if (thisReclen % 4)
            {
                thisReclen += 4 - (thisReclen % 4);
            }

            // Valid directory entry?
            uint16_t entryReclen = LITTLE_TO_HOST16(pDir->d_reclen);
            if (pDir->d_inode > 0)
            {
                // Is there enough space to add this dirent?
                /// \todo Ensure 4-byte alignment.
                if (entryReclen - thisReclen >= length)
                {
                    bFound = true;
                    // Save the current reclen.
                    uint16_t oldReclen = entryReclen;
                    // Adjust the current record's reclen field to the minimum.
                    pDir->d_reclen = HOST_TO_LITTLE16(thisReclen);
                    // Move to the new directory entry location.
                    pDir = adjust_pointer(pDir, thisReclen);
                    // New record length.
                    uint16_t newReclen = oldReclen - thisReclen;
                    // Set the new record length.
                    pDir->d_reclen = HOST_TO_LITTLE16(newReclen);
                    break;
                }
            }
            else if (entryReclen == 0)
            {
                // No more entries to follow.
                break;
            }
            else if (entryReclen - thisReclen >= length)
            {
                // We can use this unused entry - we fit into it.
                // The record length does not need to be adjusted.
                bFound = true;
                break;
            }

            // Next.
            pLastDir = pDir;
            pDir = adjust_pointer(pDir, entryReclen);
        }
        if (bFound)
            break;
    }

    if (!bFound || !pDir)
    {
        // Need to make a new block.
        uint32_t block = m_pExt2Fs->findFreeBlock(getInodeNumber());
        if (block == 0)
        {
            // We had a problem.
            SYSCALL_ERROR(NoSpaceLeftOnDevice);
            return false;
        }
        if (!addBlock(block))
            return false;
        i = m_Blocks.count() - 1;

        m_Size = m_Blocks.count() * m_pExt2Fs->m_BlockSize;
        fileAttributeChanged();

        /// \todo Previous directory entry might need its reclen updated to
        ///       point to this new entry (as directory entries cannot cross
        ///       block boundaries).

        ensureBlockLoaded(i);
        uintptr_t buffer = m_pExt2Fs->readBlock(m_Blocks[i]);

        ByteSet(reinterpret_cast<void *>(buffer), 0, m_pExt2Fs->m_BlockSize);
        pDir = reinterpret_cast<Dir *>(buffer);
        pDir->d_reclen = HOST_TO_LITTLE16(m_pExt2Fs->m_BlockSize);

        /// \todo Update our i_size for our directory.
    }

    // Set the directory contents.
    uint32_t entryInode = pFile->getInode();
    pDir->d_inode = HOST_TO_LITTLE32(entryInode);
    m_pExt2Fs->increaseInodeRefcount(entryInode);

    if (m_pExt2Fs->checkRequiredFeature(2))
    {
        // File type in directory entry.
        switch (type)
        {
            case EXT2_S_IFREG:
                pDir->d_file_type = EXT2_FILE;
                break;
            case EXT2_S_IFDIR:
                pDir->d_file_type = EXT2_DIRECTORY;
                break;
            case EXT2_S_IFLNK:
                pDir->d_file_type = EXT2_SYMLINK;
                break;
            default:
                ERROR("Unrecognised filetype.");
                pDir->d_file_type = EXT2_UNKNOWN;
        }
    }
    else
    {
        // No file type in directory entries.
        pDir->d_file_type = 0;
    }

    pDir->d_namelen = filename.length();
    MemoryCopy(
        pDir->d_name, static_cast<const char *>(filename), filename.length());

    // We're all good - add the directory to our cache.
    addDirectoryEntry(filename, pFile);

    // Trigger write back to disk.
    m_pExt2Fs->writeBlock(m_Blocks[i]);

    m_Size = m_nSize;

    return true;
}

bool Ext2Directory::removeEntry(const String &filename, Ext2Node *pFile)
{
    // Find this file in the directory.
    size_t fileInode = pFile->getInodeNumber();

    bool bFound = false;

    uint32_t i;
    Dir *pDir, *pLastDir = 0;
    for (i = 0; i < m_Blocks.count(); i++)
    {
        ensureBlockLoaded(i);
        uintptr_t buffer = m_pExt2Fs->readBlock(m_Blocks[i]);
        pDir = reinterpret_cast<Dir *>(buffer);
        pLastDir = 0;
        while (reinterpret_cast<uintptr_t>(pDir) <
               buffer + m_pExt2Fs->m_BlockSize)
        {
            if (LITTLE_TO_HOST32(pDir->d_inode) == fileInode)
            {
                if (pDir->d_namelen == filename.length())
                {
                    if (!StringCompareN(
                            pDir->d_name, static_cast<const char *>(filename),
                            pDir->d_namelen))
                    {
                        // Wipe out the directory entry.
                        uint16_t old_reclen = LITTLE_TO_HOST16(pDir->d_reclen);
                        ByteSet(pDir, 0, old_reclen);

                        /// \todo Okay, this is not quite enough. The previous
                        ///       entry needs to be updated to skip past this
                        ///       now-empty entry. If this was the first entry,
                        ///       a blank record must be created to point to
                        ///       either the next entry or the end of the block.

                        pDir->d_reclen = HOST_TO_LITTLE16(old_reclen);

                        m_pExt2Fs->writeBlock(m_Blocks[i]);
                        bFound = true;
                        break;
                    }
                }
            }
            else if (!pDir->d_reclen)
            {
                // No more entries.
                break;
            }

            pDir = reinterpret_cast<Dir *>(
                reinterpret_cast<uintptr_t>(pDir) +
                LITTLE_TO_HOST16(pDir->d_reclen));
        }

        if (bFound)
            break;
    }

    m_Size = m_nSize;

    if (bFound)
    {
        if (m_pExt2Fs->releaseInode(fileInode))
        {
            // Remove all blocks for the file, inode has hit zero refcount.
            pFile->wipe();
        }
        return true;
    }
    else
    {
        SYSCALL_ERROR(DoesNotExist);
        return false;
    }
}

void Ext2Directory::cacheDirectoryContents()
{
    if (isCachePopulated())
    {
        return;
    }

    uint32_t i;
    Dir *pDir;
    for (i = 0; i < m_Blocks.count(); i++)
    {
        ensureBlockLoaded(i);

        // Grab the block and pin it while we parse it.
        uintptr_t buffer = m_pExt2Fs->readBlock(m_Blocks[i]);
        uintptr_t endOfBlock = buffer + m_pExt2Fs->m_BlockSize;
        assert(buffer);  /// \todo need to handle short/failed reads better
        pDir = reinterpret_cast<Dir *>(buffer);

        while (reinterpret_cast<uintptr_t>(pDir) < endOfBlock)
        {
            size_t reclen = LITTLE_TO_HOST16(pDir->d_reclen);

            Dir *pNextDir = adjust_pointer(pDir, reclen);
            if (pDir->d_inode == 0)
            {
                if (pDir == pNextDir)
                {
                    // No further iteration possible (null entry).
                    break;
                }

                // Oops, not a valid entry (possibly deleted file). Skip.
                pDir = pNextDir;
                continue;
            }
            else if (pNextDir >= reinterpret_cast<Dir *>(endOfBlock))
            {
                // TODO: this naive approach breaks both sides of the boundary
                // as the next entry likely starts offset into the block
                ERROR("EXT2: Directory entry straddles a block boundary");
                pDir = pNextDir;
                continue;
            }

            // we only need inode + file type fields, to save memory
            size_t copylen = offsetof(Dir, d_name);

            DirectoryEntryMetadata meta;
            meta.pDirectory = this;
            meta.opaque =
                pedigree_std::move(UniqueArray<char>::allocate(copylen));
            MemoryCopy(meta.opaque.get(), pDir, copylen);

            size_t namelen = pDir->d_namelen;

            // Can we get the file type from the directory entry?
            size_t fileType = EXT2_UNKNOWN;
            bool ok = true;
            if (m_pExt2Fs->checkRequiredFeature(2))
            {
                // Yep! Use that here.
                fileType = pDir->d_file_type;
                switch (fileType)
                {
                    case EXT2_FILE:
                    case EXT2_DIRECTORY:
                    case EXT2_SYMLINK:
                        break;
                    default:
                        ERROR(
                            "EXT2: Directory entry has unsupported file type: "
                            << pDir->d_file_type);
                        ok = false;
                        break;
                }
            }
            else
            {
                // No! Need to read the inode.
                uint32_t inodeNum = LITTLE_TO_HOST32(pDir->d_inode);
                Inode *inode = m_pExt2Fs->getInode(inodeNum);

                // Acceptable file type?
                size_t inode_ftype = inode->i_mode & 0xF000;
                switch (inode_ftype)
                {
                    case EXT2_S_IFLNK:
                    case EXT2_S_IFREG:
                    case EXT2_S_IFDIR:
                        break;
                    default:
                        ERROR(
                            "EXT2: Inode has unsupported file type: "
                            << inode_ftype << ".");
                        ok = false;
                        break;
                }

                // In this case, the file type entry is the top 8 bits of the
                // filename length.
                namelen |= pDir->d_file_type << 8;
            }

            if (ok)
            {
                String filename(pDir->d_name, namelen);
                meta.filename = filename;  // copy into the metadata structure
                addDirectoryEntry(filename, pedigree_std::move(meta));
            }

            // Next.
            pDir = pNextDir;
        }

        // Done with this block now; nothing remains that points to it.
        m_pExt2Fs->unpinBlock(m_Blocks[i]);
    }

    markCachePopulated();
}

void Ext2Directory::fileAttributeChanged()
{
    static_cast<Ext2Node *>(this)->fileAttributeChanged(
        m_Size, m_AccessedTime, m_ModifiedTime, m_CreationTime);
    static_cast<Ext2Node *>(this)->updateMetadata(
        getUid(), getGid(), permissionsToMode(getPermissions()));
}

File *Ext2Directory::convertToFile(const DirectoryEntryMetadata &meta)
{
    Dir *pDir = reinterpret_cast<Dir *>(meta.opaque.get());

    uint32_t inodeNum = LITTLE_TO_HOST32(pDir->d_inode);
    Inode *inode = m_pExt2Fs->getInode(inodeNum);

    // Can we get the file type from the directory entry?
    size_t fileType = EXT2_UNKNOWN;
    if (m_pExt2Fs->checkRequiredFeature(2))
    {
        // Directory entry holds file type.
        fileType = pDir->d_file_type;
    }
    else
    {
        // Inode holds file type.
        size_t inode_ftype = inode->i_mode & 0xF000;
        switch (inode_ftype)
        {
            case EXT2_S_IFLNK:
                fileType = EXT2_SYMLINK;
                break;
            case EXT2_S_IFREG:
                fileType = EXT2_FILE;
                break;
            case EXT2_S_IFDIR:
                fileType = EXT2_DIRECTORY;
                break;
            default:
                // this should have been validated previously
                FATAL("Bad inode file type in Ext2Directory::convertToFile");
                break;
        }
    }

    File *pFile = 0;
    switch (fileType)
    {
        case EXT2_FILE:
            pFile =
                new Ext2File(meta.filename, inodeNum, inode, m_pExt2Fs, this);
            break;
        case EXT2_DIRECTORY:
            pFile = new Ext2Directory(
                meta.filename, inodeNum, inode, m_pExt2Fs, this);
            break;
        case EXT2_SYMLINK:
            pFile = new Ext2Symlink(
                meta.filename, inodeNum, inode, m_pExt2Fs, this);
            break;
        default:
            // this should have been validated previously
            FATAL("Bad file type in Ext2Directory::convertToFile");
            break;
    }

    return pFile;
}
