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

#include "Ext2Node.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Ext2Filesystem.h"
#include "ext2.h"
#include "modules/system/vfs/File.h"

Ext2InodeState::Ext2InodeState(Inode* pInode, Ext2Filesystem* filesystem)
    : blocks(),
      metadataBlocks(0),
      allocatedDataBlocks(0),
      size(LITTLE_TO_HOST32(pInode->i_size)),
      dataLock(),
      writeLock(),
      writebackLock(),
      pageLoans(0),
      references(1),
      orphan(false),
      futexIdentity(0),
      files(),
      metadata(pInode),
      filesystem(filesystem) {
  reloadMappings(pInode, filesystem);
}

Ext2InodeState::~Ext2InodeState() {
  delete cache;
}

void Ext2InodeState::reloadMappings(Inode* pInode, Ext2Filesystem* filesystem) {
  assert(blocks.count() == 0);
  size = LITTLE_TO_HOST32(pInode->i_size);
  const size_t blockSize = filesystem->m_BlockSize;
  uint32_t totalBlocks = 0;
  bool inlineSymlink = false;
  allocationValid = Ext2Node::decodeAllocation(*pInode, blockSize, totalBlocks, inlineSymlink);
  if (!allocationValid) {
    metadataBlocks = allocatedDataBlocks = 0;
    return;
  }
  size_t dataBlockCount = 0;
  if (!inlineSymlink) {
    dataBlockCount = size / blockSize;
    if (size % blockSize) {
      ++dataBlockCount;
    }
  }

  blocks.reserve(dataBlockCount, false);
  metadataBlocks = 0;

  for (size_t i = 0; i < 12 && i < dataBlockCount; i++)
    blocks.pushBack(LITTLE_TO_HOST32(pInode->i_block[i]));

  // We'll read these later.
  for (size_t i = 12; i < dataBlockCount; ++i) {
    blocks.pushBack(~0);
  }
  if (!inlineSymlink) {
    for (size_t i = dataBlockCount; i < 12; ++i) {
      const uint32_t block = LITTLE_TO_HOST32(pInode->i_block[i]);
      if (block) {
        while (blocks.count() <= i) {
          blocks.pushBack(0);
        }
        blocks[i] = block;
      }
    }
    const size_t entries = blockSize / sizeof(uint32_t);
    loadMappings(filesystem, LITTLE_TO_HOST32(pInode->i_block[12]), 1, 12, entries);
    loadMappings(filesystem, LITTLE_TO_HOST32(pInode->i_block[13]), 2, 12 + entries,
                 entries * entries);
    loadMappings(filesystem, LITTLE_TO_HOST32(pInode->i_block[14]), 3,
                 12 + entries + entries * entries, entries * entries * entries);
  }
  allocationValid = totalBlocks >= metadataBlocks;
  allocatedDataBlocks = allocationValid ? totalBlocks - metadataBlocks : 0;
}

void Ext2InodeState::loadMappings(Ext2Filesystem* filesystem, uint32_t block, unsigned depth,
                                  size_t first, size_t span) {
  if (!block) {
    const size_t end = first + span < blocks.count() ? first + span : blocks.count();
    for (size_t i = first; i < end; ++i) {
      blocks[i] = 0;
    }
    return;
  }
  ++metadataBlocks;
  const DiskReadView buffer = filesystem->readBlockView(block);
  if (!buffer) {
    return;
  }
  const size_t entries = filesystem->m_BlockSize / sizeof(uint32_t);
  for (size_t i = 0; i < entries; ++i) {
    uint32_t child;
    if (!buffer.readAt(child, i * sizeof(child)))
      return;
    child = LITTLE_TO_HOST32(child);
    if (depth > 1) {
      loadMappings(filesystem, child, depth - 1, first + i * (span / entries), span / entries);
    } else if (child || first + i < blocks.count()) {
      while (blocks.count() <= first + i) {
        blocks.pushBack(0);
      }
      blocks[first + i] = child;
    }
  }
}

Ext2Node::Ext2Node(uintptr_t inode_num, Inode* pInode, Ext2Filesystem* pFs)
    : m_State(pFs->acquireInodeState(inode_num, pInode)),
      m_pInode(pInode),
      m_InodeNumber(inode_num),
      m_pExt2Fs(pFs),
      m_Blocks(m_State->blocks),
      m_nMetadataBlocks(m_State->metadataBlocks),
      m_nSize(m_State->size) {}

Ext2Node::Ext2Node(uintptr_t inode, Inode* metadata, Ext2Filesystem* filesystem,
                   Ext2InodeState& admitted)
    : m_State(&admitted),
      m_pInode(metadata),
      m_InodeNumber(inode),
      m_pExt2Fs(filesystem),
      m_Blocks(admitted.blocks),
      m_nMetadataBlocks(admitted.metadataBlocks),
      m_nSize(admitted.size) {}

Ext2Node::~Ext2Node() {
  m_pExt2Fs->releaseInodeState(m_InodeNumber, m_State, this);
}

uintptr_t Ext2Node::readBlock(uint64_t location) {
  if (!m_State->allocationValid) {
    SYSCALL_ERROR(IoError);
    return 0;
  }
  // Sanity check.
  uint32_t nBlock = location / m_pExt2Fs->m_BlockSize;
  if (nBlock >= m_Blocks.count()) {
    ERROR("Ext2Node::readBlock beyond blocks [" << nBlock << ", " << m_Blocks.count() << "]");
    return 0;
  }
  if (location >= m_nSize) {
    ERROR("Ext2Node::readBlock beyond size [" << location << ", " << m_nSize << "]");
    return 0;
  }

  if (!ensureBlockLoaded(nBlock)) {
    return 0;
  }
  uintptr_t result = m_pExt2Fs->readBlock(m_Blocks[nBlock]);
  if (!result) {
    return 0;
  }

  // Add any remaining offset we chopped off.
  result += location % m_pExt2Fs->m_BlockSize;
  return result;
}

void Ext2Node::writeBlock(uint64_t location) {
  // Sanity check.
  uint32_t nBlock = location / m_pExt2Fs->m_BlockSize;
  if (nBlock >= m_Blocks.count())
    return;
  if (location >= m_nSize)
    return;

  // Update on disk.
  if (!ensureBlockLoaded(nBlock)) {
    return;
  }
  m_pExt2Fs->writeBlock(m_Blocks[nBlock]);
}

void Ext2Node::trackBlock(uint32_t block, bool writeInode) {
  m_Blocks.pushBack(block);
  if (block) {
    ++m_State->allocatedDataBlocks;
  }

  updateAllocatedSectorCount();

  if (writeInode) {
    // Write updated inode.
    m_pExt2Fs->writeInode(getInodeNumber());
  }
}

bool Ext2Node::wipe(bool allocationLockHeld) {
  if (!m_State->allocationValid) {
    SYSCALL_ERROR(IoError);
    return false;
  }
  if (isInlineSymlink()) {
    ByteSet(m_pInode->i_block, 0, sizeof(m_pInode->i_block));
    m_nSize = 0;
    m_pInode->i_size = 0;
    m_pExt2Fs->writeInode(getInodeNumber());
    return true;
  }
  if (!trimToBlocks(0, allocationLockHeld)) {
    return false;
  }
  m_nSize = 0;
  m_pInode->i_size = 0;
  m_pExt2Fs->writeInode(getInodeNumber());
  return true;
}

void Ext2Node::extend(size_t newSize) {
  ensureLargeEnough(newSize, 0, 0);
}

void Ext2Node::extend(size_t newSize, uint64_t location, uint64_t size) {
  ensureLargeEnough(newSize, location, size);
}

uint64_t Ext2Node::maximumFileSize() const {
  const uint64_t blockSize = m_pExt2Fs->m_BlockSize;
  const uint64_t entries = blockSize / sizeof(uint32_t);
  const uint64_t maximumBlocks = 12 + entries + entries * entries;
  constexpr uint64_t InodeSizeLimit = 0xffffffffULL;
  return maximumBlocks > InodeSizeLimit / blockSize ? InodeSizeLimit : maximumBlocks * blockSize;
}

bool Ext2Node::ensureLargeEnough(size_t size, uint64_t location, uint64_t opsize, bool onlyBlocks,
                                 bool nozeroblocks) {
  if (!m_State->allocationValid) {
    SYSCALL_ERROR(IoError);
    return false;
  }
  const size_t blockSize = m_pExt2Fs->m_BlockSize;
  if (size > maximumFileSize()) {
    SYSCALL_ERROR(FileTooLarge);
    return false;
  }
  const size_t oldBlocks = m_Blocks.count();
  const size_t currentMaxSize = oldBlocks * blockSize;
  if (size <= currentMaxSize) {
    if (size > m_nSize && !onlyBlocks) {
      if (!zeroRange(m_nSize, size)) {
        return false;
      }
      m_nSize = size;
      m_pInode->i_size = HOST_TO_LITTLE32(size);
      m_pExt2Fs->writeInode(getInodeNumber());
    }
    return true;
  }

  const size_t delta = size - currentMaxSize;
  const size_t deltaBlocks = delta / blockSize + (delta % blockSize != 0);
  Vector<uint32_t> newBlocks;
  if (!m_pExt2Fs->findFreeBlocks(m_InodeNumber, deltaBlocks, newBlocks)) {
    return false;
  }
  Vector<uint32_t> pendingWrites;
  bool success = true;
  size_t attached = 0;
  for (uint32_t block : newBlocks) {
    if (!addBlock(block, &pendingWrites)) {
      success = false;
      break;
    }
    ++attached;
    if (nozeroblocks) {
      continue;
    }
    if (!m_pExt2Fs->m_pDisk->zero(static_cast<uint64_t>(block) * blockSize, blockSize)) {
      SYSCALL_ERROR(IoError);
      success = false;
      break;
    }
  }
  for (uint32_t block : pendingWrites) {
    m_pExt2Fs->writeBlock(block);
    m_pExt2Fs->unpinBlock(block);
  }
  if (success && !onlyBlocks && m_nSize < currentMaxSize) {
    success = zeroRange(m_nSize, currentMaxSize);
  }
  if (!success) {
    for (size_t i = attached; i < newBlocks.count(); ++i) {
      m_pExt2Fs->releaseBlock(newBlocks[i], m_InodeNumber);
    }
    if (!trimToBlocks(oldBlocks)) {
      ERROR("Ext2: unable to retire partially allocated extension blocks");
    }
    return false;
  }
  if (!onlyBlocks) {
    m_nSize = size;
    m_pInode->i_size = HOST_TO_LITTLE32(size);
  }
  m_pExt2Fs->writeInode(getInodeNumber());
  return true;
}

bool Ext2Node::ensureBlockLoaded(size_t nBlock) {
  if (nBlock >= m_Blocks.count()) {
    FATAL("EXT2: ensureBlockLoaded: Algorithmic error [block " << nBlock << " > "
                                                               << m_Blocks.count() << "].");
  }
  if (m_Blocks[nBlock] == ~0U) {
    return getBlockNumber(nBlock);
  }

  return true;
}

bool Ext2Node::getBlockNumber(size_t nBlock) {
  size_t nPerBlock = m_pExt2Fs->m_BlockSize / 4;

  assert(nBlock >= 12);

  if (nBlock < nPerBlock + 12) {
    return getBlockNumberIndirect(LITTLE_TO_HOST32(m_pInode->i_block[12]), 12, nBlock);
  }

  if (nBlock < (nPerBlock * nPerBlock) + nPerBlock + 12) {
    return getBlockNumberBiindirect(LITTLE_TO_HOST32(m_pInode->i_block[13]), nPerBlock + 12,
                                    nBlock);
  }

  return getBlockNumberTriindirect(LITTLE_TO_HOST32(m_pInode->i_block[14]),
                                   (nPerBlock * nPerBlock) + nPerBlock + 12, nBlock);
}

bool Ext2Node::getBlockNumberIndirect(uint32_t inode_block, size_t nBlocks, size_t nBlock) {
  if (!inode_block) {
    m_Blocks[nBlock] = 0;
    return true;
  }
  const DiskReadView buffer = m_pExt2Fs->readBlockView(inode_block);
  if (!buffer) {
    return false;
  }

  for (size_t i = 0; i < m_pExt2Fs->m_BlockSize / 4 && nBlocks < m_Blocks.count(); i++) {
    uint32_t block;
    if (!buffer.readAt(block, i * sizeof(block)))
      return false;
    m_Blocks[nBlocks++] = LITTLE_TO_HOST32(block);
  }

  return true;
}

bool Ext2Node::getBlockNumberBiindirect(uint32_t inode_block, size_t nBlocks, size_t nBlock) {
  size_t nPerBlock = m_pExt2Fs->m_BlockSize / 4;

  if (!inode_block) {
    m_Blocks[nBlock] = 0;
    return true;
  }
  DiskReadView buffer = m_pExt2Fs->readBlockView(inode_block);
  if (!buffer) {
    return false;
  }

  // What indirect block does nBlock exist on?
  size_t nIndirectBlock = (nBlock - nBlocks) / nPerBlock;

  uint32_t indirectBlock;
  if (!buffer.readAt(indirectBlock, nIndirectBlock * sizeof(indirectBlock)))
    return false;
  buffer.reset();
  return getBlockNumberIndirect(LITTLE_TO_HOST32(indirectBlock),
                                nBlocks + nIndirectBlock * nPerBlock, nBlock);
}

bool Ext2Node::getBlockNumberTriindirect(uint32_t inode_block, size_t nBlocks, size_t nBlock) {
  size_t nPerBlock = m_pExt2Fs->m_BlockSize / 4;

  if (!inode_block) {
    m_Blocks[nBlock] = 0;
    return true;
  }
  DiskReadView buffer = m_pExt2Fs->readBlockView(inode_block);
  if (!buffer) {
    return false;
  }

  // What biindirect block does nBlock exist on?
  size_t nBiBlock = (nBlock - nBlocks) / (nPerBlock * nPerBlock);

  uint32_t biBlock;
  if (!buffer.readAt(biBlock, nBiBlock * sizeof(biBlock)))
    return false;
  buffer.reset();
  return getBlockNumberBiindirect(LITTLE_TO_HOST32(biBlock),
                                  nBlocks + nBiBlock * nPerBlock * nPerBlock, nBlock);
}

void Ext2Node::writeBlockOrQueue(uint32_t block, Vector<uint32_t>* pendingWrites) {
  if (!pendingWrites) {
    m_pExt2Fs->writeBlock(block);
    return;
  }

  for (auto pending : *pendingWrites) {
    if (pending == block) {
      return;
    }
  }
  // Keep the page resident until the batched write is submitted. The normal
  // caller reference is released immediately after addBlock(), so without
  // this pin a memory-pressure eviction could discard a deferred update.
  if (!m_pExt2Fs->pinBlock(block)) {
    // Fall back to the original write-through behaviour if the device cannot
    // acquire a second reference to the mapping page.
    m_pExt2Fs->writeBlock(block);
    return;
  }
  pendingWrites->pushBack(block);
}

bool Ext2Node::setBlockNumber(size_t blockNum, uint32_t blockValue,
                              Vector<uint32_t>* pendingWrites) {
  if (blockNum < 12) {
    m_pInode->i_block[blockNum] = HOST_TO_LITTLE32(blockValue);
    return true;
  }

  const size_t entries = m_pExt2Fs->m_BlockSize / sizeof(uint32_t);
  size_t index = blockNum - 12;
  const unsigned depth = index < entries ? 1 : 2;
  if (depth == 2) {
    index -= entries;
    if (index >= entries * entries) {
      SYSCALL_ERROR(FileTooLarge);
      return false;
    }
  }
  const size_t indices[2] = {depth == 1 ? index : index / entries, index % entries};
  const size_t inodeIndex = depth == 1 ? 12 : 13;
  uint32_t numbers[2] = {};
  uintptr_t buffers[2] = {};
  bool allocated[2] = {};
  uint32_t next = LITTLE_TO_HOST32(m_pInode->i_block[inodeIndex]);
  bool ready = true;
  for (unsigned level = 0; level < depth; ++level) {
    numbers[level] = next;
    if (!next) {
      numbers[level] = m_pExt2Fs->findFreeBlock(m_InodeNumber);
      allocated[level] = numbers[level] != 0;
      if (!allocated[level]) {
        ready = false;
        break;
      }
    }
    buffers[level] = m_pExt2Fs->readBlock(numbers[level]);
    if (!buffers[level]) {
      SYSCALL_ERROR(IoError);
      ready = false;
      break;
    }
    if (allocated[level]) {
      ByteSet(reinterpret_cast<void*>(buffers[level]), 0, m_pExt2Fs->m_BlockSize);
    }
    next = LITTLE_TO_HOST32(reinterpret_cast<uint32_t*>(buffers[level])[indices[level]]);
  }
  if (ready) {
    // All pages and allocations exist before any old mapping is changed.
    for (unsigned level = depth; level-- > 0;) {
      uint32_t* table = reinterpret_cast<uint32_t*>(buffers[level]);
      table[indices[level]] =
          HOST_TO_LITTLE32(level + 1 == depth ? blockValue : numbers[level + 1]);
      writeBlockOrQueue(numbers[level], pendingWrites);
      if (allocated[level]) {
        ++m_nMetadataBlocks;
      }
    }
    m_pInode->i_block[inodeIndex] = HOST_TO_LITTLE32(numbers[0]);
  }
  for (unsigned level = 0; level < depth; ++level) {
    if (buffers[level]) {
      m_pExt2Fs->unpinBlock(numbers[level]);
    }
    if (!ready && allocated[level]) {
      m_pExt2Fs->releaseBlock(numbers[level], m_InodeNumber);
    }
  }
  return ready;
}

bool Ext2Node::addBlock(uint32_t blockValue, Vector<uint32_t>* pendingWrites) {
  if (!setBlockNumber(m_Blocks.count(), blockValue, pendingWrites)) {
    return false;
  }
  trackBlock(blockValue, !pendingWrites);
  return true;
}

bool Ext2Node::ensureWritableRange(size_t location, size_t length) {
  if (!m_State->allocationValid) {
    SYSCALL_ERROR(IoError);
    return false;
  }
  if (!length) {
    return true;
  }
  const size_t blockSize = m_pExt2Fs->m_BlockSize;
  const size_t first = location / blockSize;
  const size_t last = (location + length - 1) / blockSize;
  for (size_t index = first; index <= last; ++index) {
    if (index >= m_Blocks.count() || !ensureBlockLoaded(index)) {
      SYSCALL_ERROR(IoError);
      return false;
    }
    if (m_Blocks[index]) {
      continue;
    }
    const uint32_t block = m_pExt2Fs->findFreeBlock(m_InodeNumber);
    if (!block) {
      return false;
    }
    if (!m_pExt2Fs->m_pDisk->zero(static_cast<uint64_t>(block) * blockSize, blockSize)) {
      m_pExt2Fs->releaseBlock(block, m_InodeNumber);
      SYSCALL_ERROR(IoError);
      return false;
    }
    if (!setBlockNumber(index, block)) {
      m_pExt2Fs->releaseBlock(block, m_InodeNumber);
      return false;
    }
    m_Blocks[index] = block;
    ++m_State->allocatedDataBlocks;
    updateAllocatedSectorCount();
    m_pExt2Fs->writeInode(getInodeNumber());
  }
  return true;
}

void Ext2Node::fileAttributeChanged(size_t size, size_t atime, size_t mtime, size_t ctime) {
  m_pInode->i_size = HOST_TO_LITTLE32(size);  /// \todo 4GB files.
  m_pInode->i_atime = HOST_TO_LITTLE32(atime);
  m_pInode->i_mtime = HOST_TO_LITTLE32(mtime);
  m_pInode->i_ctime = HOST_TO_LITTLE32(ctime);

  // Update our internal record of the file size accordingly.
  m_nSize = size;

  // Write updated inode.
  m_pExt2Fs->writeInode(getInodeNumber());
}

File::Attributes Ext2Node::inodeAttributes() const {
  LockGuard<Mutex> guard(m_State->writebackLock);
  File::Attributes attributes;
  attributes.accessed = LITTLE_TO_HOST32(m_pInode->i_atime);
  attributes.modified = LITTLE_TO_HOST32(m_pInode->i_mtime);
  attributes.changed = LITTLE_TO_HOST32(m_pInode->i_ctime);
  attributes.uid = Ext2Owner::uid(*m_pInode);
  attributes.gid = Ext2Owner::gid(*m_pInode);
  attributes.permissions = modeToPermissions(LITTLE_TO_HOST16(m_pInode->i_mode));
  attributes.size = LITTLE_TO_HOST32(m_pInode->i_size);
  attributes.links = LITTLE_TO_HOST16(m_pInode->i_links_count);
  attributes.blocks = LITTLE_TO_HOST32(m_pInode->i_blocks);
  return attributes;
}

void Ext2Node::updateInodeAttributes(const File::Attributes& attributes, uint32_t mask) {
  if (mask & (File::Owner | File::Group)) {
    if (!changeInodeOwnership(attributes.uid, attributes.gid, mask & File::Owner,
                              mask & File::Group))
      return;
    mask &= ~(File::Owner | File::Group);
    if (!mask)
      return;
  }
  LockGuard<Mutex> guard(m_State->writebackLock);
  if (mask & File::AccessTime) {
    m_pInode->i_atime = HOST_TO_LITTLE32(attributes.accessed);
  }
  if (mask & File::ModifyTime) {
    m_pInode->i_mtime = HOST_TO_LITTLE32(attributes.modified);
  }
  m_pInode->i_ctime =
      HOST_TO_LITTLE32((mask & File::ChangeTime) ? attributes.changed : Time::getTime());
  if (mask & File::Permissions) {
    const uint16_t mode = LITTLE_TO_HOST16(m_pInode->i_mode);
    m_pInode->i_mode =
        HOST_TO_LITTLE16((mode & ~01777U) | permissionsToMode(attributes.permissions));
  }
  m_pExt2Fs->writeInode(getInodeNumber());
}

bool Ext2Node::sync(size_t offset, bool async) {
  const size_t nBlock = offset / m_pExt2Fs->m_BlockSize;
  if (offset >= m_nSize) {
    return true;
  }
  if (nBlock >= m_Blocks.count() || !ensureBlockLoaded(nBlock)) {
    return false;
  }
  return m_pExt2Fs->syncBlock(m_Blocks[nBlock], async);
}

bool Ext2Node::pinBlock(uint64_t location) {
  uint32_t nBlock = location / m_pExt2Fs->m_BlockSize;
  if (nBlock >= m_Blocks.count())
    return false;
  if (location >= m_nSize)
    return false;

  if (!ensureBlockLoaded(nBlock))
    return false;
  if (!m_Blocks[nBlock])
    return true;
  return m_pExt2Fs->pinBlock(m_Blocks[nBlock]);
}

void Ext2Node::unpinBlock(uint64_t location) {
  uint32_t nBlock = location / m_pExt2Fs->m_BlockSize;
  if (nBlock >= m_Blocks.count())
    return;
  if (location >= m_nSize)
    return;

  if (!ensureBlockLoaded(nBlock))
    return;
  if (!m_Blocks[nBlock])
    return;
  m_pExt2Fs->unpinBlock(m_Blocks[nBlock]);
}

uint32_t Ext2Node::modeToPermissions(uint32_t mode) const {
  uint32_t permissions = 0;
  if (mode & 01000)
    permissions |= FILE_STICKY;
  if (mode & EXT2_S_IRUSR)
    permissions |= FILE_UR;
  if (mode & EXT2_S_IWUSR)
    permissions |= FILE_UW;
  if (mode & EXT2_S_IXUSR)
    permissions |= FILE_UX;
  if (mode & EXT2_S_IRGRP)
    permissions |= FILE_GR;
  if (mode & EXT2_S_IWGRP)
    permissions |= FILE_GW;
  if (mode & EXT2_S_IXGRP)
    permissions |= FILE_GX;
  if (mode & EXT2_S_IROTH)
    permissions |= FILE_OR;
  if (mode & EXT2_S_IWOTH)
    permissions |= FILE_OW;
  if (mode & EXT2_S_IXOTH)
    permissions |= FILE_OX;
  return permissions;
}

uint32_t Ext2Node::permissionsToMode(uint32_t permissions) const {
  uint32_t mode = 0;
  if (permissions & FILE_STICKY)
    mode |= 01000;
  if (permissions & FILE_UR)
    mode |= EXT2_S_IRUSR;
  if (permissions & FILE_UW)
    mode |= EXT2_S_IWUSR;
  if (permissions & FILE_UX)
    mode |= EXT2_S_IXUSR;
  if (permissions & FILE_GR)
    mode |= EXT2_S_IRGRP;
  if (permissions & FILE_GW)
    mode |= EXT2_S_IWGRP;
  if (permissions & FILE_GX)
    mode |= EXT2_S_IXGRP;
  if (permissions & FILE_OR)
    mode |= EXT2_S_IROTH;
  if (permissions & FILE_OW)
    mode |= EXT2_S_IWOTH;
  if (permissions & FILE_OX)
    mode |= EXT2_S_IXOTH;
  return mode;
}
