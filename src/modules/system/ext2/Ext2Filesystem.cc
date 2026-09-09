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

#include "Ext2Filesystem.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Ext2Directory.h"
#include "Ext2File.h"
#include "Ext2Node.h"
#include "Ext2Symlink.h"
#include "ext2.h"
#include "modules/system/users/Group.h"
#include "modules/system/users/User.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/VFS.h"

#ifndef EXT2_STANDALONE
#include "modules/Module.h"
#endif

// The sparse block page. This is zeroed and made read-only. A handler is set
// and if written, it traps.
/// \todo Set CR0.WP bit else this will never happen.
/// \todo Work out what to do when it traps.
static uint8_t g_pSparseBlock[4096] ALIGN(4096) SECTION(".bss");

#ifdef EXT2_STANDALONE
extern uint32_t getUnixTimestamp();
#else
static uint32_t getUnixTimestamp() {
  Timer* pTimer = Machine::instance().getTimer();
  return pTimer->getUnixTimestamp();
}
#endif

Ext2Filesystem::Ext2Filesystem()
    : m_pSuperblock(0),
      m_pGroupDescriptors(0),
      m_pInodeTables(0),
      m_pInodeBitmaps(0),
      m_pBlockBitmaps(0),
      m_BlockSize(0),
      m_InodeSize(0),
      m_nGroupDescriptors(0),
#if THREADS || defined(STANDALONE_MUTEXES)
      m_WriteLock(),
      m_InodeTableLoadLock(),
#endif
      m_pRoot(0) {
}

Ext2Filesystem::~Ext2Filesystem() {
  closeQuotaFiles();
  delete m_pRoot;
  drainAttributeWrites();

  for (auto it = m_InodeStates.begin(); it != m_InodeStates.end(); ++it) {
    assert(!it.value()->references);
    delete it.value();
  }
  m_InodeStates.clear();

  if (m_pDisk && m_pSuperblock) {
    if (m_pGroupDescriptors) {
      for (size_t group = 0; group < m_nGroupDescriptors; ++group) {
        GroupDesc* descriptor = m_pGroupDescriptors[group];
        if (!descriptor) {
          continue;
        }

        if (m_pBlockBitmaps) {
          const uint32_t start = LITTLE_TO_HOST32(descriptor->bg_block_bitmap);
          for (size_t i = 0; i < m_pBlockBitmaps[group].count(); ++i) {
            unpinBlock(start + i);
          }
        }
        if (m_pInodeBitmaps) {
          const uint32_t start = LITTLE_TO_HOST32(descriptor->bg_inode_bitmap);
          for (size_t i = 0; i < m_pInodeBitmaps[group].count(); ++i) {
            unpinBlock(start + i);
          }
        }
        if (m_pInodeTables) {
          const uint32_t start = LITTLE_TO_HOST32(descriptor->bg_inode_table);
          for (size_t i = 0; i < m_pInodeTables[group].count(); ++i) {
            unpinBlock(start + i);
          }
        }
      }

      const uint32_t gdBlock = LITTLE_TO_HOST32(m_pSuperblock->s_first_data_block) + 1;
      for (size_t i = 0; i < m_nGroupDescriptors; ++i) {
        if (m_pGroupDescriptors[i]) {
          const size_t block = gdBlock + ((i * sizeof(GroupDesc)) / m_BlockSize);
          unpinBlock(block);
        }
      }
    }

    // The successful read() is the persistent superblock reference.
    m_pDisk->unpin(1024ULL);
  }

  delete[] m_pBlockBitmaps;
  delete[] m_pInodeBitmaps;
  delete[] m_pInodeTables;
  delete[] m_pGroupDescriptors;
}

bool Ext2Filesystem::initialise(Disk* pDisk) {
  String devName;
  m_pDisk = pDisk;
  pDisk->getName(devName);

  // Attempt to read the superblock. A successful Disk::read() transfers the
  // persistent reference that this filesystem holds until destruction.
  const BufferView block = m_pDisk->read(1024ULL);
  if (!block || block.size() < sizeof(Superblock)) {
    if (block) {
      m_pDisk->unpin(1024ULL);
    }
    ERROR("Ext2: Failed to read a superblock on " << devName);
    return false;
  }
  m_pSuperblock = block.as<Superblock>();

  // Read correctly?
  if (LITTLE_TO_HOST16(m_pSuperblock->s_magic) != 0xEF53) {
    ERROR("Ext2: Superblock was not found on device " << devName);
    return false;
  }

  // Other creator formats assign different meanings to inode owner-high fields.
  const uint32_t creator = LITTLE_TO_HOST32(m_pSuperblock->s_creator_os);
  if (creator != 0 && creator != 1) {
    ERROR("Ext2: unsupported inode creator format on " << devName);
    return false;
  }

  // Clean?
  if (LITTLE_TO_HOST16(m_pSuperblock->s_state) != EXT2_STATE_CLEAN) {
    WARNING("Ext2: filesystem on device " << devName << " is not clean.");
  }

  // Compressed filesystem?
  if (checkRequiredFeature(1)) {
    WARNING("Ext2: filesystem on device " << devName
                                          << " requires compression, some files may fail to read.");

    // Compression type.
    uint32_t algo_bitmap = LITTLE_TO_HOST32(m_pSuperblock->s_algo_bitmap);
    switch (algo_bitmap) {
      case EXT2_LZV1_ALG:
        NOTICE("Ext2: filesystem on device '" << devName << "' uses compression algorithm LZV1.");
        break;
      case EXT2_LZRW3A_ALG:
        NOTICE("Ext2: filesystem on device '" << devName << "' uses compression algorithm LZRW3A.");
        break;
      case EXT2_GZIP_ALG:
        NOTICE("Ext2: filesystem on device '" << devName << "' uses compression algorithm gzip.");
        break;
      case EXT2_BZIP2_ALG:
        NOTICE("Ext2: filesystem on device '" << devName << "' uses compression algorithm bzip2.");
        break;
      case EXT2_LZO_ALG:
        NOTICE("Ext2: filesystem on device '" << devName << "' uses compression algorithm LZO.");
        break;
      default:
        ERROR("Ext2: unknown compression algorithm " << algo_bitmap << " on device '" << devName
                                                     << "' -- cannot mount!");
        return false;
    }
  }

  /// \todo Check for journal required features.
  /// \todo Check all read-only features.

  // If we can, check extended superblock fields.
  if (LITTLE_TO_HOST32(m_pSuperblock->s_rev_level) >= 1) {
    // Non-standard inode sizes are permitted, handle that.
    m_InodeSize = LITTLE_TO_HOST16(m_pSuperblock->s_inode_size);
  } else {
    m_InodeSize = sizeof(Inode);
  }

  // Calculate the block size.
  m_BlockSize = 1024 << LITTLE_TO_HOST32(m_pSuperblock->s_log_block_size);

  if (m_BlockSize > 4096) {
    ERROR("Ext2: filesystem's block size is too large (must be 4096 or less, but is " << m_BlockSize
                                                                                      << ")");
    return false;
  }
  if (m_BlockSize > TargetInfo::getPageSize()) {
    ERROR("Ext2: filesystem block size " << m_BlockSize << " exceeds the target page size "
                                         << TargetInfo::getPageSize());
    return false;
  }

  // Where is the group descriptor table?
  uint32_t gdBlock = LITTLE_TO_HOST32(m_pSuperblock->s_first_data_block) + 1;

  // How many group descriptors do we have? Round up the result.
  uint32_t inodeCount = LITTLE_TO_HOST32(m_pSuperblock->s_inodes_count);
  uint32_t inodesPerGroup = LITTLE_TO_HOST32(m_pSuperblock->s_inodes_per_group);
  if (!inodeCount || !inodesPerGroup) {
    ERROR("Ext2: filesystem on device '" << devName << "' has invalid inode geometry.");
    return false;
  }
  m_nGroupDescriptors = (inodeCount / inodesPerGroup) + ((inodeCount % inodesPerGroup) ? 1 : 0);

  // Add an entry to the group descriptor tree for each GD.
  m_pGroupDescriptors = new GroupDesc*[m_nGroupDescriptors];
  for (size_t i = 0; i < m_nGroupDescriptors; ++i) {
    m_pGroupDescriptors[i] = 0;
  }
  for (size_t i = 0; i < m_nGroupDescriptors; i++) {
    uintptr_t idx = (i * sizeof(GroupDesc)) / m_BlockSize;
    uintptr_t off = (i * sizeof(GroupDesc)) % m_BlockSize;

    uintptr_t groupBlock = readBlock(gdBlock + idx);
    if (!groupBlock) {
      ERROR("Ext2: Failed to read block group descriptor " << i);
      return false;
    }
    m_pGroupDescriptors[i] = reinterpret_cast<GroupDesc*>(groupBlock + off);
  }

  // Create our bitmap arrays and tables.
  m_pInodeTables = new Vector<size_t>[m_nGroupDescriptors];
  m_pInodeBitmaps = new Vector<size_t>[m_nGroupDescriptors];
  m_pBlockBitmaps = new Vector<size_t>[m_nGroupDescriptors];

  /// \todo Set g_pSparseBlock as read-only.

  // load root directory and sanity check it
  Inode* inode = getInode(EXT2_ROOT_INO);
  if (!inode) {
    ERROR("failed to retrieve root directory inode (corrupted inode table?");
    return false;
  }
  if ((LITTLE_TO_HOST16(inode->i_mode) & 0xF000) != EXT2_S_IFDIR) {
    ERROR("root directory is not a directory");
    return false;
  }
  m_pRoot = new Ext2Directory(String(""), EXT2_ROOT_INO, inode, this, 0);

  // cache volume label
  bool hasVolumeLabel = LITTLE_TO_HOST32(m_pSuperblock->s_rev_level) >= 1;
  if ((!hasVolumeLabel) || (m_pSuperblock->s_volume_name[0] == '\0')) {
    NormalStaticString str;
    str += "no-volume-label@";
    str.append(reinterpret_cast<uintptr_t>(this), 16);
    m_VolumeLabel.assign(str, str.length(), true);
  } else {
    char buffer[17];
    StringCopyN(buffer, m_pSuperblock->s_volume_name, 16);
    buffer[16] = '\0';
    m_VolumeLabel.assign(buffer);
  }

  return true;
}

Filesystem* Ext2Filesystem::probe(Disk* pDisk) {
  Ext2Filesystem* pFs = new Ext2Filesystem();
  if (!pFs->initialise(pDisk)) {
    // No ext2 filesystem found - don't leak the filesystem object.
    delete pFs;
    return 0;
  } else
    return pFs;
}

File* Ext2Filesystem::getRoot() const {
  return m_pRoot;
}

const String& Ext2Filesystem::getVolumeLabel() const {
  return m_VolumeLabel;
}

bool Ext2Filesystem::getUuid(String& uuid) const {
  const uint8_t* value = reinterpret_cast<const uint8_t*>(m_pSuperblock->s_uuid);
  uuid.Format("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
              value[0], value[1], value[2], value[3], value[4], value[5], value[6], value[7],
              value[8], value[9], value[10], value[11], value[12], value[13], value[14],
              value[15]);
  return true;
}

bool Ext2Filesystem::createNode(File* parent, const String& filename, uint32_t mask,
                                const String& value, size_t type, uint32_t inodeOverride) {
  LockGuard<Mutex> quotaNamespace(m_QuotaNamespaceLock);
  if (inodeOverride && isQuotaFile(inodeOverride)) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return false;
  }
  // Quick sanity check;
  if (!parent->isDirectory()) {
    SYSCALL_ERROR(NotADirectory);
    return false;
  }

  // The filename cannot be the special entries "." or "..".
  if (filename.length() == 0 || !StringCompare(filename.cstr(), ".") ||
      !StringCompare(filename.cstr(), "..")) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }

#ifdef EXT2_STANDALONE
  uint32_t uid = 0, gid = 0;
#else
  FilesystemCredentials credentials;
  if (!Process::currentFilesystemCredentials(credentials)) {
    SYSCALL_ERROR(PermissionDenied);
    return false;
  }
  const uint32_t uid = credentials.uid, gid = credentials.gid;
#endif

  // Find a free inode.
  uint32_t inode_num = inodeOverride;
  if (!inode_num) {
    inode_num = findFreeInode(uid, gid);
    if (inode_num == 0) {
      return false;
    }
  }

  uint32_t timestamp = getUnixTimestamp();

  // Populate the inode.
  /// \todo Endianness!
  Inode* newInode = getInode(inode_num);
  if (!inodeOverride) {
    // Allocation has already cleared the inode and advanced its generation.
    newInode->i_mode = HOST_TO_LITTLE16(mask | type);
    newInode->i_atime = newInode->i_ctime = newInode->i_mtime = HOST_TO_LITTLE32(timestamp);
  }

  // If we have a value to store, and it's small enough, use the block
  // indices.
  if (value.length() && value.length() < 4 * 15) {
    MemoryCopy(reinterpret_cast<void*>(newInode->i_block), value.cstr(), value.length());
    newInode->i_size = HOST_TO_LITTLE32(value.length());
  }
  // Else case comes later, after pFile is created.

  Ext2Directory* pE2Parent = reinterpret_cast<Ext2Directory*>(parent);
  Ext2Node* pNewNode = 0;
  Ext2Directory* pNewDirectory = nullptr;
  bool dotEntryCreated = false;
  bool dotDotEntryCreated = false;

  // Create the new File object.
  File* pFile = 0;
  switch (type) {
    case EXT2_S_IFREG: {
      Ext2File* pNewFile = new Ext2File(filename, inode_num, newInode, this, parent);
      if (!pNewFile || !pNewFile->valid()) {
        if (!inodeOverride) {
          releaseInode(inode_num, pNewFile);
        }
        delete pNewFile;
        SYSCALL_ERROR(OutOfMemory);
        return false;
      }
      pFile = pNewFile;
      pNewNode = pNewFile;
      break;
    }
    case EXT2_S_IFDIR: {
      Ext2Directory* pE2Dir = new Ext2Directory(filename, inode_num, newInode, this, parent);
      pFile = pE2Dir;
      pNewNode = pE2Dir;
      pNewDirectory = pE2Dir;

      // If we already have an inode, assume we already have dot/dotdot
      // entries and so don't need to make them.
      if (!inodeOverride) {
        // Dot entries are backing metadata, not separate VFS objects. Avoid
        // manufacturing a second directory object and mutex for this inode.
        syscallError(0);
        dotEntryCreated = pE2Dir->addEntry(String("."), pE2Dir, EXT2_S_IFDIR);
        if (dotEntryCreated) {
          dotDotEntryCreated = pE2Dir->addEntry(String(".."), pE2Parent, EXT2_S_IFDIR);
        }
        if (!dotEntryCreated || !dotDotEntryCreated) {
          const int failure = currentIoError();
          if (dotEntryCreated && !pE2Dir->removeEntry(String("."), pE2Dir)) {
            ERROR("EXT2: Failed to unwind a new directory's self link");
            releaseInode(inode_num, pE2Dir);
          } else if (!dotEntryCreated) {
            releaseInode(inode_num, pE2Dir);
          }
          delete pE2Dir;
          syscallError(failure);
          return false;
        }
      }
      break;
    }
    case EXT2_S_IFLNK: {
      Ext2Symlink* pNewSymlink = new Ext2Symlink(filename, inode_num, newInode, this, parent);
      pFile = pNewSymlink;
      pNewNode = pNewSymlink;
      break;
    }
    default:
      FATAL("EXT2: Unrecognised file type: " << Hex << type);
      break;
  }

  // Else case from earlier.
  if (value.length() && value.length() >= 4 * 15) {
    syscallError(0);
    if (pFile->write(0ULL, value.length(), reinterpret_cast<uintptr_t>(value.cstr())) !=
        value.length()) {
      const int failure = currentIoError();
      if (!inodeOverride)
        releaseInode(inode_num, pNewNode);
      delete pFile;
      syscallError(failure);
      return false;
    }
  }

  // Add to the parent directory.
  syscallError(0);
  if (!pE2Parent->addEntry(filename, pFile, type)) {
    const int failure = currentIoError();
    ERROR("EXT2: Internal error adding directory entry.");
    if (!inodeOverride) {
      if (pNewDirectory && dotDotEntryCreated &&
          !pNewDirectory->removeEntry(String(".."), pE2Parent)) {
        ERROR("EXT2: Failed to unwind a new directory's parent link");
        releaseInode(pE2Parent->getInodeNumber(), pE2Parent);
      }
      if (pNewDirectory && dotEntryCreated) {
        if (!pNewDirectory->removeEntry(String("."), pNewDirectory)) {
          ERROR("EXT2: Failed to unwind a new directory's self link");
          releaseInode(inode_num, pNewNode);
        }
      } else {
        releaseInode(inode_num, pNewNode);
      }
    }
    delete pFile;
    syscallError(failure);
    return false;
  }

  // Edit the atime and mtime of the parent directory.
  parent->setAccessedTime(timestamp);
  parent->setModifiedTime(timestamp);

  // Write updated inodes.
  writeInode(inode_num);
  writeInode(pE2Parent->getInodeNumber());

  // Update directory count in the group descriptor.
  if (type == EXT2_S_IFDIR) {
#if THREADS || defined(STANDALONE_MUTEXES)
    LockGuard<Mutex> guard(m_WriteLock);
#endif
    uint32_t group = (inode_num - 1) / LITTLE_TO_HOST32(m_pSuperblock->s_inodes_per_group);
    GroupDesc* pDesc = m_pGroupDescriptors[group];

    pDesc->bg_used_dirs_count++;

    // Update group descriptor on disk.
    /// \todo save group descriptor block number elsewhere
    uint32_t gdBlock = LITTLE_TO_HOST32(m_pSuperblock->s_first_data_block) + 1;
    uint32_t groupBlock = (group * sizeof(GroupDesc)) / m_BlockSize;
    writeBlock(gdBlock + groupBlock);
  }

  // OK, now we can preallocate blocks if desired.
  // Note: don't preallocate symlinks, which can store data in i_blocks.
  if (m_pSuperblock->s_prealloc_blocks && !(pFile->isDirectory() || pFile->isSymlink())) {
    pNewNode->ensureLargeEnough(m_pSuperblock->s_prealloc_blocks * m_BlockSize, 0, 0, true);
  } else if (m_pSuperblock->s_prealloc_dir_blocks && pFile->isDirectory()) {
    pNewNode->ensureLargeEnough(m_pSuperblock->s_prealloc_dir_blocks * m_BlockSize, 0, 0, true);
  }

  return true;
}

bool Ext2Filesystem::createFile(File* parent, const String& filename, uint32_t mask) {
  return createNode(parent, filename, mask, String(""), EXT2_S_IFREG);
}

bool Ext2Filesystem::createDirectory(File* parent, const String& filename, uint32_t mask) {
  if (!createNode(parent, filename, mask, String(""), EXT2_S_IFDIR)) {
    return false;
  }
  return true;
}

bool Ext2Filesystem::createSymlink(File* parent, const String& filename, const String& value) {
  return createNode(parent, filename, 0777, value, EXT2_S_IFLNK);
}

bool Ext2Filesystem::createLink(File* parent, const String& filename, File* target) {
  Ext2Directory* pE2Parent = reinterpret_cast<Ext2Directory*>(parent);

  Ext2Node* pNode = 0;
  if (target->isDirectory()) {
    Ext2Directory* pDirectory = static_cast<Ext2Directory*>(target);
    pNode = pDirectory;
  } else if (target->isSymlink()) {
    Ext2Symlink* pSymlink = static_cast<Ext2Symlink*>(target);
    pNode = pSymlink;
  } else {
    Ext2File* pFile = static_cast<Ext2File*>(target);
    pNode = pFile;
  }

  if (!pNode) {
    return false;
  }

  // Extract permissions and entry type.
  Inode* inode = pNode->getInode();
  uint32_t mask = LITTLE_TO_HOST16(inode->i_mode) & 0x0FFF;
  size_t type = LITTLE_TO_HOST16(inode->i_mode) & 0xF000;

  return createNode(parent, filename, mask, String(""), type, pNode->getInodeNumber());
}

bool Ext2Filesystem::removeNode(File* parent, const String& filename, File* file) {
  LockGuard<Mutex> quotaNamespace(m_QuotaNamespaceLock);
  if (isQuotaFile(file->getInode())) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return false;
  }
  // Quick sanity check.
  if (!parent->isDirectory()) {
    SYSCALL_ERROR(IoError);
    return false;
  }

  Ext2Node* pNode = 0;
  if (file->isDirectory()) {
    Ext2Directory* pDirectory = static_cast<Ext2Directory*>(file);
    pNode = pDirectory;
  } else if (file->isSymlink()) {
    Ext2Symlink* pSymlink = static_cast<Ext2Symlink*>(file);
    pNode = pSymlink;
  } else {
    Ext2File* pFile = static_cast<Ext2File*>(file);
    pNode = pFile;
  }

  Ext2Directory* pE2Parent = reinterpret_cast<Ext2Directory*>(parent);
  const bool ordinaryDirectory =
      file->isDirectory() && !(filename.compare(".") || filename.compare(".."));
  bool result = ordinaryDirectory
                    ? static_cast<Ext2Directory*>(file)->removeFromParent(pE2Parent, filename)
                    : pE2Parent->removeEntry(filename, pNode);

  // Update the group descriptor directory count to reflect the deletion.
  if (result && ordinaryDirectory) {
#if THREADS || defined(STANDALONE_MUTEXES)
    LockGuard<Mutex> guard(m_WriteLock);
#endif
    uint32_t inode_num = pNode->getInodeNumber();

    uint32_t group = (inode_num - 1) / LITTLE_TO_HOST32(m_pSuperblock->s_inodes_per_group);
    GroupDesc* pDesc = m_pGroupDescriptors[group];

    pDesc->bg_used_dirs_count--;

    // Update group descriptor on disk.
    /// \todo save group descriptor block number elsewhere
    uint32_t gdBlock = LITTLE_TO_HOST32(m_pSuperblock->s_first_data_block) + 1;
    uint32_t groupBlock = (group * sizeof(GroupDesc)) / m_BlockSize;
    writeBlock(gdBlock + groupBlock);
  }

  return result;
}

uintptr_t Ext2Filesystem::readBlock(uint32_t block) {
  if (block == 0)
    return reinterpret_cast<uintptr_t>(g_pSparseBlock);

  const uint64_t location = static_cast<uint64_t>(m_BlockSize) * static_cast<uint64_t>(block);
  const BufferView view = m_pDisk->read(location);
  if (!view || view.size() < m_BlockSize) {
    if (view) {
      m_pDisk->unpin(location);
    }
    return 0;
  }
  return view.address();
}

void Ext2Filesystem::writeBlock(uint32_t block) {
  if (block == 0)
    return;

  m_pDisk->write(static_cast<uint64_t>(m_BlockSize) * static_cast<uint64_t>(block));
}

bool Ext2Filesystem::pinBlock(uint64_t location) {
  if (!location) {
    return true;
  }
  return m_pDisk->pin(static_cast<uint64_t>(m_BlockSize) * location);
}

void Ext2Filesystem::unpinBlock(uint64_t location) {
  if (!location) {
    return;
  }
  m_pDisk->unpin(static_cast<uint64_t>(m_BlockSize) * location);
}

bool Ext2Filesystem::syncBlock(uint32_t block, bool async) {
  if (!block) {
    return true;
  }
  return m_pDisk->sync(static_cast<uint64_t>(m_BlockSize) * block, async);
}

bool Ext2Filesystem::syncInode(uint32_t inode, Ext2Node& node, bool includeNamespaceMetadata) {
  if (!inode || !m_pSuperblock) {
    return false;
  }
#if THREADS || defined(STANDALONE_MUTEXES)
  LockGuard<Mutex> allocationGuard(m_WriteLock);
#endif
  // Keep the current payload resident while the dependency set releases its pins.
  AttributeRetirement attributes;
  if (readAttributeBlockLocked(node.m_pInode, attributes) != XattrStatus::Success ||
      !flushAttributeWritesLocked())
    return false;
  const uint32_t inodesPerGroup = LITTLE_TO_HOST32(m_pSuperblock->s_inodes_per_group);
  const uint32_t blocksPerGroup = LITTLE_TO_HOST32(m_pSuperblock->s_blocks_per_group);
  if (!inodesPerGroup || !blocksPerGroup) {
    return false;
  }
  const uint32_t inodeGroup = (inode - 1) / inodesPerGroup;
  const uint32_t index = (inode - 1) % inodesPerGroup;
  if (inodeGroup >= m_nGroupDescriptors || !ensureInodeTableLoaded(inodeGroup)) {
    return false;
  }

  Vector<uint8_t> groups(m_nGroupDescriptors);
  for (size_t i = 0; i < m_nGroupDescriptors; ++i) {
    groups.pushBack(i == inodeGroup ? 1 : 0);
  }
  bool succeeded = true;
  const uint32_t firstBlock = LITTLE_TO_HOST32(m_pSuperblock->s_first_data_block);
  auto includeBlockGroup = [&](uint32_t block) {
    if (!block) {
      return;
    }
    if (block < firstBlock || (block - firstBlock) / blocksPerGroup >= groups.count()) {
      succeeded = false;
      return;
    }
    groups[(block - firstBlock) / blocksPerGroup] = 1;
  };
  const uint32_t attributeBlock = LITTLE_TO_HOST32(node.m_pInode->i_file_acl);
  includeBlockGroup(attributeBlock);
  if (attributeBlock && !syncBlock(attributeBlock, false))
    return false;
  if (!node.isInlineSymlink()) {
    for (size_t i = 0; i < 12; ++i)
      includeBlockGroup(LITTLE_TO_HOST32(node.m_pInode->i_block[i]));
  }

  const size_t entries = m_BlockSize / sizeof(uint32_t);
  Vector<Ext2Node::MappingPage> mappings;
  succeeded = node.collectMappingPages(
                  node.isInlineSymlink() ? 0 : LITTLE_TO_HOST32(node.m_pInode->i_block[12]), 1, 12,
                  entries, mappings) &&
              succeeded;
  succeeded = node.collectMappingPages(
                  node.isInlineSymlink() ? 0 : LITTLE_TO_HOST32(node.m_pInode->i_block[13]), 2,
                  12 + entries, entries * entries, mappings) &&
              succeeded;
  succeeded = node.collectMappingPages(
                  node.isInlineSymlink() ? 0 : LITTLE_TO_HOST32(node.m_pInode->i_block[14]), 3,
                  12 + entries + entries * entries, entries * entries * entries, mappings) &&
              succeeded;
  for (const Ext2Node::MappingPage& mapping : mappings) {
    includeBlockGroup(mapping.block);
    if (mapping.depth == 1) {
      const uint32_t* children = reinterpret_cast<const uint32_t*>(mapping.buffer);
      for (size_t i = 0; i < entries; ++i) {
        includeBlockGroup(LITTLE_TO_HOST32(children[i]));
      }
    }
    succeeded = syncBlock(mapping.block, false) && succeeded;
    unpinBlock(mapping.block);
  }

  // The inode is durable only once the allocation metadata needed to recover
  // its data and mapping blocks has also reached the backend.
  for (size_t group = 0; group < groups.count(); ++group) {
    if (!groups[group]) {
      continue;
    }
    GroupDesc* descriptor = m_pGroupDescriptors[group];
    if (ensureFreeBlockBitmapLoaded(group)) {
      const uint32_t start = LITTLE_TO_HOST32(descriptor->bg_block_bitmap);
      for (size_t i = 0; i < m_pBlockBitmaps[group].count(); ++i) {
        succeeded = syncBlock(start + i, false) && succeeded;
      }
    } else {
      succeeded = false;
    }
    const uint32_t descriptorBlock = firstBlock + 1 + (group * sizeof(GroupDesc)) / m_BlockSize;
    succeeded = syncBlock(descriptorBlock, false) && succeeded;
  }
  if (ensureFreeInodeBitmapLoaded(inodeGroup)) {
    const uint32_t start = LITTLE_TO_HOST32(m_pGroupDescriptors[inodeGroup]->bg_inode_bitmap);
    for (size_t i = 0; i < m_pInodeBitmaps[inodeGroup].count(); ++i) {
      succeeded = syncBlock(start + i, false) && succeeded;
    }
  } else {
    succeeded = false;
  }
  if (includeNamespaceMetadata) {
    // Removed entries no longer identify the affected inode or freed blocks.
    // Include loaded metadata from every group so namespace sync also submits
    // child creation, link changes, and retirement, including failed retries.
#if THREADS || defined(STANDALONE_MUTEXES)
    LockGuard<Mutex> tableGuard(m_InodeTableLoadLock);
#endif
    // These tables and bitmaps stay pinned for the filesystem lifetime. Let
    // the disk share one durability barrier across a bounded set of pages.
    uint64_t locations[Disk::MaxSyncPages];
    size_t locationCount = 0;
    auto submitMetadata = [&](uint32_t block) {
      if (!block)
        return;
      locations[locationCount++] = static_cast<uint64_t>(m_BlockSize) * block;
      if (locationCount == Disk::MaxSyncPages) {
        succeeded = m_pDisk->syncPages(locations, locationCount) && succeeded;
        locationCount = 0;
      }
    };
    for (size_t group = 0; group < m_nGroupDescriptors; ++group) {
      GroupDesc* descriptor = m_pGroupDescriptors[group];
      const uint32_t inodeTable = LITTLE_TO_HOST32(descriptor->bg_inode_table);
      for (size_t i = 0; i < m_pInodeTables[group].count(); ++i) {
        submitMetadata(inodeTable + i);
      }
      const uint32_t blockBitmap = LITTLE_TO_HOST32(descriptor->bg_block_bitmap);
      for (size_t i = 0; i < m_pBlockBitmaps[group].count(); ++i) {
        submitMetadata(blockBitmap + i);
      }
      const uint32_t inodeBitmap = LITTLE_TO_HOST32(descriptor->bg_inode_bitmap);
      for (size_t i = 0; i < m_pInodeBitmaps[group].count(); ++i) {
        submitMetadata(inodeBitmap + i);
      }
      if (m_pInodeTables[group].count() || m_pBlockBitmaps[group].count() ||
          m_pInodeBitmaps[group].count()) {
        const uint32_t descriptorBlock = firstBlock + 1 + (group * sizeof(GroupDesc)) / m_BlockSize;
        submitMetadata(descriptorBlock);
      }
    }
    if (locationCount)
      succeeded = m_pDisk->syncPages(locations, locationCount) && succeeded;
  }
  succeeded = m_pDisk->sync(1024ULL, false) && succeeded;
  const uint32_t inodeBlock = LITTLE_TO_HOST32(m_pGroupDescriptors[inodeGroup]->bg_inode_table) +
                              ((index * m_InodeSize) / m_BlockSize);
  return succeeded && syncBlock(inodeBlock, false);
}

uint32_t Ext2Filesystem::findFreeBlock(uint32_t inode) {
  Vector<uint32_t> blocks;
  if (findFreeBlocks(inode, 1, blocks)) {
    return blocks[0];
  }

  return 0;
}

bool Ext2Filesystem::findFreeBlocks(uint32_t inode, size_t count, Vector<uint32_t>& blocks) {
#if THREADS || defined(STANDALONE_MUTEXES)
  LockGuard<Mutex> guard(m_WriteLock);
#endif

  if (!count)
    return true;
  if (!blocks.tryReserve(count)) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  if (!m_BlockSize || count > ~uint64_t(0) / m_BlockSize) {
    SYSCALL_ERROR(ValueTooLarge);
    return false;
  }
  const uint64_t reserved = static_cast<uint64_t>(count) * m_BlockSize;
  auto status = prepareQuotaInodeLocked(inode);
  if (status == QuotaStatus::Success)
    status = m_Quota.reserve(inode, reserved);
  if (!quotaSucceeded(status))
    return false;
  const uint32_t inodeNumber = inode;
  // The ledger includes reservations before callers attach their mappings.
  --inode;

  // Try to allocate near the inode's group (but we can fall back to a
  // different group if needed).
  uint32_t group = inode / LITTLE_TO_HOST32(m_pSuperblock->s_inodes_per_group);
  uint32_t startGroup = group;
  int error = 0;

  for (; count && group < m_nGroupDescriptors; ++group) {
    // A zero allocation result also means full storage. Check the fallible
    // bitmap load separately so I/O failures survive reservation rollback.
    if (m_pGroupDescriptors[group]->bg_free_blocks_count && !ensureFreeBlockBitmapLoaded(group)) {
      error = currentIoError();
      break;
    }
    count -= findFreeBlocksInGroup(group, count, blocks);
  }

  // Try again from the start of the disk if we couldn't find a group (if
  // we started e.g. halfway through the disk due to the inode closeness
  // thing above, we need to check the rest of the groups).
  if (count && !error)
    ERROR("FALLING BACK TO STARTING FROM ZERO");
  for (group = 0; count && !error && group < startGroup; ++group) {
    if (m_pGroupDescriptors[group]->bg_free_blocks_count && !ensureFreeBlockBitmapLoaded(group)) {
      error = currentIoError();
      break;
    }
    count -= findFreeBlocksInGroup(group, count, blocks);
  }

  if (count) {
    for (uint32_t block : blocks) {
      releaseBlockLocked(block);
    }
    blocks.clear();
    m_Quota.refund(inodeNumber, reserved);
    syscallError(error ? error : Error::NoSpaceLeftOnDevice);
  }
  return count == 0;
}

size_t Ext2Filesystem::findFreeBlocksInGroup(uint32_t group, size_t maxCount,
                                             Vector<uint32_t>& blocks) {
  if (!maxCount) {
    return 0;
  }

  const uint32_t blocksPerGroup = LITTLE_TO_HOST32(m_pSuperblock->s_blocks_per_group);
  const size_t bitmapBlockBytes = m_BlockSize;
  size_t currentCount = 0;

  // Any free blocks here?
  GroupDesc* pDesc = m_pGroupDescriptors[group];
  if (!pDesc->bg_free_blocks_count) {
    // No blocks free in this group.
    return currentCount;
  }

  if (!ensureFreeBlockBitmapLoaded(group)) {
    return 0;
  }

  // 8 blocks per byte - i == bitmap offset in bytes.
  Vector<size_t>& list = m_pBlockBitmaps[group];
  const uint32_t bytesToSearch = blocksPerGroup >> 3;
  size_t idx = 0;

  // Block bitmap pointer.
  typedef uint64_t searchType;
  size_t base = list[idx];
  searchType* ptr = reinterpret_cast<searchType*>(base);
  searchType* ptr_end = adjust_pointer(ptr, bitmapBlockBytes);

  // Find a free block in this group.
  bool changedBitmap = false;
  while (true) {
    // Grab the specific block for the bitmap.
    /// \todo Endianness - to ensure correct operation, must ptr be
    /// little endian?
    searchType tmp = *ptr;

    // Bitmap full of bits? Skip it.
    if (tmp != static_cast<searchType>(-1)) {
      // Check each bit in this field.
      for (size_t j = 0; j < (sizeof(searchType) * 8); j++, tmp >>= static_cast<searchType>(1)) {
        // Free?
        if ((tmp & 1) == 0) {
          // This block is free! Mark used.
          *ptr |= (static_cast<searchType>(1) << j);
          pDesc->bg_free_blocks_count--;

          // Yes, we changed the bitmap.
          changedBitmap = true;

          // Update superblock.
          m_pSuperblock->s_free_blocks_count--;

          // First block of this group...
          uint32_t result = group * LITTLE_TO_HOST32(m_pSuperblock->s_blocks_per_group);
          // Add the data block offset for this filesystem.
          result += LITTLE_TO_HOST32(m_pSuperblock->s_first_data_block);
          // Blocks skipped so far (i == offset in bytes)...
          result += ((idx * bitmapBlockBytes) + (reinterpret_cast<uintptr_t>(ptr) - base)) << 3;
          // Blocks skipped so far (j == bits ie blocks)...
          result += j;
          // Return block.
          blocks.pushBack(result);

          // Check if we're done - we have nothing left to do if
          // there's no more blocks free in this bitmap.
          if ((++currentCount >= maxCount) || (!pDesc->bg_free_blocks_count)) {
            break;
          }
        }
      }
    }

    // Did we make changes to the bitmap? Write back now if so - we don't
    // want to keep writing over and over if e.g. we're setting more than
    // one block above.
    if (changedBitmap) {
      // Update bitmap on disk.
      uint32_t desc_block = LITTLE_TO_HOST32(m_pGroupDescriptors[group]->bg_block_bitmap) + idx;
      writeBlock(desc_block);

      changedBitmap = false;
    }

    // Are we finished with this loop?
    if (currentCount >= maxCount) {
      break;
    }

    // Haven't found anything yet - need to take care here.
    if (++ptr >= ptr_end) {
      if ((++idx * bitmapBlockBytes) >= bytesToSearch)
        break;

      base = list[idx];
      ptr = reinterpret_cast<searchType*>(base);
      ptr_end = adjust_pointer(ptr, bitmapBlockBytes);
    }
  }

  if (currentCount >= maxCount) {
    // Write back the superblock/group descriptor updates now.
    m_pDisk->write(1024ULL);

    // Update group descriptor on disk.
    /// \todo save group descriptor block number elsewhere
    uint32_t gdBlock = LITTLE_TO_HOST32(m_pSuperblock->s_first_data_block) + 1;
    uint32_t groupBlock = (group * sizeof(GroupDesc)) / m_BlockSize;
    writeBlock(gdBlock + groupBlock);
  }

  return currentCount;
}

void Ext2Filesystem::releaseBlock(uint32_t block, uint32_t inode) {
#if THREADS || defined(STANDALONE_MUTEXES)
  LockGuard<Mutex> guard(m_WriteLock);
#endif

  releaseBlockLocked(block, inode);
}

bool Ext2Filesystem::prepareBlockReleaseLocked(uint32_t block) {
  const uint32_t first = LITTLE_TO_HOST32(m_pSuperblock->s_first_data_block);
  const uint32_t perGroup = LITTLE_TO_HOST32(m_pSuperblock->s_blocks_per_group);
  if (block <= first || !perGroup || (block - first) / perGroup >= m_nGroupDescriptors) {
    SYSCALL_ERROR(IoError);
    return false;
  }
  return ensureFreeBlockBitmapLoaded((block - first) / perGroup);
}

bool Ext2Filesystem::prepareInodeWrite(uint32_t inode) {
  const uint32_t perGroup = LITTLE_TO_HOST32(m_pSuperblock->s_inodes_per_group);
  if (!inode || !perGroup || (inode - 1) / perGroup >= m_nGroupDescriptors) {
    SYSCALL_ERROR(IoError);
    return false;
  }
  return ensureInodeTableLoaded((inode - 1) / perGroup);
}

void Ext2Filesystem::releaseBlockLocked(uint32_t block, uint32_t inode) {
  // In some ext2 filesystems, this is zero so we don't need to do this. But
  // for those that do, not doing this messes up the bit offsets below.
  block -= LITTLE_TO_HOST32(m_pSuperblock->s_first_data_block);

  uint32_t blocksPerGroup = LITTLE_TO_HOST32(m_pSuperblock->s_blocks_per_group);
  uint32_t group = block / blocksPerGroup;
  uint32_t index = block % blocksPerGroup;

  if (!block) {
    // Error out, zero is used as a sentinel in a few places - and we almost
    // certainly never actually mean to free block zero.
    FATAL("Releasing block zero!");
  }

  if (!ensureFreeBlockBitmapLoaded(group)) {
    return;
  }

  // Free block.
  GroupDesc* pDesc = m_pGroupDescriptors[group];

  // Index = block offset from the start of this block.
  size_t bitmapField = (index / 8) / m_BlockSize;
  size_t bitmapOffset = (index / 8) % m_BlockSize;

  Vector<size_t>& list = m_pBlockBitmaps[group];
  uintptr_t diskBlock = list[bitmapField];
  uint8_t* ptr = reinterpret_cast<uint8_t*>(diskBlock + bitmapOffset);
  uint8_t bit = (index % 8);
  if ((*ptr & (1 << bit)) == 0) {
    ERROR("bit already freed for block " << Dec << block << Hex);
    return;
  }
  *ptr &= ~(1 << bit);
  if (inode)
    m_Quota.refund(inode, m_BlockSize);

  // Update hints.
  pDesc->bg_free_blocks_count++;
  m_pSuperblock->s_free_blocks_count++;

  // Update superblock.
  m_pDisk->write(1024ULL);

  // Update bitmap on disk.
  uint32_t desc_block = LITTLE_TO_HOST32(m_pGroupDescriptors[group]->bg_block_bitmap) + bitmapField;
  writeBlock(desc_block);

  // Update group descriptor on disk.
  /// \todo save group descriptor block number elsewhere
  uint32_t gdBlock = LITTLE_TO_HOST32(m_pSuperblock->s_first_data_block) + 1;
  uint32_t groupBlock = (group * sizeof(GroupDesc)) / m_BlockSize;
  writeBlock(gdBlock + groupBlock);
}

Ext2InodeState* Ext2Filesystem::acquireInodeState(uint32_t inode, Inode* metadata) {
  LockGuard<Mutex> guard(m_InodeStateLock);
  Ext2InodeState* state = m_InodeStates.lookup(inode);
  if (state) {
    if (!state->references && !state->cache) {
      state->reloadMappings(metadata, this);
    }
    ++state->references;
    return state;
  }
  state = new Ext2InodeState(metadata, this);
  m_InodeStates.insert(inode, state);
  return state;
}

Ext2InodeState* Ext2Filesystem::acquireInodeStateLocked(uint32_t inode, Inode* metadata) {
  Ext2InodeState* state = m_InodeStates.lookup(inode);
  if (state) {
    if (!state->references && !state->cache) {
      state->reloadMappings(metadata, this);
    }
    ++state->references;
    return state;
  }
  state = new Ext2InodeState(metadata, this);
  if (!state || !m_InodeStates.tryInsert(inode, state)) {
    delete state;
    return nullptr;
  }
  return state;
}

void Ext2Filesystem::releaseInodeState(uint32_t inode, Ext2InodeState* state, Ext2Node* lastNode) {
  LockGuard<Mutex> stateGuard(m_InodeStateLock);
  assert(state == m_InodeStates.lookup(inode) && state->references);
  if (--state->references) {
    return;
  }
  if (!state->orphan) {
    assert(!state->pageLoans && !state->files.count());
    // Keep the nonreusable futex identity through the linked inode lifetime,
    // but reload potentially large block maps when an alias next opens it.
    if (!state->cache) {
      state->blocks.clear(true);
      state->metadataBlocks = 0;
    }
    state->files.clear(true);
    return;
  }
  m_InodeStates.remove(inode);
  if (state->orphan) {
#if THREADS || defined(STANDALONE_MUTEXES)
    LockGuard<Mutex> guard(m_WriteLock);
#endif
    retireInodeLocked(inode, lastNode);
  }
  delete state;
}

bool Ext2Filesystem::releaseInode(uint32_t inodeNumber, Ext2Node* retiringNode) {
  LockGuard<Mutex> stateGuard(m_InodeStateLock);
  Ext2InodeState* state = m_InodeStates.lookup(inodeNumber);
  LockGuard<Mutex> metadataGuard(state ? state->writebackLock : m_InodeStateLock, state != nullptr);
#if THREADS || defined(STANDALONE_MUTEXES)
  LockGuard<Mutex> guard(m_WriteLock);
#endif
  const bool remove = decreaseInodeRefcount(inodeNumber);
  if (remove) {
    if (state) {
      state->orphan = true;
      state->inodeEvents.beginRetirement();
    } else {
      retireInodeLocked(inodeNumber, retiringNode);
    }
  }
  return remove;
}

void Ext2Filesystem::retireInodeLocked(uint32_t inodeNumber, Ext2Node* retiringNode) {
  Inode* pInode = getInode(inodeNumber);
  if (!pInode)
    return;
  const uint32_t inodeIndex = inodeNumber - 1;  // Inode zero is undefined, so it's not used.

  uint32_t inodesPerGroup = LITTLE_TO_HOST32(m_pSuperblock->s_inodes_per_group);
  uint32_t group = inodeIndex / inodesPerGroup;
  uint32_t index = inodeIndex % inodesPerGroup;

  uint32_t allocatedBlocks = 0;
  bool inlineSymlink = false;
  AttributeRetirement attributes;
  if (!Ext2Node::decodeAllocation(*pInode, m_BlockSize, allocatedBlocks, inlineSymlink) ||
      prepareAttributeRetirementLocked(pInode, attributes) != XattrStatus::Success ||
      !ensureFreeInodeBitmapLoaded(group) ||
      (attributes.block && reserveAttributeWritesLocked(12) != XattrStatus::Success)) {
    ERROR("Ext2: retaining an orphan inode whose attributes could not be retired");
    return;
  }
  {
    // Keep the allocation bit set until all old data has been retired. This
    // prevents a concurrent creator from reusing the inode before wipe()
    // finishes zeroing it.
    if (retiringNode && !retiringNode->wipe(true)) {
      ERROR("Ext2: retaining an orphan inode whose blocks could not be retired");
      return;
    }

    if (attributes.block) {
      const uint32_t sectors = m_BlockSize / 512;
      const uint32_t allocated = LITTLE_TO_HOST32(pInode->i_blocks);
      assert(allocated >= sectors);
      pInode->i_file_acl = 0;
      pInode->i_blocks = HOST_TO_LITTLE32(allocated - sectors);
      commitAttributeRetirementLocked(attributes);
      recordAttributeInodeLocked(inodeNumber);
    }
    // Set dtime on inode.
    pInode->i_dtime = HOST_TO_LITTLE32(getUnixTimestamp());

    if (!ensureFreeInodeBitmapLoaded(group)) {
      return;
    }

    // Free inode.
    GroupDesc* pDesc = m_pGroupDescriptors[group];
    pDesc->bg_free_inodes_count++;
    m_pSuperblock->s_free_inodes_count++;

    // Index = inode offset from the start of this block.
    size_t bitmapField = (index / 8) / m_BlockSize;
    size_t bitmapOffset = (index / 8) % m_BlockSize;

    Vector<size_t>& list = m_pInodeBitmaps[group];
    uintptr_t block = list[bitmapField];
    uint8_t* ptr = reinterpret_cast<uint8_t*>(block + bitmapOffset);
    *ptr &= ~(1 << (index % 8));
    m_Quota.forget(inodeNumber);
    if (attributes.block) {
      const uint32_t bitmap = LITTLE_TO_HOST32(pDesc->bg_inode_bitmap) + bitmapField;
      recordAttributeWriteLocked(static_cast<uint64_t>(bitmap) * m_BlockSize,
                                 AttributeWriteKind::Allocation);
      recordAttributeAllocationLocked(attributes.block);
      const uint32_t descriptor = LITTLE_TO_HOST32(m_pSuperblock->s_first_data_block) + 1 +
                                  (group * sizeof(GroupDesc)) / m_BlockSize;
      recordAttributeWriteLocked(static_cast<uint64_t>(descriptor) * m_BlockSize,
                                 AttributeWriteKind::Allocation);
    }

    // Update superblock.
    m_pDisk->write(1024ULL);

    // Update on disk.
    uint32_t desc_block =
        LITTLE_TO_HOST32(m_pGroupDescriptors[group]->bg_inode_bitmap) + bitmapField;
    writeBlock(desc_block);

    // Update group descriptor on disk.
    /// \todo save group descriptor block number elsewhere
    uint32_t gdBlock = LITTLE_TO_HOST32(m_pSuperblock->s_first_data_block) + 1;
    uint32_t groupBlock = (group * sizeof(GroupDesc)) / m_BlockSize;
    writeBlock(gdBlock + groupBlock);
  }

  writeInode(inodeNumber);
}

Inode* Ext2Filesystem::getInode(uint32_t inode) {
  assert(inode > 0);

  inode--;  // Inode zero is undefined, so it's not used.

  uint32_t inodesPerGroup = LITTLE_TO_HOST32(m_pSuperblock->s_inodes_per_group);
  uint32_t group = inode / inodesPerGroup;
  uint32_t index = inode % inodesPerGroup;

  if (!ensureInodeTableLoaded(group)) {
    return nullptr;
  }
  Vector<size_t>& list = m_pInodeTables[group];

  size_t blockNum = (index * m_InodeSize) / m_BlockSize;
  size_t blockOff = (index * m_InodeSize) % m_BlockSize;

  uintptr_t block = list[blockNum];

  Inode* pInode = reinterpret_cast<Inode*>(block + blockOff);
  if (pInode->i_flags & EXT2_COMPRBLK_FL) {
    WARNING("Ext2: inode " << inode << " has compressed blocks - not yet supported!");
  }
  return pInode;
}

void Ext2Filesystem::writeInode(uint32_t inode) {
  inode--;  // Inode zero is undefined, so it's not used.

  uint32_t inodesPerGroup = LITTLE_TO_HOST32(m_pSuperblock->s_inodes_per_group);
  uint32_t group = inode / inodesPerGroup;
  uint32_t index = inode % inodesPerGroup;

  if (!ensureInodeTableLoaded(group)) {
    return;
  }

  size_t blockNum = (index * m_InodeSize) / m_BlockSize;
  uint64_t diskBlock = LITTLE_TO_HOST32(m_pGroupDescriptors[group]->bg_inode_table) + blockNum;
  writeBlock(diskBlock);
}

bool Ext2Filesystem::checkOptionalFeature(size_t feature) {
  if (LITTLE_TO_HOST32(m_pSuperblock->s_rev_level) < 1)
    return false;
  return m_pSuperblock->s_feature_compat & feature;
}

bool Ext2Filesystem::checkRequiredFeature(size_t feature) {
  if (LITTLE_TO_HOST32(m_pSuperblock->s_rev_level) < 1)
    return false;
  return m_pSuperblock->s_feature_incompat & feature;
}

bool Ext2Filesystem::checkReadOnlyFeature(size_t feature) {
  if (LITTLE_TO_HOST32(m_pSuperblock->s_rev_level) < 1)
    return false;
  return m_pSuperblock->s_feature_ro_compat & feature;
}

bool Ext2Filesystem::ensureFreeBlockBitmapLoaded(size_t group) {
  assert(group < m_nGroupDescriptors);
  Vector<size_t>& list = m_pBlockBitmaps[group];

  if (list.count() > 0)
    // Descriptors already loaded.
    return true;

  // Determine how many blocks to load to bring in the full block bitmap.
  // The bitmap works so that 8 blocks fit into one byte.
  uint32_t blocksPerGroup = LITTLE_TO_HOST32(m_pSuperblock->s_blocks_per_group);
  size_t nBlocks = blocksPerGroup / (m_BlockSize * 8);
  if (blocksPerGroup % (m_BlockSize * 8))
    nBlocks++;

  if (!list.tryReserve(nBlocks)) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }

  const uint32_t start = LITTLE_TO_HOST32(m_pGroupDescriptors[group]->bg_block_bitmap);
  for (size_t i = 0; i < nBlocks; i++) {
    uint32_t blockNumber = start + i;
    if (!blockNumber) {
      while (list.count()) {
        unpinBlock(start + list.count() - 1);
        list.popBack();
      }
      SYSCALL_ERROR(IoError);
      return false;
    }
    uintptr_t buffer = readBlock(blockNumber);
    if (!buffer) {
      while (list.count()) {
        unpinBlock(start + list.count() - 1);
        list.popBack();
      }
      SYSCALL_ERROR(IoError);
      return false;
    }
    list.pushBack(buffer);
  }

  return true;
}

bool Ext2Filesystem::ensureFreeInodeBitmapLoaded(size_t group) {
  assert(group < m_nGroupDescriptors);
  Vector<size_t>& list = m_pInodeBitmaps[group];

  if (list.count() > 0)
    // Descriptors already loaded.
    return true;

  // Determine how many blocks to load to bring in the full inode bitmap.
  // The bitmap works so that 8 inodes fit into one byte.
  uint32_t inodesPerGroup = LITTLE_TO_HOST32(m_pSuperblock->s_inodes_per_group);
  size_t nBlocks = inodesPerGroup / (m_BlockSize * 8);
  if (inodesPerGroup % (m_BlockSize * 8))
    nBlocks++;

  if (!list.tryReserve(nBlocks)) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }

  const uint32_t start = LITTLE_TO_HOST32(m_pGroupDescriptors[group]->bg_inode_bitmap);
  for (size_t i = 0; i < nBlocks; i++) {
    uint32_t blockNumber = start + i;
    if (!blockNumber) {
      while (list.count()) {
        unpinBlock(start + list.count() - 1);
        list.popBack();
      }
      SYSCALL_ERROR(IoError);
      return false;
    }
    uintptr_t buffer = readBlock(blockNumber);
    if (!buffer) {
      while (list.count()) {
        unpinBlock(start + list.count() - 1);
        list.popBack();
      }
      SYSCALL_ERROR(IoError);
      return false;
    }
    list.pushBack(buffer);
  }

  return true;
}

bool Ext2Filesystem::ensureInodeTableLoaded(size_t group) {
#if THREADS || defined(STANDALONE_MUTEXES)
  LockGuard<Mutex> guard(m_InodeTableLoadLock);
#endif

  assert(group < m_nGroupDescriptors);
  Vector<size_t>& list = m_pInodeTables[group];

  if (list.count() > 0) {
    // Descriptors already loaded.
    return true;
  }

  // Determine how many blocks to load to bring in the full inode table.
  uint32_t inodesPerGroup = LITTLE_TO_HOST32(m_pSuperblock->s_inodes_per_group);
  const uint64_t inodeTableBytes = static_cast<uint64_t>(inodesPerGroup) * m_InodeSize;
  const size_t nBlocks = (inodeTableBytes + m_BlockSize - 1) / m_BlockSize;

  if (!nBlocks) {
    ERROR("inode table has zero blocks [inode size=" << m_InodeSize
                                                     << "], possibly corrupted filesystem.");
    SYSCALL_ERROR(IoError);
    return false;
  }

  if (!list.tryReserve(nBlocks)) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }

  // Load each block in the inode table.
  const uint32_t inodeTableStart = LITTLE_TO_HOST32(m_pGroupDescriptors[group]->bg_inode_table);
  if (!inodeTableStart) {
    SYSCALL_ERROR(IoError);
    return false;
  }
  for (size_t i = 0; i < nBlocks; i++) {
    uint32_t blockNumber = inodeTableStart + i;
    uintptr_t buffer = readBlock(blockNumber);
    if (!buffer) {
      // Do not publish a partially loaded table. Every successful
      // read() above owns exactly one reference.
      while (list.count()) {
        const size_t loaded = list.count() - 1;
        unpinBlock(inodeTableStart + loaded);
        list.popBack();
      }
      SYSCALL_ERROR(IoError);
      return false;
    }
    list.pushBack(buffer);
  }

  return true;
}

void Ext2Filesystem::increaseInodeRefcount(uint32_t inode) {
  LockGuard<Mutex> stateGuard(m_InodeStateLock);
  Ext2InodeState* state = m_InodeStates.lookup(inode);
  LockGuard<Mutex> metadataGuard(state ? state->writebackLock : m_InodeStateLock, state != nullptr);
#if THREADS || defined(STANDALONE_MUTEXES)
  LockGuard<Mutex> guard(m_WriteLock);
#endif

  Inode* pInode = getInode(inode);
  if (!pInode)
    return;

  uint32_t current_count = LITTLE_TO_HOST16(pInode->i_links_count);
  pInode->i_links_count = HOST_TO_LITTLE16(current_count + 1);
  if (state) {
    state->orphan = false;
  }

  writeInode(inode);
}

bool Ext2Filesystem::decreaseInodeRefcount(uint32_t inode) {
  Inode* pInode = getInode(inode);
  if (!pInode)
    return true;  // No inode found - but didn't decrement to zero.

  uint32_t current_count = LITTLE_TO_HOST16(pInode->i_links_count);
  bool bRemove = current_count <= 1;
  if (current_count)
    pInode->i_links_count = HOST_TO_LITTLE16(current_count - 1);

  writeInode(inode);
  return bRemove;
}

#ifndef EXT2_STANDALONE
static bool initExt2() {
  VFS::instance().addProbeCallback(&Ext2Filesystem::probe);
  return true;
}

static void destroyExt2() {
  if (!VFS::instance().removeProbeCallback(&Ext2Filesystem::probe)) {
    FATAL("Ext2 probe callback was not registered during unload");
  }
}

MODULE_INFO("ext2", &initExt2, &destroyExt2, "vfs");
#endif
