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
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/stddef.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Pointers.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Ext2File.h"
#include "Ext2Filesystem.h"
#include "Ext2Symlink.h"
#include "ext2.h"
#include "modules/system/vfs/File.h"

class Filesystem;

Ext2Directory::Ext2Directory(const String& name, uintptr_t inode_num, Inode* inode,
                             Ext2Filesystem* pFs, File* pParent)
    : Directory(name, LITTLE_TO_HOST32(inode->i_atime), LITTLE_TO_HOST32(inode->i_mtime),
                LITTLE_TO_HOST32(inode->i_ctime), inode_num, static_cast<Filesystem*>(pFs),
                LITTLE_TO_HOST32(inode->i_size),  /// \todo Deal with >4GB files here.
                pParent),
      Ext2Node(inode_num, inode, pFs),
      m_DirectoryLock(),
      m_Removed(false) {
  uint32_t mode = LITTLE_TO_HOST32(inode->i_mode);
  setPermissionsOnly(modeToPermissions(mode));
  setUidOnly(Ext2Owner::uid(*inode));
  setGidOnly(Ext2Owner::gid(*inode));
}

Ext2Directory::~Ext2Directory() {}

bool Ext2Directory::sync() {
  LockGuard<Mutex> directoryGuard(m_DirectoryLock);
  LockGuard<Mutex> writebackGuard(m_State->writebackLock);
  if (!m_pExt2Fs->m_BlockSize) {
    return false;
  }

  // Directory records bypass File's page cache, so even an empty cache must
  // submit their backing blocks before reporting a completed namespace change.
  bool succeeded = true;
  for (size_t i = 0; i < m_Blocks.count(); ++i) {
    if (!ensureBlockLoaded(i)) {
      succeeded = false;
      continue;
    }
    succeeded = syncDirectoryBlock(m_Blocks[i]) && succeeded;
  }
  for (size_t i = 0; i < m_State->namespaceSyncBlocks.count();) {
    if (syncDirectoryBlock(m_State->namespaceSyncBlocks[i])) {
      m_State->namespaceSyncBlocks.erase(i);
    } else {
      succeeded = false;
      ++i;
    }
  }
  return m_pExt2Fs->syncInode(getInodeNumber(), *this, true) && succeeded;
}

bool Ext2Directory::syncDirectoryBlock(uint32_t block) {
  if (!block || !m_pExt2Fs->readBlock(block)) {
    return false;
  }
  const bool succeeded = m_pExt2Fs->syncBlock(block, false);
  m_pExt2Fs->unpinBlock(block);
  return succeeded;
}

void Ext2Directory::queueSyncDependency(uint32_t block) {
  LockGuard<Mutex> guard(m_State->writebackLock);
  for (uint32_t dependency : m_State->namespaceSyncBlocks) {
    if (dependency == block) {
      return;
    }
  }
  // Remember the block identity, not a cache address: removal may retire and
  // reuse it before this parent is synced. Resolve current contents at sync.
  m_State->namespaceSyncBlocks.pushBack(block);
}

bool Ext2Directory::addEntry(const String& filename, File* pFile, size_t type) {
  if (!filename.length() || filename.length() > 255) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }

  NameReservation reservation;
  if (!reserveDirectoryEntry(HashedStringView(filename), reservation)) {
    SYSCALL_ERROR(FileExists);
    return false;
  }

  LockGuard<Mutex> guard(m_DirectoryLock);
  if (m_Removed) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }

  uint64_t existingOffset = 0;
  while (existingOffset < m_nSize) {
    ParsedEntry entry;
    if (readEntry(existingOffset, entry) != ReadStatus::Complete) {
      SYSCALL_ERROR(IoError);
      return false;
    }
    if (entry.inode && filename.length() == entry.nameLength &&
        !StringCompareN(filename.cstr(), entry.name, entry.nameLength)) {
      SYSCALL_ERROR(FileExists);
      return false;
    }
    existingOffset += entry.recordLength;
  }

  // Calculate the size of our Dir* entry.
  size_t length = offsetof(Dir, d_name) + filename.length();
  if (length % 4) {
    length += 4 - (length % 4);
  }

  bool bFound = false;

  uint32_t i;
  Dir* pDir = 0;
  Dir* pBlockEnd = 0;
  Dir* pSplitDir = 0;
  uint16_t splitLength = 0;
  uint16_t splitRemainder = 0;
  for (i = 0; i < m_Blocks.count(); i++) {
    if (!ensureBlockLoaded(i)) {
      return false;
    }
    uintptr_t buffer = m_pExt2Fs->readBlock(m_Blocks[i]);
    if (!buffer) {
      return false;
    }
    pDir = reinterpret_cast<Dir*>(buffer);
    pBlockEnd = adjust_pointer(pDir, m_pExt2Fs->m_BlockSize);
    while (pDir < pBlockEnd) {
      const size_t remaining = pointer_diff(pDir, pBlockEnd);
      if (remaining < offsetof(Dir, d_name)) {
        m_pExt2Fs->unpinBlock(m_Blocks[i]);
        SYSCALL_ERROR(IoError);
        return false;
      }

      uint16_t entryReclen = LITTLE_TO_HOST16(pDir->d_reclen);
      if (entryReclen < offsetof(Dir, d_name) || entryReclen > remaining || (entryReclen % 4)) {
        m_pExt2Fs->unpinBlock(m_Blocks[i]);
        SYSCALL_ERROR(IoError);
        return false;
      }

      // What's the minimum length of this directory entry?
      size_t currentNameLength = pDir->d_namelen;
      if (!m_pExt2Fs->checkRequiredFeature(2)) {
        currentNameLength |= static_cast<size_t>(pDir->d_file_type) << 8;
      }
      if (currentNameLength > 255 || currentNameLength > entryReclen - offsetof(Dir, d_name)) {
        m_pExt2Fs->unpinBlock(m_Blocks[i]);
        SYSCALL_ERROR(IoError);
        return false;
      }
      size_t thisReclen = offsetof(Dir, d_name) + currentNameLength;
      // Align to 4-byte boundary.
      if (thisReclen % 4) {
        thisReclen += 4 - (thisReclen % 4);
      }

      // Valid directory entry?
      if (pDir->d_inode > 0) {
        // Is there enough space to add this dirent?
        /// \todo Ensure 4-byte alignment.
        if (entryReclen - thisReclen >= length) {
          bFound = true;
          pSplitDir = pDir;
          splitLength = thisReclen;
          splitRemainder = entryReclen - thisReclen;
          pDir = adjust_pointer(pDir, thisReclen);
          break;
        }
      } else if (entryReclen >= length) {
        // We can use this unused entry - we fit into it.
        // The record length does not need to be adjusted.
        bFound = true;
        break;
      }

      // Next.
      pDir = adjust_pointer(pDir, entryReclen);
    }
    if (bFound)
      break;
    m_pExt2Fs->unpinBlock(m_Blocks[i]);
  }

  if (!bFound || !pDir) {
    // Need to make a new block.
    uint32_t block = m_pExt2Fs->findFreeBlock(getInodeNumber());
    if (block == 0) {
      // We had a problem.
      return false;
    }
    const uintptr_t buffer = m_pExt2Fs->readBlock(block);
    if (!buffer) {
      m_pExt2Fs->releaseBlock(block, getInodeNumber());
      SYSCALL_ERROR(IoError);
      return false;
    }
    // Publish a valid empty directory block only after its contents are ready.
    ByteSet(reinterpret_cast<void*>(buffer), 0, m_pExt2Fs->m_BlockSize);
    pDir = reinterpret_cast<Dir*>(buffer);
    pDir->d_reclen = HOST_TO_LITTLE16(m_pExt2Fs->m_BlockSize);
    m_pExt2Fs->writeBlock(block);
    if (!addBlock(block)) {
      const int failure = m_pExt2Fs->currentIoError();
      m_pExt2Fs->unpinBlock(block);
      m_pExt2Fs->releaseBlock(block, getInodeNumber());
      syscallError(failure);
      return false;
    }
    i = m_Blocks.count() - 1;
    m_nSize = m_Blocks.count() * m_pExt2Fs->m_BlockSize;
    m_Size = m_nSize;
    fileAttributeChanged();
  }

  const bool special = filename.compare(".") || filename.compare("..");

  if (!special && type == EXT2_S_IFDIR) {
    Ext2Directory* child = static_cast<Ext2Directory*>(pFile);
    for (size_t block = 0; block < child->m_Blocks.count(); ++block) {
      if (!child->ensureBlockLoaded(block)) {
        m_pExt2Fs->unpinBlock(m_Blocks[i]);
        SYSCALL_ERROR(IoError);
        return false;
      }
      queueSyncDependency(child->m_Blocks[block]);
    }
  }

  if (pSplitDir) {
    pSplitDir->d_reclen = HOST_TO_LITTLE16(splitLength);
    pDir->d_reclen = HOST_TO_LITTLE16(splitRemainder);
  }

  // Set the directory contents.
  uint32_t entryInode = pFile->getInode();
  pDir->d_inode = HOST_TO_LITTLE32(entryInode);
  m_pExt2Fs->increaseInodeRefcount(entryInode);

  if (m_pExt2Fs->checkRequiredFeature(2)) {
    // File type in directory entry.
    switch (type) {
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
  } else {
    // No file type in directory entries.
    pDir->d_file_type = 0;
  }

  pDir->d_namelen = static_cast<uint8_t>(filename.length());
  MemoryCopy(pDir->d_name, static_cast<const char*>(filename), filename.length());

  // Trigger write back to disk.
  m_pExt2Fs->writeBlock(m_Blocks[i]);
  m_pExt2Fs->unpinBlock(m_Blocks[i]);

  if (!special) {
    const bool published = addCachedDirectoryEntry(reservation, pFile);
    assert(published);
    publishEvent(FileEvents::Created, filename.view(), pFile->isDirectory());
    reservation.complete(LookupStatus::Found);
  }

  m_Size = m_nSize;

  return true;
}

bool Ext2Directory::removeEntry(const String& filename, Ext2Node* pFile) {
  LockGuard<Mutex> guard(m_DirectoryLock);
  return removeEntryLocked(filename, pFile);
}

bool Ext2Directory::removeEntryLocked(const String& filename, Ext2Node* pFile) {
  // Find this file in the directory.
  size_t fileInode = pFile->getInodeNumber();

  bool bFound = false;

  uint32_t i;
  Dir* pDir;
  for (i = 0; i < m_Blocks.count(); i++) {
    if (!ensureBlockLoaded(i)) {
      return false;
    }
    uintptr_t buffer = m_pExt2Fs->readBlock(m_Blocks[i]);
    if (!buffer) {
      return false;
    }
    pDir = reinterpret_cast<Dir*>(buffer);
    while (reinterpret_cast<uintptr_t>(pDir) < buffer + m_pExt2Fs->m_BlockSize) {
      const uintptr_t current = reinterpret_cast<uintptr_t>(pDir);
      const size_t remaining = buffer + m_pExt2Fs->m_BlockSize - current;
      if (remaining < offsetof(Dir, d_name)) {
        m_pExt2Fs->unpinBlock(m_Blocks[i]);
        SYSCALL_ERROR(IoError);
        return false;
      }

      const uint16_t recordLength = LITTLE_TO_HOST16(pDir->d_reclen);
      if (recordLength < offsetof(Dir, d_name) || recordLength > remaining || (recordLength % 4)) {
        m_pExt2Fs->unpinBlock(m_Blocks[i]);
        SYSCALL_ERROR(IoError);
        return false;
      }

      size_t nameLength = pDir->d_namelen;
      if (!m_pExt2Fs->checkRequiredFeature(2)) {
        nameLength |= static_cast<size_t>(pDir->d_file_type) << 8;
      }
      if (nameLength > 255 || nameLength > recordLength - offsetof(Dir, d_name)) {
        m_pExt2Fs->unpinBlock(m_Blocks[i]);
        SYSCALL_ERROR(IoError);
        return false;
      }

      if (LITTLE_TO_HOST32(pDir->d_inode) == fileInode) {
        if (nameLength == filename.length()) {
          if (!StringCompareN(pDir->d_name, static_cast<const char*>(filename), nameLength)) {
            // Wipe out the directory entry.
            uint16_t old_reclen = recordLength;
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

      pDir = reinterpret_cast<Dir*>(reinterpret_cast<uintptr_t>(pDir) + recordLength);
    }

    m_pExt2Fs->unpinBlock(m_Blocks[i]);
    if (bFound)
      break;
  }

  m_Size = m_nSize;

  if (bFound) {
    m_pExt2Fs->releaseInode(fileInode, pFile);
    invalidateDirectoryEntry(HashedStringView(filename));
    return true;
  } else {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
}

void Ext2Directory::fileAttributeChanged() {
  LockGuard<Mutex> guard(m_State->writebackLock);
  Ext2Node::fileAttributeChanged(m_Size, LITTLE_TO_HOST32(m_pInode->i_atime),
                                 LITTLE_TO_HOST32(m_pInode->i_mtime),
                                 LITTLE_TO_HOST32(m_pInode->i_ctime));
}

File::Attributes Ext2Directory::getAttributes() const {
  return inodeAttributes();
}

void Ext2Directory::updateAttributes(const Attributes& attributes, uint32_t mask) {
  updateInodeAttributes(attributes, mask);
}

bool Ext2Directory::readBytes(uint64_t offset, size_t length, void* output) {
  if (offset > m_nSize || length > (m_nSize - offset)) {
    return false;
  }

  uint8_t* destination = reinterpret_cast<uint8_t*>(output);
  while (length) {
    const size_t block = offset / m_pExt2Fs->m_BlockSize;
    const size_t blockOffset = offset % m_pExt2Fs->m_BlockSize;
    if (block >= m_Blocks.count() || !ensureBlockLoaded(block)) {
      return false;
    }

    const uintptr_t buffer = m_pExt2Fs->readBlock(m_Blocks[block]);
    if (!buffer) {
      return false;
    }

    size_t available = m_pExt2Fs->m_BlockSize - blockOffset;
    if (available > length) {
      available = length;
    }
    MemoryCopy(destination, reinterpret_cast<const void*>(buffer + blockOffset), available);
    m_pExt2Fs->unpinBlock(m_Blocks[block]);

    destination += available;
    offset += available;
    length -= available;
  }

  return true;
}

Directory::ReadStatus Ext2Directory::readEntry(uint64_t offset, ParsedEntry& entry) {
  if (!m_pExt2Fs->m_BlockSize) {
    return ReadStatus::IoError;
  }

  Dir header;
  if (!readBytes(offset, offsetof(Dir, d_name), &header)) {
    return ReadStatus::IoError;
  }

  entry.inode = LITTLE_TO_HOST32(header.d_inode);
  entry.recordLength = LITTLE_TO_HOST16(header.d_reclen);
  entry.nameLength = header.d_namelen;
  entry.fileType = header.d_file_type;
  if (!m_pExt2Fs->checkRequiredFeature(2)) {
    entry.nameLength |= static_cast<uint16_t>(header.d_file_type) << 8;
    entry.fileType = EXT2_UNKNOWN;
  }

  const size_t bytesRemainingInBlock = m_pExt2Fs->m_BlockSize - (offset % m_pExt2Fs->m_BlockSize);
  if (entry.recordLength < offsetof(Dir, d_name) || (entry.recordLength % 4) ||
      entry.recordLength > (m_nSize - offset) || entry.recordLength > bytesRemainingInBlock ||
      entry.nameLength > 255 || entry.nameLength > entry.recordLength - offsetof(Dir, d_name)) {
    return ReadStatus::IoError;
  }

  if (!entry.inode) {
    entry.name[0] = 0;
    return ReadStatus::Complete;
  }

  const uint32_t inodeCount = LITTLE_TO_HOST32(m_pExt2Fs->m_pSuperblock->s_inodes_count);
  if (!entry.nameLength || entry.inode > inodeCount || entry.fileType >= EXT2_MAX ||
      !readBytes(offset + offsetof(Dir, d_name), entry.nameLength, entry.name)) {
    return ReadStatus::IoError;
  }
  entry.name[entry.nameLength] = 0;
  return ReadStatus::Complete;
}

Directory::LookupStatus Ext2Directory::resolveEntry(const ParsedEntry& entry,
                                                    const StringView& name, File*& child) {
  child = nullptr;
  if (!entry.inode || !name.compare(entry.name, entry.nameLength)) {
    return LookupStatus::NotFound;
  }

  Inode* inode = m_pExt2Fs->getInode(entry.inode);
  if (!inode) {
    return LookupStatus::IoError;
  }

  uint8_t fileType = entry.fileType;
  if (!m_pExt2Fs->checkRequiredFeature(2) || fileType == EXT2_UNKNOWN) {
    switch (LITTLE_TO_HOST16(inode->i_mode) & 0xF000) {
      case EXT2_S_IFREG:
        fileType = EXT2_FILE;
        break;
      case EXT2_S_IFDIR:
        fileType = EXT2_DIRECTORY;
        break;
      case EXT2_S_IFLNK:
        fileType = EXT2_SYMLINK;
        break;
      case EXT2_S_IFCHR:
      case EXT2_S_IFBLK:
      case EXT2_S_IFIFO:
      case EXT2_S_IFSOCK:
        return LookupStatus::NotFound;
      default:
        return LookupStatus::IoError;
    }
  }

  const String filename(entry.name, entry.nameLength);
  switch (fileType) {
    case EXT2_FILE: {
      Ext2File* file = new Ext2File(filename, entry.inode, inode, m_pExt2Fs, this);
      if (!file || !file->valid()) {
        delete file;
        return LookupStatus::IoError;
      }
      child = file;
      break;
    }
    case EXT2_DIRECTORY:
      child = new Ext2Directory(filename, entry.inode, inode, m_pExt2Fs, this);
      break;
    case EXT2_SYMLINK:
      child = new Ext2Symlink(filename, entry.inode, inode, m_pExt2Fs, this);
      break;
    case EXT2_CHAR_DEV:
    case EXT2_BLOCK_DEV:
    case EXT2_FIFO:
    case EXT2_SOCKET:
      return LookupStatus::NotFound;
    default:
      return LookupStatus::IoError;
  }

  return LookupStatus::Found;
}

Directory::LookupStatus Ext2Directory::resolveChildLocked(const StringView& name, File*& child) {
  child = nullptr;
  uint64_t offset = 0;
  while (offset < m_nSize) {
    ParsedEntry entry;
    if (readEntry(offset, entry) != ReadStatus::Complete) {
      return LookupStatus::IoError;
    }
    if (entry.inode && name.compare(entry.name, entry.nameLength)) {
      return resolveEntry(entry, name, child);
    }
    offset += entry.recordLength;
  }
  return LookupStatus::NotFound;
}

Directory::LookupStatus Ext2Directory::resolveChild(const StringView& name, File*& child) {
  child = nullptr;
  if (name.compare(".", 1) || name.compare("..", 2)) {
    return LookupStatus::NotFound;
  }

  LockGuard<Mutex> guard(m_DirectoryLock);
  if (m_Removed) {
    return LookupStatus::NotFound;
  }
  return resolveChildLocked(name, child);
}

Directory::LookupStatus Ext2Directory::resolveChildAt(uint64_t cookie, const StringView& name,
                                                      File*& child) {
  child = nullptr;
  if (name.compare(".", 1) || name.compare("..", 2)) {
    return LookupStatus::NotFound;
  }

  LockGuard<Mutex> guard(m_DirectoryLock);
  if (m_Removed) {
    return LookupStatus::NotFound;
  }
  if (cookie < m_nSize) {
    ParsedEntry entry;
    if (readEntry(cookie, entry) == ReadStatus::Complete && entry.inode &&
        name.compare(entry.name, entry.nameLength)) {
      return resolveEntry(entry, name, child);
    }
  }

  // A directory mutation may make a previously returned cookie stale. Fall
  // back to a name lookup rather than turning that race into a false miss.
  return resolveChildLocked(name, child);
}

Directory::ReadStatus Ext2Directory::readDirectory(uint64_t& cookie, DirectoryEntryEmitter emitter,
                                                   void* context) {
  LockGuard<Mutex> guard(m_DirectoryLock);
  if (m_Removed) {
    return ReadStatus::Complete;
  }
  if (cookie > m_nSize) {
    return ReadStatus::IoError;
  }

  while (cookie < m_nSize) {
    ParsedEntry parsed;
    if (readEntry(cookie, parsed) != ReadStatus::Complete) {
      return ReadStatus::IoError;
    }

    const uint64_t currentCookie = cookie;
    const uint64_t nextCookie = currentCookie + parsed.recordLength;
    if (!parsed.inode) {
      cookie = nextCookie;
      continue;
    }

    EntryType type = EntryType::Unknown;
    switch (parsed.fileType) {
      case EXT2_UNKNOWN:
        type = EntryType::Unknown;
        break;
      case EXT2_FILE:
        type = EntryType::Regular;
        break;
      case EXT2_DIRECTORY:
        type = EntryType::Directory;
        break;
      case EXT2_SYMLINK:
        type = EntryType::Symlink;
        break;
      case EXT2_CHAR_DEV:
        type = EntryType::CharacterDevice;
        break;
      case EXT2_BLOCK_DEV:
        type = EntryType::BlockDevice;
        break;
      case EXT2_FIFO:
        type = EntryType::Fifo;
        break;
      case EXT2_SOCKET:
        type = EntryType::Socket;
        break;
      default:
        return ReadStatus::IoError;
    }

    DirectoryEntryView entry = {StringView(parsed.name, parsed.nameLength), parsed.inode, type,
                                currentCookie, nextCookie};
    if (!emitter(context, entry)) {
      return ReadStatus::Stopped;
    }
    cookie = nextCookie;
  }

  return ReadStatus::Complete;
}

bool Ext2Directory::removeFromParent(Ext2Directory* parent, const String& filename) {
  if (parent == this) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  LockGuard<Mutex> namespaceGuard(namespaceMutationLock());
  bool empty = false;
  if (isEmpty(empty) != ReadStatus::Complete) {
    SYSCALL_ERROR(IoError);
    return false;
  }
  if (!empty) {
    SYSCALL_ERROR(NotEmpty);
    return false;
  }

  LockGuard<Mutex> guard(m_DirectoryLock);
  if (m_Removed) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }

  bool foundDot = false;
  bool foundDotDot = false;
  uint64_t offset = 0;
  while (offset < m_nSize) {
    ParsedEntry entry;
    if (readEntry(offset, entry) != ReadStatus::Complete) {
      SYSCALL_ERROR(IoError);
      return false;
    }

    if (entry.inode) {
      const StringView name(entry.name, entry.nameLength);
      if (name.compare(".", 1)) {
        foundDot = entry.inode == getInodeNumber();
        if (!foundDot) {
          SYSCALL_ERROR(IoError);
          return false;
        }
      } else if (name.compare("..", 2)) {
        foundDotDot = entry.inode == parent->getInodeNumber();
        if (!foundDotDot) {
          SYSCALL_ERROR(IoError);
          return false;
        }
      } else {
        SYSCALL_ERROR(NotEmpty);
        return false;
      }
    }
    offset += entry.recordLength;
  }

  if (!foundDot || !foundDotDot) {
    SYSCALL_ERROR(IoError);
    return false;
  }

  LockGuard<Mutex> parentGuard(parent->m_DirectoryLock);
  if (!parent->removeEntryLocked(filename, this)) {
    return false;
  }
  m_Removed = true;
  markDetached();
  if (!removeEntryLocked(String(".."), parent)) {
    ERROR("Ext2 directory was unlinked, but its parent link could not be retired");
    return true;
  }
  if (!removeEntryLocked(String("."), this)) {
    ERROR("Ext2 directory was unlinked, but its inode could not be retired");
  }

  return true;
}
