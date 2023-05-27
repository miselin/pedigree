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

#ifndef EXT2FILESYSTEM_H
#define EXT2FILESYSTEM_H

#include "modules/system/vfs/Filesystem.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/String.h"

class Disk;
class File;
struct GroupDesc;
struct Inode;
struct Superblock;
template <class T>
class Vector;

/** This class provides an implementation of the second extended filesystem. */
class Ext2Filesystem : public Filesystem
{
    friend class Ext2File;
    friend class Ext2Node;
    friend class Ext2Directory;
    friend class Ext2Symlink;

  public:
    Ext2Filesystem();

    virtual ~Ext2Filesystem();

    //
    // Filesystem interface.
    //
    virtual bool initialise(Disk *pDisk);
    static Filesystem *probe(Disk *pDisk);
    virtual File *getRoot() const;
    virtual String getVolumeLabel() const;

  protected:
    virtual bool
    createFile(File *parent, const String &filename, uint32_t mask);
    virtual bool
    createDirectory(File *parent, const String &filename, uint32_t mask);
    virtual bool
    createSymlink(File *parent, const String &filename, const String &value);
    virtual bool createLink(File *parent, const String &filename, File *target);
    virtual bool remove(File *parent, File *file);

  private:
    virtual bool createNode(
        File *parent, const String &filename, uint32_t mask,
        const String &value, size_t type, uint32_t inodeOverride = 0);

    /** Inaccessible copy constructor and operator= */
    Ext2Filesystem(const Ext2Filesystem &);
    void operator=(const Ext2Filesystem &);

    /** Reads a block of data from the disk. */
    uintptr_t readBlock(uint32_t block);
    /** Writes a block of data to the disk. */
    void writeBlock(uint32_t block);

    void pinBlock(uint64_t location);
    void unpinBlock(uint64_t location);

    void sync(size_t offset, bool async);

    uint32_t findFreeBlock(uint32_t inode);
    bool findFreeBlocks(uint32_t inode, size_t count, Vector<uint32_t> &blocks);
    size_t findFreeBlocksInGroup(
        uint32_t group, size_t maxCount, Vector<uint32_t> &blocks);
    uint32_t findFreeInode();

    void releaseBlock(uint32_t block);
    /** Releases the given inode, returns true if the inode had no more links.
     */
    bool releaseInode(uint32_t inode);

    Inode *getInode(uint32_t num);
    void writeInode(uint32_t num);

    void ensureFreeBlockBitmapLoaded(size_t group);
    void ensureFreeInodeBitmapLoaded(size_t group);
    void ensureInodeTableLoaded(size_t group);

    bool checkOptionalFeature(size_t feature);
    bool checkRequiredFeature(size_t feature);
    bool checkReadOnlyFeature(size_t feature);

    void increaseInodeRefcount(uint32_t inode);
    bool decreaseInodeRefcount(uint32_t inode);

    /** Our superblock. */
    Superblock *m_pSuperblock;

    /** Group descriptors, in a tree because each GroupDesc* may be in a
     * different block. */
    GroupDesc **m_pGroupDescriptors;

    /** Inode tables, indexed by group descriptor. */
    Vector<size_t> *m_pInodeTables;
    /** Free inode bitmaps, indexed by group descriptor. */
    Vector<size_t> *m_pInodeBitmaps;
    /** Free block bitmaps, indexed by group descriptor. */
    Vector<size_t> *m_pBlockBitmaps;

    /** Size of a block. */
    uint32_t m_BlockSize;

    /** Size of an Inode. */
    uint32_t m_InodeSize;

    /** Number of group descriptors. */
    size_t m_nGroupDescriptors;

#if THREADS
    /** Write lock - we're finding some inodes and updating the superblock and
     * block group structures. */
    Mutex m_WriteLock;
#endif

    /** The root filesystem node. */
    File *m_pRoot;

    /** Cached volume label. */
    String m_VolumeLabel;
};

#endif
