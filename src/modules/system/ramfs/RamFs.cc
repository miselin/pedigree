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

#include "RamFs.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/new"

#include "modules/Module.h"

String RamFs::m_VolumeLabel("ramfs");

RamFile::RamFile(const String& name, uintptr_t inode, Filesystem* pParentFS, File* pParent)
    : File(name, 0, 0, 0, inode, pParentFS, 0, pParent),
      m_FileBlocks(),
      m_FileBlocksLock(),
      m_nOwnerPid(0) {
  // Full permissions.
  setPermissions(0777);

#if THREADS
  m_nOwnerPid = Processor::information().getCurrentThread()->getParent()->getId();
#else
  m_nOwnerPid = 0;
#endif
}

RamFile::~RamFile() {
  truncate();
}

File::Attributes RamFile::getAttributes() const {
  LockGuard<Mutex> guard(m_FileBlocksLock);
  Attributes attributes = File::getAttributes();
  attributes.blocks = static_cast<uint64_t>(m_BlockOffsets.count()) * (getBlockSize() / 512);
  return attributes;
}

void RamFile::truncate() {
  resize(0);
}

bool RamFile::resizeFile(size_t size) {
  if (!canWrite()) {
    SYSCALL_ERROR(PermissionDenied);
    return false;
  }
  LockGuard<Mutex> guard(m_FileBlocksLock);
  const size_t oldSize = getSize();
  const size_t blockSize = getBlockSize();
  for (size_t i = 0; i < m_BlockOffsets.count();) {
    const uint64_t offset = m_BlockOffsets[i];
    const uintptr_t buffer = m_FileBlocks.lookup(offset);
    if (size < oldSize && offset >= size) {
      if (buffer) {
        m_FileBlocks.release(offset);
        m_FileBlocks.release(offset);
        m_FileBlocks.evict(offset);
      }
      m_BlockOffsets[i] = m_BlockOffsets[m_BlockOffsets.count() - 1];
      m_BlockOffsets.popBack();
      continue;
    }
    const size_t boundary = size < oldSize ? size : oldSize;
    if (buffer) {
      if (offset <= boundary && boundary - offset < blockSize) {
        const size_t within = boundary - offset;
        ByteSet(reinterpret_cast<void*>(buffer + within), 0, blockSize - within);
      }
      m_FileBlocks.release(offset);
    }
    ++i;
  }
  setSize(size);
  return true;
}

bool RamFile::canWrite() {
  RamFs* pParent = static_cast<RamFs*>(getFilesystem());
  if (!pParent->getProcessOwnership()) {
    return true;
  }

#if THREADS
  size_t pid = Processor::information().getCurrentThread()->getParent()->getId();
  return pid == m_nOwnerPid;
#else
  return true;
#endif
}

uintptr_t RamFile::readBlock(uint64_t location) {
  LockGuard<Mutex> guard(m_FileBlocksLock);
  uintptr_t buffer = m_FileBlocks.lookup(location);
  if (!buffer) {
    // Super trivial. But we are a ram filesystem... can't compact.
    bool didExist = false;
    buffer = m_FileBlocks.insert(location, &didExist);
    if (!buffer) {
      return 0;
    }
    if (!didExist) {
      ByteSet(reinterpret_cast<void*>(buffer), 0, getBlockSize());
      m_BlockOffsets.pushBack(location);
      m_FileBlocks.markNoLongerEditing(location);
    }
    buffer = m_FileBlocks.lookup(location);
  }
  return buffer;
}

bool RamFile::pinBlock(uint64_t location) {
  return m_FileBlocks.pin(location);
}

void RamFile::unpinBlock(uint64_t location) {
  m_FileBlocks.release(location);
}

RamDir::RamDir(const String& name, size_t inode, class Filesystem* pFs, File* pParent)
    : Directory(name, 0, 0, 0, inode, pFs, 0, pParent), m_DirectoryLock() {
  // Full permissions.
  setPermissions(0777);
}

RamDir::~RamDir() {};

bool RamDir::addEntry(String filename, File* pFile) {
  return addDirectoryEntry(filename, pFile);
}

bool RamDir::removeEntry(const String& filename, File* pFile) {
  LockGuard<Mutex> guard(m_DirectoryLock);
  if (!pFile->isDirectory() && !static_cast<RamFile*>(pFile)->canWrite())
    return false;

  return removeDirectoryEntry(filename, pFile);
}

bool RamDir::removeFromParent(RamDir* parent, const String& filename) {
  if (parent == this) {
    SYSCALL_ERROR(InvalidArgument);
    return false;
  }
  LockGuard<Mutex> namespaceGuard(namespaceMutationLock());
  LockGuard<Mutex> guard(m_DirectoryLock);
  bool empty = false;
  if (isEmpty(empty) != ReadStatus::Complete) {
    SYSCALL_ERROR(IoError);
    return false;
  }
  if (!empty) {
    SYSCALL_ERROR(NotEmpty);
    return false;
  }
  if (!parent->removeEntry(filename, this))
    return false;
  markDetached();
  return true;
}

RamFs::RamFs() : m_pRoot(0), m_bProcessOwners(false) {}

RamFs::~RamFs() {
  if (m_pRoot)
    delete m_pRoot;
}

bool RamFs::initialise(Disk* pDisk) {
  // Root directory with ./.. entries
  m_pRoot = new RamDir(String(""), 0, this, 0);
  return true;
}

bool RamFs::createFile(File* parent, const String& filename, uint32_t mask) {
  if (!parent->isDirectory())
    return false;

  File* f = new RamFile(filename, 0, this, parent);

  RamDir* p = static_cast<RamDir*>(parent);
  if (!p->addEntry(filename, f)) {
    delete f;
    return false;
  }
  return true;
}

bool RamFs::createDirectory(File* parent, const String& filename, uint32_t mask) {
  if (!parent->isDirectory())
    return false;

  RamDir* pDir = new RamDir(filename, 0, this, parent);

  RamDir* pParent = static_cast<RamDir*>(parent);
  if (!pParent->addEntry(filename, pDir)) {
    delete pDir;
    return false;
  }
  return true;
}

bool RamFs::createSymlink(File* parent, const String& filename, const String& value) {
  return false;
}

bool RamFs::removeNode(File* parent, const String& filename, File* file) {
  RamDir* p = static_cast<RamDir*>(parent);
  if (file->isDirectory()) {
    return static_cast<RamDir*>(file)->removeFromParent(p, filename);
  }
  return p->removeEntry(filename, file);
}

bool RamFs::renameNode(Directory*, const String&, File* source, Directory*, const String&,
                       File* replaced) {
  if (m_bProcessOwners &&
      ((!source->isDirectory() && !static_cast<RamFile*>(source)->canWrite()) ||
       (replaced && !replaced->isDirectory() && !static_cast<RamFile*>(replaced)->canWrite()))) {
    SYSCALL_ERROR(PermissionDenied);
    return false;
  }
  return true;
}

static bool entry() {
  return true;
}

static void destroy() {}

MODULE_INFO_NON_UNLOADABLE("ramfs", &entry, &destroy, "vfs");
