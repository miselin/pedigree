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

#ifndef FATFILESYSTEM_H
#define FATFILESYSTEM_H

#include "FatFile.h"
#include "modules/system/vfs/Filesystem.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/utilities/Cache.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/Tree.h"
#include "pedigree/kernel/utilities/UnlikelyLock.h"
#include "pedigree/kernel/utilities/Vector.h"

#include "FatDirectory.h"
#include "FatFile.h"
#include "fat.h"

/** This class provides an implementation of the FAT filesystem. */
class FatFilesystem : public Filesystem
{
    friend class FatFile;
    friend class FatDirectory;

  public:
    FatFilesystem();

    virtual ~FatFilesystem();

    //
    // Filesystem interface.
    //

    virtual bool initialise(Disk *pDisk);
    static Filesystem *probe(Disk *pDisk);
    virtual File *getRoot() const;
    virtual const String &getVolumeLabel() const;
    virtual uint64_t read(
        File *pFile, uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    virtual uint64_t write(
        File *pFile, uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    virtual void truncate(File *pFile);
    virtual void fileAttributeChanged(File *pFile);
    virtual void cacheDirectoryContents(File *pFile);
    virtual void extend(File *pFile, size_t size);

  protected:
    virtual bool
    createFile(File *parent, const String &filename, uint32_t mask);
    virtual bool
    createDirectory(File *parent, const String &filename, uint32_t mask);
    virtual bool
    createSymlink(File *parent, const String &filename, const String &value);
    virtual bool remove(File *parent, File *file);

    FatFilesystem(const FatFilesystem &);
    void operator=(const FatFilesystem &);

    void loadRootDir();

    void cacheVolumeLabel();

    /** Reads a cluster from the disk. */
    bool readCluster(uint32_t block, uintptr_t buffer) const;

    /** Writes a cluster to the disk. */
    bool writeCluster(uint32_t block, uintptr_t buffer);

    /** Reads a block starting from a specific sector from the disk. */
    bool writeSectorBlock(uint32_t sec, size_t size, uintptr_t buffer);

    /** Writes a block starting from a specific sector to the disk. */
    bool readSectorBlock(uint32_t sec, size_t size, uintptr_t buffer) const;

    /** Obtains the first sector given a cluster number */
    uint32_t getSectorNumber(uint32_t cluster) const;

    /** Grabs a cluster entry - bLock determines if this should enforce locking
     * internally or allow the caller to ensure the FAT is locked. */
    uint32_t getClusterEntry(uint32_t cluster, bool bLock = true);

    /** Sets a cluster entry - bLock determines if this should enforce locking
     * internally or allow the caller to ensure the FAT is locked. */
    uint32_t
    setClusterEntry(uint32_t cluster, uint32_t value, bool bLock = true);

    /** Converts a string to 8.3 format */
    String convertFilenameTo(String filename) const;

    /** Converts a string from 8.3 format */
    String convertFilenameFrom(String filename) const;

    /** Finds a free cluster - bLock determines if we should enforce locking,
     * defaults to false because findFreeCluster is generally called within a
     * function that has already locked the FAT */
    uint32_t findFreeCluster(bool bLock = false);

    /** Updates the size of a file on disk */
    void updateFileSize(File *pFile, int64_t sizeChange);

    /** Sets the cluster for a file on disk */
    void setCluster(File *pFile, uint32_t clus);

    /** Reads part of a directory into a buffer, returns the allocated buffer
     * (which needs to be freed */
    void *readDirectoryPortion(uint32_t clus) const;

    /** Writes part of a directory from a buffer */
    void writeDirectoryPortion(uint32_t clus, void *p);

    /** Creates a file - actual doer for the public createFile */
    File *createFile(
        File *parentDir, const String &filename, uint32_t mask,
        bool bDirectory = false, uint32_t dirClus = 0);

    /** Reads a directory entry from disk */
    Dir *getDirectoryEntry(uint32_t clus, uint32_t offset) const;

    /** Writes a directry entry to disk */
    void writeDirectoryEntry(Dir *dir, uint32_t clus, uint32_t offset);

    /** Is a given cluster *VALUE* EOF? */
    bool isEof(uint32_t cluster) const
    {
        return (cluster >= eofValue());
    }

    /** EOF values */
    uint32_t eofValue() const
    {
        if (m_Type == FAT12)
            return 0x0FF8;
        if (m_Type == FAT16)
            return 0xFFF8;
        if (m_Type == FAT32)
            return 0x0FFFFFF8;
        return 0;
    }

    /** Gets a UNIX timestamp from a FAT date/time */
    Time::Timestamp getUnixTimestamp(uint16_t time, uint16_t date) const
    {
        // struct version of the passed parameters
        Timestamp *sTime = reinterpret_cast<Timestamp *>(&time);
        Date *sDate = reinterpret_cast<Date *>(&date);

        // Sanity check.
        if (!(sTime->secCount + sTime->minutes + sTime->hours))
            if (!(sDate->day + sDate->month + sDate->years))
                return 0;

        // grab the time information
        uint32_t seconds = sTime->secCount * 2;
        uint32_t minutes = sTime->minutes;
        uint32_t hours = sTime->hours;

        // grab the date information
        uint32_t day = sDate->day ? sDate->day - 1 : 0;
        uint32_t month = sDate->month;
        uint32_t years = sDate->years + 10;  // FAT timestamps start at 1980

        /** This should actually work for practically any year. */
        uint32_t realYear = years + 1970;
        uint32_t leapDays =
            ((realYear / 4) - (realYear / 100) + (realYear / 400));
        leapDays -= ((1980 / 4) - (1980 / 100) + (1980 / 400));

        // Cumulative days as the year progresses. Added to the current day's
        // month to get the proper offset into the year. The leap days are added
        // to this as well to give the proper final answer.
        static uint16_t cumulativeDays[] = {0,   31,  59,  90,  120, 151, 181,
                                            212, 243, 273, 304, 334, 365};
        uint32_t cumulDays = cumulativeDays[month ? month - 1 : 0];

        Time::Timestamp ret = 0;

        // add the time
        ret += seconds;
        ret += minutes * 60;
        ret += hours * 60 * 60;

        // and finally the date
        ret += day * 24 * 60 * 60;
        ret += cumulDays * 24 * 60 * 60;
        ret += leapDays * 24 * 60 * 60;
        ret += years * 365 * 24 * 60 * 60;

        // completed
        return ret;
    }

    /** Gets a FAT date from a UNIX timestamp */
    uint16_t getFatDate(Time::Timestamp timestamp) const
    {
        /** \todo Write */
        return 0;
    }

    /** Our superblocks */
    Superblock m_Superblock;
    Superblock16 m_Superblock16;
    Superblock32 m_Superblock32;
    FSInfo32 m_FsInfo;

    /** Type of the FAT */
    FatType m_Type;

    /** Required information */
    uint64_t m_DataAreaStart;  // data area can potentially start above 4 GB
    uint32_t m_RootDirCount;

    /** FAT sector */
    uint16_t m_FatSector;

    /** Root directory information */
    union RootDirInfo
    {
        uint32_t sector;   // FAT12 and 16 don't use a cluster
        uint32_t cluster;  // but FAT32 does...
    } m_RootDir;

    /** Size of a block (in this case, a cluster) */
    uint32_t m_BlockSize;

    /** FAT cache */
    uint8_t *m_pFatCache;

    /** FAT lock */
    // Mutex m_FatLock;
    UnlikelyLock m_FatLock;

    /** Root filesystem node. */
    File *m_pRoot;

    // FAT cache
    // Cache<uint8_t*, 512> m_FatCache;
    Tree<uintptr_t, uintptr_t> m_FatCache;

    /**
     * Hint for the free cluster code, to avoid searching the ENTIRE FAT each
     * time someone wants a free cluster (on non-FAT32 volumes).
     */
    uint32_t m_FreeClusterHint;

    /** Cached volume label for the filesystem. */
    String m_VolumeLabel;
};

#endif
