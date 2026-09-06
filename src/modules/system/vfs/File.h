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

#ifndef FILE_H
#define FILE_H
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Readiness.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Cache.h"
#include "pedigree/kernel/utilities/CacheConstants.h"
#include "pedigree/kernel/utilities/HashTable.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/new"

#include <config.h>

#include "FileEvent.h"

class Filesystem;
class Thread;

// RWX for owner.
#define FILE_UR 0001
#define FILE_UW 0002
#define FILE_UX 0004
#define FILE_UMASK 0007
#define FILE_UBITS 0
// RWX for group.
#define FILE_GR 0010
#define FILE_GW 0020
#define FILE_GX 0040
#define FILE_GMASK 0070
#define FILE_GBITS 3
// RWX for others.
#define FILE_OR 0100
#define FILE_OW 0200
#define FILE_OX 0400
#define FILE_OMASK 0700
#define FILE_OBITS 6
// Ancillary file bits.
#define FILE_STICKY 01000
#define FILE_AMASK 07000
#define FILE_ABITS 9

#define FILE_BAD_BLOCK static_cast<uintptr_t>(-1)

/** A File is a regular file - it is also the superclass of Directory, Symlink
    and Pipe. */
class EXPORTED_PUBLIC File : public ReadinessSource, public FileEventSource {
  friend class Filesystem;
  friend class Directory;
#if defined(PEDIGREE_BUILDUTILS)
  friend class Ext2FillCacheTestPeer;
  friend class Ext2WritebackTestPeer;
#endif

 public:
  class ParentLease {
   public:
    ParentLease();
    ~ParentLease();
    File* get() const {
      return m_Parent;
    }
    void swap(ParentLease& other);

   private:
    friend class File;
    NOT_COPYABLE_OR_ASSIGNABLE(ParentLease);
    File* m_Parent;
    bool m_Retained;
#if THREADS && !defined(STANDALONE_MUTEXES)
    TerminationDeferral m_TerminationDeferral;
#endif
  };

  /** Holds this file's write transaction lock across one or more fragments. */
  class WriteGuard {
   public:
    uint64_t write(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock = true);
    uint64_t append(uint64_t size, uintptr_t buffer, uint64_t& location, bool bCanBlock = true);

   private:
    friend class File;

    explicit WriteGuard(File& file);

    File& m_File;
    LockGuard<Mutex> m_Guard;
  };

  /** Constructor, creates an invalid file. */
  File();

  /** Copy constructors are hidden - unused! */
 private:
  File(const File& file);
  File& operator=(const File&);

 public:
  /** Constructor, should be called only by a Filesystem. */
  File(const String& name, Time::Timestamp accessedTime, Time::Timestamp modifiedTime,
       Time::Timestamp creationTime, uintptr_t inode, class Filesystem* pFs, size_t size,
       File* pParent);
  /** Destructor - doesn't do anything. */
  virtual ~File();

  /** Reads from the file.
   *  \param[in] buffer Buffer to write the read data into. Can be null, in
   *      which case the data can be found by calling getPhysicalPage.
   *  \param[in] bCanBlock Whether or not the File can block when reading
   */
  virtual uint64_t read(uint64_t location, uint64_t size, uintptr_t buffer,
                        bool bCanBlock = true) final;
  /** Writes to the file.
   *  \param[in] bCanBlock Whether or not the File can block when reading
   */
  virtual uint64_t write(uint64_t location, uint64_t size, uintptr_t buffer,
                         bool bCanBlock = true) final;

  /** Atomically selects EOF and performs one append write. */
  uint64_t append(uint64_t size, uintptr_t buffer, uint64_t& location, bool bCanBlock = true);

  /** Begin a multi-fragment write which must not interleave with another writer. */
  WriteGuard lockWrites();

  /** Get the physical address for the given offset into the file.
   * Returns (physical_uintptr_t) ~0 if the offset isn't in the cache.
   */
  virtual physical_uintptr_t getPhysicalPage(size_t offset);
  virtual bool isDirectPhysicalMapping() const {
    return false;
  }

  /**
   * Specifies that the system is done with the physical page retrieved
   * from getPhysicalPage now. Allows the physical page to be evicted
   * from the file cache again.
   */
  virtual void returnPhysicalPage(size_t offset);
  /** Memory-pressure compaction must never wait on a backing mutation. */
  virtual bool tryBeginMappingRelease();
  void endMappingRelease();

  /**
   * Syncs a mapped page before releasing its backing-cache reference.
   */
  bool syncAndReturnPhysicalPage(size_t offset, bool async);

  /**
   * Sync all cached pages for the file back to disk.
   *
   * Returns false if any cached page cannot be synchronised. Successful
   * pages remain written when another page fails.
   */
  virtual bool sync();

  /**
   * Trigger a sync of an inner cache back to disk and report completion
   * (or queue admission for an asynchronous request).
   */
  virtual bool sync(size_t offset, bool async);

  /** Returns the time the file was created. */
  Time::Timestamp getCreationTime();
  /** Sets the time the file was created. */
  void setCreationTime(Time::Timestamp t);

  /** Returns the time the file was last accessed. */
  Time::Timestamp getAccessedTime();
  /** Sets the time the file was last accessed. */
  void setAccessedTime(Time::Timestamp t);

  /** Returns the time the file was last modified. */
  Time::Timestamp getModifiedTime();
  /** Sets the time the file was last modified. */
  void setModifiedTime(Time::Timestamp t);

  struct Attributes {
    Time::Timestamp accessed = 0;
    Time::Timestamp modified = 0;
    Time::Timestamp changed = 0;
    size_t uid = 0;
    size_t gid = 0;
    uint32_t permissions = 0;
    size_t size = 0;
    size_t links = 1;
    uint64_t blocks = 0;
  };

  enum AttributeMask : uint32_t {
    AccessTime = 1U << 0,
    ModifyTime = 1U << 1,
    ChangeTime = 1U << 2,
    Owner = 1U << 3,
    Group = 1U << 4,
    Permissions = 1U << 5
  };

  virtual Attributes getAttributes() const;

  /** Prepare backing storage before a writable shared mapping is published. */
  virtual bool prepareSharedMapping(size_t offset, size_t length);

  /** Returns the name of the file. */
  String getName() const;
  void getName(String& s) const;

  /** Obtains the full path of the File in the root namespace. */
  virtual void getFullPath(String& result, bool bWithMount = true);

  /** Obtains the full path of the File and returns it. */
  virtual String getFullPath(bool bWithMount = true);

  /** Delete all data from the file. */
  virtual void truncate();

  /** Resize a regular file without changing any open description's position. */
  bool resize(size_t size);

  virtual size_t getSize();
  void setSize(size_t sz);

  /** Returns true if the File is actually a symlink. */
  virtual bool isSymlink();

  /** Returns true if the File is actually a directory. */
  virtual bool isDirectory();

  /** Returns true if the File is actually a pipe. */
  virtual bool isPipe() const;

  /** Returns true if the File is actually a fifo. */
  virtual bool isFifo() const;

  /** Returns true if the File is actually a socket. */
  virtual bool isSocket() const;

  /** Whether ordinary reads and writes consume a persistent byte offset. */
  virtual bool isSeekable() const;

  /** Whether the backing supports ordinary file range operations. */
  bool supportsRegularFileOperations();

  uintptr_t getInode() const;
  virtual void setInode(uintptr_t inode);

  /** Stable backing token; aliases of one backing object must share it. */
  virtual uintptr_t futexIdentity();

  Filesystem* getFilesystem() const;
  void setFilesystem(Filesystem* pFs);

  virtual void fileAttributeChanged();

  /** Publish a completed VFS operation to payload event subscribers. */
  void publishEvent(FileEventMask mask, const StringView& name = StringView(),
                    bool targetIsDirectory = false);

  virtual void increaseRefCount(bool bIsWriter);
  virtual void decreaseRefCount(bool bIsWriter);

  /**
   * Attempts to retain this File through its existing VFS ownership record.
   * Untracked filesystem roots remain borrowed and return false. Virtual
   * dispatch lets kernel-owned contexts use the loadable VFS lifetime model
   * without creating a kernel-to-module link dependency.
   */
  virtual bool retainVfsReference();

  /** Releases a reference acquired by retainVfsReference(). */
  virtual void releaseVfsReference();

  /** Whether this untracked File is the externally owned filesystem root. */
  virtual bool isStableVfsRoot() const;

  void setPermissions(uint32_t perms);
  uint32_t getPermissions() const;

  void setUid(size_t uid);
  size_t getUid() const;

  void setGid(size_t gid);
  size_t getGid() const;

  File* getParent() const;

  /** Snapshot a name and retain its parent across concurrent namespace moves. */
  void getNamespace(ParentLease& parent, String& name) const;

  /** Similar to POSIX's select() function
   * \return 1 if ready for reading/writing, 0 otherwise
   * \note Default implementation says always ready to read/write
   *       so be sure to override if that's not right
   */
  virtual int select(bool bWriting = false, int timeout = 0);

  /** Return the current readiness state for this open endpoint. */
  virtual ReadyMask queryReady(bool reading, bool writing);

  /** Whether readiness transitions are reported through ReadinessSource. */
  virtual bool supportsReadinessNotifications() const;

  /**
   * Causes the event pEvent to be dispatched to pThread when activity occurs
   * on this File. Activity includes the file becoming available for reading,
   * writing or erroring. */
  void monitor(Thread* pThread, Event* pEvent);

  /** Walks the monitor-target queue, removing all for \p pThread .*/
  void cullMonitorTargets(Thread* pThread);

  /** Walks the monitor-target queue, removing all for \p pEvent. */
  void cullMonitorTargets(Event* pEvent);

  /** Does this File object support the given integer-based command? */
  virtual bool supports(const size_t command) const;

  /** Handle a command. */
  virtual int command(const size_t command, void* buffer);

  /** Function to retrieve the block size returned by readBlock.
      \note This must be constant throughout the life of the file. */
  virtual size_t getBlockSize() const;

  /** Enables direct mode (no File-level cache). */
  void enableDirect();

  /** Disables direct mode (use File-level cache). */
  void disableDirect();

  /** Optionally preallocates blocks to fit the given size. */
  virtual void preallocate(size_t expectedSize, bool zero = true);

  /** Obtain the actual File object to use when opening this file.
   *
   * This allows a File to exist and perform logic on an open() before
   * returning either itself or some other File object. Some files require
   * side effects as part of their opening, this facilitates that.
   */
  virtual File* open();

 protected:
  virtual void updateAttributes(const Attributes& attributes, uint32_t mask);
  virtual bool prepareWrite(uint64_t location, uint64_t size);
  virtual bool resizeFile(size_t size);
  virtual Mutex& writeSerializationLock();
  virtual Mutex& dataMutationLock();
  virtual size_t& physicalPageLoans();
  /** Caller owns the data mutation lock and has excluded physical-page loans. */
  bool clearDataCache();
  /**
   * File subclasses can define this and return true if they require read()
   * calls to perform actual data reads, and false if readBlock() is
   * otherwise sufficient.
   */
  virtual bool isBytewise() const;

  /** Reads bytes from the file, if isBytewise() == true. */
  virtual uint64_t readBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                bool bCanBlock = true);
  /** Writes bytes to the file, if isBytewise() == true. */
  virtual uint64_t writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                 bool bCanBlock = true);

  /**
   * Retrieves an aligned block with one caller-owned lifetime reference.
   *
   * Every successful call must be balanced by exactly one unpinBlock() at
   * the same location. Cache-backed implementations must acquire a reference
   * in addition to the cache entry's publication reference on both hit and
   * miss paths.
   */
  virtual uintptr_t readBlock(uint64_t location);
  /**
   * Internal function to write a block retrieved with readBlock back to
   * the file. The address of the block is provided for convenience.
   */
  virtual void writeBlock(uint64_t location, uintptr_t addr);

  /** Writes a range of whole filesystem blocks back to the file. */
  virtual void writeBlocks(uint64_t location, uintptr_t addr, size_t length);

  /** Internal function to extend a file to be at least the given size. */
  virtual void extend(size_t newSize);

  /**
   * Does the same thing as extend() but with an optional hint indicating
   * that the upcoming write will fill an entire block. This hint can be
   * used to do things like avoid zeroing the block that would otherwise
   * be written over anyway.
   */
  virtual void extend(size_t newSize, uint64_t location, uint64_t size);

  /** Internal function to notify all registered MonitorTargets. */
  void dataChanged();

  /** Internal function to get the filesystem label for this file. */
  void getFilesystemLabel(HugeStaticString& s);

  /**
   * Called by a cache to write back changed data to disk.
   *
   * File subclasses that use a Cache for readBlock can utilise this
   * as the callback on their Cache instance to get a write-back
   * notification.
   */
  static bool writeCallback(CacheConstants::CallbackCause cause, uintptr_t loc, uintptr_t page,
                            void* meta);

  static bool fillCacheCallback(CacheConstants::CallbackCause cause, uintptr_t loc, uintptr_t page,
                                void* meta);

  /** Installs and drains the native-page fill-cache callback. */
  void enableFillCacheWriteback();
  void shutdownFillCacheWriteback();

  /** Reports writeback success separately from an absent fill page. */
  bool syncFillCache(size_t offset, bool async, bool& present);

  /** Whether this file currently uses native-page fill caching. */
  virtual bool useFillCache() const;

  /**
   * Pins the given page.
   *
   * If your File subclass uses a Cache for readBlock, this method should
   * be implemented to call Cache::pin. The VFS layer calls this method
   * when it determines it is about to give a physical page to an upper
   * layer, and therefore will be unable to guarantee the virtual page's
   * dirty status is a correct reflection of the page's state.
   *
   * Returns true only when the page currently published for \p location is
   * held until the matching unpinBlock(). The base implementation returns
   * false so a cache-backed subclass cannot silently claim unsafe pages.
   */
  MUST_USE_RESULT virtual bool pinBlock(uint64_t location);

  /**
   * Unpins the given page.
   */
  virtual void unpinBlock(uint64_t location);

  /**
   * Removes the given location from the VFS-level File cache.
   *
   * Each File offers read() and write(), which in turn call readBlock.
   * The result from readBlock is cached at the File object level in such
   * a way that requires notification from the File subclass when the
   * address returned from readBlock is no longer valid.
   */
  void evict(uint64_t location);

  /** Set permissions without raising fileAttributeChanged. */
  void setPermissionsOnly(uint32_t perms);

  /** Set UID without raising fileAttributeChanged. */
  void setUidOnly(size_t uid);

  /** Set GID without raising fileAttributeChanged. */
  void setGidOnly(size_t gid);

  /** Keep the former parent alive after this node leaves its namespace. */
  void retainDetachedParent();

  void moveNamespace(const String& name, File* parent);

  String m_Name;
  Time::Timestamp m_AccessedTime;
  Time::Timestamp m_ModifiedTime;
  Time::Timestamp m_CreationTime;
  uintptr_t m_Inode;
  uintptr_t m_FutexIdentity = 0;

  class Filesystem* m_pFilesystem;
  size_t m_Size;

  File* m_pParent;
  mutable Mutex m_MetadataLock;

  /** Pins a former tracked parent for this detached node's remaining life. */
  File* m_pDetachedParent;
  bool m_bDetachedParentHandled;

  size_t m_nWriters, m_nReaders;

  size_t m_Uid;
  size_t m_Gid;
  uint32_t m_Permissions;

 public:
  class DataCacheKey {
   public:
    DataCacheKey() = default;
    DataCacheKey(size_t block) : m_Block(block) {}
    ~DataCacheKey() = default;

    size_t hash() const {
      return m_Block;
    }

    bool operator==(const DataCacheKey& other) const {
      return m_Block == other.m_Block;
    }

   private:
    size_t m_Block = ~static_cast<size_t>(0);
  };

  struct CacheState {
    CacheState();
    HashTable<DataCacheKey, uintptr_t> data;
    Mutex indexLock;
    Cache fill;
    Mutex fillLock;
  };

 protected:
  virtual CacheState& cacheState();
  CacheState m_CacheState;

  bool m_bDirect;
#if defined(PEDIGREE_BUILDUTILS)
  bool m_bForceFillCache = false;
#endif

  /** Serializes file mutation, including append EOF selection, across open descriptions. */
  Mutex m_WriteLock;
  Mutex m_DataMutationLock;
  size_t m_PhysicalPageLoans = 0;

  Mutex m_Lock;

  struct MonitorTarget {
    MonitorTarget(Thread* pT, Event* pE, Event::SendLease registration)
        : pThread(pT), pEvent(pE), eventRegistration(pedigree_std::move(registration)) {}
    Thread* pThread;
    Event* pEvent;
    Event::SendLease eventRegistration;
  };

  List<MonitorTarget*> m_MonitorTargets;

 private:
  /** Performs a write while m_WriteLock is already held. */
  uint64_t writeUnlocked(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock);

  /** Retrieve a page from our cache. */
  uintptr_t getCachedPage(size_t block, bool locked = true);

  /** Set a page in our cache. */
  void setCachedPage(size_t block, uintptr_t value, bool locked = true);

  /**
   * Reads the given cache block and returns one per-use reference.
   *
   * The block index does not own this reference.
   */
  uintptr_t readIntoCache(uintptr_t block);

  /** Releases the per-use reference returned by readIntoCache(). */
  void releaseReadReference(uintptr_t block);
};

#endif
