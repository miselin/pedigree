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

#ifndef DIRECTORY_H
#define DIRECTORY_H
#include <config.h>

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Mutex.h"
#if THREADS && !defined(STANDALONE_MUTEXES)
#include "pedigree/kernel/process/TerminationDeferral.h"
#endif
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/HashTable.h"
#include "pedigree/kernel/utilities/LazyEvaluate.h"
#include "pedigree/kernel/utilities/Pointers.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/utility.h"

#include "File.h"

class StringView;
class HashedStringView;

/**
 * A Directory node.
 *
 * \todo Need to add a way to count # of times a particular lookup has happened
 *       so we can potentially offer a way to cull directory entries that are
 *       otherwise just consuming space.
 **/
class EXPORTED_PUBLIC Directory : public File {
  friend class Filesystem;

 public:
  class EXPORTED_PUBLIC ChildLease {
   public:
    ChildLease();
    ~ChildLease();

    File* get() const {
      return m_pFile;
    }

    explicit operator bool() const {
      return m_pFile != nullptr;
    }

    void reset();

   private:
    friend class Directory;

    ChildLease(const ChildLease&) = delete;
    ChildLease& operator=(const ChildLease&) = delete;
    ChildLease(ChildLease&&) = delete;
    ChildLease& operator=(ChildLease&&) = delete;

    void adopt(File* file);

    File* m_pFile;
#if THREADS && !defined(STANDALONE_MUTEXES)
    // A lease's deferral follows its lexical lifetime. Dynamically arming an
    // older object would cross any newer guard's strict LIFO scope.
    TerminationDeferral m_TerminationDeferral;
#endif
  };

  /** Eases the pain of casting, and performs a sanity check. */
  static Directory* fromFile(File* pF) {
    if (!pF->isDirectory())
      FATAL("Casting non-directory File to Directory!");
    return reinterpret_cast<Directory*>(pF);
  }

  /** Constructor, creates an invalid directory. */
  Directory();

  /** Copy constructors are hidden - unused! */
 private:
  Directory(const Directory& file);
  Directory& operator=(const Directory&);

 public:
  /** Constructor, should be called only by a Filesystem. */
  Directory(const String& name, Time::Timestamp accessedTime, Time::Timestamp modifiedTime,
            Time::Timestamp creationTime, uintptr_t inode, class Filesystem* pFs, size_t size,
            File* pParent);
  /** Destructor - doesn't do anything. */
  virtual ~Directory();

  /** Returns true if the File is actually a directory. */
  virtual bool isDirectory() {
    return true;
  }

  /** Returns the n'th child of this directory, or an invalid file. */
  File* getChild(size_t n);

  /** Returns the number of children in this directory. */
  size_t getNumChildren();

  /** Load the directory's contents into the cache. */
  virtual void cacheDirectoryContents();

  /** Does this directory have cache? */
  virtual bool isCachePopulated() const;

  /** Look up the given filename in the directory. */
  File* lookup(const HashedStringView& s) const;

  /**
   * Atomically retain the current child of an already-stable directory.
   * A previous output lease is preserved on failure and released only after
   * the replacement lookup has finished using this directory. Leases are
   * thread-affine and must have strictly lexical stack lifetimes on the Thread
   * which acquired them.
   */
  MUST_USE_RESULT bool lookupRetained(const HashedStringView& s, ChildLease& child) const;

  /** Remove the given filename in the directory. */
  void remove(const HashedStringView& s);

  /**
   * \brief Get the reparse point attached to this directory.
   * Reparse points allow locations on the filesystem to redirect lookups to
   * a separate directory. While the reparse point is active, the target
   * directory is used for lookups instead of this one.
   * The returned pointer is borrowed; atomic publication does not pin the
   * target directory's lifetime.
   */
  Directory* getReparsePoint() const;

  /** Set/unset the reparse point for this directory. */
  void setReparsePoint(Directory* pTarget);

  /**
   * \brief Add an ephemeral file to the directory
   *
   * This is used to store files that need to be visible in the VFS but are
   * not backed by a "real" file on disk. For example, a socket might need to
   * be present with a filesystem path but should not be written to disk.
   */
  bool addEphemeralFile(File* pFile);

  /**
   * Empty the entire directory, deleting all files within it
   * (non-recursively). This does NOT check that the directory is "empty"
   * first.
   */
  bool empty();

  /**
   * Empty the entire directory cache.
   */
  void emptyCache();

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  enum class RetainedLookupPhase { BeforeLookup, AfterRetain };
  using RetainedLookupHook = void (*)(Directory*, File*, RetainedLookupPhase);
  static void setRetainedLookupHookForHostedTest(RetainedLookupHook hook);
  const void* cacheLockAddressForHostedTest() const {
    return static_cast<const Semaphore*>(&m_CacheLock);
  }
  bool tryCacheLockForHostedTest() const;
#endif

 protected:
  struct DirectoryEntryMetadata {
    DirectoryEntryMetadata();
    DirectoryEntryMetadata(DirectoryEntryMetadata&& other);

    ~DirectoryEntryMetadata();

    // No copy construction (UniqueArray)
    NOT_COPYABLE_OR_ASSIGNABLE(DirectoryEntryMetadata);

    // These two should always be known at metadata creation time.
    Directory* pDirectory;
    String filename;

    // Space for anything else to be stored by the filesystem.
    UniqueArray<char> opaque;
  };

 private:
  static File* evaluateEntry(const DirectoryEntryMetadata& meta);
  static void destroyEntry(File* file);

  virtual bool isBytewise() const {
    // This will cause read()/write() to fail as readBytewise/writeBytewise
    // are not overridden and remain as their default implementation, which
    // errors out.
    return true;
  }

 protected:
  typedef LazyEvaluate<File, DirectoryEntryMetadata, evaluateEntry, destroyEntry> DirectoryEntry;

 private:
  typedef HashTable<String, DirectoryEntry*, HashedStringView> DirectoryEntryCache;
  typedef HashTable<String, uint64_t, HashedStringView> DirectoryEntryGenerationCache;

  /** Directory contents cache. */
  DirectoryEntryCache m_Cache;

  /** Publication identity for cache entries across callback unlocks. */
  DirectoryEntryGenerationCache m_CacheGenerations;
  uint64_t m_NextCacheGeneration;

  /**
   * Whether the directory cache is populated with entries or still needs to
   * be loaded. Directories are lazy-loaded using this.
   */
  bool m_bCachePopulated;

  /** Reparse target. */
  Directory* m_ReparseTarget = nullptr;

  /** Serialises child cache membership and evaluation. */
  mutable Mutex m_CacheLock;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  static RetainedLookupHook m_RetainedLookupHook;
#endif

 protected:
  /** Provides subclasses with direct access to the directory's listing. */
  virtual const DirectoryEntryCache& getCache() {
    return m_Cache;
  }

  /** Mark the directory cache as populated now. */
  void markCachePopulated();

  /** Add an entry to the directory. */
  void addDirectoryEntry(const String& name, File* pTarget);

  /** Add a lazily-evaluated entry to the directory. */
  void addDirectoryEntry(const String& name, DirectoryEntryMetadata&& meta);

  /** Preallocate space for the given number of directory entries. */
  void preallocateDirectoryEntries(size_t count);

  /** Convert given metadata into a useful File object. */
  virtual File* convertToFile(const DirectoryEntryMetadata& meta);

 private:
  uint64_t nextCacheGeneration();
  void removeIfGeneration(const HashedStringView& name, uint64_t generation);
};

extern template class HashTable<String, Directory::DirectoryEntry*, HashedStringView>;

#endif
