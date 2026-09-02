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

#include "Directory.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/Pair.h"
#include "pedigree/kernel/utilities/Result.h"
#include "pedigree/kernel/utilities/StringView.h"
#include "pedigree/kernel/utilities/Vector.h"

#include "Filesystem.h"
#include "VFS.h"

template class HashTable<String, Directory::DirectoryEntry*, HashedStringView>;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
Directory::RetainedLookupHook Directory::m_RetainedLookupHook = nullptr;
#endif

Directory::ChildLease::ChildLease()
    : m_pFile(nullptr)
#if THREADS && !defined(STANDALONE_MUTEXES)
      ,
      m_TerminationDeferral(true)
#endif
{
}

Directory::ChildLease::~ChildLease() {
  reset();
}

void Directory::ChildLease::reset() {
  File* file = m_pFile;
  m_pFile = nullptr;
  if (file) {
    VFS::instance().untrackFile(file);
  }
}

void Directory::ChildLease::adopt(File* file) {
  assert(file != nullptr);
  assert(m_pFile == nullptr);
  m_pFile = file;
}

Directory::Directory()
    : File(),
      m_Cache(nullptr),
      m_CacheGenerations(0),
      m_NextCacheGeneration(0),
      m_bCachePopulated(false),
      m_CacheLock() {}

Directory::Directory(const String& name, Time::Timestamp accessedTime, Time::Timestamp modifiedTime,
                     Time::Timestamp creationTime, uintptr_t inode, Filesystem* pFs, size_t size,
                     File* pParent)
    : File(name, accessedTime, modifiedTime, creationTime, inode, pFs, size, pParent),
      m_Cache(nullptr),
      m_CacheGenerations(0),
      m_NextCacheGeneration(0),
      m_bCachePopulated(false),
      m_CacheLock() {}

Directory::~Directory() {
  emptyCache();
}

File* Directory::getChild(size_t n) {
  if (UNLIKELY(!isCachePopulated())) {
    cacheDirectoryContents();
    markCachePopulated();
  }

  LockGuard<Mutex> guard(m_CacheLock);
  DirectoryEntryCache::PairLookupResult result = m_Cache.getNth(n);
  if (result.hasError()) {
    return 0;
  } else {
    return result.value().second()->get();
  }
}

size_t Directory::getNumChildren() {
  if (UNLIKELY(!isCachePopulated())) {
    cacheDirectoryContents();
    markCachePopulated();
  }

  LockGuard<Mutex> guard(m_CacheLock);
  return m_Cache.count();
}

void Directory::cacheDirectoryContents() {}

bool Directory::isCachePopulated() const {
  LockGuard<Mutex> guard(m_CacheLock);
  return m_bCachePopulated;
}

File* Directory::lookup(const HashedStringView& s) const {
  LockGuard<Mutex> guard(m_CacheLock);
  if (LIKELY(m_bCachePopulated)) {
    DirectoryEntryCache::LookupResult result = m_Cache.lookup(s);
    if (result.hasValue()) {
      return result.value()->get();
    }
  }
  return nullptr;
}

bool Directory::lookupRetained(const HashedStringView& s, ChildLease& child) const {
  File* replacement = nullptr;

  {
    LockGuard<Mutex> guard(m_CacheLock);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    RetainedLookupHook hook = __atomic_load_n(&m_RetainedLookupHook, __ATOMIC_ACQUIRE);
    if (hook) {
      hook(const_cast<Directory*>(this), nullptr, RetainedLookupPhase::BeforeLookup);
    }
#endif

    if (!m_bCachePopulated) {
      return false;
    }

    DirectoryEntryCache::LookupResult result = m_Cache.lookup(s);
    if (!result.hasValue()) {
      return false;
    }

    File* file = result.value()->get();
    if (!file) {
      return false;
    }

    if (!VFS::instance().retainTrackedFile(file)) {
      return false;
    }
    replacement = file;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    if (hook) {
      hook(const_cast<Directory*>(this), file, RetainedLookupPhase::AfterRetain);
    }
#endif
  }

  child.reset();
  child.adopt(replacement);
  return true;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void Directory::setRetainedLookupHookForHostedTest(RetainedLookupHook hook) {
  __atomic_store_n(&m_RetainedLookupHook, hook, __ATOMIC_RELEASE);
}

bool Directory::tryCacheLockForHostedTest() const {
  if (!m_CacheLock.tryAcquire()) {
    return false;
  }
  m_CacheLock.release();
  return true;
}
#endif

void Directory::remove(const HashedStringView& s) {
  DirectoryEntry* entry = nullptr;
  {
    LockGuard<Mutex> guard(m_CacheLock);
    DirectoryEntryCache::LookupResult result = m_Cache.lookup(s);
    if (result.hasValue()) {
      entry = result.value();
      /// \todo add sibling keys for other HashTable functions
      m_Cache.remove(s.toString());
      m_CacheGenerations.remove(s.toString());
    }
  }
  delete entry;
}

void Directory::addDirectoryEntry(const String& name, File* pTarget) {
  assert(pTarget != nullptr);

  bool inserted = false;
  {
    LockGuard<Mutex> guard(m_CacheLock);
    if (!m_Cache.lookup(name).hasValue()) {
      // Membership must precede cache visibility so retained lookup can adopt it.
      VFS::instance().trackFile(pTarget);
      DirectoryEntry* entry = new DirectoryEntry(pTarget);
      inserted = m_Cache.insert(name, entry);
      assert(inserted);
      const bool generationInserted = m_CacheGenerations.insert(name, nextCacheGeneration());
      assert(generationInserted);
      m_bCachePopulated = true;
    }
  }

  if (!inserted) {
    ERROR("can't add directory entry for '" << name << "' as it already exists.");
  }
}

void Directory::addDirectoryEntry(const String& name, DirectoryEntryMetadata&& meta) {
  bool inserted = false;
  {
    LockGuard<Mutex> guard(m_CacheLock);
    if (!m_Cache.lookup(name).hasValue()) {
      DirectoryEntry* entry = new DirectoryEntry(pedigree_std::move(meta));
      inserted = m_Cache.insert(name, entry);
      assert(inserted);
      const bool generationInserted = m_CacheGenerations.insert(name, nextCacheGeneration());
      assert(generationInserted);
      m_bCachePopulated = true;
    }
  }

  if (!inserted) {
    ERROR("can't add directory entry for '" << name << "' as it already exists.");
  }
}

Directory* Directory::getReparsePoint() const {
  return __atomic_load_n(&m_ReparseTarget, __ATOMIC_ACQUIRE);
}

void Directory::setReparsePoint(Directory* pTarget) {
  __atomic_store_n(&m_ReparseTarget, pTarget, __ATOMIC_RELEASE);
}

bool Directory::addEphemeralFile(File* pFile) {
  assert(pFile != nullptr);

  if (UNLIKELY(!isCachePopulated())) {
    cacheDirectoryContents();
    markCachePopulated();
  }

  /// \todo removal will still want to hit the Filesystem here! not good!
  bool inserted = false;
  {
    LockGuard<Mutex> guard(m_CacheLock);
    if (!m_Cache.lookup(pFile->getName()).hasValue()) {
      VFS::instance().trackFile(pFile);
      DirectoryEntry* entry = new DirectoryEntry(pFile);
      inserted = m_Cache.insert(pFile->getName(), entry);
      assert(inserted);
      const bool generationInserted =
          m_CacheGenerations.insert(pFile->getName(), nextCacheGeneration());
      assert(generationInserted);
      m_bCachePopulated = true;
    }
  }

  return inserted;
}

bool Directory::empty() {
  while (true) {
    String name;
    ChildLease child;
    File* retainedFile = nullptr;
    uint64_t generation = 0;
    {
      LockGuard<Mutex> guard(m_CacheLock);
      if (!m_Cache.count()) {
        return true;
      }

      DirectoryEntryCache::PairLookupResult result = m_Cache.getNth(0);
      if (result.hasError()) {
        return false;
      }

      name = result.value().first();
      DirectoryEntryGenerationCache::LookupResult generationResult =
          m_CacheGenerations.lookup(HashedStringView(name));
      if (!generationResult.hasValue()) {
        return false;
      }
      generation = generationResult.value();

      File* file = result.value().second()->get();
      if (!file || !VFS::instance().retainTrackedFile(file)) {
        return false;
      }
      retainedFile = file;
    }

    // No terminal boundary exists between the cache guard and adoption.
    child.adopt(retainedFile);

    if (!getFilesystem()->remove(this, child.get())) {
      /// \note partial failure - some entries have been deleted by this
      /// point!
      return false;
    }

    // The filesystem callback may already have removed this cache entry.
    removeIfGeneration(HashedStringView(name), generation);
  }
  return true;
}

void Directory::emptyCache() {
  Vector<DirectoryEntry*> entries;
  {
    LockGuard<Mutex> guard(m_CacheLock);
    for (auto it : m_Cache) {
      entries.pushBack(it);
    }

    m_Cache.clear();
    m_CacheGenerations.clear();
    m_bCachePopulated = false;
  }

  // Now that the hashtable is flattened into this vector, it's safe to
  // delete without worrying about our deletion modifying the table.
  for (auto it : entries) {
    delete it;
  }
}

File* Directory::evaluateEntry(const DirectoryEntryMetadata& meta) {
  if (!meta.pDirectory) {
    return nullptr;
  }
  File* newFile = meta.pDirectory->convertToFile(meta);

  if (newFile) {
    // Track this lazy-loaded directory entry.
    VFS::instance().trackFile(newFile);
  }

  return newFile;
}

void Directory::destroyEntry(File* file) {
  VFS::instance().untrackFile(file);
}

File* Directory::convertToFile(const DirectoryEntryMetadata& meta) {
  return nullptr;
}

void Directory::preallocateDirectoryEntries(size_t count) {
  LockGuard<Mutex> guard(m_CacheLock);
  m_Cache.reserve(count);
}

void Directory::markCachePopulated() {
  LockGuard<Mutex> guard(m_CacheLock);
  m_bCachePopulated = true;
}

uint64_t Directory::nextCacheGeneration() {
  ++m_NextCacheGeneration;
  if (!m_NextCacheGeneration) {
    ++m_NextCacheGeneration;
  }
  return m_NextCacheGeneration;
}

void Directory::removeIfGeneration(const HashedStringView& name, uint64_t generation) {
  DirectoryEntry* entry = nullptr;
  {
    LockGuard<Mutex> guard(m_CacheLock);
    DirectoryEntryGenerationCache::LookupResult generationResult = m_CacheGenerations.lookup(name);
    if (!generationResult.hasValue() || generationResult.value() != generation) {
      return;
    }

    DirectoryEntryCache::LookupResult result = m_Cache.lookup(name);
    if (!result.hasValue()) {
      return;
    }
    entry = result.value();
    m_Cache.remove(name.toString());
    m_CacheGenerations.remove(name.toString());
  }
  delete entry;
}

Directory::DirectoryEntryMetadata::DirectoryEntryMetadata()
    : pDirectory(nullptr), filename(), opaque() {}
Directory::DirectoryEntryMetadata::DirectoryEntryMetadata(
    Directory::DirectoryEntryMetadata&& other) noexcept
    : pDirectory(pedigree_std::move(other.pDirectory)),
      filename(pedigree_std::move(other.filename)),
      opaque(pedigree_std::move(other.opaque)) {
  other.pDirectory = nullptr;
}

Directory::DirectoryEntryMetadata::~DirectoryEntryMetadata() {
  opaque.reset();
}
