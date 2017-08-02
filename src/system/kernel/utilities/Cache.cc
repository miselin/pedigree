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

#include "pedigree/kernel/utilities/Cache.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#ifndef STANDALONE_CACHE
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#endif

#include "pedigree/kernel/utilities/smhasher/MurmurHash3.h"

// Don't allocate cache space in reverse, but DO re-use cache pages.
// This gives us wins because we don't need to reallocate page tables for
// evicted pages. Without reuse, we end up needing to clean up old page tables
// eventually.
MemoryAllocator Cache::m_Allocator(false, true);
Spinlock Cache::m_AllocatorLock;
static bool g_AllocatorInited = false;

CacheManager CacheManager::m_Instance;

#ifdef THREADS
static int trimTrampoline(void *p)
{
    CacheManager::instance().trimThread();
    return 0;
}
#endif

CacheManager::CacheManager()
    : m_Caches(),
#ifdef THREADS
      m_pTrimThread(0),
#endif
      m_bActive(false)
{
}

CacheManager::~CacheManager()
{
    m_bActive = false;
#ifdef THREADS
    m_pTrimThread->join();
#endif
}

void CacheManager::initialise()
{
#ifndef STANDALONE_CACHE
    Timer *t = Machine::instance().getTimer();
    if (t)
    {
        t->registerHandler(this);
    }
#endif

    // Call out to the base class initialise() so the RequestQueue goes live.
    RequestQueue::initialise();

#ifdef THREADS
    // Create our main trim thread.
    Process *pParent = Processor::information().getCurrentThread()->getParent();
    m_bActive = true;
    m_pTrimThread = new Thread(pParent, trimTrampoline, 0);
#endif
}

void CacheManager::registerCache(Cache *pCache)
{
    m_Caches.pushBack(pCache);
}

void CacheManager::unregisterCache(Cache *pCache)
{
    for (List<Cache *>::Iterator it = m_Caches.begin(); it != m_Caches.end();
         ++it)
    {
        if ((*it) == pCache)
        {
            m_Caches.erase(it);
            return;
        }
    }
}

bool CacheManager::trimAll(size_t count)
{
    size_t totalEvicted = 0;
    for (List<Cache *>::Iterator it = m_Caches.begin();
         (it != m_Caches.end()) && count; ++it)
    {
        size_t evicted = (*it)->trim(count);
        totalEvicted += evicted;
        count -= evicted;
    }

    return totalEvicted != 0;
}

void CacheManager::timer(uint64_t delta, InterruptState &state)
{
    for (List<Cache *>::Iterator it = m_Caches.begin(); it != m_Caches.end();
         ++it)
    {
        (*it)->timer(delta, state);
    }
}

uint64_t CacheManager::executeRequest(
    uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5,
    uint64_t p6, uint64_t p7, uint64_t p8)
{
    Cache *pCache = reinterpret_cast<Cache *>(p1);
    if (!pCache)
        return 0;

    // Valid registered cache?
    bool bCacheFound = false;
    for (List<Cache *>::Iterator it = m_Caches.begin(); it != m_Caches.end();
         ++it)
    {
        if ((*it) == pCache)
        {
            bCacheFound = true;
            break;
        }
    }

    if (!bCacheFound)
    {
        ERROR("CacheManager::executeRequest for an unregistered cache!");
        return 0;
    }

    return pCache->executeRequest(p1, p2, p3, p4, p5, p6, p7, p8);
}

#ifdef THREADS
void CacheManager::trimThread()
{
    while (m_bActive)
    {
        // Ask caches to trim if we're heading towards memory usage problems.
        size_t currFree = PhysicalMemoryManager::instance().freePageCount();
        size_t lowMark = MemoryPressureManager::getLowWatermark();
        if (UNLIKELY(currFree <= lowMark))
        {
            // Start trimming. Trim more the closer to the high watermark we
            // get.
            NOTICE_NOLOCK("trimThread: free page count nears high watermark, "
                          "automatically trimming");
            // Increase as the amount of memory decreases beyond the low
            // watermark.
            size_t trimCount = (lowMark - currFree) + 1;
            trimAll(trimCount);
        }
        else
            Scheduler::instance().yield();
    }
}
#endif

Cache::Cache(size_t pageConstraints)
    : m_Pages(), m_PageFilter(0xe80000, 11), m_pLruHead(0), m_pLruTail(0),
      m_Lock(false), m_Callback(0), m_Nanoseconds(0), m_PageConstraints(pageConstraints)
{
    if (!g_AllocatorInited)
    {
#ifdef STANDALONE_CACHE
        uintptr_t start = 0;
        uintptr_t end = 0;
        discover_range(start, end);
#else
        uintptr_t start =
            VirtualAddressSpace::getKernelAddressSpace().getKernelCacheStart();
        uintptr_t end =
            VirtualAddressSpace::getKernelAddressSpace().getKernelCacheEnd();
#endif
        m_Allocator.free(start, end - start);
        g_AllocatorInited = true;
    }

    // Allocate any necessary iterators now, so that they're available
    // immediately and we consume their memory early.
    m_Pages.begin();
    m_Pages.end();

    CacheManager::instance().registerCache(this);
}

Cache::~Cache()
{
    // Clean up existing cache pages
    for (Tree<uintptr_t, CachePage *>::Iterator it = m_Pages.begin();
         it != m_Pages.end(); it++)
    {
        evict(it.key());
    }

    CacheManager::instance().unregisterCache(this);
}

uintptr_t Cache::lookup(uintptr_t key)
{
    LockGuard<Mutex> guard(m_Lock);

    // Check against the bloom filter first, before we hit the tree.
    if (!m_PageFilter.contains(key))
    {
        return 0;
    }

    CachePage *pPage = m_Pages.lookup(key);
    if (!pPage)
    {
        return 0;
    }

    uintptr_t ptr = pPage->location;
    pPage->refcnt++;
    promotePage(pPage);

    return ptr;
}

uintptr_t Cache::insert(uintptr_t key)
{
    LockGuard<Mutex> guard(m_Lock);

    // We check the bloom filter to avoid hitting the tree, which is useful
    // as this is quite a hot path at times.
    CachePage *pPage = 0;
    if (m_PageFilter.contains(key))
    {
        pPage = m_Pages.lookup(key);
        if (pPage)
        {
            return pPage->location;
        }
    }

    m_AllocatorLock.acquire();
    uintptr_t location = 0;
    bool succeeded = m_Allocator.allocate(4096, location);
    m_AllocatorLock.release();

    if (!succeeded)
    {
        FATAL(
            "Cache: out of address space [have " << m_Pages.count()
                                                 << " items].");
        return 0;
    }

    // Do we have memory pressure - do we need to do an LRU eviction?
    lruEvict();

    if (!map(location))
    {
        FATAL("Map failed in Cache::insert())");
    }

    pPage = new CachePage;
    ByteSet(pPage, 0, sizeof(CachePage));
    pPage->key = key;
    pPage->location = location;
    pPage->refcnt = 1;
    pPage->checksum[0] = 0;
    pPage->checksum[1] = 0;
    pPage->status = CachePage::Editing;
    m_Pages.insert(key, pPage);
    m_PageFilter.add(key);
    linkPage(pPage);

    return location;
}

uintptr_t Cache::insert(uintptr_t key, size_t size)
{
    LockGuard<Mutex> guard(m_Lock);

    if (size % 4096)
    {
        WARNING("Cache::insert called with a size that isn't page-aligned");
        size &= ~0xFFF;
    }

    size_t nPages = size / 4096;

    // Already allocated buffer?
    CachePage *pPage = 0;
    if (m_PageFilter.contains(key))
    {
        pPage = m_Pages.lookup(key);
        if (pPage)
        {
            return pPage->location;
        }
    }

    // Nope, so let's allocate this block
    m_AllocatorLock.acquire();
    uintptr_t location;
    bool succeeded = m_Allocator.allocate(size, location);
    m_AllocatorLock.release();

    if (!succeeded)
    {
        ERROR("Cache: can't allocate " << Dec << size << Hex << " bytes.");
        return 0;
    }

    uintptr_t returnLocation = location;
    bool bOverlap = false;
    for (size_t page = 0; page < nPages; page++)
    {
        pPage = m_Pages.lookup(key + (page * 4096));
        if (pPage)
        {
            bOverlap = true;
            continue;  // Don't overwrite existing buffers
        }

        // Check for and evict pages if we're running low on memory.
        lruEvict();

        if (!map(location))
        {
            FATAL("Map failed in Cache::insert())");
        }

        pPage = new CachePage;
        pPage->key = key + (page * 4096);
        pPage->location = location;

        // Enter into cache unpinned, but only if we can call an eviction
        // callback.
        pPage->refcnt = 1;
        pPage->checksum[0] = 0;
        pPage->checksum[1] = 0;
        pPage->status = CachePage::Editing;

        m_Pages.insert(key + (page * 4096), pPage);
        m_PageFilter.add(key + (page * 4096));
        linkPage(pPage);

        location += 4096;
    }

    if (bOverlap)
        return false;

    return returnLocation;
}

bool Cache::map(uintptr_t virt) const
{
#ifdef STANDALONE_CACHE
    // Will be part of the already-OK region in the allocator.
    return true;
#else
    physical_uintptr_t phys = PhysicalMemoryManager::instance().allocatePage(m_PageConstraints);
    return Processor::information().getVirtualAddressSpace().map(
        phys, reinterpret_cast<void *>(virt),
        VirtualAddressSpace::Write | VirtualAddressSpace::KernelMode);
#endif
}

bool Cache::exists(uintptr_t key, size_t length)
{
    LockGuard<Mutex> guard(m_Lock);

    bool result = true;
    for (size_t i = 0; i < length; i += 0x1000)
    {
        if (!m_PageFilter.contains(key + (i * 0x1000)))
        {
            result = false;
            break;
        }

        CachePage *pPage = m_Pages.lookup(key + (i * 0x1000));
        if (!pPage)
        {
            result = false;
            break;
        }
    }

    return result;
}

bool Cache::evict(uintptr_t key)
{
    return evict(key, true, true, true);
}

void Cache::empty()
{
    LockGuard<Mutex> guard(m_Lock);

    // Remove anything older than the given time threshold.
    for (Tree<uintptr_t, CachePage *>::Iterator it = m_Pages.begin();
         it != m_Pages.end(); ++it)
    {
        CachePage *page = it.value();
        page->refcnt = 0;

        evict(it.key(), false, true, false);
    }

    m_Pages.clear();
}

bool Cache::evict(uintptr_t key, bool bLock, bool bPhysicalLock, bool bRemove)
{
    if (bLock)
    {
        m_Lock.acquire();
    }

    CachePage *pPage = 0;
    if (!m_PageFilter.contains(key))
    {
        pPage = m_Pages.lookup(key);
    }
    if (!pPage)
    {
        NOTICE(
            "Cache::evict didn't evict " << key
                                         << " as it didn't actually exist");
        if (bLock)
            m_Lock.release();
        return false;
    }

    bool result = false;

    // Sanity check: don't evict pinned pages.
    // If we have a callback, we can evict refcount=1 pages as we can fire an
    // eviction event. Pinned pages with a configured callback have a base
    // refcount of one. Otherwise, we must be at a refcount of precisely zero
    // to permit the eviction.
    if ((m_Callback && pPage->refcnt <= 1) ||
        ((!m_Callback) && (!pPage->refcnt)))
    {
        // Good to go. Trigger a writeback if we know this was a dirty page.
        if (!verifyChecksum(pPage))
        {
            m_Callback(
                CacheConstants::WriteBack, key, pPage->location,
                m_CallbackMeta);
        }

#ifndef STANDALONE_CACHE
        VirtualAddressSpace &va =
            Processor::information().getVirtualAddressSpace();
        void *loc = reinterpret_cast<void *>(pPage->location);

        physical_uintptr_t phys;
        size_t flags;
        va.getMapping(loc, phys, flags);
#endif

        // Remove from our tracking.
        if (bRemove)
        {
            m_Pages.remove(key);
            unlinkPage(pPage);
        }

        // Eviction callback.
        if (m_Callback)
            m_Callback(
                CacheConstants::Eviction, key, pPage->location, m_CallbackMeta);

#ifndef STANDALONE_CACHE
        // Clean up resources now that all callbacks and removals are complete.
        va.unmap(loc);
        PhysicalMemoryManager::instance().freePage(phys);
#endif

        // Allow the space to be used again.
        m_Allocator.free(pPage->location, 4096);
        delete pPage;
        result = true;
    }

    if (bLock)
        m_Lock.release();

    return result;
}

bool Cache::pin(uintptr_t key)
{
    LockGuard<Mutex> guard(m_Lock);

    if (!m_PageFilter.contains(key))
    {
        return false;
    }

    CachePage *pPage = m_Pages.lookup(key);
    if (!pPage)
    {
        return false;
    }

    pPage->refcnt++;
    promotePage(pPage);

    return true;
}

void Cache::release(uintptr_t key)
{
    LockGuard<Mutex> guard(m_Lock);

    if (!m_PageFilter.contains(key))
    {
        return;
    }

    CachePage *pPage = m_Pages.lookup(key);
    if (!pPage)
    {
        return;
    }

    assert(pPage->refcnt);
    pPage->refcnt--;

    if (!pPage->refcnt)
    {
        // Trigger an eviction. The eviction will check refcnt, and won't do
        // anything if the refcnt is raised again.
        CacheManager::instance().addAsyncRequest(
            1, reinterpret_cast<uint64_t>(this), CacheConstants::PleaseEvict,
            key);
    }
}

size_t Cache::trim(size_t count)
{
    LockGuard<Mutex> guard(m_Lock);

    if (!count)
        return 0;

    size_t nPages = 0;

    // Attempt an LRU compact.
    size_t n = 0;
    while ((nPages < count) && ((n = lruEvict(true)) > 0))
    {
        nPages += n;
    }

    return nPages;
}

void Cache::sync(uintptr_t key, bool async)
{
    if (!m_Callback)
        return;

    LockGuard<Mutex> guard(m_Lock);

    if (!m_PageFilter.contains(key))
    {
        return;
    }

    CachePage *pPage = m_Pages.lookup(key);
    if (!pPage)
    {
        return;
    }

    uintptr_t location = pPage->location;
    promotePage(pPage);

    if (async)
    {
        CacheManager::instance().addAsyncRequest(
            1, reinterpret_cast<uint64_t>(this), CacheConstants::WriteBack, key,
            location);
    }
    else
    {
        uint64_t result = CacheManager::instance().addRequest(
            1, reinterpret_cast<uint64_t>(this), CacheConstants::WriteBack, key,
            location);
        if (result != 2)
        {
            WARNING("Cache: writeback failed in sync");
        }
    }
}

void Cache::triggerChecksum(uintptr_t key)
{
    LockGuard<Mutex> guard(m_Lock);

    if (!m_PageFilter.contains(key))
    {
        return;
    }

    CachePage *pPage = m_Pages.lookup(key);
    if (!pPage)
    {
        return;
    }

    calculateChecksum(pPage);
}

void Cache::timer(uint64_t delta, InterruptState &state)
{
    m_Nanoseconds += delta;
    if (LIKELY(m_Nanoseconds < (CACHE_WRITEBACK_PERIOD * 1000000ULL)))
        return;
    else if (UNLIKELY(m_Callback == 0))
        return;
    else if (UNLIKELY(m_bInCritical == 1))
    {
        // Missed - don't smash the system constantly doing this check.
        m_Nanoseconds = 0;
        return;
    }

    /// \todo something with locks

    for (Tree<uintptr_t, CachePage *>::Iterator it = m_Pages.begin();
         it != m_Pages.end(); ++it)
    {
        CachePage *page = it.value();
        if (page->status == CachePage::Editing)
        {
            // Don't touch page if it's being edited.
            continue;
        }
        else if (page->status == CachePage::EditTransition)
        {
            // This is now the least-recently-used page.
            promotePage(page);
            page->status = CachePage::ChecksumStable;
            continue;
        }
        else if (page->status == CachePage::ChecksumChanging)
        {
            // Did the checksum change?
            if (verifyChecksum(page, true))
            {
                // No. Write back.
                page->status = CachePage::ChecksumStable;
            }
            else
            {
                // Yes - don't write back.
                continue;
            }
        }
        else if (page->status == CachePage::ChecksumStable)
        {
            // Is it actually stable?
            if (!verifyChecksum(page, true))
            {
                // It changed again - don't write back.
                page->status = CachePage::ChecksumChanging;
            }

            // No need to write back if the checksum is stable.
            continue;
        }
        else
        {
            ERROR("Unknown page status!");
            continue;
        }

        // Promote - page is dirty since we last saw it.
        promotePage(page);

        // Queue a writeback for this dirty page to its backing store.
        NOTICE("** writeback @" << Hex << it.key());
        CacheManager::instance().addAsyncRequest(
            1, reinterpret_cast<uint64_t>(this), CacheConstants::WriteBack,
            it.key(), page->location);
    }

    m_Nanoseconds = 0;
}

void Cache::setCallback(Cache::writeback_t newCallback, void *meta)
{
    m_Callback = newCallback;
    m_CallbackMeta = meta;
}

uint64_t Cache::executeRequest(
    uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5,
    uint64_t p6, uint64_t p7, uint64_t p8)
{
    if (!m_Callback)
        return 0;

    // Eviction request?
    if (static_cast<CacheConstants::CallbackCause>(p2) ==
        CacheConstants::PleaseEvict)
    {
        evict(p3, false, true, true);
        return 1;
    }

    // Pin page while we do our writeback
    pin(p3);

#ifdef SUPERDEBUG
    NOTICE("Cache: writeback for off=" << p3 << " @" << p3 << "!");
#endif
    m_Callback(
        static_cast<CacheConstants::CallbackCause>(p2), p3, p4, m_CallbackMeta);
#ifdef SUPERDEBUG
    NOTICE_NOLOCK(
        "Cache: writeback for off=" << p3 << " @" << p3 << " complete!");
#endif

    // Unpin page, writeback complete
    release(p3);

    return 2;
}

size_t Cache::lruEvict(bool force)
{
#ifdef STANDALONE_CACHE
    return 0;
#else
    if (!(m_pLruHead && m_pLruTail))
        return 0;

    // Do we have memory pressure - do we need to do an LRU eviction?
    if (force || (PhysicalMemoryManager::instance().freePageCount() <
                  MemoryPressureManager::getLowWatermark()))
    {
        // Yes, perform the LRU eviction.
        CachePage *toEvict = m_pLruTail;
        if (evict(toEvict->key, false, true, true))
            return 1;
        else
        {
            // Bump the page's priority up as eviction failed for some reason.
            promotePage(toEvict);
        }
    }

    return 0;
#endif
}

void Cache::linkPage(CachePage *pPage)
{
    pPage->pPrev = 0;
    pPage->pNext = m_pLruHead;
    if (m_pLruHead)
        m_pLruHead->pPrev = pPage;
    m_pLruHead = pPage;
    if (!m_pLruTail)
        m_pLruTail = m_pLruHead;
}

void Cache::promotePage(CachePage *pPage)
{
    unlinkPage(pPage);
    linkPage(pPage);
}

void Cache::unlinkPage(CachePage *pPage)
{
    if (pPage->pPrev)
        pPage->pPrev->pNext = pPage->pNext;
    if (pPage->pNext)
        pPage->pNext->pPrev = pPage->pPrev;
    if (pPage == m_pLruTail)
        m_pLruTail = pPage->pPrev;
    if (pPage == m_pLruHead)
        m_pLruHead = pPage->pNext;
}

void Cache::calculateChecksum(CachePage *pPage)
{
    void *buffer = reinterpret_cast<void *>(pPage->location);
    checksum(buffer, 4096, pPage->checksum);
}

bool Cache::verifyChecksum(CachePage *pPage, bool replace)
{
    void *buffer = reinterpret_cast<void *>(pPage->location);

    uint64_t new_checksum[2];
    checksum(buffer, 4096, new_checksum);

    bool result =
        pPage->checkZeroChecksum() || pPage->checkChecksum(new_checksum);
    if (replace)
    {
        pPage->checksum[0] = new_checksum[0];
        pPage->checksum[1] = new_checksum[1];
    }

    return result;
}

void Cache::checksum(const void *data, size_t len, uint64_t out[2])
{
    MurmurHash3_x64_128(data, len, 0, out);
}

void Cache::markEditing(uintptr_t key, size_t length)
{
    LockGuard<Mutex> guard(m_Lock);

    if (length % 4096)
    {
        WARNING(
            "Cache::markEditing called with a length that isn't page-aligned");
        length &= ~0xFFFU;
    }

    if (!length)
    {
        length = 4096;
    }

    size_t nPages = length / 4096;

    for (size_t page = 0; page < nPages; page++)
    {
        if (!m_PageFilter.contains(key + (page * 4096)))
        {
            continue;
        }

        CachePage *pPage = m_Pages.lookup(key + (page * 4096));
        if (!pPage)
        {
            continue;
        }

        pPage->status = CachePage::Editing;
    }
}

void Cache::markNoLongerEditing(uintptr_t key, size_t length)
{
    LockGuard<Mutex> guard(m_Lock);

    if (length % 4096)
    {
        WARNING(
            "Cache::markEditing called with a length that isn't page-aligned");
        length &= ~0xFFFU;
    }

    if (!length)
    {
        length = 4096;
    }

    size_t nPages = length / 4096;

    for (size_t page = 0; page < nPages; page++)
    {
        if (!m_PageFilter.contains(key + (page * 4096)))
        {
            continue;
        }

        CachePage *pPage = m_Pages.lookup(key + (page * 4096));
        if (!pPage)
        {
            continue;
        }

        pPage->status = CachePage::EditTransition;

        // We have to checksum here as a write could happen between now and the
        // actual handling of the EditTransition, which would lead to some pages
        // potentially failing to complete a writeback (not good).
        calculateChecksum(pPage);
    }
}

CachePageGuard::CachePageGuard(Cache &cache, uintptr_t location)
    : m_Cache(cache), m_Location(location)
{
}

CachePageGuard::~CachePageGuard()
{
    m_Cache.release(m_Location);
}

bool Cache::CachePage::checkChecksum(uint64_t other[2]) const
{
    return checksum[0] == other[0] && checksum[1] == other[1];
}

bool Cache::CachePage::checkZeroChecksum() const
{
    return checksum[0] == 0 && checksum[1] == 0;
}
