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
#include "modules/system/vfs/Filesystem.h"

/** This class provides an implementation of the FAT filesystem. */
class FatFilesystem : public Filesystem {
  friend class FatFile;
  friend class FatDirectory;
  friend class FatSymlink;

 public:
  FatFilesystem();

  ~FatFilesystem() override;

  //
  // Filesystem interface.
  //

  bool initialise(Disk* pDisk) override;
  static Filesystem* probe(Disk* pDisk);
  File* getRoot() const override;
  const String& getVolumeLabel() const override;
  bool getUuid(String&) const override;
  SyncStatus sync() override;
  uint64_t read(File* pFile, uint64_t location, uint64_t size, uintptr_t buffer,
                bool bCanBlock = true);
  uint64_t write(File* pFile, uint64_t location, uint64_t size, uintptr_t buffer,
                 bool bCanBlock = true);
  void truncate(File* pFile);
  void fileAttributeChanged(File* pFile);
  void cacheDirectoryContents(File* pFile);
  void extend(File* pFile, size_t size);

 protected:
  bool createFile(File* parent, const String& filename, uint32_t mask) override;
  bool createDirectory(File* parent, const String& filename, uint32_t mask) override;
  bool createSymlink(File* parent, const String& filename, const String& value) override;
  bool removeNode(File* parent, const String& filename, File* file) override;
  bool renameNode(Directory* oldParent, const String& oldName, File* source, Directory* newParent,
                  const String& newName, File* replaced) override;

  FatFilesystem(const FatFilesystem&);
  void operator=(const FatFilesystem&);

  void loadRootDir();

  void cacheVolumeLabel();

  /** Reads a cluster from the disk. */
  bool readCluster(uint32_t block, uintptr_t buffer) const;

  /** Writes a cluster to the disk. */
  bool writeCluster(uint32_t block, uintptr_t buffer);

  /** Writes a block starting from a specific sector to the disk. */
  bool writeSectorBlock(uint32_t sec, size_t size, uintptr_t buffer);

  /** Reads a block starting from a specific sector from the disk. */
  bool readSectorBlock(uint32_t sec, size_t size, uintptr_t buffer) const;

  /** Obtains the first sector given a cluster number */
  uint32_t getSectorNumber(uint32_t cluster) const;

  /** Grabs a cluster entry - bLock determines if this should enforce locking
   * internally or allow the caller to ensure the FAT is locked. */
  uint32_t getClusterEntry(uint32_t cluster, bool bLock = true);

  /** Sets a cluster entry - bLock determines if this should enforce locking
   * internally or allow the caller to ensure the FAT is locked. */
  bool setClusterEntry(uint32_t cluster, uint32_t value, bool bLock = true, bool persist = true);

  /** Converts a string to 8.3 format */
  String convertFilenameTo(String filename) const;

  /** Converts a string from 8.3 format */
  String convertFilenameFrom(String filename) const;

  /** Finds and reserves a free cluster. */
  uint32_t findFreeCluster(bool* persisted = nullptr);

  bool syncFat(bool bLock = true);
  bool invalidateFsInfoHints();
  bool m_FsInfoInvalidated = false;
  uint8_t* getFatSector(uint32_t sector);
  bool chainExtent(File* file, uint32_t& count, uint32_t& last);
  uint32_t fileClusterAt(File*, size_t);
  uint64_t m_ChainRevision = 1;
  bool truncateFile(File* file);
  class ShrinkPlan;
  bool prepareFileShrink(FatFile*, size_t, UniquePointer<File::PreparedShrink>&);
  bool trimFileAllocation(FatFile*);
  void publishSize(File*, size_t);
  void unlinkNode(File*);
  void retireNode(File*);
  void moveNode(File*, uint32_t, uint32_t);
  bool isNodeUnlinked(File*) const;
  void writeEntryAttributes(File*, Dir*, bool creating = false);
  void encodeEntryAttributes(const File::Attributes&, Dir*, bool creating = false);
  bool syncNodeAttributes(File*);
  bool syncNode(File*);
  struct PendingAttributes {
    uint64_t slot;
    File::Attributes attributes;
    PendingAttributes* next = nullptr;
  };
  PendingAttributes* m_PendingAttributes = nullptr;
  bool writePendingAttributes(const PendingAttributes&);
  // Pending-attribute helpers require m_FileMutationLock.
  bool syncPendingAttributes();
  void movePendingAttributes(uint32_t oldCluster, uint32_t oldOffset, uint32_t newCluster,
                             uint32_t newOffset);
  void discardPendingAttributes(uint32_t cluster, uint32_t offset);
  void clearPendingAttributes();
  FatFile::State* acquireFileState(FatFile*, uintptr_t, size_t, uint32_t, uint32_t, Time::Timestamp,
                                   Time::Timestamp, Time::Timestamp);
  void releaseFileState(FatFile*);
  uintptr_t fileIdentifier(uint32_t cluster, uint32_t offset);
  uintptr_t fileIdentifierLocked(uint64_t slot);
  bool writeCachedPages(FatFile::State&, const Cache::WritebackPage*, size_t);
  void drainFileStates();
  struct NodeState {
    uintptr_t inode = 0;
    uint32_t directoryCluster = 0, directoryOffset = 0;
    bool unlinked = false;
    Vector<File*> aliases;
  };
  Tree<uintptr_t, NodeState*> m_NodeStates;
  Tree<File*, NodeState*> m_NodeAliases;
  void registerNode(File*);
  void releaseNode(File*);
  void moveNonFileNode(File*, uint32_t, uint32_t);
  void unlinkNonFileNode(File*);
  Mutex m_StateLock;
  Tree<uint64_t, FatFile::State*> m_FileStates;
  Tree<uint64_t, uintptr_t> m_FileIdentifiers;
  uintptr_t m_NextFileIdentifier = 0x10000000;
  FatFile::State* m_StateList = nullptr;
  bool m_IoFailed = false;
  bool ensureCapacity(File* file, size_t size);
  bool zeroRange(File* file, size_t begin, size_t end);
  bool updateFileMetadata(File* file, size_t size);
  bool syncFileMetadata(File* file);
  uint64_t allocatedBlocks(File* file);

  /** Serialises chain changes and attribute snapshots across file aliases. */
  Mutex m_FileMutationLock;

  /** Reads part of a directory into a buffer, returns the allocated buffer
   * (which needs to be freed */
  void* readDirectoryPortion(uint32_t clus) const;

  /** Reads part of a directory into a caller-provided buffer. */
  bool readDirectoryPortion(uint32_t clus, uintptr_t buffer) const;

  /** Writes part of a directory from a buffer */
  bool writeDirectoryPortion(uint32_t clus, void* p);

  /** Creates a file - actual doer for the public createFile */
  File* createFile(File* parentDir, const String& filename, uint32_t mask, bool bDirectory = false,
                   uint32_t dirClus = 0, bool publish = true);

  /** Releases a validated cluster chain as one allocation transaction. */
  bool releaseClusterChain(uint32_t clus, bool lockFile = true);

  /** Reads a directory entry from disk */
  Dir* getDirectoryEntry(uint32_t clus, uint32_t offset) const;

  /** Writes a directry entry to disk */
  bool writeDirectoryEntry(Dir* dir, uint32_t clus, uint32_t offset);

  /** Is a given cluster *VALUE* EOF? */
  bool isEof(uint32_t cluster) const {
    return (cluster >= eofValue());
  }

  /** EOF values */
  uint32_t eofValue() const {
    if (m_Type == FAT12)
      return 0x0FF8;
    if (m_Type == FAT16)
      return 0xFFF8;
    if (m_Type == FAT32)
      return 0x0FFFFFF8;
    return 0;
  }

  Time::Timestamp getUnixTimestamp(uint16_t time, uint16_t date) const;
  uint16_t getFatDate(Time::Timestamp timestamp) const;
  uint16_t getFatTime(Time::Timestamp timestamp) const;

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
  union RootDirInfo {
    uint32_t sector;   // FAT12 and 16 don't use a cluster
    uint32_t cluster;  // but FAT32 does...
  } m_RootDir;

  /** Size of a block (in this case, a cluster) */
  uint32_t m_BlockSize;

  /** Number of addressable data clusters. */
  uint32_t m_ClusterCount;

  /** FAT cache */
  uint8_t* m_pFatCache;

  /** FAT lock */
  // Mutex m_FatLock;
  UnlikelyLock m_FatLock;

#if THREADS || defined(STANDALONE_MUTEXES)
  /** Serialises the scan-and-reserve transaction across the whole volume. */
  Mutex m_AllocationLock;
#endif

  /** Root filesystem node. */
  File* m_pRoot;

  // FAT cache
  // Cache<uint8_t*, 512> m_FatCache;
  Tree<uintptr_t, uintptr_t> m_FatCache;
  Tree<uint32_t, bool> m_DirtyFatSectors;

  /**
   * Hint for the free cluster code, to avoid searching the ENTIRE FAT each
   * time someone wants a free cluster (on non-FAT32 volumes).
   */
  uint32_t m_FreeClusterHint;

  /** Cached volume label for the filesystem. */
  String m_VolumeLabel;
};

#endif
