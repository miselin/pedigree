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
#include "pedigree/kernel/syscallError.h"
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

void Directory::ChildLease::swap(ChildLease& other) {
  File* file = m_pFile;
  m_pFile = other.m_pFile;
  other.m_pFile = file;
}

void Directory::ChildLease::adopt(File* file) {
  assert(file != nullptr);
  assert(m_pFile == nullptr);
  m_pFile = file;
}

Directory::NameReservation::NameReservation()
    : m_pDirectory(nullptr),
      m_Name(),
      m_pToken(nullptr),
      m_OwnsNamespaceLock(false)
#if THREADS && !defined(STANDALONE_MUTEXES)
      ,
      m_TerminationDeferral(true)
#endif
{
}

Directory::NameReservation::~NameReservation() {
  if (m_pDirectory) {
    m_pDirectory->finishNameReservation(*this, LookupStatus::Retry);
  }
}

void Directory::NameReservation::complete(LookupStatus result) {
  if (m_pDirectory) {
    m_pDirectory->finishNameReservation(*this, result);
  }
}

Directory::Directory()
    : File(),
      m_Cache(nullptr),
      m_ResidentEntries(nullptr),
      m_ResidentOrder(),
      m_EphemeralEntries(false),
      m_CacheGenerations(0),
      m_NextCacheGeneration(0),
      m_InFlightLookups(nullptr),
      m_bCachePopulated(false),
      m_Detached(false),
      m_ParentInode(0),
      m_CacheLock(),
      m_NamespaceMutationLock() {}

Directory::Directory(const String& name, Time::Timestamp accessedTime, Time::Timestamp modifiedTime,
                     Time::Timestamp creationTime, uintptr_t inode, Filesystem* pFs, size_t size,
                     File* pParent)
    : File(name, accessedTime, modifiedTime, creationTime, inode, pFs, size, pParent),
      m_Cache(nullptr),
      m_ResidentEntries(nullptr),
      m_ResidentOrder(),
      m_EphemeralEntries(false),
      m_CacheGenerations(0),
      m_NextCacheGeneration(0),
      m_InFlightLookups(nullptr),
      m_bCachePopulated(false),
      m_Detached(false),
      m_ParentInode(pParent ? pParent->getInode() : inode),
      m_CacheLock(),
      m_NamespaceMutationLock() {}

Directory::InFlightLookup::InFlightLookup()
    : changed(), users(1), result(LookupStatus::Retry), complete(false), invalidated(false) {}

Directory::~Directory() {
  emptyCache();
}

File* Directory::getChild(size_t n) {
  if (n == 0) {
    return this;
  }
  if (n == 1) {
    return getParent() ? getParent() : this;
  }
  if (!cacheResolvedChildren()) {
    SYSCALL_ERROR(OperationNotSupported);
    return nullptr;
  }

  struct Context {
    size_t wanted;
    size_t seen;
    String name;
    uint64_t cookie;
  } context = {n - 2, 0, String(), 0};

  auto emitter = [](void* opaque, const DirectoryEntryView& entry) -> bool {
    Context* context = reinterpret_cast<Context*>(opaque);
    if (context->seen++ == context->wanted) {
      context->name = entry.name.toString();
      context->cookie = entry.currentCookie;
      return false;
    }
    return true;
  };

  uint64_t cookie = 2;
  ReadStatus status = enumerate(cookie, emitter, &context);
  if (context.name.length() == 0 ||
      (status != ReadStatus::Stopped && status != ReadStatus::Complete)) {
    return nullptr;
  }

  ChildLease child;
  if (lookupChildAt(context.cookie, HashedStringView(context.name), child) != LookupStatus::Found) {
    return nullptr;
  }
  return child.get();
}

size_t Directory::getNumChildren() {
  size_t count = 0;
  auto emitter = [](void* opaque, const DirectoryEntryView&) -> bool {
    ++*reinterpret_cast<size_t*>(opaque);
    return true;
  };
  uint64_t cookie = 0;
  enumerate(cookie, emitter, &count);
  return count;
}

struct Directory::EnumerationContext {
  Directory* directory;
  DirectoryEntryEmitter emitter;
  void* context;
  bool invalidCookie;
};

bool Directory::emitBackingEntry(void* opaque, const DirectoryEntryView& entry) {
  EnumerationContext* context = reinterpret_cast<EnumerationContext*>(opaque);
  if (entry.name.compare(".", 1) || entry.name.compare("..", 2)) {
    return true;
  }

  {
    LockGuard<Mutex> guard(context->directory->m_CacheLock);
    if (context->directory->m_ResidentEntries.lookup(HashedStringView(entry.name)).hasValue()) {
      return true;
    }
  }

  if (entry.currentCookie >= (ResidentCookie - 2) || entry.nextCookie >= (ResidentCookie - 2)) {
    context->invalidCookie = true;
    return false;
  }

  DirectoryEntryView translated = {entry.name, entry.inode, entry.type, entry.currentCookie + 2,
                                   entry.nextCookie + 2};
  return context->emitter(context->context, translated);
}

Directory::ReadStatus Directory::enumerate(uint64_t& cookie, DirectoryEntryEmitter emitter,
                                           void* context) {
  if (!emitter) {
    return ReadStatus::IoError;
  }
  if (cookie >= (ResidentCookie << 1)) {
    return ReadStatus::IoError;
  }

  if (cookie == 0) {
    DirectoryEntryView dot = {StringView("."), getInode(), EntryType::Directory, 0, 1};
    if (!emitter(context, dot)) {
      return ReadStatus::Stopped;
    }
    cookie = 1;
  }

  if (cookie == 1) {
    DirectoryEntryView dotdot = {StringView(".."),
                                 __atomic_load_n(&m_ParentInode, __ATOMIC_ACQUIRE),
                                 EntryType::Directory, 1, 2};
    if (!emitter(context, dotdot)) {
      return ReadStatus::Stopped;
    }
    cookie = 2;
  }

  if (isDetached()) {
    cookie = ResidentCookie;
    return ReadStatus::Complete;
  }

  if (!(cookie & ResidentCookie)) {
    uint64_t backingCookie = cookie - 2;
    EnumerationContext adapter = {this, emitter, context, false};
    ReadStatus status = readDirectory(backingCookie, emitBackingEntry, &adapter);
    cookie = backingCookie + 2;
    if (adapter.invalidCookie) {
      return ReadStatus::IoError;
    }
    if (status != ReadStatus::Complete) {
      return status;
    }
    cookie = ResidentCookie;
  }

  while (true) {
    String name;
    uint64_t generation = 0;
    ChildLease child;
    {
      LockGuard<Mutex> guard(m_CacheLock);
      size_t slot = static_cast<size_t>(cookie & ~ResidentCookie);
      while (slot < m_ResidentOrder.count()) {
        const String& candidateName = m_ResidentOrder[slot++];
        if (!candidateName.length()) {
          continue;
        }
        if (candidateName.compare(".", 1) || candidateName.compare("..", 2)) {
          continue;
        }
        name = candidateName;
        generation = slot;
        break;
      }

      if (!generation) {
        cookie = ResidentCookie | m_ResidentOrder.count();
        return ReadStatus::Complete;
      }

      File* file = cachedFileLocked(HashedStringView(name));
      if (!file || !VFS::instance().retainTrackedFile(file)) {
        return ReadStatus::IoError;
      }
      child.adopt(file);
    }

    const uint64_t currentCookie = ResidentCookie | (generation - 1);
    const uint64_t nextCookie = ResidentCookie | generation;
    cookie = currentCookie;
    DirectoryEntryView entry = {name.view(), child.get()->getInode(), entryType(child.get()),
                                currentCookie, nextCookie};
    if (!emitter(context, entry)) {
      return ReadStatus::Stopped;
    }
    cookie = nextCookie;
  }
}

Directory::ReadStatus Directory::isEmpty(bool& empty) {
  empty = true;
  auto emitter = [](void* opaque, const DirectoryEntryView&) -> bool {
    *reinterpret_cast<bool*>(opaque) = false;
    return false;
  };
  uint64_t cookie = 2;
  ReadStatus status = enumerate(cookie, emitter, &empty);
  if (status == ReadStatus::Stopped) {
    return ReadStatus::Complete;
  }
  return status;
}

void Directory::cacheDirectoryContents() {}

bool Directory::isCachePopulated() const {
  LockGuard<Mutex> guard(m_CacheLock);
  return m_bCachePopulated;
}

File* Directory::lookup(const HashedStringView& s) const {
  if (!cacheResolvedChildren()) {
    SYSCALL_ERROR(OperationNotSupported);
    return nullptr;
  }
  ChildLease child;
  if (lookupChild(s, child) != LookupStatus::Found) {
    return nullptr;
  }
  return child.get();
}

bool Directory::lookupRetained(const HashedStringView& s, ChildLease& child) const {
  return lookupChild(s, child) == LookupStatus::Found;
}

Directory::LookupStatus Directory::lookupChild(const HashedStringView& s, ChildLease& child) const {
  return lookupChildInternal(s, 0, false, child);
}

Directory::LookupStatus Directory::lookupChildAt(uint64_t cookie, const HashedStringView& name,
                                                 ChildLease& child) const {
  if (cookie >= (ResidentCookie << 1)) {
    return LookupStatus::IoError;
  }
  if (cookie < 2) {
    const bool dot = cookie == 0 && name == ".";
    const bool dotdot = cookie == 1 && name == "..";
    if (!dot && !dotdot) {
      return LookupStatus::NotFound;
    }

    ParentLease parent;
    String unused;
    if (dotdot) {
      getNamespace(parent, unused);
    }
    File* file = dot ? const_cast<Directory*>(this) : parent.get();
    if (!file) {
      file = const_cast<Directory*>(this);
    }
    if (!VFS::instance().retainTrackedFile(file)) {
      return LookupStatus::IoError;
    }
    ChildLease replacement;
    replacement.adopt(file);
    child.swap(replacement);
    return LookupStatus::Found;
  }
  if (cookie & ResidentCookie) {
    File* file = nullptr;
    bool stale = false;
    {
      LockGuard<Mutex> guard(m_CacheLock);
      const size_t slot = static_cast<size_t>(cookie & ~ResidentCookie);
      if (m_InFlightLookups.lookup(name).hasValue() || slot >= m_ResidentOrder.count() ||
          m_ResidentOrder[slot] != name) {
        stale = true;
      } else {
        DirectoryEntryCache::LookupResult resident = m_ResidentEntries.lookup(name);
        if (!resident.hasValue()) {
          stale = true;
        } else {
          file = resident.value()->get();
          if (!file || !VFS::instance().retainTrackedFile(file)) {
            return LookupStatus::IoError;
          }
        }
      }
    }
    if (stale) {
      return lookupChild(name, child);
    }
    ChildLease replacement;
    replacement.adopt(file);
    child.swap(replacement);
    return LookupStatus::Found;
  }
  return lookupChildInternal(name, cookie - 2, true, child);
}

Directory::LookupStatus Directory::lookupChildInternal(const HashedStringView& s, uint64_t cookie,
                                                       bool hasCookie, ChildLease& child) const {
  Directory* self = const_cast<Directory*>(this);
  const String name = s.toString();

  if (!cacheResolvedChildren()) {
    if (isDetached())
      return LookupStatus::NotFound;
    File* candidate = nullptr;
    LookupStatus status = hasCookie ? self->resolveChildAt(cookie, name.view(), candidate)
                                    : self->resolveChild(name.view(), candidate);
    if (status != LookupStatus::Found || !candidate) {
      const bool invalid = candidate || status == LookupStatus::Found;
      delete candidate;
      return invalid ? LookupStatus::IoError : status;
    }
    if (isDetached()) {
      delete candidate;
      return LookupStatus::NotFound;
    }
    // No directory cache owns this reference. Keep the parent until the
    // last lookup/OFD reference retires the generated child.
    candidate->retainDetachedParent();
    VFS::instance().trackFile(candidate);
    ChildLease replacement;
    replacement.adopt(candidate);
    child.swap(replacement);
    return LookupStatus::Found;
  }

  while (true) {
    File* replacement = nullptr;
    InFlightLookup* lookup = nullptr;
    InFlightLookup* retiredLookup = nullptr;
    bool owner = false;
    LookupStatus completedResult = LookupStatus::Retry;

    {
      LockGuard<Mutex> guard(m_CacheLock);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      RetainedLookupHook hook = __atomic_load_n(&m_RetainedLookupHook, __ATOMIC_ACQUIRE);
      if (hook) {
        hook(self, nullptr, RetainedLookupPhase::BeforeLookup);
      }
#endif

      if (isDetached()) {
        return LookupStatus::NotFound;
      }

      InFlightLookupCache::LookupResult pending = m_InFlightLookups.lookup(s);
      if (pending.hasValue()) {
        lookup = pending.value();
        ++lookup->users;
        while (!lookup->complete) {
          lookup->changed.waitForCompletion(m_CacheLock);
        }
        completedResult = lookup->result;
        if (!--lookup->users) {
          retiredLookup = lookup;
        }
      } else {
        File* file = cachedFileLocked(s);
        if (file && VFS::instance().retainTrackedFile(file)) {
          replacement = file;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
          if (hook) {
            hook(self, file, RetainedLookupPhase::AfterRetain);
          }
#endif
        } else {
          lookup = new InFlightLookup;
          const bool inserted = m_InFlightLookups.insert(name, lookup);
          assert(inserted);
          owner = true;
        }
      }
    }

    delete retiredLookup;

    if (replacement) {
      ChildLease replacementLease;
      replacementLease.adopt(replacement);
      child.swap(replacementLease);
      return LookupStatus::Found;
    }

    if (!owner) {
      if (completedResult == LookupStatus::Found || completedResult == LookupStatus::Retry) {
        continue;
      }
      return completedResult;
    }

    File* candidate = nullptr;
    LookupStatus result = hasCookie ? self->resolveChildAt(cookie, name.view(), candidate)
                                    : self->resolveChild(name.view(), candidate);
    if (result == LookupStatus::Found && !candidate) {
      result = LookupStatus::IoError;
    } else if (result != LookupStatus::Found && candidate) {
      delete candidate;
      candidate = nullptr;
      result = LookupStatus::IoError;
    }

    {
      LockGuard<Mutex> guard(m_CacheLock);
      if (lookup->invalidated) {
        result = LookupStatus::Retry;
      } else if (result == LookupStatus::Found) {
        File* existing = cachedFileLocked(s);
        if (!existing) {
          VFS::instance().trackFile(candidate);
          DirectoryEntry* entry = new DirectoryEntry(candidate);
          const bool inserted = self->m_Cache.insert(name, entry);
          assert(inserted);
          const bool generationInserted =
              self->m_CacheGenerations.insert(name, self->nextCacheGeneration());
          assert(generationInserted);
          candidate = nullptr;
        }
      }

      lookup->result = result;
      lookup->complete = true;
      lookup->changed.broadcast();
      InFlightLookupCache::LookupResult published = m_InFlightLookups.lookup(s);
      if (published.hasValue() && published.value() == lookup) {
        m_InFlightLookups.remove(name);
      }
      if (!--lookup->users) {
        retiredLookup = lookup;
      }
    }

    delete candidate;
    delete retiredLookup;

    if (result == LookupStatus::Found || result == LookupStatus::Retry) {
      continue;
    }
    return result;
  }
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

#if HOSTED
size_t Directory::inFlightUsersForHostedTest(const HashedStringView& name) const {
  LockGuard<Mutex> guard(m_CacheLock);
  InFlightLookupCache::LookupResult pending = m_InFlightLookups.lookup(name);
  return pending.hasValue() ? pending.value()->users : 0;
}
#endif

void Directory::remove(const HashedStringView& s) {
  LockGuard<Mutex> namespaceGuard(m_NamespaceMutationLock);
  DirectoryEntry* residentEntry = nullptr;
  DirectoryEntry* cachedEntry = nullptr;
  {
    LockGuard<Mutex> guard(m_CacheLock);
    DirectoryEntryCache::LookupResult resident = m_ResidentEntries.lookup(s);
    if (resident.hasValue()) {
      residentEntry = resident.value();
      tombstoneResidentLocked(s);
      m_ResidentEntries.remove(s.toString());
      m_EphemeralEntries.remove(s.toString());
    }
    DirectoryEntryCache::LookupResult cached = m_Cache.lookup(s);
    if (cached.hasValue()) {
      cachedEntry = cached.value();
      m_Cache.remove(s.toString());
    }
    if (residentEntry || cachedEntry) {
      m_CacheGenerations.remove(s.toString());
    }
    InFlightLookupCache::LookupResult pending = m_InFlightLookups.lookup(s);
    if (pending.hasValue()) {
      pending.value()->invalidated = true;
    }
  }
  delete residentEntry;
  delete cachedEntry;
}

bool Directory::reserveDirectoryEntry(const HashedStringView& name, NameReservation& reservation) {
  assert(!reservation);
  const String ownedName = name.toString();
  m_NamespaceMutationLock.acquire();
  if (isDetached()) {
    m_NamespaceMutationLock.release();
    return false;
  }

  while (true) {
    InFlightLookup* retiredLookup = nullptr;
    {
      LockGuard<Mutex> guard(m_CacheLock);
      if (cachedFileLocked(name)) {
        m_NamespaceMutationLock.release();
        return false;
      }

      InFlightLookupCache::LookupResult pending = m_InFlightLookups.lookup(name);
      if (!pending.hasValue()) {
        InFlightLookup* lookup = new InFlightLookup;
        const bool inserted = m_InFlightLookups.insert(ownedName, lookup);
        assert(inserted);
        reservation.m_pDirectory = this;
        reservation.m_Name = ownedName;
        reservation.m_pToken = lookup;
        reservation.m_OwnsNamespaceLock = true;
        return true;
      }

      InFlightLookup* lookup = pending.value();
      ++lookup->users;
      while (!lookup->complete) {
        lookup->changed.waitForCompletion(m_CacheLock);
      }
      if (!--lookup->users) {
        retiredLookup = lookup;
      }
    }
    delete retiredLookup;
  }
}

void Directory::finishNameReservation(NameReservation& reservation, LookupStatus result) {
  const bool ownsNamespaceLock = reservation.m_OwnsNamespaceLock;
  InFlightLookup* retiredLookup = nullptr;
  {
    LockGuard<Mutex> guard(m_CacheLock);
    InFlightLookupCache::LookupResult pending =
        m_InFlightLookups.lookup(HashedStringView(reservation.m_Name));
    InFlightLookup* lookup = reinterpret_cast<InFlightLookup*>(reservation.m_pToken);
    assert(pending.hasValue() && pending.value() == lookup && !lookup->complete);

    lookup->result = result;
    lookup->complete = true;
    lookup->changed.broadcast();
    m_InFlightLookups.remove(reservation.m_Name);
    if (!--lookup->users) {
      retiredLookup = lookup;
    }
    reservation.m_pDirectory = nullptr;
    reservation.m_Name.clear();
    reservation.m_pToken = nullptr;
    reservation.m_OwnsNamespaceLock = false;
  }
  delete retiredLookup;
  if (ownsNamespaceLock) {
    m_NamespaceMutationLock.release();
  }
}

bool Directory::reserveRenameEntry(const String& name, NameReservation& reservation) {
  assert(!reservation);
  while (true) {
    InFlightLookup* retired = nullptr;
    {
      LockGuard<Mutex> guard(m_CacheLock);
      if (isDetached()) {
        return false;
      }
      auto pending = m_InFlightLookups.lookup(name);
      if (!pending.hasValue()) {
        InFlightLookup* lookup = new InFlightLookup;
        const bool inserted = m_InFlightLookups.insert(name, lookup);
        assert(inserted);
        reservation.m_pDirectory = this;
        reservation.m_Name = name;
        reservation.m_pToken = lookup;
        return true;
      }
      InFlightLookup* lookup = pending.value();
      ++lookup->users;
      while (!lookup->complete) {
        lookup->changed.waitForCompletion(m_CacheLock);
      }
      if (!--lookup->users) {
        retired = lookup;
      }
    }
    delete retired;
  }
}

void Directory::moveReservedEntry(NameReservation& sourceReservation, Directory* destination,
                                  NameReservation& destinationReservation, File* source) {
  assert(sourceReservation.m_pDirectory == this);
  assert(destinationReservation.m_pDirectory == destination);
  const String& oldName = sourceReservation.m_Name;
  const String& newName = destinationReservation.m_Name;
  DirectoryEntry* moved = nullptr;
  DirectoryEntry* retired = nullptr;
  bool resident = false;
  bool ephemeral = false;
  {
    LockGuard<Mutex> guard(m_CacheLock);
    auto residentEntry = m_ResidentEntries.lookup(oldName);
    resident = residentEntry.hasValue();
    if (resident) {
      moved = residentEntry.value();
    } else {
      auto cachedEntry = m_Cache.lookup(oldName);
      assert(cachedEntry.hasValue());
      moved = cachedEntry.value();
    }
    assert(moved->get() == source);
    ephemeral = m_EphemeralEntries.lookup(oldName).hasValue();
    if (resident) {
      tombstoneResidentLocked(HashedStringView(oldName));
      m_ResidentEntries.remove(oldName);
      m_EphemeralEntries.remove(oldName);
    } else {
      m_Cache.remove(oldName);
    }
    m_CacheGenerations.remove(oldName);
  }
  {
    LockGuard<Mutex> guard(destination->m_CacheLock);
    auto existing = destination->m_ResidentEntries.lookup(newName);
    if (existing.hasValue()) {
      retired = existing.value();
      destination->tombstoneResidentLocked(HashedStringView(newName));
      destination->m_ResidentEntries.remove(newName);
    } else {
      auto cachedEntry = destination->m_Cache.lookup(newName);
      if (cachedEntry.hasValue()) {
        retired = cachedEntry.value();
        destination->m_Cache.remove(newName);
      }
    }
    destination->m_EphemeralEntries.remove(newName);
    destination->m_CacheGenerations.remove(newName);
    if (resident) {
      const bool inserted = destination->m_ResidentEntries.insert(newName, moved);
      assert(inserted);
      destination->m_CacheGenerations.insert(newName, destination->m_ResidentOrder.count() + 1);
      destination->m_ResidentOrder.pushBack(newName);
      if (ephemeral) {
        destination->m_EphemeralEntries.insert(newName, true);
      }
    } else {
      const bool inserted = destination->m_Cache.insert(newName, moved);
      assert(inserted);
      destination->m_CacheGenerations.insert(newName, destination->nextCacheGeneration());
    }
  }
  delete retired;
}

bool Directory::addResidentDirectoryEntry(NameReservation& reservation, File* pTarget,
                                          bool ephemeral) {
  assert(pTarget != nullptr);
  return addResidentDirectoryEntry(reservation, new DirectoryEntry(pTarget), ephemeral, pTarget);
}

bool Directory::addResidentDirectoryEntry(NameReservation& reservation, DirectoryEntry* entry,
                                          bool ephemeral, File* createdTarget) {
  assert(reservation.m_pDirectory == this);
  assert(entry != nullptr);

  bool inserted = false;
  {
    LockGuard<Mutex> guard(m_CacheLock);
    const String& name = reservation.m_Name;
    if (!m_ResidentEntries.lookup(name).hasValue() && !m_Cache.lookup(name).hasValue()) {
      inserted = m_ResidentEntries.insert(name, entry);
      assert(inserted);
      const bool generationInserted = m_CacheGenerations.insert(name, m_ResidentOrder.count() + 1);
      m_ResidentOrder.pushBack(name);
      assert(generationInserted);
      if (ephemeral) {
        const bool kindInserted = m_EphemeralEntries.insert(name, true);
        assert(kindInserted);
      }
      m_bCachePopulated = true;
    }
  }

  if (inserted) {
    if (createdTarget) {
      publishEvent(FileEvents::Created, reservation.m_Name.view(), createdTarget->isDirectory());
    }
    reservation.complete(LookupStatus::Found);
  } else {
    delete entry;
  }
  return inserted;
}

bool Directory::addDirectoryEntry(const String& name, File* pTarget) {
  assert(pTarget != nullptr);
  NameReservation reservation;
  if (!reserveDirectoryEntry(HashedStringView(name), reservation)) {
    ERROR("can't add directory entry for '" << name << "' as it already exists.");
    return false;
  }
  VFS::instance().trackFile(pTarget);
  return addResidentDirectoryEntry(reservation, pTarget, false);
}

bool Directory::addDirectoryEntry(const String& name, DirectoryEntryMetadata&& meta) {
  NameReservation reservation;
  if (!reserveDirectoryEntry(HashedStringView(name), reservation)) {
    ERROR("can't add directory entry for '" << name << "' as it already exists.");
    return false;
  }
  return addResidentDirectoryEntry(reservation, new DirectoryEntry(pedigree_std::move(meta)),
                                   false);
}

bool Directory::addCachedDirectoryEntry(NameReservation& reservation, File* pTarget) {
  assert(pTarget != nullptr);
  assert(reservation.m_pDirectory == this);

  bool inserted = false;
  {
    LockGuard<Mutex> guard(m_CacheLock);
    const String& name = reservation.m_Name;
    if (!m_ResidentEntries.lookup(name).hasValue() && !m_Cache.lookup(name).hasValue()) {
      VFS::instance().trackFile(pTarget);
      DirectoryEntry* entry = new DirectoryEntry(pTarget);
      inserted = m_Cache.insert(name, entry);
      assert(inserted);
      const bool generationInserted = m_CacheGenerations.insert(name, nextCacheGeneration());
      assert(generationInserted);
    }
  }
  return inserted;
}

void Directory::invalidateDirectoryEntry(const HashedStringView& name) {
  DirectoryEntry* entry = nullptr;
  {
    LockGuard<Mutex> guard(m_CacheLock);
    DirectoryEntryCache::LookupResult result = m_Cache.lookup(name);
    if (result.hasValue()) {
      entry = result.value();
      m_Cache.remove(name.toString());
      m_CacheGenerations.remove(name.toString());
    }
    InFlightLookupCache::LookupResult pending = m_InFlightLookups.lookup(name);
    if (pending.hasValue()) {
      pending.value()->invalidated = true;
    }
  }
  delete entry;
}

bool Directory::removeDirectoryEntry(const HashedStringView& name, File* expected) {
  DirectoryEntry* entry = nullptr;
  {
    LockGuard<Mutex> guard(m_CacheLock);
    DirectoryEntryCache::LookupResult resident = m_ResidentEntries.lookup(name);
    if (!resident.hasValue() || resident.value()->get() != expected) {
      return false;
    }
    entry = resident.value();
    tombstoneResidentLocked(name);
    m_ResidentEntries.remove(name.toString());
    m_EphemeralEntries.remove(name.toString());
    m_CacheGenerations.remove(name.toString());
    InFlightLookupCache::LookupResult pending = m_InFlightLookups.lookup(name);
    if (pending.hasValue()) {
      pending.value()->invalidated = true;
    }
  }
  delete entry;
  return true;
}

Directory* Directory::getReparsePoint() const {
  return __atomic_load_n(&m_ReparseTarget, __ATOMIC_ACQUIRE);
}

void Directory::setReparsePoint(Directory* pTarget) {
  __atomic_store_n(&m_ReparseTarget, pTarget, __ATOMIC_RELEASE);
}

Directory::AddStatus Directory::addEphemeralFile(File* pFile) {
  assert(pFile != nullptr);
  NameReservation reservation;
  if (!reserveDirectoryEntry(HashedStringView(pFile->getName()), reservation)) {
    return isDetached() ? AddStatus::Detached : AddStatus::Exists;
  }

  File* backing = nullptr;
  LookupStatus status = resolveChild(pFile->getName().view(), backing);
  if (status == LookupStatus::Found && !backing) {
    status = LookupStatus::IoError;
  } else if (status != LookupStatus::Found && backing) {
    delete backing;
    backing = nullptr;
    status = LookupStatus::IoError;
  }

  if (status == LookupStatus::Found) {
    if (!addCachedDirectoryEntry(reservation, backing)) {
      delete backing;
      return AddStatus::Exists;
    }
    reservation.complete(LookupStatus::Found);
    return AddStatus::Exists;
  }
  if (status != LookupStatus::NotFound) {
    return AddStatus::IoError;
  }

  VFS::instance().trackFile(pFile);
  return addResidentDirectoryEntry(reservation, pFile, true) ? AddStatus::Added : AddStatus::Exists;
}

bool Directory::removeEphemeralFileLocked(const HashedStringView& name, File* expected) {
  DirectoryEntry* entry = nullptr;
  {
    LockGuard<Mutex> guard(m_CacheLock);
    if (!m_EphemeralEntries.lookup(name).hasValue()) {
      return false;
    }
    DirectoryEntryCache::LookupResult resident = m_ResidentEntries.lookup(name);
    if (!resident.hasValue() || resident.value()->get() != expected) {
      return false;
    }
    entry = resident.value();
    tombstoneResidentLocked(name);
    m_ResidentEntries.remove(name.toString());
    m_EphemeralEntries.remove(name.toString());
    m_CacheGenerations.remove(name.toString());
    InFlightLookupCache::LookupResult pending = m_InFlightLookups.lookup(name);
    if (pending.hasValue()) {
      pending.value()->invalidated = true;
    }
  }
  delete entry;
  return true;
}

bool Directory::empty() {
  while (true) {
    struct FirstEntry {
      String name;
      uint64_t cookie;
      bool found;
    } first = {String(), 0, false};
    auto captureFirst = [](void* opaque, const DirectoryEntryView& entry) -> bool {
      FirstEntry* first = reinterpret_cast<FirstEntry*>(opaque);
      first->name = entry.name.toString();
      first->cookie = entry.currentCookie;
      first->found = true;
      return false;
    };

    uint64_t cookie = 2;
    const ReadStatus readStatus = enumerate(cookie, captureFirst, &first);
    if (readStatus == ReadStatus::IoError) {
      return false;
    }
    if (!first.found) {
      return readStatus == ReadStatus::Complete;
    }

    ChildLease child;
    const LookupStatus lookup = lookupChildAt(first.cookie, HashedStringView(first.name), child);
    if (lookup == LookupStatus::Retry) {
      continue;
    }
    // A driver may deliberately enumerate an entry type it cannot
    // materialise (for example, an unsupported device inode). Treat that as
    // a failed empty operation instead of selecting the same stable record
    // forever.
    if (lookup == LookupStatus::NotFound) {
      return false;
    }
    if (lookup != LookupStatus::Found) {
      return false;
    }
    if (!getFilesystem()->removeChild(this, first.name, child.get())) {
      return false;
    }
  }
}

void Directory::markDetached() {
  if (isDetached()) {
    return;
  }

  retainDetachedParent();
  __atomic_store_n(&m_Detached, true, __ATOMIC_RELEASE);
}

void Directory::emptyCache() {
  LockGuard<Mutex> namespaceGuard(m_NamespaceMutationLock);
  Vector<DirectoryEntry*> entries;
  {
    LockGuard<Mutex> guard(m_CacheLock);
    for (auto it : m_Cache) {
      entries.pushBack(it);
    }
    for (auto it : m_ResidentEntries) {
      entries.pushBack(it);
    }
    for (DirectoryEntry* entry : entries) {
      if (entry->active()) {
        entry->get()->retainDetachedParent();
      }
    }

    m_Cache.clear();
    m_ResidentEntries.clear();
    m_ResidentOrder.clear();
    m_EphemeralEntries.clear();
    m_CacheGenerations.clear();
    m_bCachePopulated = false;
    for (auto it : m_InFlightLookups) {
      it->invalidated = true;
    }
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

Directory::LookupStatus Directory::resolveChild(const StringView&, File*& child) {
  child = nullptr;
  return LookupStatus::NotFound;
}

Directory::LookupStatus Directory::resolveChildAt(uint64_t, const StringView& name, File*& child) {
  return resolveChild(name, child);
}

Directory::ReadStatus Directory::readDirectory(uint64_t&, DirectoryEntryEmitter, void*) {
  return ReadStatus::Complete;
}

void Directory::preallocateDirectoryEntries(size_t count) {
  LockGuard<Mutex> guard(m_CacheLock);
  m_ResidentEntries.reserve(count);
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

void Directory::tombstoneResidentLocked(const HashedStringView& name) {
  DirectoryEntryGenerationCache::LookupResult generation = m_CacheGenerations.lookup(name);
  if (!generation.hasValue() || !generation.value() ||
      generation.value() > m_ResidentOrder.count()) {
    return;
  }

  String& slot = m_ResidentOrder[generation.value() - 1];
  if (slot == name) {
    slot.clear();
  }
}

File* Directory::cachedFileLocked(const HashedStringView& name) const {
  DirectoryEntryCache::LookupResult resident = m_ResidentEntries.lookup(name);
  if (resident.hasValue()) {
    return resident.value()->get();
  }
  DirectoryEntryCache::LookupResult cached = m_Cache.lookup(name);
  if (cached.hasValue()) {
    return cached.value()->get();
  }
  return nullptr;
}

Directory::EntryType Directory::entryType(File* file) {
  if (!file) {
    return EntryType::Unknown;
  }
  if (file->isDirectory()) {
    return EntryType::Directory;
  }
  if (file->isSymlink()) {
    return EntryType::Symlink;
  }
  if (file->isFifo()) {
    return EntryType::Fifo;
  }
  if (file->isSocket()) {
    return EntryType::Socket;
  }
  return EntryType::Regular;
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
