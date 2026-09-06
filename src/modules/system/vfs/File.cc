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

#include "File.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/Pair.h"
#include "pedigree/kernel/utilities/Result.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Filesystem.h"
#include "MemoryMappedFile.h"
#include "VFS.h"

uintptr_t File::futexIdentity() {
  uintptr_t identity = __atomic_load_n(&m_FutexIdentity, __ATOMIC_ACQUIRE);
  if (!identity) {
    static uintptr_t nextIdentity = 0;
    const uintptr_t allocated = __atomic_add_fetch(&nextIdentity, uintptr_t(1), __ATOMIC_RELAXED);
    // Futex keys tag these tokens. Never let wraparound alias a retired file.
    assert(allocated && allocated <= (~uintptr_t(0) >> 1));
    __atomic_compare_exchange_n(&m_FutexIdentity, &identity, allocated, false, __ATOMIC_ACQ_REL,
                                __ATOMIC_ACQUIRE);
    if (!identity) {
      identity = allocated;
    }
  }
  return identity;
}

File::ParentLease::ParentLease()
    : m_Parent(nullptr),
      m_Retained(false)
#if THREADS && !defined(STANDALONE_MUTEXES)
      ,
      m_TerminationDeferral(true)
#endif
{
}

File::ParentLease::~ParentLease() {
  if (m_Retained) {
    VFS::instance().untrackFile(m_Parent);
  }
}

void File::ParentLease::swap(ParentLease& other) {
  File* parent = m_Parent;
  bool retained = m_Retained;
  m_Parent = other.m_Parent;
  m_Retained = other.m_Retained;
  other.m_Parent = parent;
  other.m_Retained = retained;
}

bool File::writeCallback(CacheConstants::CallbackCause cause, uintptr_t loc, uintptr_t page,
                         void* meta) {
  File* pFile = reinterpret_cast<File*>(meta);

  switch (cause) {
    case CacheConstants::WriteBack: {
      pFile->writeBlocks(loc, page, PhysicalMemoryManager::getPageSize());
    } break;
    case CacheConstants::Eviction:
      // Remove this page from our data cache.
      /// \todo handle block size < 4K??
      pFile->setCachedPage(loc / pFile->getBlockSize(), FILE_BAD_BLOCK);
      break;
    default:
      WARNING(
          "File: unknown cache callback -- could indicate potential "
          "future I/O issues.");
      break;
  }
  return true;
}

bool File::fillCacheCallback(CacheConstants::CallbackCause cause, uintptr_t loc, uintptr_t page,
                             void* meta) {
  File* pFile = reinterpret_cast<File*>(meta);
  if (cause == CacheConstants::WriteBack) {
    pFile->writeBlocks(loc, page, PhysicalMemoryManager::getPageSize());
  } else if (cause == CacheConstants::Eviction) {
    pFile->setCachedPage(loc / PhysicalMemoryManager::getPageSize(), FILE_BAD_BLOCK);
  } else if (cause != CacheConstants::Eviction) {
    WARNING("File: unknown fill-cache callback cause.");
  }
  return true;
}

File::CacheState::CacheState() : data(FILE_BAD_BLOCK), indexLock(), fill(), fillLock() {}

File::CacheState& File::cacheState() {
  return m_CacheState;
}

File::File()
    : m_Name(),
      m_AccessedTime(0),
      m_ModifiedTime(0),
      m_CreationTime(0),
      m_Inode(0),
      m_pFilesystem(0),
      m_Size(0),
      m_pParent(0),
      m_MetadataLock(),
      m_pDetachedParent(0),
      m_bDetachedParentHandled(false),
      m_nWriters(0),
      m_nReaders(0),
      m_Uid(0),
      m_Gid(0),
      m_Permissions(0),
      m_CacheState(),
      m_bDirect(false),
      m_WriteLock(),
      m_Lock(),
      m_MonitorTargets() {}

File::File(const String& name, Time::Timestamp accessedTime, Time::Timestamp modifiedTime,
           Time::Timestamp creationTime, uintptr_t inode, Filesystem* pFs, size_t size,
           File* pParent)
    : m_Name(name),
      m_AccessedTime(accessedTime),
      m_ModifiedTime(modifiedTime),
      m_CreationTime(creationTime),
      m_Inode(inode),
      m_pFilesystem(pFs),
      m_Size(size),
      m_pParent(pParent),
      m_MetadataLock(),
      m_pDetachedParent(0),
      m_bDetachedParentHandled(false),
      m_nWriters(0),
      m_nReaders(0),
      m_Uid(0),
      m_Gid(0),
      m_Permissions(0),
      m_CacheState(),
      m_bDirect(false),
      m_WriteLock(),
      m_Lock(),
      m_MonitorTargets() {
  size_t maxBlock = size / getBlockSize();
  if (size % getBlockSize()) {
    ++maxBlock;
  }

  // Prefill the block cache with bad pages to avoid continuous resizes.
  setCachedPage(maxBlock, FILE_BAD_BLOCK);
}

File::~File() {
  closeFileEvents();
  closeReadiness();

  {
    LockGuard<Mutex> guard(m_Lock);
    for (auto target : m_MonitorTargets) {
      delete target;
    }
    m_MonitorTargets.clear();
  }

  // Releasing the parent can run its destructor, so no File lock may be held.
  if (m_pDetachedParent) {
    VFS::instance().untrackFile(m_pDetachedParent);
  }
}

uint64_t File::read(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock) {
  if (isBytewise()) {
    // Have to perform bytewise reads
    /// \todo consider caching this still
    return readBytewise(location, size, buffer, bCanBlock);
  }

  LockGuard<Mutex> guard(dataMutationLock());

  const size_t fileSize = getSize();
  if (!size || location >= fileSize) {
    return 0;
  }

  const uint64_t remaining = fileSize - location;
  if (size > remaining) {
    size = remaining;
  }

  const size_t blockSize = useFillCache() ? PhysicalMemoryManager::getPageSize() : getBlockSize();

  size_t n = 0;
  while (size) {
    if (location >= fileSize)
      return n;

    uintptr_t block = location / blockSize;
    uintptr_t offs = location % blockSize;
    uintptr_t sz = (size + offs > blockSize) ? blockSize - offs : size;

    // Handle a possible early EOF.
    if (sz > (fileSize - location))
      sz = fileSize - location;

    uintptr_t buff = readIntoCache(block);
    if (buff == FILE_BAD_BLOCK) {
      ERROR("File::read - failed to get page from cache, returning early");
      return n;
    }

    if (buffer) {
      ForwardMemoryCopy(reinterpret_cast<void*>(buffer), reinterpret_cast<void*>(buff + offs), sz);
      buffer += sz;
    }
    releaseReadReference(block);

    location += sz;
    size -= sz;
    n += sz;
  }
  return n;
}

uint64_t File::write(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock) {
  WriteGuard guard = lockWrites();
  return guard.write(location, size, buffer, bCanBlock);
}

uint64_t File::writeUnlocked(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock) {
  if (!size || location > (~static_cast<uint64_t>(0) - size)) {
    return 0;
  }

  const uint64_t endLocation = location + size;
  if (endLocation > static_cast<uint64_t>(~static_cast<size_t>(0))) {
    return 0;
  }

  if (isBytewise()) {
    // Have to perform bytewise reads
    /// \todo consider caching this still
    return writeBytewise(location, size, buffer, bCanBlock);
  }

  const size_t filesystemBlockSize = getBlockSize();
  const size_t blockSize =
      useFillCache() ? PhysicalMemoryManager::getPageSize() : filesystemBlockSize;

  if (!prepareWrite(location, size)) {
    return 0;
  }

  size_t n = 0;
  while (size) {
    uintptr_t block = location / blockSize;
    uintptr_t offs = location % blockSize;
    uintptr_t sz = (size + offs > blockSize) ? blockSize - offs : size;

    uintptr_t buff = readIntoCache(block);
    if (buff == FILE_BAD_BLOCK) {
      ERROR("File::read - failed to get page from cache, returning early");
      return n;
    }

    ForwardMemoryCopy(reinterpret_cast<void*>(buff + offs), reinterpret_cast<void*>(buffer), sz);

    // Trigger an immediate write-back - write-through cache.
    if (useFillCache()) {
      const uint64_t pageOffset = block * blockSize;
      const size_t firstBlock = offs / filesystemBlockSize;
      const size_t endBlock = (offs + sz + filesystemBlockSize - 1) / filesystemBlockSize;
      const size_t fileBlockOffset = firstBlock * filesystemBlockSize;
      writeBlocks(pageOffset + fileBlockOffset, buff + fileBlockOffset,
                  (endBlock - firstBlock) * filesystemBlockSize);
    } else {
      writeBlock(block * blockSize, buff);
    }
    releaseReadReference(block);

    location += sz;
    buffer += sz;
    size -= sz;
    n += sz;
  }

  if (location >= getSize()) {
    m_Size = location;
    fileAttributeChanged();
  }
  return n;
}

uint64_t File::append(uint64_t size, uintptr_t buffer, uint64_t& location, bool bCanBlock) {
  WriteGuard guard = lockWrites();
  return guard.append(size, buffer, location, bCanBlock);
}

File::WriteGuard File::lockWrites() {
  return WriteGuard(*this);
}

File::WriteGuard::WriteGuard(File& file) : m_File(file), m_Guard(file.writeSerializationLock()) {}

uint64_t File::WriteGuard::write(uint64_t location, uint64_t size, uintptr_t buffer,
                                 bool bCanBlock) {
  LockGuard<Mutex> guard(m_File.dataMutationLock());
  const uint64_t written = m_File.writeUnlocked(location, size, buffer, bCanBlock);
  if (written) {
    if (!m_File.isBytewise() && !m_File.isDirectory() && !m_File.isSymlink() &&
        m_File.isSeekable()) {
      Attributes attributes;
      attributes.modified = attributes.changed = Time::getTime();
      m_File.updateAttributes(attributes, ModifyTime | ChangeTime);
    }
    m_File.publishEvent(FileEvents::Modify);
  }
  return written;
}

uint64_t File::WriteGuard::append(uint64_t size, uintptr_t buffer, uint64_t& location,
                                  bool bCanBlock) {
  LockGuard<Mutex> guard(m_File.dataMutationLock());
  location = m_File.getSize();
  const uint64_t written = m_File.writeUnlocked(location, size, buffer, bCanBlock);
  if (written) {
    if (!m_File.isBytewise() && !m_File.isDirectory() && !m_File.isSymlink() &&
        m_File.isSeekable()) {
      Attributes attributes;
      attributes.modified = attributes.changed = Time::getTime();
      m_File.updateAttributes(attributes, ModifyTime | ChangeTime);
    }
    m_File.publishEvent(FileEvents::Modify);
  }
  return written;
}

physical_uintptr_t File::getPhysicalPage(size_t offset) {
  LockGuard<Mutex> guard(dataMutationLock());
  if (m_bDirect) {
    WARNING("File in direct mode, cannot get backing page.");
    return ~0UL;
  }

  EMIT_IF(VFS_NOMMU) {
    return ~0UL;
  }

  // Sanitise input.
  size_t blockSize = getBlockSize();
  size_t nativeBlockSize = PhysicalMemoryManager::getPageSize();
  if (useFillCache()) {
    blockSize = nativeBlockSize;
  }
  offset &= ~(blockSize - 1);

  // Quick and easy exit.
  if (offset >= getSize()) {
    return ~0UL;
  }

  // Check if we have this page in the cache and acquire the cache reference
  // before translating it. A cache address observed before a successful pin
  // may already be retiring.
  uintptr_t vaddr = FILE_BAD_BLOCK;
  bool pinned = false;
  if (LIKELY(!useFillCache())) {
    // A key can be evicted and replaced between the address snapshot and
    // pinBlock(). Validate that the address still names the pinned page.
    vaddr = getCachedPage(offset / blockSize);
    if ((!vaddr) || (vaddr == FILE_BAD_BLOCK) || !pinBlock(offset)) {
      return ~0UL;
    }
    pinned = true;

    if (getCachedPage(offset / blockSize) != vaddr) {
      unpinBlock(offset);
      return ~0UL;
    }
  } else {
    // Using the fill cache, because the filesystem has a block size
    // smaller than our native page size. lookup() itself acquires the
    // reference; taking a second pin here would leak one on every mmap.
    vaddr = cacheState().fill.lookup(offset);
    if (!vaddr) {
      return ~0UL;
    }
    pinned = true;
  }

  // Translate only while the exact published page remains pinned.
  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  if (va.isMapped(reinterpret_cast<void*>(vaddr))) {
    physical_uintptr_t phys = 0;
    size_t flags = 0;
    va.getMapping(reinterpret_cast<void*>(vaddr), phys, flags);
    __atomic_add_fetch(&physicalPageLoans(), 1, __ATOMIC_RELEASE);
    return phys;
  }

  if (pinned) {
    if (UNLIKELY(useFillCache()))
      cacheState().fill.release(offset);
    else
      unpinBlock(offset);
  }
  return ~0UL;
}

void File::returnPhysicalPage(size_t offset) {
  if (m_bDirect) {
    return;
  }

  // Sanitise input.
  size_t blockSize = getBlockSize();
  size_t nativeBlockSize = PhysicalMemoryManager::getPageSize();
  if (useFillCache()) {
    blockSize = nativeBlockSize;
  }
  offset &= ~(blockSize - 1);

  // Release the page. Beware - this could cause a cache evict, which will
  // make the next read/write at this offset do real (slow) I/O.
  if (UNLIKELY(useFillCache())) {
    cacheState().fill.release(offset);
  } else {
    unpinBlock(offset);
  }
  assert(__atomic_load_n(&physicalPageLoans(), __ATOMIC_ACQUIRE));
  __atomic_sub_fetch(&physicalPageLoans(), 1, __ATOMIC_RELEASE);
}

bool File::syncAndReturnPhysicalPage(size_t offset, bool async) {
  const bool succeeded = sync(offset, async);
  returnPhysicalPage(offset);
  return succeeded;
}

bool File::tryBeginMappingRelease() {
  return dataMutationLock().tryAcquire();
}

void File::endMappingRelease() {
  dataMutationLock().release();
}

bool File::sync() {
  LockGuard<Mutex> dataGuard(dataMutationLock());
  struct SyncPage {
    size_t block;
    uintptr_t buffer;
  };

  size_t snapshotSize = 0;
  {
    LockGuard<Mutex> guard(cacheState().indexLock);
    snapshotSize = cacheState().data.count();
  }

  // Reserve outside the index lock. Allocation can trigger cache pressure, whose
  // eviction callback removes entries under this same File lock.
  Vector<SyncPage> pages(snapshotSize);
  {
    LockGuard<Mutex> guard(cacheState().indexLock);
    const size_t count =
        cacheState().data.count() < snapshotSize ? cacheState().data.count() : snapshotSize;
    for (size_t i = 0; i < count; ++i) {
      auto result = cacheState().data.getNth(i);
      if (result.hasError()) {
        break;
      }

      const uintptr_t buffer = result.value().second();
      if (buffer != FILE_BAD_BLOCK) {
        pages.pushBack({result.value().first().hash(), buffer});
      }
    }
  }

  const bool filled = useFillCache();
  const size_t blockSize = filled ? PhysicalMemoryManager::getPageSize() : getBlockSize();
  bool succeeded = true;
  for (const SyncPage& page : pages) {
    const uint64_t location = page.block * blockSize;
    if (filled) {
      succeeded = sync(location, false) && succeeded;
      continue;
    }
    if (!pinBlock(location)) {
      continue;
    }

    // cacheState().data is a weak identity index. Pin the producer cache, then
    // verify that the snapshot still names the pinned page.
    if (getCachedPage(page.block) == page.buffer) {
      writeBlock(location, page.buffer);
      succeeded = sync(location, false) && succeeded;
    }
    unpinBlock(location);
  }
  return succeeded;
}

bool File::sync(size_t offset, bool async) {
  return true;
}

Time::Timestamp File::getCreationTime() {
  return getAttributes().changed;
}

void File::setCreationTime(Time::Timestamp t) {
  Attributes attributes;
  attributes.changed = t;
  updateAttributes(attributes, ChangeTime);
  publishEvent(FileEvents::Attributes);
}

Time::Timestamp File::getAccessedTime() {
  return getAttributes().accessed;
}

void File::setAccessedTime(Time::Timestamp t) {
  Attributes attributes;
  attributes.accessed = t;
  updateAttributes(attributes, AccessTime);
  publishEvent(FileEvents::Attributes);
}

Time::Timestamp File::getModifiedTime() {
  return getAttributes().modified;
}

void File::setModifiedTime(Time::Timestamp t) {
  Attributes attributes;
  attributes.modified = t;
  updateAttributes(attributes, ModifyTime);
  publishEvent(FileEvents::Attributes);
}

String File::getName() const {
  LockGuard<Mutex> guard(m_MetadataLock);
  return m_Name;
}

void File::getName(String& s) const {
  LockGuard<Mutex> guard(m_MetadataLock);
  s = m_Name;
}

size_t File::getSize() {
  return m_Size;
}

void File::setSize(size_t sz) {
  m_Size = sz;
}

bool File::isSymlink() {
  return false;
}

bool File::isDirectory() {
  return false;
}

bool File::isPipe() const {
  return false;
}

bool File::isFifo() const {
  return false;
}

bool File::isSocket() const {
  return false;
}

bool File::isSeekable() const {
  return true;
}

bool File::supportsRegularFileOperations() {
  return !isDirectory() && !isSymlink() && !isPipe() && !isFifo() && !isSocket() && !isBytewise() &&
         isSeekable() && !isDirectPhysicalMapping();
}

uint64_t File::maximumFileSize() const {
  return ~uint64_t(0) >> 1;
}

uintptr_t File::getInode() const {
  return m_Inode;
}

void File::setInode(uintptr_t inode) {
  m_Inode = inode;
}

Filesystem* File::getFilesystem() const {
  return m_pFilesystem;
}

void File::setFilesystem(Filesystem* pFs) {
  m_pFilesystem = pFs;
}

void File::fileAttributeChanged() {}

void File::publishEvent(FileEventMask mask, const StringView& name, bool targetIsDirectory) {
  const FileEvent event(mask, name, targetIsDirectory);
  if (mask & FileEvents::DeletedSelf) {
    notifyFinalFileEvent(event);
  } else {
    notifyFileEvent(event);
  }

  constexpr FileEventMask ChildEvents = FileEvents::Access | FileEvents::Modify |
                                        FileEvents::Attributes | FileEvents::CloseWrite |
                                        FileEvents::CloseNoWrite | FileEvents::Open;
  if (!name.length() && (mask & ChildEvents)) {
    ParentLease parent;
    String childName;
    getNamespace(parent, childName);
    if (parent.get()) {
      parent.get()->notifyFileEvent(FileEvent(mask, childName.view(), isDirectory()));
    }
  }
}

void File::increaseRefCount(bool bIsWriter) {
  if (bIsWriter)
    m_nWriters++;
  else
    m_nReaders++;
}

void File::decreaseRefCount(bool bIsWriter) {
  if (bIsWriter)
    m_nWriters--;
  else
    m_nReaders--;
}

bool File::retainVfsReference() {
  return VFS::instance().retainTrackedFile(this);
}

void File::releaseVfsReference() {
  VFS::instance().untrackFile(this);
}

bool File::isStableVfsRoot() const {
  return m_pFilesystem && m_pFilesystem->getRoot() == this;
}

void File::setPermissions(uint32_t perms) {
  Attributes attributes;
  attributes.permissions = perms;
  updateAttributes(attributes, Permissions);
  publishEvent(FileEvents::Attributes);
}

uint32_t File::getPermissions() const {
  return getAttributes().permissions;
}

void File::setUid(size_t uid) {
  Attributes attributes;
  attributes.uid = uid;
  updateAttributes(attributes, Owner);
  publishEvent(FileEvents::Attributes);
}

size_t File::getUid() const {
  return getAttributes().uid;
}

void File::setGid(size_t gid) {
  Attributes attributes;
  attributes.gid = gid;
  updateAttributes(attributes, Group);
  publishEvent(FileEvents::Attributes);
}

size_t File::getGid() const {
  return getAttributes().gid;
}

File* File::getParent() const {
  return __atomic_load_n(&m_pParent, __ATOMIC_ACQUIRE);
}

void File::getNamespace(ParentLease& parent, String& name) const {
  ParentLease replacement;
  {
    LockGuard<Mutex> guard(m_MetadataLock);
    name = m_Name;
    File* current = getParent();
    if (current) {
      replacement.m_Retained = VFS::instance().retainTrackedFile(current);
      if (replacement.m_Retained || (m_pFilesystem && current == m_pFilesystem->getRoot())) {
        replacement.m_Parent = current;
      }
    }
  }
  parent.swap(replacement);
}

void File::moveNamespace(const String& name, File* parent) {
  LockGuard<Mutex> guard(m_MetadataLock);
  m_Name = name;
  __atomic_store_n(&m_pParent, parent, __ATOMIC_RELEASE);
}

void File::retainDetachedParent() {
  LockGuard<Mutex> guard(m_MetadataLock);
  if (__atomic_exchange_n(&m_bDetachedParentHandled, true, __ATOMIC_ACQ_REL)) {
    return;
  }

  File* parent = getParent();
  if (!parent) {
    return;
  }

  File* root = m_pFilesystem ? m_pFilesystem->getRoot() : nullptr;
  if (parent == root) {
    return;
  }

  if (VFS::instance().retainTrackedFile(parent)) {
    m_pDetachedParent = parent;
    return;
  }

  // A failed retain for any non-root parent means it is being retired.
  __atomic_store_n(&m_pParent, nullptr, __ATOMIC_RELEASE);
}

int File::select(bool bWriting, int timeout) {
  return 1;
}

ReadyMask File::queryReady(bool reading, bool writing) {
  ReadyMask ready = ReadyNone;
  if (reading && select(false, 0)) {
    ready |= ReadyRead;
  }
  if (writing && select(true, 0)) {
    ready |= ReadyWrite;
  }
  return ready;
}

bool File::supportsReadinessNotifications() const {
  return false;
}

bool File::supports(const size_t command) const {
  return false;
}

int File::command(const size_t command, void* buffer) {
  return 0;
}

size_t File::getBlockSize() const {
  return PhysicalMemoryManager::getPageSize();
}

void File::enableDirect() {
  m_bDirect = true;
}

void File::disableDirect() {
  m_bDirect = false;
}

void File::preallocate(size_t expectedSize, bool zero) {}

File::Attributes File::getAttributes() const {
  Attributes attributes;
  {
    LockGuard<Mutex> guard(m_MetadataLock);
    attributes.accessed = m_AccessedTime;
    attributes.modified = m_ModifiedTime;
    attributes.changed = m_CreationTime;
    attributes.uid = m_Uid;
    attributes.gid = m_Gid;
    attributes.permissions = m_Permissions;
  }
  attributes.size = const_cast<File*>(this)->getSize();
  attributes.blocks = attributes.size / 512 + (attributes.size % 512 != 0);
  return attributes;
}

void File::updateAttributes(const Attributes& attributes, uint32_t mask) {
  {
    LockGuard<Mutex> guard(m_MetadataLock);
    if (mask & AccessTime)
      m_AccessedTime = attributes.accessed;
    if (mask & ModifyTime)
      m_ModifiedTime = attributes.modified;
    m_CreationTime = (mask & ChangeTime) ? attributes.changed : Time::getTime();
    if (mask & Owner)
      m_Uid = attributes.uid;
    if (mask & Group)
      m_Gid = attributes.gid;
    if (mask & Permissions)
      m_Permissions = attributes.permissions;
  }
  fileAttributeChanged();
}

bool File::prepareSharedMapping(size_t, size_t) {
  return true;
}

bool File::prepareWrite(uint64_t location, uint64_t size) {
  const uint64_t end = location + size;
  extend(static_cast<size_t>(end), location, size);
  return getSize() >= end;
}

void File::truncate() {}

Mutex& File::writeSerializationLock() {
  return m_WriteLock;
}

Mutex& File::dataMutationLock() {
  return m_DataMutationLock;
}

size_t& File::physicalPageLoans() {
  return m_PhysicalPageLoans;
}

bool File::clearDataCache() {
  if (!cacheState().fill.empty()) {
    SYSCALL_ERROR(IoError);
    return false;
  }
  LockGuard<Mutex> guard(cacheState().indexLock);
  cacheState().data.clear();
  return true;
}

bool File::allowMapping(bool, bool, bool&) {
  return true;
}

bool File::allowResize(size_t, size_t) {
  return true;
}

bool File::resize(size_t size) {
  if (!supportsRegularFileOperations()) {
    syscallError(isDirectory() ? Error::IsADirectory : Error::InvalidArgument);
    return false;
  }
  LockGuard<Mutex> writeGuard(writeSerializationLock());
#if !VFS_NOMMU
  MemoryMapManager& mappings = MemoryMapManager::instance();
  MemoryMapManager::OperationGuard mappingGuard(mappings);
#endif
  LockGuard<Mutex> guard(dataMutationLock());
  if (m_pFilesystem && m_pFilesystem->isReadOnly()) {
    SYSCALL_ERROR(ReadOnlyFilesystem);
    return false;
  }
  const size_t oldSize = getSize();
  // Reject backing policy before retiring cache loans or changing mappings.
  if (!allowResize(oldSize, size)) {
    return false;
  }
  if (size == oldSize) {
    return resizeFile(size);
  }
  if (size < oldSize) {
    ShrinkContext context{oldSize, size, nullptr, 0};
    size_t mappingLoans = 0;
#if !VFS_NOMMU
    UniquePointer<MemoryMapManager::PreparedFileResize> mappingPlan;
    const auto mappingStatus = mappings.prepareFileResize(this, oldSize, size, mappingPlan);
    if (mappingStatus != MemoryMapManager::ResizeStatus::Ready) {
      syscallError(mappingStatus == MemoryMapManager::ResizeStatus::NoMemory ? Error::OutOfMemory
                   : mappingStatus == MemoryMapManager::ResizeStatus::Invalid
                       ? Error::InvalidArgument
                       : Error::OperationNotSupported);
      return false;
    }
    context.mappingLoans = mappingPlan.get()->loans();
    context.mappingLoanCount = mappingPlan.get()->loanCount();
    mappingLoans = mappingPlan.get()->totalLoans();
#endif
    // Prefix mappings remain valid borrowers; other physical-page users cannot
    // be revoked by the mapping journal.
    if (__atomic_load_n(&physicalPageLoans(), __ATOMIC_ACQUIRE) != mappingLoans) {
      SYSCALL_ERROR(DeviceBusy);
      return false;
    }
    const size_t pageSize = PhysicalMemoryManager::getPageSize();
    const size_t boundaryOffset = size - size % pageSize;
    const size_t cutoff = boundaryOffset + (size % pageSize ? pageSize : 0);
    if (cutoff < size) {
      SYSCALL_ERROR(InvalidArgument);
      return false;
    }
    UniquePointer<Cache::PreparedDiscard> fillPlan;
    struct BoundaryPage {
      Cache& cache;
      size_t offset;
      uintptr_t address;
      ~BoundaryPage() {
        if (address)
          cache.release(offset);
      }
    } boundary{cacheState().fill, boundaryOffset, 0};
    if (useFillCache()) {
      const auto status = cacheState().fill.prepareDiscardFrom(cutoff, context.mappingLoans,
                                                               context.mappingLoanCount, fillPlan);
      if (status != Cache::DiscardStatus::Ready) {
        syscallError(status == Cache::DiscardStatus::NoMemory  ? Error::OutOfMemory
                     : status == Cache::DiscardStatus::Busy    ? Error::DeviceBusy
                     : status == Cache::DiscardStatus::Invalid ? Error::InvalidArgument
                                                               : Error::IoError);
        return false;
      }
      if (size % pageSize)
        boundary.address = cacheState().fill.lookup(boundaryOffset);
    }
    UniquePointer<PreparedShrink> backend;
    if (!prepareShrink(context, backend))
      return false;
#if !VFS_NOMMU
    mappingPlan.get()->commit();
#endif
    backend.get()->commit();
    if (fillPlan)
      fillPlan.get()->commit();
    if (boundary.address)
      ByteSet(reinterpret_cast<void*>(boundary.address + size % pageSize), 0,
              pageSize - size % pageSize);
    // Native-block backends also cache borrowed addresses in this index.
    // Preserve prefix entries while invalidating every detached suffix alias.
    const size_t cacheBlockSize = useFillCache() ? pageSize : getBlockSize();
    const size_t firstDiscard = cutoff / cacheBlockSize;
    LockGuard<Mutex> indexGuard(cacheState().indexLock);
    for (auto it = cacheState().data.begin(); it != cacheState().data.end();) {
      if (it.__getNode()->key.hash() >= firstDiscard)
        it = cacheState().data.erase(it);
      else
        ++it;
    }
  } else if (!resizeFile(size)) {
    return false;
  }
  if (size > oldSize && useFillCache()) {
    const size_t pageSize = PhysicalMemoryManager::getPageSize();
    const size_t offset = oldSize % pageSize;
    if (offset) {
      const size_t pageOffset = oldSize - offset;
      const uintptr_t buffer = cacheState().fill.lookup(pageOffset);
      if (buffer) {
        const size_t amount =
            (size - oldSize < pageSize - offset) ? size - oldSize : pageSize - offset;
        ByteSet(reinterpret_cast<void*>(buffer + offset), 0, amount);
        cacheState().fill.release(pageOffset);
      }
    }
  }
  Attributes attributes;
  attributes.modified = attributes.changed = Time::getTime();
  updateAttributes(attributes, ModifyTime | ChangeTime);
  publishEvent(FileEvents::Modify);
  return true;
}

bool File::prepareShrink(const ShrinkContext&, UniquePointer<PreparedShrink>&) {
  SYSCALL_ERROR(OperationNotSupported);
  return false;
}

bool File::resizeFile(size_t) {
  SYSCALL_ERROR(OperationNotSupported);
  return false;
}

File* File::open() {
  return this;
}

bool File::isBytewise() const {
  return false;
}

uint64_t File::readBytewise(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock) {
  if (isBytewise()) {
    FATAL("A bytewise File subclass didn't implement readBytewise");
  }
  return 0;
}

uint64_t File::writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock) {
  if (isBytewise()) {
    FATAL("A bytewise File subclass didn't implement writeBytewise");
  }
  return 0;
}

uintptr_t File::readBlock(uint64_t location) {
  String fullPath;
  getFullPath(fullPath);
  ERROR("File: base class readBlock() called for " << fullPath);
  // only truly breaks on debug-enabled builds - in release builds this will
  // just cause an error in the caller
  assert(false);
  return 0;
}

void File::writeBlock(uint64_t location, uintptr_t addr) {}

void File::writeBlocks(uint64_t location, uintptr_t addr, size_t length) {
  const size_t blockSize = getBlockSize();
  if (!blockSize) {
    return;
  }

  for (size_t offset = 0; offset < length; offset += blockSize) {
    writeBlock(location + offset, addr + offset);
  }
}

void File::extend(size_t newSize) {
  if (m_Size < newSize)
    m_Size = newSize;
}

void File::extend(size_t newSize, uint64_t location, uint64_t size) {
  extend(newSize);
}

bool File::pinBlock(uint64_t location) {
  return false;
}

void File::unpinBlock(uint64_t location) {}

void File::evict(uint64_t location) {
  setCachedPage(location / getBlockSize(), FILE_BAD_BLOCK);
}

void File::setPermissionsOnly(uint32_t perms) {
  m_Permissions = perms;
}

void File::setUidOnly(size_t uid) {
  m_Uid = uid;
}

void File::setGidOnly(size_t gid) {
  m_Gid = gid;
}

void File::dataChanged() {
  EMIT_IF(THREADS) {
    bool bAny = false;
    {
      LockGuard<Mutex> guard(m_Lock);

      for (List<MonitorTarget*>::Iterator it = m_MonitorTargets.begin();
           it != m_MonitorTargets.end(); it++) {
        MonitorTarget* pMT = *it;

        pMT->pThread->sendEvent(pMT->pEvent);
        delete pMT;

        bAny = true;
      }

      m_MonitorTargets.clear();
    }

    // If anything was waiting on a change, wake it up now.
    if (bAny) {
      Scheduler::instance().yield();
    }
  }

  // Readiness observers only receive a change hint and re-query the source.
  // Keep callbacks outside the File lock and the legacy Event registry.
  notifyReadiness(ReadyAll);
}

void File::monitor(Thread* pThread, Event* pEvent) {
  assert(pThread);
  assert(pEvent);

  EMIT_IF(THREADS) {
    LockGuard<Mutex> guard(m_Lock);
    Event::SendLease registration;
    if (pEvent->tryAcquireRegistration(registration)) {
      m_MonitorTargets.pushBack(
          new MonitorTarget(pThread, pEvent, pedigree_std::move(registration)));
    }
  }
}

void File::cullMonitorTargets(Thread* pThread) {
  EMIT_IF(THREADS) {
    LockGuard<Mutex> guard(m_Lock);

    for (List<MonitorTarget*>::Iterator it = m_MonitorTargets.begin();
         it != m_MonitorTargets.end();) {
      MonitorTarget* pMT = *it;

      if (pMT->pThread == pThread) {
        delete pMT;
        it = m_MonitorTargets.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void File::cullMonitorTargets(Event* pEvent) {
  EMIT_IF(THREADS) {
    LockGuard<Mutex> guard(m_Lock);

    for (List<MonitorTarget*>::Iterator it = m_MonitorTargets.begin();
         it != m_MonitorTargets.end();) {
      MonitorTarget* pMT = *it;
      if (pMT->pEvent == pEvent) {
        delete pMT;
        it = m_MonitorTargets.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void File::getFilesystemLabel(HugeStaticString& s) {
  s = m_pFilesystem->getVolumeLabel();
}

void File::getFullPath(String& result, bool bWithMount) {
  HugeStaticString str;
  HugeStaticString tmp;
  str.clear();
  tmp.clear();

  File* f = this;
  ParentLease current;
  while (f) {
    ParentLease parent;
    String name;
    f->getNamespace(parent, name);
    if (!parent.get()) {
      break;
    }
    tmp = str;
    str = name;
    if (tmp.length()) {
      str += "/";
      str += tmp;
    }
    current.swap(parent);
    f = current.get();
  }

  tmp = str;
  str = "/";
  str += tmp;

  if (bWithMount && m_pFilesystem) {
    String mountPath;
    if (VFS::instance().getMountPath(m_pFilesystem, mountPath) && mountPath != "/") {
      tmp = str;
      str = mountPath;
      if (tmp != "/") {
        str += tmp;
      }
    }
  } else if (bWithMount && !m_pFilesystem) {
    ERROR("File::getFullPath called without a filesystem!");
  }

  result.assign(str, str.length());
}

String File::getFullPath(bool bWithMount) {
  String path;
  getFullPath(path, bWithMount);
  return path;
}

uintptr_t File::getCachedPage(size_t block, bool locked) {
  LockGuard<Mutex> guard(cacheState().indexLock, locked);

  DataCacheKey key(block);
  auto result = cacheState().data.lookup(key);
  if (result.hasValue()) {
    return result.value();
  } else {
    return FILE_BAD_BLOCK;
  }
}

void File::setCachedPage(size_t block, uintptr_t value, bool locked) {
  LockGuard<Mutex> guard(cacheState().indexLock, locked);

  assert(value);

  DataCacheKey key(block);
  if (cacheState().data.contains(key)) {
    if (value == FILE_BAD_BLOCK) {
      cacheState().data.remove(key);
    } else {
      cacheState().data.update(key, value);
    }
  } else {
    cacheState().data.insert(key, value);
  }
}

bool File::useFillCache() const {
  EMIT_IF(VFS_NOMMU) {
#if defined(PEDIGREE_BUILDUTILS)
    if (m_bForceFillCache) {
      return getBlockSize() < PhysicalMemoryManager::getPageSize();
    }
#endif
    // No fill cache in NOMMU builds.
    return false;
  }
  else {
    size_t blockSize = getBlockSize();
    size_t nativeBlockSize = PhysicalMemoryManager::getPageSize();
    return blockSize < nativeBlockSize;
  }
}

void File::enableFillCacheWriteback() {
  cacheState().fill.setCallback(fillCacheCallback, this);
}

void File::shutdownFillCacheWriteback() {
  cacheState().fill.shutdown();
}

bool File::syncFillCache(size_t offset, bool async, bool& present) {
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t pageOffset = offset - (offset % pageSize);
  LockGuard<Mutex> guard(cacheState().fillLock);
  present = cacheState().fill.lookup(pageOffset) != 0;
  if (!present) {
    return true;
  }

  const bool succeeded = cacheState().fill.sync(pageOffset, async);
  cacheState().fill.release(pageOffset);
  return succeeded;
}

uintptr_t File::readIntoCache(uintptr_t block) {
  size_t blockSize = getBlockSize();
  size_t nativeBlockSize = PhysicalMemoryManager::getPageSize();
  const bool fillCache = useFillCache();

  const size_t offset = block * (fillCache ? nativeBlockSize : blockSize);

  if (fillCache) {
    LockGuard<Mutex> fillGuard(cacheState().fillLock);

    // Using Cache::insert() here is atomic compared to if we did a
    // lookup() followed by an insert() - means we don't need to lock the
    // File object to do this.
    bool didExist = false;
    uintptr_t vaddr = cacheState().fill.insert(offset, nativeBlockSize, &didExist);
    if (!vaddr) {
      return FILE_BAD_BLOCK;
    }

    // If in direct mode we are required to read() again
    bool existingReference = false;
    if (didExist) {
      vaddr = cacheState().fill.lookup(offset);
      if (!vaddr) {
        return FILE_BAD_BLOCK;
      }
      if (!m_bDirect) {
        setCachedPage(block, vaddr);
        return vaddr;
      }
      existingReference = true;
    }

    // Read the blocks
    ByteSet(reinterpret_cast<void*>(vaddr), 0, nativeBlockSize);
    for (size_t i = 0; i < nativeBlockSize; i += blockSize) {
      if ((offset + i) >= getSize()) {
        break;
      }
      uintptr_t blockAddr = readBlock(offset + i);
      if (!blockAddr || blockAddr == FILE_BAD_BLOCK) {
        if (existingReference) {
          cacheState().fill.release(offset);
        }
        if (!didExist && !cacheState().fill.discardEditing(offset)) {
          WARNING(
              "File::readIntoCache could not discard a failed fill "
              "for offset "
              << offset);
        }
        return FILE_BAD_BLOCK;
      }
      const size_t remaining = getSize() - (offset + i);
      ForwardMemoryCopy(reinterpret_cast<void*>(vaddr + i), reinterpret_cast<void*>(blockAddr),
                        remaining < blockSize ? remaining : blockSize);
      unpinBlock(offset + i);
    }

    cacheState().fill.markNoLongerEditing(offset, nativeBlockSize);

    if (existingReference) {
      return vaddr;
    }

    vaddr = cacheState().fill.lookup(offset);
    if (vaddr) {
      setCachedPage(block, vaddr);
    }
    return vaddr ? vaddr : FILE_BAD_BLOCK;
  }

  uintptr_t buff = FILE_BAD_BLOCK;
  if (!m_bDirect) {
    while ((buff = getCachedPage(block)) != FILE_BAD_BLOCK) {
      if (!pinBlock(offset)) {
        buff = FILE_BAD_BLOCK;
        break;
      }
      if (getCachedPage(block) == buff) {
        return buff;
      }
      unpinBlock(offset);
    }
  }
  if (buff == FILE_BAD_BLOCK) {
    buff = readBlock(offset);
    if (!buff) {
      ERROR("File::readIntoCache - bad read (" << (block * blockSize) << " - block size is "
                                               << blockSize << ")");
      return FILE_BAD_BLOCK;
    }

    if (!m_bDirect) {
      setCachedPage(block, buff);
    }
  }

  return buff;
}

void File::releaseReadReference(uintptr_t block) {
  if (useFillCache()) {
    cacheState().fill.release(block * PhysicalMemoryManager::getPageSize());
  } else {
    unpinBlock(block * getBlockSize());
  }
}
