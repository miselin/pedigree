/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Ext2File.h"
#include "Ext2Filesystem.h"
#include "ext2.h"
#include "modules/system/vfs/VFS.h"

namespace {
constexpr int32_t HandleType = 0x50444731;
constexpr uint32_t HandleLength = 24;

bool hasUuid(const Superblock* superblock) {
  uint8_t value = 0;
  for (size_t i = 0; i < 16; ++i) {
    value |= static_cast<uint8_t>(superblock->s_uuid[i]);
  }
  return value != 0;
}

uint32_t load32(const uint8_t* bytes) {
  uint32_t value;
  MemoryCopy(&value, bytes, sizeof(value));
  return LITTLE_TO_HOST32(value);
}

void store32(uint8_t* bytes, uint32_t value) {
  value = HOST_TO_LITTLE32(value);
  MemoryCopy(bytes, &value, sizeof(value));
}
}  // namespace

FileHandleStatus Ext2Filesystem::fileHandleFsid(FileSystemId& result) {
  result = {};
  if (!m_pSuperblock || !hasUuid(m_pSuperblock)) {
    return FileHandleStatus::Unsupported;
  }
  const auto* uuid = reinterpret_cast<const uint8_t*>(m_pSuperblock->s_uuid);
  result.words[0] = load32(uuid) ^ load32(uuid + 8);
  result.words[1] = load32(uuid + 4) ^ load32(uuid + 12);
  if (!result.words[0] && !result.words[1]) {
    result.words[1] = 1;
  }
  return FileHandleStatus::Success;
}

FileHandleStatus Ext2Filesystem::validateHandleInodeLocked(uint32_t number, uint32_t generation,
                                                           Inode*& inode) {
  inode = nullptr;
  const uint32_t perGroup = LITTLE_TO_HOST32(m_pSuperblock->s_inodes_per_group);
  if (!number || number > LITTLE_TO_HOST32(m_pSuperblock->s_inodes_count) || !perGroup ||
      (number - 1) / perGroup >= m_nGroupDescriptors) {
    return FileHandleStatus::Stale;
  }
  const uint32_t group = (number - 1) / perGroup, index = (number - 1) % perGroup;
  if (!ensureFreeInodeBitmapLoaded(group)) {
    return FileHandleStatus::IoError;
  }
  const size_t field = (index / 8) / m_BlockSize;
  const auto* byte =
      reinterpret_cast<const uint8_t*>(m_pInodeBitmaps[group][field] + (index / 8) % m_BlockSize);
  if (!(*byte & (1U << (index % 8)))) {
    return FileHandleStatus::Stale;
  }
  inode = getInode(number);
  if (!inode) {
    return FileHandleStatus::IoError;
  }
  // The remaining fields can be initialized outside the allocation lock only
  // while the inode has no links and cannot be decoded.
  if (!LITTLE_TO_HOST16(inode->i_links_count) ||
      LITTLE_TO_HOST32(inode->i_generation) != generation ||
      (LITTLE_TO_HOST16(inode->i_mode) & 0xf000) != EXT2_S_IFREG) {
    return FileHandleStatus::Stale;
  }
  return FileHandleStatus::Success;
}

FileHandleStatus Ext2Filesystem::encodeFileHandle(File& file, FileHandle& result) {
  result = {};
  if (file.getFilesystem() != this || !file.supportsRegularFileOperations() || !m_pSuperblock ||
      !hasUuid(m_pSuperblock)) {
    return FileHandleStatus::Unsupported;
  }
  const uintptr_t number = file.getInode();
  if (!number || number > LITTLE_TO_HOST32(m_pSuperblock->s_inodes_count)) {
    return FileHandleStatus::Stale;
  }
  LockGuard<Mutex> registry(m_InodeStateLock);
  Ext2InodeState* state = m_InodeStates.lookup(number);
  if (!state || state->orphan) {
    return FileHandleStatus::Stale;
  }
  LockGuard<Mutex> metadata(state->writebackLock);
#if THREADS || defined(STANDALONE_MUTEXES)
  LockGuard<Mutex> allocation(m_WriteLock);
#endif
  Inode* inode = getInode(number);
  if (!inode) {
    return FileHandleStatus::IoError;
  }
  const uint32_t generation = LITTLE_TO_HOST32(inode->i_generation);
  const auto status = validateHandleInodeLocked(number, generation, inode);
  if (status != FileHandleStatus::Success) {
    return status;
  }
  result.length = HandleLength;
  result.type = HandleType;
  MemoryCopy(result.bytes, m_pSuperblock->s_uuid, 16);
  store32(result.bytes + 16, number);
  store32(result.bytes + 20, generation);
  return FileHandleStatus::Success;
}

FileHandleStatus Ext2Filesystem::decodeFileHandle(const FileHandle& handle, RetainedFile& result) {
  result.reset();
  if (!m_pSuperblock || !hasUuid(m_pSuperblock)) {
    return FileHandleStatus::Unsupported;
  }
  if (handle.type != HandleType || handle.length != HandleLength ||
      MemoryCompare(handle.bytes, m_pSuperblock->s_uuid, 16)) {
    return FileHandleStatus::Stale;
  }
  const uint32_t number = load32(handle.bytes + 16), generation = load32(handle.bytes + 20);
  Inode* inode = nullptr;
  Ext2InodeState* admitted = nullptr;
  {
    LockGuard<Mutex> registry(m_InodeStateLock);
    Ext2InodeState* state = m_InodeStates.lookup(number);
    LockGuard<Mutex> metadata(state ? state->writebackLock : m_InodeStateLock, state != nullptr);
#if THREADS || defined(STANDALONE_MUTEXES)
    LockGuard<Mutex> allocation(m_WriteLock);
#endif
    const auto status = validateHandleInodeLocked(number, generation, inode);
    if (status != FileHandleStatus::Success) {
      return status;
    }
    if (state && state->orphan) {
      return FileHandleStatus::Stale;
    }
    admitted = acquireInodeStateLocked(number, inode);
    if (!admitted) {
      return FileHandleStatus::NoMemory;
    }
  }

  // This temporary node owns the admission reference, including orphan block
  // retirement on a failed allocation racing the final unlink.
  Ext2Node admission(number, inode, this, *admitted);
  if (!admitted->allocationValid) {
    return FileHandleStatus::IoError;
  }
  Ext2File* file = new Ext2File(String(), number, inode, this);
  if (!file || !file->valid() || !VFS::instance().tryTrackFile(file)) {
    delete file;
    return FileHandleStatus::NoMemory;
  }
  result.adopt(file);
  return FileHandleStatus::Success;
}
