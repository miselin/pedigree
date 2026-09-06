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
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/new"

#include "modules/Module.h"
#include "modules/system/vfs/Symlink.h"

String RamFs::m_VolumeLabel("ramfs");

namespace {
class RamSymlink final : public Symlink {
 public:
  RamSymlink(const String& name, RamFs& filesystem, File* parent, const String& target)
      : Symlink(name, 0, 0, 0, 0, &filesystem, target.length(), parent), m_OwnerPid(0) {
    // A stored target is immutable and already loaded; generic link following
    // must never reload and trim its trailing pathname characters.
    m_sTarget = target;
#if THREADS
    m_OwnerPid = Processor::information().getCurrentThread()->getParent()->getId();
#endif
  }

  bool canWrite() {
    if (!static_cast<RamFs*>(getFilesystem())->getProcessOwnership())
      return true;
#if THREADS
    return Processor::information().getCurrentThread()->getParent()->getId() == m_OwnerPid;
#else
    return true;
#endif
  }

 protected:
  uint64_t readBytewise(uint64_t location, uint64_t size, uintptr_t buffer, bool) override {
    if (location >= m_sTarget.length())
      return 0;
    size_t amount = m_sTarget.length() - location;
    if (amount > size)
      amount = size;
    MemoryCopy(reinterpret_cast<void*>(buffer), m_sTarget.cstr() + location, amount);
    return amount;
  }

 private:
  size_t m_OwnerPid;
};

bool canModifyRamNode(File* file) {
  if (file->isDirectory())
    return true;
  if (file->isSymlink())
    return static_cast<RamSymlink*>(file)->canWrite();
  return static_cast<RamFile*>(file)->canWrite();
}

void initialiseCreatedNode(File& file, uint32_t mode) {
  // Creation attributes must be complete before another process can find the node.
  // VFS permissions place owner rights in the low bits, unlike Unix modes.
  uint32_t permissions = mode & FILE_AMASK;
  if (mode & 0400)
    permissions |= FILE_UR;
  if (mode & 0200)
    permissions |= FILE_UW;
  if (mode & 0100)
    permissions |= FILE_UX;
  if (mode & 0040)
    permissions |= FILE_GR;
  if (mode & 0020)
    permissions |= FILE_GW;
  if (mode & 0010)
    permissions |= FILE_GX;
  if (mode & 0004)
    permissions |= FILE_OR;
  if (mode & 0002)
    permissions |= FILE_OW;
  if (mode & 0001)
    permissions |= FILE_OX;
  file.setPermissions(permissions);
#if THREADS
  Thread* thread = Processor::information().getCurrentThread();
  if (thread) {
    FilesystemCredentials credentials;
    if (Process::currentFilesystemCredentials(credentials)) {
      file.setUid(credentials.uid);
      file.setGid(credentials.gid);
    }
  }
#endif
}
}  // namespace

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

XattrStatus RamFile::getExtendedAttribute(const StringView& name, void* buffer, size_t capacity,
                                          size_t& required) {
  return m_ExtendedAttributes.get(name, buffer, capacity, required);
}

XattrStatus RamFile::listExtendedAttributes(void* buffer, size_t capacity, size_t& required) {
  return m_ExtendedAttributes.list(buffer, capacity, required);
}

XattrStatus RamFile::setExtendedAttribute(const StringView& name, const void* value, size_t length,
                                          unsigned flags) {
  TerminationDeferral lifetime;
  if (!canWrite())
    return XattrStatus::Denied;
  const auto status = m_ExtendedAttributes.set(name, value, length, flags);
  if (status == XattrStatus::Success)
    setCreationTime(Time::getTime());
  return status;
}

XattrStatus RamFile::removeExtendedAttribute(const StringView& name) {
  TerminationDeferral lifetime;
  if (!canWrite())
    return XattrStatus::Denied;
  const auto status = m_ExtendedAttributes.remove(name);
  if (status == XattrStatus::Success)
    setCreationTime(Time::getTime());
  return status;
}

void RamFile::truncate() {
  resize(0);
}

class RamFile::ShrinkPlan : public File::PreparedShrink {
 public:
  ShrinkPlan(RamFile& file, size_t size)
      : file(file), size(size), boundary(size - size % file.getBlockSize()), tail(0) {}
  ~ShrinkPlan() override {
    if (tail)
      file.m_FileBlocks.release(boundary);
  }
  void commit() override {
    LockGuard<Mutex> guard(file.m_FileBlocksLock);
    discarded.get()->commit();
    const size_t cutoff = boundary + (size % file.getBlockSize() ? file.getBlockSize() : 0);
    for (size_t i = 0; i < file.m_BlockOffsets.count();) {
      if (file.m_BlockOffsets[i] >= cutoff) {
        file.m_BlockOffsets[i] = file.m_BlockOffsets[file.m_BlockOffsets.count() - 1];
        file.m_BlockOffsets.popBack();
      } else {
        ++i;
      }
    }
    if (tail)
      ByteSet(reinterpret_cast<void*>(tail + size - boundary), 0,
              file.getBlockSize() - (size - boundary));
    file.setSize(size);
  }
  RamFile& file;
  size_t size;
  size_t boundary;
  uintptr_t tail;
  UniquePointer<Cache::PreparedDiscard> discarded;
};

bool RamFile::prepareShrink(const ShrinkContext& context, UniquePointer<PreparedShrink>& prepared) {
  if (!canWrite()) {
    SYSCALL_ERROR(PermissionDenied);
    return false;
  }
  ShrinkPlan* plan = new ShrinkPlan(*this, context.newSize);
  UniquePointer<PreparedShrink> owner = UniquePointer<PreparedShrink>::adopt(plan);
  if (!plan) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  LockGuard<Mutex> guard(m_FileBlocksLock);
  const size_t blockSize = getBlockSize();
  const size_t cutoff = plan->boundary + (context.newSize % blockSize ? blockSize : 0);
  const auto status = m_FileBlocks.prepareDiscardFrom(cutoff, context.mappingLoans,
                                                      context.mappingLoanCount, plan->discarded);
  if (status != Cache::DiscardStatus::Ready) {
    syscallError(status == Cache::DiscardStatus::NoMemory  ? Error::OutOfMemory
                 : status == Cache::DiscardStatus::Busy    ? Error::DeviceBusy
                 : status == Cache::DiscardStatus::Invalid ? Error::InvalidArgument
                                                           : Error::IoError);
    return false;
  }
  if (context.newSize % blockSize)
    plan->tail = m_FileBlocks.lookup(plan->boundary);
  // The cache plan retains its rollback state until the generic mapping journal
  // has returned the suffix loans.
  prepared = pedigree_std::move(owner);
  return true;
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
    if (size <= oldSize && offset >= size) {
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

XattrStatus RamDir::getExtendedAttribute(const StringView& name, void* buffer, size_t capacity,
                                         size_t& required) {
  return m_ExtendedAttributes.get(name, buffer, capacity, required);
}

XattrStatus RamDir::listExtendedAttributes(void* buffer, size_t capacity, size_t& required) {
  return m_ExtendedAttributes.list(buffer, capacity, required);
}

XattrStatus RamDir::setExtendedAttribute(const StringView& name, const void* value, size_t length,
                                         unsigned flags) {
  TerminationDeferral lifetime;
  const auto status = m_ExtendedAttributes.set(name, value, length, flags);
  if (status == XattrStatus::Success)
    setCreationTime(Time::getTime());
  return status;
}

XattrStatus RamDir::removeExtendedAttribute(const StringView& name) {
  TerminationDeferral lifetime;
  const auto status = m_ExtendedAttributes.remove(name);
  if (status == XattrStatus::Success)
    setCreationTime(Time::getTime());
  return status;
}

bool RamDir::addEntry(String filename, File* pFile) {
  return addDirectoryEntry(filename, pFile);
}

bool RamDir::removeEntry(const String& filename, File* pFile) {
  LockGuard<Mutex> guard(m_DirectoryLock);
  if (!canModifyRamNode(pFile)) {
    SYSCALL_ERROR(PermissionDenied);
    return false;
  }

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

Filesystem::SyncStatus RamFs::sync() {
  // Files and shared mappings already modify the authoritative memory pages.
  return SyncStatus::Success;
}

bool RamFs::initialise(Disk* pDisk) {
  // Root directory with ./.. entries
  m_pRoot = new RamDir(String(""), 0, this, 0);
  return true;
}

bool RamFs::createFile(File* parent, const String& filename, uint32_t mask) {
  if (!parent->isDirectory()) {
    SYSCALL_ERROR(NotADirectory);
    return false;
  }

  File* f = new RamFile(filename, 0, this, parent);
  if (!f) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  initialiseCreatedNode(*f, mask);

  RamDir* p = static_cast<RamDir*>(parent);
  if (!p->addEntry(filename, f)) {
    delete f;
    return false;
  }
  return true;
}

bool RamFs::createDirectory(File* parent, const String& filename, uint32_t mask) {
  if (!parent->isDirectory()) {
    SYSCALL_ERROR(NotADirectory);
    return false;
  }

  RamDir* pDir = new RamDir(filename, 0, this, parent);
  if (!pDir) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  initialiseCreatedNode(*pDir, mask);

  RamDir* pParent = static_cast<RamDir*>(parent);
  if (!pParent->addEntry(filename, pDir)) {
    delete pDir;
    return false;
  }
  return true;
}

bool RamFs::createSymlink(File* parent, const String& filename, const String& value) {
  if (!parent->isDirectory()) {
    SYSCALL_ERROR(NotADirectory);
    return false;
  }
  if (!value.length()) {
    SYSCALL_ERROR(DoesNotExist);
    return false;
  }
  auto* link = new RamSymlink(filename, *this, parent, value);
  if (!link) {
    SYSCALL_ERROR(OutOfMemory);
    return false;
  }
  initialiseCreatedNode(*link, 0777);
  if (!static_cast<RamDir*>(parent)->addEntry(filename, link)) {
    delete link;
    return false;
  }
  return true;
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
      (!canModifyRamNode(source) || (replaced && !canModifyRamNode(replaced)))) {
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
