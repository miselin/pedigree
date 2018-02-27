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

#ifndef CACHE_H
#define CACHE_H

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/BloomFilter.h"
#include "pedigree/kernel/utilities/MemoryAllocator.h"
#include "pedigree/kernel/utilities/RequestQueue.h"
#include "pedigree/kernel/utilities/Tree.h"
#include "pedigree/kernel/utilities/UnlikelyLock.h"

#include "pedigree/kernel/machine/TimerHandler.h"

#include "pedigree/kernel/process/MemoryPressureManager.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"

#include "pedigree/kernel/utilities/CacheConstants.h"

/// The age at which a cache page is considered "old" and can be evicted
/// This is expressed in seconds.
#define CACHE_AGE_THRESHOLD 10

/// In the case where no pages are old enough, this is the number of pages that
/// will be force-freed.
#define CACHE_NUM_THRESHOLD 2

/// How regularly (in milliseconds) the writeback timer handler should fire.
#define CACHE_WRITEBACK_PERIOD 500

// Forward declaration of Cache so CacheManager can be defined first
class Cache;

/** Provides a clean abstraction to a set of data caches. */
class CacheManager : public TimerHandler, public RequestQueue
{
  public:
    CacheManager();
    virtual ~CacheManager();

    static CacheManager &instance()
    {
        return m_Instance;
    }

    void initialise();

    void registerCache(Cache *pCache);
    void unregisterCache(Cache *pCache);

    /**
     * Trim each cache we know about until 'count' pages have been evicted.
     */
    bool trimAll(size_t count = 1);

    virtual void timer(uint64_t delta, InterruptState &state);

#ifdef THREADS
    void trimThread();
#endif

  private:
    /**
     * RequestQueue doer - children give us new jobs, and we call out to
     * them when they hit the front of the queue.
     */
    virtual uint64_t executeRequest(
        uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5,
        uint64_t p6, uint64_t p7, uint64_t p8);

    /**
     * Used to ensure we only ever fire a WriteBack for the same page once -
     * that is, we don't constantly write back the same page over and over
     * while it's still queued.
     */
    virtual bool compareRequests(const Request &a, const Request &b)
    {
        // p2 = CallbackCause, p3 = key in m_Pages
        return (a.p2 == b.p2) && (a.p3 == b.p3);
    }

    static CacheManager m_Instance;

    List<Cache *> m_Caches;

#ifdef THREADS
    Thread *m_pTrimThread;
#endif

    bool m_bActive;
};

/** Provides an abstraction of a data cache. */
class Cache
{
  private:
    struct CachePage
    {
        /// Key for this page.
        uintptr_t key;

        /// The location of this page in memory
        uintptr_t location;

        /// Reference count to handle release() being called with multiple
        /// threads having access to the page.
        size_t refcnt;

        /// Checksum of the page's contents (for dirty detection).
        uint64_t checksum[2];

        /// Marker to check that a page's contents are in flux.
        bool checksumChanging;

        /// Current page status.
        enum Status
        {
            // The page is being edited and should not be considered for any
            // writeback operation.
            Editing,
            // The page has been marked as no longer being edited and should
            // only have a checksum calculated, but no writeback.
            EditTransition,
            // The checksum is in flux.
            ChecksumChanging,
            // The checksum was in flux but is now stable. A transition into
            // this state will trigger a writeback.
            ChecksumStable
        } status;

        /// Linked list components for LRU.
        CachePage *pNext;
        CachePage *pPrev;

        /// Check the checksum against another.
        bool checkChecksum(uint64_t other[2]) const;

        /// Check for an unset checksum.
        bool checkZeroChecksum() const;
    };

  public:
    /**
     * Callback type: for functions called by the write-back timer handler.
     *
     * The write-back handler checks all pages in the cache at a regular
     * interval. If it finds a dirty page, it calls the Cache callback,
     * which should write the modified data back to a backing store, if
     * any exists.
     *
     * Then, the write-back thread will mark the page as not-dirty.
     */
    typedef void (*writeback_t)(
        CacheConstants::CallbackCause cause, uintptr_t loc, uintptr_t page,
        void *meta);

    Cache(size_t pageConstraints = 0);
    virtual ~Cache();

    /** Set the write back callback to the given function. */
    void setCallback(writeback_t newCallback, void *meta);

    /** Looks for \p key , increasing \c refcnt by one if returned. */
    uintptr_t lookup(uintptr_t key);

    /**
     * Creates a cache entry with the given key.
     *
     * The new entry will already be marked as being edited, and so won't be
     * written back until the inserter calls markNoLongerEditing again.
     */
    uintptr_t insert(uintptr_t key);

    /** Creates a bunch of cache entries to fill a specific size. Note that
     *  this is just a monster allocation of a virtual address - the physical
     *  pages are NOT CONTIGUOUS.
     */
    uintptr_t insert(uintptr_t key, size_t size);

    /** Checks if the entire range specified exists in the cache. */
    bool exists(uintptr_t key, size_t length);

    /**
     * Evicts the given key from the cache, also freeing the memory it holds.
     *
     * This will respect the refcount of the given key, so as to make pin()
     * exhibit more reliable behaviour.
     */
    bool evict(uintptr_t key);

    /**
     * Empties the cache.
     *
     * Will not respect refcounts.
     */
    void empty();

    /** Decreases \p key 's \c refcnt by one. */
    void release(uintptr_t key);

    /**
     * Increases \p key 's \c refcnt by one.
     *
     * This is used for places that, for example, use the physical address of
     * a cache page and therefore will never set the dirty flag of a virtual
     * page. This use case will need to provide its own means for writing data
     * back to the backing store, if that is desirable.
     *
     * Pinned pages will not be freed during a compact().
     *
     * \return false if key didn't exist, true otherwise
     */
    bool pin(uintptr_t key);

    /**
     * Attempts to trim the cache.
     *
     * A trim is slightly different to a compact in that it is designed to be
     * called in a non-emergency situation. This could be called, for example,
     * after a process terminates, to clean up some old cached data while the
     * system is already doing busywork. Or, it could be called when the system
     * is idle to clean up a bit.
     *
     * This will take the lock, also, unlike compact().
     */
    size_t trim(size_t count = 1);

    /**
     * Synchronises the given cache key back to a backing store, if a
     * callback has been assigned to the Cache.
     */
    void sync(uintptr_t key, bool async);

    /**
     * Triggers the cache to calculate the checksum of the given location.
     * This may be useful to avoid a spurious writeback when reading data into
     * a cache page for the first time.
     */
    void triggerChecksum(uintptr_t key);

    /**
     * Enters a critical section with respect to this cache. That is, do not
     * permit write back callbacks to be fired (aside from as a side effect
     * of eviction) until the section has been left.
     *
     * This is especially useful for an 'insert then read into buffer'
     * operation, which can cause a writeback in the middle of reading (when
     * nothing has actually changed at all).
     */
    void startAtomic()
    {
        m_bInCritical = 1;
    }

    /**
     * Leaves the critical section for this cache.
     */
    void endAtomic()
    {
        m_bInCritical = 0;
    }

    /**
     * Mark the given page as being edited.
     *
     * A page being edited will never be written back, nor will its checksum
     * be calculated. Once a page is no longer being edited, it goes into an
     * intermediate mode that means it'll have its checksum calculated
     * asynchronously. After that point, normal checksum-based writebacks take
     * place.
     */
    void markEditing(uintptr_t key, size_t length = 0);

    /**
     * Mark the given page as no longer being edited.
     */
    void markNoLongerEditing(uintptr_t key, size_t length = 0);

  private:
    /** mapping doer */
    bool map(uintptr_t virt) const;

    /**
     * evict doer
     */
    bool evict(uintptr_t key, bool bLock, bool bPhysicalLock, bool bRemove);

    /**
     * LRU evict do-er.
     *
     * \param force force an eviction to be attempted
     * \return number of cache pages evicted
     */
    size_t lruEvict(bool force = false);

    /**
     * Link the given CachePage to the LRU list.
     */
    void linkPage(CachePage *pPage);

    /**
     * Promote the given CachePage within the LRU list.
     *
     * This marks the page as the most-recently-used page.
     */
    void promotePage(CachePage *pPage);

    /**
     * Unlink the given CachePage from the LRU list.
     */
    void unlinkPage(CachePage *pPage);

    /**
     * Calculate a checksum for the given CachePage.
     */
    void calculateChecksum(CachePage *pPage);

    /**
     * Verify the given CachePage's checksum.
     */
    bool verifyChecksum(CachePage *pPage, bool replace = false);

    /**
     * Checksum do-er.
     */
    void checksum(const void *data, size_t len, uint64_t out[2]);

    struct callbackMeta
    {
        CacheConstants::CallbackCause cause;
        writeback_t callback;
        uintptr_t loc;
        uintptr_t page;
        void *meta;
        UnlikelyLock *cacheLock;
    };

  public:
    /**
     * Cache timer handler.
     *
     * Will call callbacks as needed to write dirty pages back to the backing
     * store. If no callback is set for the Cache instance, the timer will
     * not fire.
     */
    virtual void timer(uint64_t delta, InterruptState &state);

    /**
     * RequestQueue doer, called by the CacheManager instance.
     */
    virtual uint64_t executeRequest(
        uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5,
        uint64_t p6, uint64_t p7, uint64_t p8);

  private:
    /** Key-item pairs. */
    Tree<uintptr_t, CachePage *> m_Pages;

    /** Bloom filter for lookups into m_Pages. */
    BloomFilter<uintptr_t> m_PageFilter;

    /**
     * List of known CachePages, kept up-to-date with m_Pages but in LRU order.
     */
    CachePage *m_pLruHead;
    CachePage *m_pLruTail;

    /** Static MemoryAllocator to allocate virtual address space for all caches.
     */
    static MemoryAllocator m_Allocator;

    /** Lock for using the allocator. */
    static Spinlock m_AllocatorLock;

    /** Lock for this cache. */
    Spinlock m_Lock;

    /** Callback to be called in the write-back timer handler. */
    writeback_t m_Callback;

    /** Timer interface: number of nanoseconds counted so far in the timer
     * handler. */
    uint64_t m_Nanoseconds;

    /** Metadata to pass to a callback. */
    void *m_CallbackMeta;

    /** Are we currently in a critical section? */
    Atomic<size_t> m_bInCritical;

    /** Constraints we need to apply to each page we allocate. */
    size_t m_PageConstraints;

#ifdef STANDALONE_CACHE
    /** Determines the range of addresses permitted for use for Cache. */
    static void discover_range(uintptr_t &start, uintptr_t &end);
#endif
};

/**
 * RAII class for managing refcnt increases via lookup().
 *
 * Use this when you want to perform a lookup() but have many potential exits
 * that would otherwise need an associated release().
 */
class CachePageGuard
{
  public:
    CachePageGuard(Cache &cache, uintptr_t location);
    virtual ~CachePageGuard();

  private:
    Cache &m_Cache;
    uintptr_t m_Location;
};

#endif
