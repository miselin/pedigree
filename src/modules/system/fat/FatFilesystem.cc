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

#include "FatFilesystem.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Tree.h"
#include "pedigree/kernel/utilities/UnlikelyLock.h"
#include "pedigree/kernel/utilities/utility.h"

#include "FatDirectory.h"
#include "FatFile.h"
#include "FatSymlink.h"
#include "fat.h"
#include "modules/Module.h"
#include "modules/system/vfs/Directory.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/VFS.h"

class Filesystem;

// helper functions

static bool isPowerOf2(uint32_t n) {
  uint8_t log;

  for (log = 0; log < 16; log++) {
    if (n & 1) {
      n >>= 1;
      return (n != 0) ? false : true;
    }
    n >>= 1;
  }
  return false;
}

FatFilesystem::FatFilesystem()
    : m_FileMutationLock(),
      m_Superblock(),
      m_Superblock16(),
      m_Superblock32(),
      m_FsInfo(),
      m_Type(FAT12),
      m_DataAreaStart(0),
      m_RootDirCount(0),
      m_FatSector(0),
      m_RootDir(),
      m_BlockSize(0),
      m_ClusterCount(0),
      m_pFatCache(0),
      m_FatLock(),
#if THREADS || defined(STANDALONE_MUTEXES)
      m_AllocationLock(),
#endif
      m_pRoot(0),
      m_FatCache(),
      m_DirtyFatSectors(),
      m_FreeClusterHint() {
}

FatFilesystem::~FatFilesystem() {
  if (m_pRoot)
    delete m_pRoot;
  drainFileStates();
  {
    LockGuard<Mutex> guard(m_FileMutationLock);
    if (!syncPendingAttributes())
      ERROR("FAT: pending attributes remain during unmount");
    clearPendingAttributes();
  }
  if (m_pFatCache)
    delete[] m_pFatCache;
}

bool FatFilesystem::initialise(Disk* pDisk) {
  m_pDisk = pDisk;

  // Attempt to read the superblock.
  const BufferView superblock = m_pDisk->read(0);
  if (!superblock || superblock.size() < (36 + sizeof(Superblock32))) {
    if (superblock) {
      m_pDisk->unpin(0);
    }
    return false;
  }
  uint8_t* buffer = superblock.as<uint8_t>();

  MemoryCopy(reinterpret_cast<void*>(&m_Superblock), reinterpret_cast<void*>(buffer),
             sizeof(Superblock));
  MemoryCopy(reinterpret_cast<void*>(&m_Superblock16), reinterpret_cast<void*>(buffer + 36),
             sizeof(Superblock16));
  MemoryCopy(reinterpret_cast<void*>(&m_Superblock32), reinterpret_cast<void*>(buffer + 36),
             sizeof(Superblock32));
  m_pDisk->unpin(0);

  /** Validate the BPB and check for FAT FS */
  String devName;
  pDisk->getName(devName);

  /* check for EITHER a near jmp, or a jmp and a nop */
  if (m_Superblock.BS_jmpBoot[0] != 0xE9) {
    if (!(m_Superblock.BS_jmpBoot[0] == 0xEB && m_Superblock.BS_jmpBoot[2] == 0x90)) {
      ERROR("FAT: Superblock not found on device " << devName << " [" << m_Superblock.BS_jmpBoot[0]
                                                   << ", " << m_Superblock.BS_jmpBoot[2] << "]");
      return false;
    }
  }

  /** Check the FAT FS itself, ensuring it's valid */

  // SecPerClus must be a power of 2
  if (!isPowerOf2(m_Superblock.BPB_SecPerClus)) {
    ERROR("FAT: SecPerClus not a power of 2 (" << m_Superblock.BPB_SecPerClus << ")");
    return false;
  }

  // and there must be at least 1 FAT, and at most 2
  if (m_Superblock.BPB_NumFATs < 1 || m_Superblock.BPB_NumFATs > 2) {
    ERROR("FAT: Too many (or too few) FATs (" << m_Superblock.BPB_NumFATs << ")");
    return false;
  }

  /** Start loading actual FS info */

  // number of root directory sectors
  if (!m_Superblock.BPB_BytsPerSec)  // we sanity check the value, because we
                                     // divide by this later
  {
    return false;
  }
  uint32_t rootDirSectors =
      ((m_Superblock.BPB_RootEntCnt * 32) + (m_Superblock.BPB_BytsPerSec - 1)) /
      m_Superblock.BPB_BytsPerSec;

  // determine the size of the FAT
  uint32_t fatSz =
      (m_Superblock.BPB_FATSz16) ? m_Superblock.BPB_FATSz16 : m_Superblock32.BPB_FATSz32;

  // find the first data sector
  uint32_t firstDataSector =
      m_Superblock.BPB_RsvdSecCnt + (m_Superblock.BPB_NumFATs * fatSz) + rootDirSectors;

  // determine the number of data sectors, so we can determine FAT type
  uint32_t totDataSec = 0, totSec = (m_Superblock.BPB_TotSec16) ? m_Superblock.BPB_TotSec16
                                                                : m_Superblock.BPB_TotSec32;
  totDataSec = totSec - firstDataSector;

  if (!m_Superblock.BPB_SecPerClus)  // again, sanity checking due to division by this
  {
    ERROR("FAT: SecPerClus is zero!");
    return false;
  }
  uint32_t clusterCount = totDataSec / m_Superblock.BPB_SecPerClus;
  m_ClusterCount = clusterCount;

  // TODO: magic numbers here, perhaps #define MAXCLUS_{12|16|32} would work
  // better for readability
  if (clusterCount < 4085) {
    m_Type = FAT12;
    NOTICE("FAT: Device " << devName << " is type FAT12");
  } else if (clusterCount < 65525) {
    m_Type = FAT16;
    NOTICE("FAT: Device " << devName << " is type FAT16");
  } else {
    m_Type = FAT32;
    NOTICE("FAT: Device " << devName << " is type FAT32");
  }

  switch (m_Type) {
    case FAT12:
    case FAT16:

      m_RootDir.sector = m_Superblock.BPB_RsvdSecCnt + (m_Superblock.BPB_NumFATs * fatSz);

      break;

    case FAT32:

      m_RootDir.cluster = m_Superblock32.BPB_RootClus;

      break;
  }

  // fill the filesystem data
  m_DataAreaStart = firstDataSector;
  m_RootDirCount = rootDirSectors;
  m_BlockSize = m_Superblock.BPB_SecPerClus * m_Superblock.BPB_BytsPerSec;

  // read in the FAT32 FSInfo structure
  if (m_Type == FAT32) {
    uint32_t sec = m_Superblock32.BPB_FsInfo;
    readSectorBlock(sec, 512, reinterpret_cast<uintptr_t>(&m_FsInfo));
  }

  // Save the start sector of the FAT now
  m_FatSector = m_Superblock.BPB_RsvdSecCnt;

  // Setup the free cluster hint for non-FAT32 volumes
  m_FreeClusterHint = 2;

  // VFS needs a complete root and stable name before publishing this filesystem.
  loadRootDir();
  if (!m_pRoot) {
    return false;
  }
  cacheVolumeLabel();

  // FAT16/32 entry 1 records clean shutdown and absence of previous I/O errors.
  const uint32_t cleanMask = m_Type == FAT16 ? 0xc000 : 0x0c000000;
  m_MountedClean = m_Type == FAT12 || (getClusterEntry(1) & cleanMask) == cleanMask;

  return true;
}

Filesystem* FatFilesystem::probe(Disk* pDisk) {
  FatFilesystem* pFs = new FatFilesystem();
  if (!pFs->initialise(pDisk)) {
    delete pFs;
    return 0;
  } else {
    return pFs;
  }
}

void FatFilesystem::loadRootDir() {
  if (m_pRoot) {
    return;
  }

  // needs to return a file referring to the root directory
  uint32_t cluster = 0;
  if (m_Type == FAT32)
    cluster = m_RootDir.cluster;

  FatFileInfo info;
  info.creationTime = info.modifiedTime = info.accessedTime = 0;

  m_pRoot = new FatDirectory(String(""), cluster, this, 0, info);
}

File* FatFilesystem::getRoot() const {
  return m_pRoot;
}

void FatFilesystem::cacheVolumeLabel() {
  // The root directory (typically) contains the volume label, with a specific
  // flag In my experience, it's always the first entry, and it's always
  // there. Even so, we want to cater to unusual formats.
  //
  // In order to do so we check the entire root directory.

  uint32_t sz = m_Type == FAT32 ? m_BlockSize : m_RootDirCount * m_Superblock.BPB_BytsPerSec;

  uint32_t clus = 0;
  if (m_Type == FAT32)
    clus = m_RootDir.cluster;

  String volid;

  uint8_t* buffer = reinterpret_cast<uint8_t*>(readDirectoryPortion(clus));
  if (!buffer) {
    ERROR("FAT: unable to read the root directory while finding its volume label");
    return;
  }

  size_t i;
  bool endOfDir = false;
  while (true) {
    for (i = 0; i < sz; i += sizeof(Dir)) {
      Dir* ent = reinterpret_cast<Dir*>(&buffer[i]);

      if (ent->DIR_Name[0] == 0) {
        endOfDir = true;
        break;
      }

      if (ent->DIR_Attr & ATTR_VOLUME_ID) {
        const String shortName(reinterpret_cast<const char*>(ent->DIR_Name), 11, true);
        volid = convertFilenameFrom(shortName);
        delete[] buffer;
        m_VolumeLabel = volid;
        return;
      }
    }

    if (endOfDir)
      break;

    if (clus == 0 && m_Type != FAT32)
      break;  // not found

    // find the next cluster in the chain, if this is the end, break, if
    // not, continue
    clus = getClusterEntry(clus);
    if (clus == 0)
      break;  // something broke!

    if (isEof(clus))
      break;

    // continue by reading in this cluster
    if (!readCluster(clus, reinterpret_cast<uintptr_t>(buffer)))
      break;
  }

  delete[] buffer;

  // none found, do a default
  NormalStaticString str;
  str += "no-volume-label@";
  str.append(reinterpret_cast<uintptr_t>(this), 16);
  m_VolumeLabel.assign(str, str.length(), true);
}

const String& FatFilesystem::getVolumeLabel() const {
  return m_VolumeLabel;
}

/////////////////////////////////////////////////////////////////////////////

uint64_t FatFilesystem::read(File* pFile, uint64_t location, uint64_t size, uintptr_t buffer,
                             bool bCanBlock) {
  LockGuard<Mutex> guard(m_FileMutationLock);
  if (!size || !m_BlockSize)
    return 0;
  // Sanity check.
  if (pFile->isDirectory())
    return 0;

  // the inode of the file is the first cluster
  uint32_t clus = pFile->getInode();
  if (clus == 0)
    return 0;  // can't do it

  // validity checking
  if (location >= pFile->getSize()) {
    WARNING("FAT: Attempting to read past the EOF [loc=" << location << ", sz=" << size
                                                         << ", fsz=" << pFile->getSize() << "]");
    return 0;
  }

  uint64_t endOffset = location + size;
  uint64_t finalSize = size;
  if (endOffset > pFile->getSize()) {
    finalSize = pFile->getSize() - location;

    // overflow (location > size) or zero bytes required (location == size)
    if ((finalSize == 0) || (finalSize > pFile->getSize())) {
      WARNING("FAT: location + size > EOF");
      return 0;
    }
  }

  // finalSize holds the total amount of data to read, now find the cluster
  // and sector offsets
  uint32_t clusOffset = location / (m_Superblock.BPB_SecPerClus * m_Superblock.BPB_BytsPerSec);
  uint32_t firstOffset = location % (m_Superblock.BPB_SecPerClus *
                                     m_Superblock.BPB_BytsPerSec);  // the offset within the
                                                                    // cluster specified above to
                                                                    // start reading from

  // tracking info

  uint64_t bytesRead = 0;
  uint64_t currOffset = firstOffset;
  clus = fileClusterAt(pFile, clusOffset);
  if (!clus)
    return 0;

  // buffers
  uint8_t* tmpBuffer = new uint8_t[m_BlockSize];
  uint8_t* destBuffer = reinterpret_cast<uint8_t*>(buffer);

  // main read loop
  while (true) {
    // read in the entire cluster
    if (!readCluster(clus, reinterpret_cast<uintptr_t>(tmpBuffer))) {
      delete[] tmpBuffer;
      return bytesRead;
    }

    // How many bytes should we copy?
    size_t bytesToCopy = finalSize - bytesRead;
    if (bytesToCopy > m_BlockSize - currOffset) {
      bytesToCopy = m_BlockSize - currOffset;
    }

    // Perform the copy.
    MemoryCopy(&destBuffer[bytesRead], &tmpBuffer[currOffset], bytesToCopy);
    bytesRead += bytesToCopy;

    // Done?
    if (bytesRead == finalSize) {
      delete[] tmpBuffer;
      return bytesRead;
    }

    // end of cluster, set the offset back to zero
    currOffset = 0;

    // grab the next cluster, check for EOF
    clus = getClusterEntry(clus);
    if (clus == 0)
      break;  // something broke!

    if (isEof(clus))
      break;
  }

  delete[] tmpBuffer;

  // if we reach here, something's gone wrong
  WARNING("FAT: read returning zero... Something's not right.");
  return 0;
}

/////////////////////////////////////////////////////////////////////////////

uint32_t FatFilesystem::findFreeCluster(bool* persisted) {
#if THREADS || defined(STANDALONE_MUTEXES)
  LockGuard<Mutex> guard(m_AllocationLock);
#endif
  if (persisted)
    *persisted = false;
  if (!syncFat(false))
    return 0;
  const uint32_t first =
      m_FreeClusterHint >= 2 && m_FreeClusterHint < m_ClusterCount + 2 ? m_FreeClusterHint : 2;
  for (uint32_t scanned = 0; scanned < m_ClusterCount; ++scanned) {
    const uint32_t cluster = 2 + ((first - 2 + scanned) % m_ClusterCount);
    if (getClusterEntry(cluster, false))
      continue;
    const bool succeeded = setClusterEntry(cluster, eofValue(), false);
    if (!succeeded && getClusterEntry(cluster, false) != eofValue())
      return 0;
    if (persisted) {
      // A failed flush retains this reservation in the FAT cache. The caller
      // attaches it to the in-memory chain before reporting failure for retry.
      *persisted = succeeded;
      m_FreeClusterHint = cluster + 1;
      return cluster;
    }
    if (!succeeded) {
      // No caller owns this reservation. Retain its rollback as dirty until
      // the next allocator can confirm it before publishing any allocation.
      setClusterEntry(cluster, 0, false);
      return 0;
    }
    m_FreeClusterHint = cluster + 1;
    return cluster;
  }
  SYSCALL_ERROR(NoSpaceLeftOnDevice);
  return 0;
}

/////////////////////////////////////////////////////////////////////////////

uint64_t FatFilesystem::write(File* file, uint64_t location, uint64_t size, uintptr_t buffer,
                              bool bCanBlock) {
  LockGuard<Mutex> guard(m_FileMutationLock);
  if (m_bReadOnly) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return 0;
  }
  if (!size)
    return 0;
  if (!buffer || file->isDirectory() || location > UINT32_MAX || size > UINT32_MAX - location) {
    SYSCALL_ERROR(InvalidArgument);
    return 0;
  }
  if (!syncFat() || !ensureCapacity(file, location + size)) {
    SYSCALL_ERROR(IoError);
    return 0;
  }

  const size_t oldSize = file->getSize();
  if (location > oldSize && !zeroRange(file, oldSize, location)) {
    SYSCALL_ERROR(IoError);
    return 0;
  }
  uint32_t cluster = fileClusterAt(file, location / m_BlockSize);
  if (!cluster) {
    SYSCALL_ERROR(IoError);
    return 0;
  }

  uint8_t* temporary = new uint8_t[m_BlockSize];
  uint64_t written = 0;
  size_t offset = location % m_BlockSize;
  while (written < size) {
    size_t length = m_BlockSize - offset;
    if (length > size - written)
      length = size - written;
    if (!readCluster(cluster, reinterpret_cast<uintptr_t>(temporary)))
      break;
    MemoryCopy(temporary + offset, reinterpret_cast<void*>(buffer + written), length);
    if (!writeCluster(cluster, reinterpret_cast<uintptr_t>(temporary)))
      break;
    written += length;
    offset = 0;
    if (written == size)
      break;
    cluster = getClusterEntry(cluster);
    if (cluster < 2 || cluster >= m_ClusterCount + 2 || isEof(cluster))
      break;
  }
  delete[] temporary;

  const size_t newSize = written && location + written > oldSize ? location + written : oldSize;
  const bool metadataDirty = !file->isSymlink() && static_cast<FatFile*>(file)->m_MetadataDirty;
  if ((newSize != oldSize || metadataDirty) && !updateFileMetadata(file, newSize)) {
    SYSCALL_ERROR(IoError);
    return 0;
  }
  if (newSize != oldSize)
    publishSize(file, newSize);
  if (written != size)
    SYSCALL_ERROR(IoError);
  return written;
}

uint64_t FatFilesystem::allocatedBlocks(File* file) {
  if (!file->getInode() && file->isDirectory() && m_Type != FAT32)
    return (uint64_t(m_RootDirCount) * m_Superblock.BPB_BytsPerSec + 511) / 512;
  uint32_t count = 0, last = 0;
  if (!chainExtent(file, count, last)) {
    WARNING("FAT: unable to count an invalid cluster chain");
    return 0;
  }
  return uint64_t(count) * m_BlockSize / 512;
}

bool FatFilesystem::ensureCapacity(File* file, size_t size) {
  if (!m_BlockSize || size > UINT32_MAX)
    return false;
  uint32_t count = 0, last = 0;
  if (!chainExtent(file, count, last))
    return false;
  const uint32_t required = size / m_BlockSize + (size % m_BlockSize != 0);
  if (count >= required)
    return true;
  FatFile::State* state =
      !file->isDirectory() && !file->isSymlink() ? static_cast<FatFile*>(file)->m_State : nullptr;
  if (state && !state->clusters.tryReserve(required)) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
#if THREADS || defined(STANDALONE_MUTEXES)
  LockGuard<Mutex> allocation(m_AllocationLock);
#endif
  if (!syncFat(false))
    return false;
  bool complete = true;
  while (count < required) {
    uint32_t found = 0;
    const uint32_t first =
        m_FreeClusterHint >= 2 && m_FreeClusterHint < m_ClusterCount + 2 ? m_FreeClusterHint : 2;
    for (uint32_t scanned = 0; scanned < m_ClusterCount; ++scanned) {
      const uint32_t cluster = 2 + (first - 2 + scanned) % m_ClusterCount;
      if (!getClusterEntry(cluster, false)) {
        found = cluster;
        break;
      }
    }
    if (!found || !setClusterEntry(found, eofValue(), false, false)) {
      complete = false;
      break;
    }
    if (last && !setClusterEntry(last, found, false, false)) {
      setClusterEntry(found, 0, false, false);
      complete = false;
      break;
    }
    if (!last)
      file->setInode(found);
    if (state) {
      state->clusters.pushBack(found);
      state->chainRevision = __atomic_load_n(&m_ChainRevision, __ATOMIC_ACQUIRE);
    }
    last = found;
    m_FreeClusterHint = found + 1;
    ++count;
    if (!file->isDirectory() && !file->isSymlink())
      static_cast<FatFile*>(file)->m_MetadataDirty = true;
  }
  // Keep every reservation reachable in memory after a failed device flush.
  // The logical size is published only after zeroing and directory persistence.
  return syncFat(false) && complete;
}

bool FatFilesystem::updateFileMetadata(File* file, size_t size) {
  if (isNodeUnlinked(file))
    return true;
  uint32_t directoryCluster = 0, directoryOffset = 0;
  FatFile* regular = nullptr;
  if (file->isDirectory()) {
    FatDirectory* directory = static_cast<FatDirectory*>(file);
    directoryCluster = directory->getDirCluster();
    directoryOffset = directory->getDirOffset();
  } else if (file->isSymlink()) {
    FatSymlink* symlink = static_cast<FatSymlink*>(file);
    directoryCluster = symlink->getDirCluster();
    directoryOffset = symlink->getDirOffset();
  } else {
    regular = static_cast<FatFile*>(file);
    regular->m_MetadataDirty = true;
    directoryCluster = regular->getDirCluster();
    directoryOffset = regular->getDirOffset();
  }
  Dir* entry = getDirectoryEntry(directoryCluster, directoryOffset);
  if (!entry)
    return false;
  writeEntryAttributes(file, entry);
  // Absolute values make retry safe even if a failed flush updated the disk cache.
  entry->DIR_FileSize = HOST_TO_LITTLE32(size);
  const uintptr_t cluster = regular && !size ? 0 : file->getInode();
  entry->DIR_FstClusLO = HOST_TO_LITTLE16(cluster & 0xFFFF);
  entry->DIR_FstClusHI = HOST_TO_LITTLE16((cluster >> 16) & 0xFFFF);
  const bool succeeded = writeDirectoryEntry(entry, directoryCluster, directoryOffset);
  delete entry;
  if (regular && succeeded)
    regular->m_MetadataDirty = false;
  return succeeded;
}

bool FatFilesystem::zeroRange(File* file, size_t begin, size_t end) {
  if (begin >= end)
    return true;
  uint32_t cluster = fileClusterAt(file, begin / m_BlockSize);
  uint8_t* zeroes = new uint8_t[m_BlockSize];
  ByteSet(zeroes, 0, m_BlockSize);
  Disk::WriteBuffer requests[Disk::MaxWriteBuffers];
  size_t pending = 0;
  bool succeeded = true;
  auto drain = [&] {
    if (pending)
      succeeded = m_pDisk->writeFromBatch(requests, pending) && succeeded;
    pending = 0;
  };
  while (begin < end) {
    if (cluster < 2 || cluster >= m_ClusterCount + 2 || isEof(cluster)) {
      succeeded = false;
      break;
    }
    const size_t offset = begin % m_BlockSize;
    const size_t length = pedigree_std::min(size_t(m_BlockSize - offset), end - begin);
    requests[pending++] = {
        uint64_t(getSectorNumber(cluster)) * m_Superblock.BPB_BytsPerSec + offset, zeroes, length,
        false};
    if (pending == Disk::MaxWriteBuffers)
      drain();
    begin += length;
    if (begin < end)
      cluster = getClusterEntry(cluster);
  }
  drain();
  const bool durable = m_pDisk->syncData();
  delete[] zeroes;
  return succeeded && durable;
}

bool FatFilesystem::syncFileMetadata(File* file) {
  LockGuard<Mutex> guard(m_FileMutationLock);
  if (m_bReadOnly)
    return !m_IoFailed;
  if (!syncFat())
    return false;
  FatFile* regular = static_cast<FatFile*>(file);
  regular->copyStateAttributes();
  if (isNodeUnlinked(file))
    return trimFileAllocation(regular) && m_pDisk->syncData();
  if (regular->m_MetadataDirty && !updateFileMetadata(file, file->getSize()))
    return false;
  return trimFileAllocation(regular) && m_pDisk->syncData();
}

void* FatFilesystem::readDirectoryPortion(uint32_t clus) const {
  if (clus == 0 && m_Type == FAT32)
    return nullptr;

  const size_t size = clus == 0 ? m_RootDirCount * m_Superblock.BPB_BytsPerSec : m_BlockSize;
  uint8_t* dirBuffer = new uint8_t[size];
  if (!readDirectoryPortion(clus, reinterpret_cast<uintptr_t>(dirBuffer))) {
    delete[] dirBuffer;
    return nullptr;
  }

  return dirBuffer;
}

bool FatFilesystem::readDirectoryPortion(uint32_t clus, uintptr_t buffer) const {
  if (!buffer)
    return false;

  if (clus == 0) {
    if (m_Type == FAT32)
      return false;
    const uint32_t size = m_RootDirCount * m_Superblock.BPB_BytsPerSec;
    return readSectorBlock(m_RootDir.sector, size, buffer);
  }

  return readCluster(clus, buffer);
}

bool FatFilesystem::writeDirectoryPortion(uint32_t clus, void* p) {
  if (!p)
    return false;
  bool secMethod = false;
  uint32_t sz = m_BlockSize;
  uint32_t sec = m_RootDir.sector;
  if (clus == 0) {
    if (m_Type != FAT32) {
      sz = m_RootDirCount * m_Superblock.BPB_BytsPerSec;
      secMethod = true;
    } else
      return false;
  }

  if (secMethod)
    return writeSectorBlock(sec, sz, reinterpret_cast<uintptr_t>(p));
  return writeCluster(clus, reinterpret_cast<uintptr_t>(p));
}

Dir* FatFilesystem::getDirectoryEntry(uint32_t clus, uint32_t offset) const {
  const size_t size = clus == 0 ? m_RootDirCount * m_Superblock.BPB_BytsPerSec : m_BlockSize;
  if (offset > size || (size - offset) < sizeof(Dir))
    return nullptr;

  uint8_t* dirBuffer = reinterpret_cast<uint8_t*>(readDirectoryPortion(clus));
  if (!dirBuffer)
    return 0;

  Dir* ent = reinterpret_cast<Dir*>(&dirBuffer[offset]);
  Dir* ret = new Dir;
  MemoryCopy(ret, ent, sizeof(Dir));

  delete[] dirBuffer;

  return ret;
}

bool FatFilesystem::writeDirectoryEntry(Dir* dir, uint32_t clus, uint32_t offset) {
  // don't bother reading and writing if the cluster is zero or if there's no
  // entry to write
  if (dir == 0)
    return false;

  const size_t size = clus == 0 ? m_RootDirCount * m_Superblock.BPB_BytsPerSec : m_BlockSize;
  if (offset > size || (size - offset) < sizeof(Dir))
    return false;

  uint8_t* dirBuffer = reinterpret_cast<uint8_t*>(readDirectoryPortion(clus));
  if (!dirBuffer)
    return false;

  Dir* ent = reinterpret_cast<Dir*>(&dirBuffer[offset]);
  MemoryCopy(ent, dir, sizeof(Dir));

  const bool success = writeDirectoryPortion(clus, dirBuffer);

  delete[] dirBuffer;
  return success;
}

void FatFilesystem::cacheDirectoryContents(File* pFile) {}

bool FatFilesystem::readCluster(uint32_t block, uintptr_t buffer) const {
  block = getSectorNumber(block);
  return readSectorBlock(block, m_BlockSize, buffer);
}

bool FatFilesystem::readSectorBlock(uint32_t sec, size_t size, uintptr_t buffer) const {
  if (!buffer) {
    return false;
  }

  size_t off = 0;
  while (size) {
    size_t sz = (size > 512) ? 512 : size;
    const uint64_t diskLocation =
        (static_cast<uint64_t>(m_Superblock.BPB_BytsPerSec) * static_cast<uint64_t>(sec)) + off;
    const BufferView diskBuffer = m_pDisk->read(diskLocation);
    if (!diskBuffer || diskBuffer.size() < sz) {
      if (diskBuffer) {
        m_pDisk->unpin(diskLocation);
      }
      return false;
    }
    MemoryCopy(reinterpret_cast<void*>(buffer), diskBuffer.data(), sz);
    m_pDisk->unpin(diskLocation);
    buffer += sz;
    size -= sz;
    off += sz;
  }
  return true;
}

bool FatFilesystem::writeCluster(uint32_t block, uintptr_t buffer) {
  block = getSectorNumber(block);
  return writeSectorBlock(block, m_BlockSize, buffer);
}

bool FatFilesystem::writeSectorBlock(uint32_t sec, size_t size, uintptr_t buffer) {
  if (!buffer || !m_pDisk)
    return false;
  const bool written = m_pDisk->writeFrom(uint64_t(sec) * m_Superblock.BPB_BytsPerSec,
                                          reinterpret_cast<void*>(buffer), size);
  const bool durable = m_pDisk->syncData();
  return written && durable;
}

uint32_t FatFilesystem::getSectorNumber(uint32_t cluster) const {
  return ((cluster - 2) * m_Superblock.BPB_SecPerClus) + m_DataAreaStart;
}

uint8_t* FatFilesystem::getFatSector(uint32_t sector) {
  uint8_t* bytes = reinterpret_cast<uint8_t*>(m_FatCache.lookup(sector));
  if (bytes)
    return bytes;
  bytes = new uint8_t[m_Superblock.BPB_BytsPerSec];
  if (!readSectorBlock(m_FatSector + sector, m_Superblock.BPB_BytsPerSec,
                       reinterpret_cast<uintptr_t>(bytes))) {
    delete[] bytes;
    return nullptr;
  }
  m_FatCache.insert(sector, reinterpret_cast<uintptr_t>(bytes));
  return bytes;
}

uint32_t FatFilesystem::getClusterEntry(uint32_t cluster, bool bLock) {
  if (!m_Superblock.BPB_BytsPerSec)
    return 0;
  const uint32_t offset =
      m_Type == FAT12 ? cluster + cluster / 2 : cluster * (m_Type == FAT16 ? 2 : 4);
  const size_t length = m_Type == FAT32 ? 4 : 2;
  uint32_t entry = 0;
  m_FatLock.acquire();
  for (size_t byte = 0; byte < length; ++byte) {
    const uint32_t sector = (offset + byte) / m_Superblock.BPB_BytsPerSec;
    const uint8_t* bytes = getFatSector(sector);
    if (!bytes) {
      m_FatLock.release();
      return 0;
    }
    entry |= uint32_t(bytes[(offset + byte) % m_Superblock.BPB_BytsPerSec]) << (8 * byte);
  }
  m_FatLock.release();
  if (m_Type == FAT12)
    return (entry >> ((cluster & 1) ? 4 : 0)) & 0x0FFF;
  return entry & (m_Type == FAT16 ? 0xFFFF : 0x0FFFFFFF);
}

bool FatFilesystem::setClusterEntry(uint32_t cluster, uint32_t value, bool bLock, bool persist) {
  if (cluster < 2 || cluster >= m_ClusterCount + 2 || !m_Superblock.BPB_BytsPerSec)
    return false;
#if THREADS || defined(STANDALONE_MUTEXES)
  LockGuard<Mutex> guard(m_AllocationLock, bLock);
#endif
  const uint32_t offset =
      m_Type == FAT12 ? cluster + cluster / 2 : cluster * (m_Type == FAT16 ? 2 : 4);
  const size_t length = m_Type == FAT32 ? 4 : 2;
  uint8_t* bytes[4] = {};
  uint32_t original = 0;
  m_FatLock.acquire();
  for (size_t byte = 0; byte < length; ++byte) {
    const uint32_t sector = (offset + byte) / m_Superblock.BPB_BytsPerSec;
    uint8_t* page = getFatSector(sector);
    if (!page) {
      m_FatLock.release();
      return false;
    }
    bytes[byte] = page + (offset + byte) % m_Superblock.BPB_BytsPerSec;
    original |= uint32_t(*bytes[byte]) << (8 * byte);
  }
  if (m_Type == FAT12)
    value = cluster & 1 ? (original & 0x000F) | ((value & 0x0FFF) << 4)
                        : (original & 0xF000) | (value & 0x0FFF);
  else if (m_Type == FAT32)
    value = (original & 0xF0000000) | (value & 0x0FFFFFFF);
  if (original != value)
    __atomic_add_fetch(&m_ChainRevision, uint64_t(1), __ATOMIC_RELEASE);
  for (size_t byte = 0; byte < length; ++byte) {
    *bytes[byte] = static_cast<uint8_t>(value >> (8 * byte));
    m_DirtyFatSectors.insert((offset + byte) / m_Superblock.BPB_BytsPerSec, true);
  }
  m_FatLock.release();
  return !persist || syncFat(false);
}

bool FatFilesystem::syncFat(bool bLock) {
#if THREADS || defined(STANDALONE_MUTEXES)
  LockGuard<Mutex> guard(m_AllocationLock, bLock);
#endif
  LockGuard<UnlikelyLock> fat(m_FatLock);
  if (!m_DirtyFatSectors.count())
    return true;
  if (!invalidateFsInfoHints())
    return false;
  const uint32_t sectorsPerFat =
      m_Type == FAT32 ? m_Superblock32.BPB_FATSz32 : m_Superblock.BPB_FATSz16;
  const size_t copies = m_Superblock.BPB_NumFATs ? m_Superblock.BPB_NumFATs : 1;
  Disk::WriteBuffer requests[Disk::MaxWriteBuffers];
  size_t pending = 0;
  bool succeeded = true;
  auto drain = [&] {
    if (pending)
      succeeded = m_pDisk->writeFromBatch(requests, pending) && succeeded;
    pending = 0;
  };
  for (auto it = m_DirtyFatSectors.begin(); it != m_DirtyFatSectors.end(); ++it) {
    const uint32_t sector = it.key();
    const uintptr_t bytes = m_FatCache.lookup(sector);
    if (!bytes) {
      succeeded = false;
      continue;
    }
    for (size_t copy = 0; copy < copies; ++copy) {
      requests[pending++] = {
          uint64_t(m_FatSector + sector + copy * sectorsPerFat) * m_Superblock.BPB_BytsPerSec,
          reinterpret_cast<void*>(bytes), m_Superblock.BPB_BytsPerSec, false};
      if (pending == Disk::MaxWriteBuffers)
        drain();
    }
  }
  drain();
  const bool durable = m_pDisk->syncData();
  if (!succeeded || !durable)
    return false;
  m_DirtyFatSectors.clear();
  return true;
}

String FatFilesystem::convertFilenameTo(String fn) const {
  // Special dot & dotdot handling. Because periods are eaten by the
  // algorithm, we need to ensure that the dot and dotdot entries are returned
  // with only padding.
  if (!StringCompare(static_cast<const char*>(fn), ".") ||
      !StringCompare(static_cast<const char*>(fn), "..")) {
    NormalStaticString ret;
    ret = fn;
    ret.pad(11);
    return String(static_cast<const char*>(ret));
  }

  // Strip the filename of any whitespace that might be dangling off the end.
  fn.rstrip();

  NormalStaticString filename, ext;

  // Initial generation loop.
  size_t lastPeriod = ~0UL;
  for (size_t i = 0; i < fn.length(); ++i) {
    // Valid character?
    if (fn[i] == ' ' || fn[i] == '"' || fn[i] == '/' || fn[i] == '\\' || fn[i] == '[' ||
        fn[i] == ']' || fn[i] == ':' || fn[i] == ';' || fn[i] == '=' || fn[i] == ',')
      continue;  // Illegal for SFN.
    else if (fn[i] == '.') {
      if ((i + 1) >= fn.length()) {
        // Stripped input but whitespace follows. Ignore and terminate
        // loop.
        break;
      }
      lastPeriod = i;
    } else {
      filename += toUpper(fn[i]);
    }
  }

  // Truncate filename if the filename portion is > 8 characters long
  if (lastPeriod > 8) {
    filename.truncate(6);
    filename += "~1";  /// \todo This should increment if a file is found
                       /// with the same name!
  }
  // Or is the filename now longer than the distance to the last period?
  else if (lastPeriod != ~0UL) {
    filename.truncate(lastPeriod);
  }

  // Is the filename now empty?
  if (!filename.length()) {
    // Yes, dotfile (eg, .vimrc)
    ++lastPeriod;

    // .vimrc -> VIMRC~1
    for (size_t i = 0; i < 6; ++i) {
      if ((lastPeriod + i) >= fn.length())
        break;
      filename += toUpper(fn[lastPeriod + i]);
    }

    // Add tail, pad, and return.
    filename += "~1";  /// todo Increment on duplicate
    filename.pad(11);
    return String(static_cast<const char*>(filename));
  }

  // Pull the extension out, truncated to 3 characters, and skipping the full
  // stoop.
  for (size_t i = 1; i < 4; ++i) {
    if ((lastPeriod + i) >= fn.length())
      break;
    ext += toUpper(fn[lastPeriod + i]);
  }

  // Pad as necessary.
  filename.pad(8);
  ext.pad(3);

  // Merge the two strings and return!
  filename.append(ext);
  filename += '\0';
  return String(static_cast<const char*>(filename));
}

String FatFilesystem::convertFilenameFrom(String filename) const {
  NormalStaticString ret;

  size_t i;
  for (i = 0; i < 8; i++) {
    if (i >= filename.length())
      break;
    if (filename[i] != ' ')
      ret += toLower(filename[i]);
    else
      break;
  }

  for (i = 0; i < 3; i++) {
    if ((8 + i) >= filename.length())
      break;
    if (filename[8 + i] != ' ') {
      if (i == 0)
        ret += '.';
      ret += toLower(filename[8 + i]);
    } else
      break;
  }

  ret += '\0';

  return String(static_cast<const char*>(ret));
}

File* FatFilesystem::createFile(File* parentDir, const String& filename, uint32_t mask,
                                bool bDirectory, uint32_t dirClus, bool publish) {
  if (m_bReadOnly) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return 0;
  }
  // Validate input
  if (!parentDir->isDirectory()) {
    return 0;
  }

  FatFileInfo info;
  info.creationTime = Time::getTime();
  info.modifiedTime = info.creationTime;
  info.accessedTime = info.creationTime;

  // Directory or File?
  // Note that new files in FAT always have a zero cluster, but new
  // directories require a cluster (to keep the "." and ".." entries from
  // jumping in)
  File* pFile;
  if (bDirectory) {
    pFile = new FatDirectory(filename, dirClus, this, parentDir, info);

    char* buffer = new char[m_BlockSize];
    ByteSet(buffer, 0, m_BlockSize);

    // Clean out the clusters for the directory before creating ./..
    // entries.
    uint32_t clus = dirClus;
    do {
      // Write zero cluster.
      if (!writeCluster(clus, reinterpret_cast<uintptr_t>(buffer))) {
        delete[] buffer;
        delete pFile;
        return nullptr;
      }
      clus = getClusterEntry(clus);
    } while (!isEof(clus));
    delete[] buffer;
  } else {
    pFile =
        new FatFile(filename, info.accessedTime, info.modifiedTime, info.creationTime, 0, this, 0,
                    0xdeadbeef,  // Sentinel values that'll throw an error if they're
                                 // used
                    0xbeefdead,  // before being set to correct values.
                    parentDir);
  }

  if (publish) {
    FatDirectory* parent = static_cast<FatDirectory*>(Directory::fromFile(parentDir));
    if (!parent->addEntry(filename, pFile, (bDirectory ? 1 : 0))) {
      if (!bDirectory && !m_bReadOnly)
        releaseClusterChain(pFile->getInode());
      delete pFile;
      return 0;
    }
  }

  return pFile;
}

bool FatFilesystem::createFile(File* parent, const String& filename, uint32_t mask) {
  File* f = createFile(parent, filename, mask, false);
  return (f != 0);
}

bool FatFilesystem::createDirectory(File* parent, const String& filename, uint32_t mask) {
  if (m_bReadOnly) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return false;
  }
  // Allocate a cluster for the directory itself
  uint32_t clus = findFreeCluster();
  if (!clus)
    return false;

  File* f = createFile(parent, filename, mask, true, clus, false);
  if (!f) {
    setClusterEntry(clus, 0);
    return false;
  }

  FatDirectory* fatDir = static_cast<FatDirectory*>(f);
  FatFileInfo info = {};
  FatDirectory dot(String("."), clus, this, f, info);
  FatDirectory dotdot(String(".."), parent->getInode(), this, f, info);
  if (!fatDir->addEntry(String("."), &dot, 1) || !fatDir->addEntry(String(".."), &dotdot, 1)) {
    if (!m_bReadOnly)
      releaseClusterChain(clus);
    delete f;
    return false;
  }

  FatDirectory* fatParent = static_cast<FatDirectory*>(Directory::fromFile(parent));
  if (!fatParent->addEntry(filename, f, 1)) {
    if (!m_bReadOnly)
      releaseClusterChain(clus);
    delete f;
    return false;
  }

  return true;
}

bool FatFilesystem::createSymlink(File* parent, const String& filename, const String& value) {
  if (m_bReadOnly) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return 0;
  }
  // Validate input
  if (!parent->isDirectory()) {
    return false;
  }

  FatFileInfo info;
  info.creationTime = Time::getTime();
  info.modifiedTime = info.creationTime;
  info.accessedTime = info.creationTime;

  // Deviation from the spec here: Because the 'inode' is used for fstat,
  // we can't leave it at zero or else all newly created files without
  // data will look the same!
  uint32_t clus = findFreeCluster();
  if (!clus)
    return false;
  File* pFile = new FatSymlink(
      filename, info.accessedTime, info.modifiedTime, info.creationTime, clus, this, value.length(),
      0xdeadbeef,  // Sentinel values that'll throw an error if they're used
      0xbeefdead,  // before being set to correct values.
      parent);

  // The unpublished node has no directory entry to update. Publish its final
  // size with the name only after the target data has reached the disk.
  if (value.length() && write(pFile, 0, value.length(),
                              reinterpret_cast<uintptr_t>(value.cstr())) != value.length()) {
    if (!m_bReadOnly)
      releaseClusterChain(clus);
    delete pFile;
    return false;
  }

  String symlinkFilename = filename;
  symlinkFilename += FatDirectory::symlinkSuffix();

  FatDirectory* fatParent = static_cast<FatDirectory*>(Directory::fromFile(parent));
  if (!fatParent->addEntry(symlinkFilename, pFile, 0)) {
    if (!m_bReadOnly)
      releaseClusterChain(clus);
    delete pFile;
    return false;
  }

  return true;
}

bool FatFilesystem::releaseClusterChain(uint32_t clus, bool lockFile) {
  if (!clus)
    return true;

  LockGuard<Mutex> fileGuard(m_FileMutationLock, lockFile);
#if THREADS || defined(STANDALONE_MUTEXES)
  LockGuard<Mutex> guard(m_AllocationLock);
#endif
  const uint32_t first = clus;
  size_t visited = 0;
  while (true) {
    if (clus < 2 || clus >= (m_ClusterCount + 2) || visited++ >= m_ClusterCount) {
      ERROR("Invalid or cyclic FAT cluster chain during release");
      return false;
    }

    clus = getClusterEntry(clus, false);
    if (!clus) {
      ERROR("Found a free cluster in an allocated FAT chain");
      return false;
    }
    if (isEof(clus))
      break;
  }

  // Preflight populated every FAT sector needed below. Stage the whole
  // reclamation even if a flush fails, retaining all frees for later retry.
  bool succeeded = true;
  clus = first;
  while (visited--) {
    const uint32_t next = getClusterEntry(clus, false);
    succeeded = setClusterEntry(clus, 0, false, false) && succeeded;
    clus = next;
  }
  return syncFat(false) && succeeded;
}

bool FatFilesystem::removeNode(File* parent, const String& filename, File* file) {
  if (m_bReadOnly) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return false;
  }
  FatDirectory* parentDir = static_cast<FatDirectory*>(Directory::fromFile(parent));

  if (file->isDirectory()) {
    FatDirectory* child = static_cast<FatDirectory*>(file);
    if (child == parentDir) {
      SYSCALL_ERROR(InvalidArgument);
      return false;
    }
    LockGuard<Mutex> namespaceGuard(child->namespaceMutationLock());
    bool empty = false;
    const Directory::ReadStatus status = child->isEmpty(empty);
    if (status != Directory::ReadStatus::Complete) {
      SYSCALL_ERROR(IoError);
      return false;
    }
    if (!empty) {
      SYSCALL_ERROR(NotEmpty);
      return false;
    }

    LockGuard<Mutex> childGuard(child->m_Lock);
    if (child->isDetached() || !parentDir->removeEntry(filename, file))
      return false;
    child->markDetached();
    return true;
  }

  if (!parentDir->removeEntry(filename, file))
    return false;
  return true;
}

#ifndef FAT_STANDALONE
static bool initFat() {
  VFS::instance().addProbeCallback(&FatFilesystem::probe);
  return true;
}

static void destroyFat() {
  if (!VFS::instance().removeProbeCallback(&FatFilesystem::probe)) {
    FATAL("FAT probe callback was not registered during unload");
  }
}

MODULE_INFO("fat", &initFat, &destroyFat, "vfs");
#endif
