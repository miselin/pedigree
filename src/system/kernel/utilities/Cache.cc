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

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/process/MemoryPressureManager.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/Cache.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#if !STANDALONE_CACHE
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#endif

#include "pedigree/kernel/utilities/smhasher/MurmurHash3.h"

class Process;

static constexpr size_t CachePageSize = TargetInfo::getPageSize();
static constexpr uintptr_t CachePageMask = TargetInfo::getPageOffsetMask();

// Don't allocate cache space in reverse, but DO re-use cache pages.
// This gives us wins because we don't need to reallocate page tables for
// evicted pages. Without reuse, we end up needing to clean up old page tables
// eventually.
MemoryAllocator Cache::m_Allocator(true);
Spinlock Cache::m_AllocatorLock;
static bool g_AllocatorInited = false;

CacheManager* CacheManager::m_Instance = nullptr;

#if THREADS
static int trimTrampoline(void* p) {
  CacheManager::instance().trimThread();
  return 0;
}
#endif

CacheManager::CacheManager()
    : RequestQueue(MakeConstantString("CacheManager")),
      m_Caches(),
#if THREADS
      m_CachesLock(),
      m_NextCacheId(1),
      m_pTrimThread(0),
      m_TrimWaiters(),
      m_bTrimRequested(false),
      m_TrimDelta(0),
#endif
      m_bActive(false),
      m_pTimer(nullptr) {
}

CacheManager::~CacheManager() {
#if !STANDALONE_CACHE
  if (m_pTimer) {
    if (!m_pTimer->unregisterHandler(this)) {
      FATAL("CacheManager could not drain its timer callback");
    }
    m_pTimer = nullptr;
  }
#endif

#if THREADS
  {
    auto guard = m_TrimWaiters.acquire();
    m_bActive = false;
    guard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(this));
  }
  if (m_pTrimThread) {
    m_pTrimThread->joinForCompletion();
    m_pTrimThread = nullptr;
  }
#else
  m_bActive = false;
#endif

#if THREADS
  {
    LockGuard<Mutex> guard(m_CachesLock);
    if (m_Caches.begin() != m_Caches.end()) {
      FATAL("CacheManager destroyed while Cache objects remain registered");
    }
  }
#else
  if (m_Caches.begin() != m_Caches.end()) {
    FATAL("CacheManager destroyed while Cache objects remain registered");
  }
#endif

  RequestQueue::destroy();
}

void CacheManager::initialise() {
#if !STANDALONE_CACHE
  Timer* t = Machine::instance().getTimer();
  if (t && t->registerHandler(this)) {
    m_pTimer = t;
  } else {
    FATAL("CacheManager could not register its timer callback");
  }
#endif

  // Call out to the base class initialise() so the RequestQueue goes live.
  RequestQueue::initialise();

#if THREADS
  // Create our main trim thread.
  Process* pParent = Processor::information().getCurrentThread()->getParent();
  {
    auto guard = m_TrimWaiters.acquire();
    m_bActive = true;
    m_bTrimRequested = true;
  }
  m_pTrimThread = new Thread(pParent, trimTrampoline, 0);
  m_pTrimThread->setName("CacheManager trim thread");
#endif
}

void CacheManager::registerCache(Cache* pCache) {
#if THREADS
  LockGuard<Mutex> guard(m_CachesLock);
  if (!m_NextCacheId) {
    FATAL("CacheManager exhausted its stable cache identity space");
  }
  pCache->m_ManagerId = m_NextCacheId++;
#endif
  m_Caches.pushBack(pCache);
}

void CacheManager::unregisterCache(Cache* pCache) {
#if THREADS
  bool removed = false;
  {
    LockGuard<Mutex> guard(m_CachesLock);
#endif
    for (List<Cache*>::Iterator it = m_Caches.begin(); it != m_Caches.end(); ++it) {
      if ((*it) == pCache) {
        m_Caches.erase(it);
#if THREADS
        pCache->m_ManagerId = 0;
        removed = true;
#endif
        break;
      }
    }
#if THREADS
  }
  if (!removed) {
    FATAL("CacheManager could not unregister an unknown Cache");
  }
  pCache->m_ManagerOperations.closeAndWait();
#endif
}

bool CacheManager::trimAll(size_t count) {
  size_t totalEvicted = 0;
#if THREADS
  uint64_t afterId = 0;
  const uint64_t maximumId = cacheGenerationWatermark();
  while (count) {
    Cache* cache = nullptr;
    uint64_t cacheId = 0;
    OperationBarrier::Lease cacheLease;
    if (!acquireNextCache(afterId, maximumId, cache, cacheId, cacheLease)) {
      break;
    }

    afterId = cacheId;
    size_t evicted = cache->trim(count);
    totalEvicted += evicted;
    count -= evicted;
  }
#else
  for (List<Cache*>::Iterator it = m_Caches.begin(); (it != m_Caches.end()) && count; ++it) {
    size_t evicted = (*it)->trim(count);
    totalEvicted += evicted;
    count -= evicted;
  }
#endif

  return totalEvicted != 0;
}

void CacheManager::timer(uint64_t delta) {
#if THREADS
  {
    auto guard = m_TrimWaiters.acquire();
    m_bTrimRequested = true;
    const uint64_t maximum = ~static_cast<uint64_t>(0);
    m_TrimDelta = delta > (maximum - m_TrimDelta) ? maximum : m_TrimDelta + delta;
    guard.wakeOne(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(this));
  }
#else
  for (List<Cache*>::Iterator it = m_Caches.begin(); it != m_Caches.end(); ++it) {
    (*it)->timer(delta);
  }
#endif
}

#if THREADS
bool CacheManager::acquireCache(Cache* cache, uint64_t& generation,
                                OperationBarrier::Lease& lease) {
  LockGuard<Mutex> guard(m_CachesLock);
  for (List<Cache*>::Iterator it = m_Caches.begin(); it != m_Caches.end(); ++it) {
    if (*it == cache) {
      if (cache->m_ManagerOperations.tryAcquire(lease)) {
        generation = cache->m_ManagerId;
        return true;
      }
      generation = 0;
      return false;
    }
  }
  generation = 0;
  lease = OperationBarrier::Lease();
  return false;
}

bool CacheManager::acquireNextCache(uint64_t afterId, uint64_t maximumId, Cache*& cache,
                                    uint64_t& cacheId, OperationBarrier::Lease& lease) {
  LockGuard<Mutex> guard(m_CachesLock);
  Cache* selected = nullptr;
  uint64_t selectedId = ~static_cast<uint64_t>(0);
  for (List<Cache*>::Iterator it = m_Caches.begin(); it != m_Caches.end(); ++it) {
    Cache* candidate = *it;
    if (candidate->m_ManagerId > afterId && candidate->m_ManagerId <= maximumId &&
        candidate->m_ManagerId < selectedId) {
      selected = candidate;
      selectedId = candidate->m_ManagerId;
    }
  }

  if (!selected) {
    cache = nullptr;
    cacheId = 0;
    lease = OperationBarrier::Lease();
    return false;
  }

  if (!selected->m_ManagerOperations.tryAcquire(lease)) {
    FATAL("CacheManager found a closing Cache still registered");
  }
  cache = selected;
  cacheId = selectedId;
  return true;
}

uint64_t CacheManager::cacheGenerationWatermark() {
  LockGuard<Mutex> guard(m_CachesLock);
  return m_NextCacheId - 1;
}
#endif

uint64_t CacheManager::addCacheRequest(Cache* cache, bool asynchronous,
                                       CacheConstants::CallbackCause cause, uintptr_t key,
                                       uintptr_t location, bool transferredPin) {
#if THREADS
  uint64_t generation = 0;
  OperationBarrier::Lease cacheLease;
  if (!acquireCache(cache, generation, cacheLease)) {
    if (transferredPin) {
      cache->release(key);
    }
    return 0;
  }

  CacheRequest* request = new CacheRequest(cache, pedigree_std::move(cacheLease));
  const uint64_t requestToken = reinterpret_cast<uint64_t>(request);
#else
  const uint64_t generation = 0;
  const uint64_t requestToken = 0;
#endif

  if (asynchronous) {
    return addAsyncRequest(1, reinterpret_cast<uint64_t>(cache), cause, key, location,
                           transferredPin ? 1 : 0, generation, 0, requestToken);
  }

  return addRequest(1, RequestQueue::NewRequest, reinterpret_cast<uint64_t>(cache), cause, key,
                    location, transferredPin ? 1 : 0, generation, 0, requestToken);
}

uint64_t CacheManager::executeRequest(uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4,
                                      uint64_t p5, uint64_t p6, uint64_t p7, uint64_t p8) {
#if THREADS
  CacheRequest* request = reinterpret_cast<CacheRequest*>(p8);
  if (!request || !request->cache || request->cache != reinterpret_cast<Cache*>(p1)) {
    FATAL("CacheManager received a request without lifetime ownership");
    return 0;
  }
  Cache* pCache = request->cache;
#else
  Cache* pCache = reinterpret_cast<Cache*>(p1);
  if (!pCache)
    return 0;

  bool cacheFound = false;
  for (List<Cache*>::Iterator it = m_Caches.begin(); it != m_Caches.end(); ++it) {
    if ((*it) == pCache) {
      cacheFound = true;
      break;
    }
  }
  if (!cacheFound) {
    return 0;
  }
#endif

  uint64_t result = pCache->executeRequest(p1, p2, p3, p4, p5, p6, p7, 0);
#if THREADS
  delete request;
#endif
  return result;
}

void CacheManager::cancelRequest(const Request& request) {
#if THREADS
  CacheRequest* cacheRequest = reinterpret_cast<CacheRequest*>(request.p8);
  if (!cacheRequest) {
    FATAL("CacheManager cancelled a request without lifetime ownership");
    return;
  }
  if (request.p5) {
    cacheRequest->cache->release(request.p3);
  }
  delete cacheRequest;
#else
  if (request.p1 && request.p5) {
    Cache* cache = reinterpret_cast<Cache*>(request.p1);
    cache->release(request.p3);
  }
#endif
}

#if THREADS
void CacheManager::trimThread() {
  while (true) {
    uint64_t timerDelta = 0;
    {
      auto guard = m_TrimWaiters.acquire();
      if (!m_bActive) {
        return;
      }
      if (!m_bTrimRequested) {
        const WaitQueue::WakeReason reason = guard.wait(
            WaitQueue::Channel(this), Thread::CallbackDrain, reinterpret_cast<uintptr_t>(this));
        if (reason == WaitQueue::WakeReason::Unwinding ||
            reason == WaitQueue::WakeReason::Terminating) {
          return;
        }
        continue;
      }
      m_bTrimRequested = false;
      timerDelta = m_TrimDelta;
      m_TrimDelta = 0;
    }

    // Ask caches to trim if we're heading towards memory usage problems.
    size_t currFree = PhysicalMemoryManager::instance().freePageCount();
    size_t lowMark = MemoryPressureManager::getLowWatermark();
    if (UNLIKELY(currFree <= lowMark)) {
      // Start trimming. Trim more the closer to the high watermark we
      // get.
      NOTICE_NOLOCK(
          "trimThread: free page count nears high watermark, "
          "automatically trimming");
      // Increase as the amount of memory decreases beyond the low
      // watermark.
      size_t trimCount = (lowMark - currFree) + 1;
      trimAll(trimCount);
    }

    if (timerDelta) {
      uint64_t afterId = 0;
      const uint64_t maximumId = cacheGenerationWatermark();
      while (true) {
        Cache* cache = nullptr;
        uint64_t cacheId = 0;
        OperationBarrier::Lease cacheLease;
        if (!acquireNextCache(afterId, maximumId, cache, cacheId, cacheLease)) {
          break;
        }

        afterId = cacheId;
        cache->timer(timerDelta);
      }
    }
  }
}
#endif

Cache::Cache(size_t pageConstraints)
    : m_Pages(),
      m_PageFilter(0xe80000, 11),
      m_pLruHead(0),
      m_pLruTail(0),
      m_Lock(false),
#if THREADS
      m_EvictionWaiters(),
      m_ManagerOperations(),
      m_ManagerId(0),
#endif
      m_Callback(0),
      m_Nanoseconds(0),
      m_CallbackMeta(nullptr),
      m_bInCritical(0),
      m_ShutdownState(0),
      m_PageConstraints(pageConstraints)
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      ,
      m_WritebackAdmissionHook(nullptr),
      m_WritebackAdmissionHookMeta(nullptr)
#endif
{
  {
    LockGuard<Spinlock> allocatorGuard(m_AllocatorLock);
    if (!g_AllocatorInited) {
#if STANDALONE_CACHE
      uintptr_t start = 0;
      uintptr_t end = 0;
      discover_range(start, end);
#else
      uintptr_t start = VirtualAddressSpace::getKernelAddressSpace().getKernelCacheStart();
      uintptr_t end = VirtualAddressSpace::getKernelAddressSpace().getKernelCacheEnd();
#endif
      m_Allocator.free(start, end - start);
      g_AllocatorInited = true;
    }
  }

  // Allocate any necessary iterators now, so that they're available
  // immediately and we consume their memory early.
  m_Pages.begin();
  m_Pages.end();

  CacheManager::instance().registerCache(this);
}

Cache::~Cache() {
  shutdown();
}

void Cache::shutdown() {
  const size_t state = m_ShutdownState;
  if (state == 2) {
    return;
  }
  if (!m_ShutdownState.compareAndSwap(0, 1)) {
    FATAL("Concurrent Cache shutdown is not permitted");
    return;
  }

  // Removing registration closes queue-time admission. Every request already
  // published owns a manager-operation lease, so this waits for queued and
  // active callbacks before storage is touched.
  CacheManager::instance().unregisterCache(this);
  empty();
  m_ShutdownState = 2;
}

bool Cache::ensureUsable(const char* operation) const {
  if (static_cast<size_t>(m_ShutdownState) != 2) {
    return true;
  }

  FATAL("Cache::" << operation << " called after terminal shutdown");
  return false;
}

void Cache::waitForPageEviction(uintptr_t key) {
#if THREADS
  while (true) {
    CachePage* page = nullptr;
    auto waitGuard = m_EvictionWaiters.acquire();
    {
      LockGuard<Spinlock> guard(m_Lock);
      if (m_PageFilter.contains(key)) {
        page = m_Pages.lookup(key);
      }
      if (!page || page->evictionState == CachePage::EvictionState::None) {
        return;
      }
    }

    const WaitQueue::WakeReason reason =
        waitGuard.waitForCompletion(WaitQueue::Channel(page), Thread::CallbackDrain, key);
    (void)reason;
  }
#else
  (void)key;
#endif
}

uintptr_t Cache::lookup(uintptr_t key) {
  if (!ensureUsable("lookup")) {
    return 0;
  }

  LockGuard<Spinlock> guard(m_Lock);

  // Check against the bloom filter first, before we hit the tree.
  if (!m_PageFilter.contains(key)) {
    return 0;
  }

  CachePage* pPage = m_Pages.lookup(key);
  if (!pPage) {
    return 0;
  }
  if (pPage->evictionState == CachePage::EvictionState::Draining ||
      pPage->evictionState == CachePage::EvictionState::Retiring) {
    return 0;
  }

  uintptr_t ptr = pPage->location;
  pPage->refcnt++;
  promotePage(pPage);

  return ptr;
}

uintptr_t Cache::insert(uintptr_t key, bool* alreadyExisted) {
  if (!ensureUsable("insert")) {
    return 0;
  }

  // Eviction callbacks may block and re-enter this Cache, so memory-pressure
  // work cannot run under the insertion lock.
  lruEvict();

  while (true) {
    waitForPageEviction(key);
    LockGuard<Spinlock> guard(m_Lock);

    // We check the bloom filter to avoid hitting the tree, which is useful
    // as this is quite a hot path at times.
    CachePage* pPage = 0;
    bool triedLookup = false;
    if (m_PageFilter.contains(key)) {
      pPage = m_Pages.lookup(key);
      if (pPage && pPage->evictionState != CachePage::EvictionState::None) {
        continue;
      }
      if (pPage) {
        if (alreadyExisted) {
          *alreadyExisted = true;
        }
        return pPage->location;
      }

      triedLookup = true;
    }

    if (alreadyExisted) {
      *alreadyExisted = false;
    }

    // sanity check
    /// \todo remove this, it makes the bloom filter pointless
    if ((!triedLookup) && m_Pages.lookup(key)) {
      FATAL("Cache: bloom filter lied!");
    }

    m_AllocatorLock.acquire();
    uintptr_t location = 0;
    bool succeeded = m_Allocator.allocate(CachePageSize, location);
    m_AllocatorLock.release();

    if (!succeeded) {
      FATAL("Cache: out of address space [have " << m_Pages.count() << " items].");
      return 0;
    }

    if (!map(location)) {
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
}

uintptr_t Cache::insert(uintptr_t key, size_t size, bool* alreadyExisted) {
  if (!ensureUsable("insert")) {
    return 0;
  }

  if (size % CachePageSize) {
    WARNING("Cache::insert called with a size that isn't page-aligned");
    size &= ~CachePageMask;
  }

  size_t nPages = size / CachePageSize;
  if (!nPages) {
    return 0;
  }

  // Retire at most one old page for each page this insertion may map.
  // lruEvict owns the Cache lock and drops it around backing-store I/O.
  for (size_t page = 0; page < nPages; ++page) {
    lruEvict();
  }

  while (true) {
    for (size_t page = 0; page < nPages; ++page) {
      waitForPageEviction(key + (page * CachePageSize));
    }

    LockGuard<Spinlock> guard(m_Lock);
    bool evictionPending = false;
    for (size_t page = 0; page < nPages; ++page) {
      CachePage* pageEntry = m_Pages.lookup(key + (page * CachePageSize));
      if (pageEntry && pageEntry->evictionState != CachePage::EvictionState::None) {
        evictionPending = true;
        break;
      }
    }
    if (evictionPending) {
      continue;
    }

    // A range insertion must either reuse one complete contiguous extent
    // or create a wholly new one. Allocating around an interior overlap
    // loses the allocator chunk for the skipped page and publishes a
    // partially initialized range.
    CachePage* pPage = 0;
    CachePage* firstPage = 0;
    size_t existingPages = 0;
    bool contiguousExtent = true;
    for (size_t page = 0; page < nPages; ++page) {
      pPage = m_Pages.lookup(key + (page * CachePageSize));
      if (pPage) {
        ++existingPages;
        if (!firstPage) {
          firstPage = pPage;
        }
        if (page == 0 && pPage->location != firstPage->location) {
          contiguousExtent = false;
        } else if (page > 0 && pPage->location != firstPage->location + (page * CachePageSize)) {
          contiguousExtent = false;
        }
      }
    }
    if (existingPages) {
      if (existingPages != nPages || !firstPage || firstPage->key != key || !contiguousExtent) {
        if (alreadyExisted) {
          *alreadyExisted = false;
        }
        return 0;
      }

      if (alreadyExisted) {
        *alreadyExisted = true;
      }
      return firstPage->location;
    }

    if (alreadyExisted) {
      *alreadyExisted = false;
    }

    // Nope, so let's allocate this block
    m_AllocatorLock.acquire();
    uintptr_t location;
    bool succeeded = m_Allocator.allocate(size, location);
    m_AllocatorLock.release();

    if (!succeeded) {
      ERROR("Cache: can't allocate " << Dec << size << Hex << " bytes.");
      return 0;
    }

    uintptr_t returnLocation = location;
    for (size_t page = 0; page < nPages; page++) {
      if (!map(location)) {
        FATAL("Map failed in Cache::insert())");
      }

      pPage = new CachePage;
      ByteSet(pPage, 0, sizeof(CachePage));
      pPage->key = key + (page * CachePageSize);
      pPage->location = location;

      // Cache pages retain one base reference while published.
      pPage->refcnt = 1;
      pPage->evictionState = CachePage::EvictionState::None;
      pPage->checksum[0] = 0;
      pPage->checksum[1] = 0;
      pPage->status = CachePage::Editing;

      m_Pages.insert(key + (page * CachePageSize), pPage);
      m_PageFilter.add(key + (page * CachePageSize));
      linkPage(pPage);

      location += CachePageSize;
    }

    return returnLocation;
  }
}

bool Cache::map(uintptr_t virt) const {
#if STANDALONE_CACHE
  // Will be part of the already-OK region in the allocator.
  return true;
#else
  physical_uintptr_t phys = PhysicalMemoryManager::instance().allocatePage(m_PageConstraints);
  return Processor::information().getVirtualAddressSpace().map(
      phys, reinterpret_cast<void*>(virt),
      VirtualAddressSpace::Write | VirtualAddressSpace::KernelMode);
#endif
}

bool Cache::exists(uintptr_t key, size_t length) {
  if (!ensureUsable("exists")) {
    return false;
  }

  LockGuard<Spinlock> guard(m_Lock);

  bool result = true;
  for (size_t i = 0; i < length; i += CachePageSize) {
    if (!m_PageFilter.contains(key + i)) {
      result = false;
      break;
    }

    CachePage* pPage = m_Pages.lookup(key + i);
    if (!pPage || pPage->evictionState == CachePage::EvictionState::Retiring) {
      result = false;
      break;
    }
  }

  return result;
}

bool Cache::evict(uintptr_t key) {
  if (!ensureUsable("evict")) {
    return false;
  }
  return evict(key, EvictionMode::Ordinary);
}

bool Cache::discardEditing(uintptr_t key) {
  if (!ensureUsable("discardEditing")) {
    return false;
  }
  return evict(key, EvictionMode::DiscardEditing);
}

bool Cache::evict(uintptr_t key, EvictionMode mode) {
  CachePage* page = nullptr;
  writeback_t callback = nullptr;
  void* callbackMeta = nullptr;
  uintptr_t location = 0;
  bool dirty = false;

  {
    LockGuard<Spinlock> guard(m_Lock);
    if (m_PageFilter.contains(key)) {
      page = m_Pages.lookup(key);
    }
    if (!page) {
      NOTICE("Cache::evict didn't evict " << key << " as it didn't actually exist");
      return false;
    }
    if (page->evictionState != CachePage::EvictionState::None) {
      return false;
    }

    callback = m_Callback;
    callbackMeta = m_CallbackMeta;

    if (mode == EvictionMode::DiscardEditing) {
      if (page->status != CachePage::Editing || page->refcnt != 1) {
        return false;
      }
      page->evictionState = CachePage::EvictionState::Retiring;
    } else {
      // Callback-backed pages retain a base reference. Other caches must
      // be entirely unpinned before eviction.
      const size_t permittedReferences =
          (callback || mode == EvictionMode::DiscardBaseReference) ? 1 : 0;
      if (page->refcnt > permittedReferences) {
        return false;
      }

      page->evictionState = CachePage::EvictionState::WriteBack;
      dirty = callback && !verifyChecksum(page);
    }

    location = page->location;
  }

  // Backing-store I/O can block and may re-enter this Cache.
  if (dirty) {
    callback(CacheConstants::WriteBack, key, location, callbackMeta);
  }

  if (mode != EvictionMode::DiscardEditing) {
    bool pinnedAgain = false;
    {
      LockGuard<Spinlock> guard(m_Lock);
      CachePage* current = nullptr;
      if (m_PageFilter.contains(key)) {
        current = m_Pages.lookup(key);
      }
      if (current != page) {
        FATAL("Cache page changed identity during eviction");
        return false;
      }

      // A callback or concurrent lookup may have pinned the page while
      // the cache lock was dropped. In that case, restore ordinary
      // admission.
      const size_t permittedReferences =
          (callback || mode == EvictionMode::DiscardBaseReference) ? 1 : 0;
      if (page->refcnt > permittedReferences) {
        page->evictionState = CachePage::EvictionState::None;
        pinnedAgain = true;
      } else {
        page->evictionState = CachePage::EvictionState::Retiring;
      }
    }

    if (pinnedAgain) {
#if THREADS
      m_EvictionWaiters.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(page));
#endif
      return false;
    }
  }

  return finishRetirement(page, callback, callbackMeta);
}

bool Cache::finishRetirement(CachePage* page, writeback_t callback, void* callbackMeta) {
  const uintptr_t key = page->key;
  const uintptr_t location = page->location;

  // Same-key insertions wait while the external cache index is invalidated.
  if (callback) {
    callback(CacheConstants::Eviction, key, location, callbackMeta);
  }

  {
    LockGuard<Spinlock> guard(m_Lock);
    CachePage* current = nullptr;
    if (m_PageFilter.contains(key)) {
      current = m_Pages.lookup(key);
    }
    if (current != page || page->evictionState != CachePage::EvictionState::Retiring) {
      FATAL("Cache page changed identity during retirement");
      return false;
    }
    m_Pages.remove(key);
    unlinkPage(page);
  }

#if THREADS
  m_EvictionWaiters.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(page));
#endif

#if !STANDALONE_CACHE
  VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
  void* mappedLocation = reinterpret_cast<void*>(location);
  physical_uintptr_t physicalLocation = 0;
  size_t flags = 0;
  va.getMapping(mappedLocation, physicalLocation, flags);
  va.unmap(mappedLocation);
  PhysicalMemoryManager::instance().freePage(physicalLocation);
#endif

  {
    LockGuard<Spinlock> allocatorGuard(m_AllocatorLock);
    m_Allocator.free(location, CachePageSize);
  }
  delete page;
  return true;
}

bool Cache::retireWriteback(uintptr_t key, retirement_writeback_t callback, void* meta) {
  if (!ensureUsable("retireWriteback")) {
    return false;
  }

#if THREADS
  TerminationDeferral terminationDeferral;
#endif
  CachePage* page = nullptr;
  CachePage::Status status = CachePage::Editing;
  writeback_t evictionCallback = nullptr;
  void* evictionCallbackMeta = nullptr;
  {
    LockGuard<Spinlock> guard(m_Lock);
    if (m_PageFilter.contains(key)) {
      page = m_Pages.lookup(key);
    }
    if (!page) {
      return true;
    }
    if (!callback || page->evictionState != CachePage::EvictionState::None ||
        page->status == CachePage::Editing) {
      return false;
    }

    page->evictionState = CachePage::EvictionState::Draining;
    status = page->status;
    evictionCallback = m_Callback;
    evictionCallbackMeta = m_CallbackMeta;
  }

#if THREADS
  while (true) {
    bool ready = false;
    bool invalidated = false;
    bool reopened = false;
    auto waitGuard = m_EvictionWaiters.acquire();
    {
      LockGuard<Spinlock> guard(m_Lock);
      CachePage* current = nullptr;
      if (m_PageFilter.contains(key)) {
        current = m_Pages.lookup(key);
      }
      if (current != page || page->evictionState != CachePage::EvictionState::Draining ||
          page->status != status) {
        invalidated = true;
        if (current == page && page->evictionState == CachePage::EvictionState::Draining) {
          page->evictionState = CachePage::EvictionState::None;
          reopened = true;
        }
      } else {
        ready = page->refcnt == 1;
      }
    }

    if (reopened) {
      waitGuard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(page));
    }
    if (invalidated) {
      return false;
    }
    if (ready) {
      break;
    }

    const WaitQueue::WakeReason reason =
        waitGuard.waitForCompletion(WaitQueue::Channel(page), Thread::CallbackDrain, key);
    (void)reason;
  }
#else
  {
    LockGuard<Spinlock> guard(m_Lock);
    if (page->refcnt != 1) {
      page->evictionState = CachePage::EvictionState::None;
      return false;
    }
  }
#endif

  const bool writebackSucceeded = callback(key, page->location, meta);
  bool retire = false;
  bool wake = false;
  {
    LockGuard<Spinlock> guard(m_Lock);
    CachePage* current = nullptr;
    if (m_PageFilter.contains(key)) {
      current = m_Pages.lookup(key);
    }
    if (writebackSucceeded && current == page &&
        page->evictionState == CachePage::EvictionState::Draining && page->refcnt == 1 &&
        page->status == status) {
      page->evictionState = CachePage::EvictionState::Retiring;
      retire = true;
    } else if (current == page && page->evictionState == CachePage::EvictionState::Draining) {
      page->evictionState = CachePage::EvictionState::None;
      wake = true;
    }
  }

  if (!retire) {
#if THREADS
    if (wake) {
      m_EvictionWaiters.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(page));
    }
#else
    (void)wake;
#endif
    return false;
  }

  return finishRetirement(page, evictionCallback, evictionCallbackMeta);
}

void Cache::empty() {
  while (true) {
    uintptr_t key = 0;
#if THREADS
    CachePage* waitPage = nullptr;
    {
      auto waitGuard = m_EvictionWaiters.acquire();
      {
        LockGuard<Spinlock> guard(m_Lock);
        Tree<uintptr_t, CachePage*>::Iterator it = m_Pages.begin();
        if (it == m_Pages.end()) {
          return;
        }

        key = it.key();
        CachePage* page = it.value();
        if (page->evictionState != CachePage::EvictionState::None || page->refcnt > 1) {
          waitPage = page;
        }
      }

      if (waitPage) {
        const WaitQueue::WakeReason reason =
            waitGuard.waitForCompletion(WaitQueue::Channel(waitPage), Thread::CallbackDrain, key);
        (void)reason;
        continue;
      }
    }
#else
    {
      LockGuard<Spinlock> guard(m_Lock);
      Tree<uintptr_t, CachePage*>::Iterator it = m_Pages.begin();
      if (it == m_Pages.end()) {
        return;
      }
      key = it.key();
    }
#endif

    // Another caller can win the eviction race after the predicate check.
    // Restarting discovers either its in-progress state or the next page.
    if (!evict(key, EvictionMode::DiscardBaseReference)) {
      continue;
    }
  }
}

bool Cache::pin(uintptr_t key) {
  if (!ensureUsable("pin")) {
    return false;
  }

  LockGuard<Spinlock> guard(m_Lock);

  if (!m_PageFilter.contains(key)) {
    return false;
  }

  CachePage* pPage = m_Pages.lookup(key);
  if (!pPage) {
    return false;
  }
  if (pPage->evictionState == CachePage::EvictionState::Draining ||
      pPage->evictionState == CachePage::EvictionState::Retiring) {
    return false;
  }

  pPage->refcnt++;
  promotePage(pPage);

  return true;
}

void Cache::release(uintptr_t key) {
  if (!ensureUsable("release")) {
    return;
  }

  bool shouldEvict = false;
  CachePage* releasedPage = nullptr;
  {
    LockGuard<Spinlock> guard(m_Lock);

    if (!m_PageFilter.contains(key)) {
      return;
    }

    CachePage* pPage = m_Pages.lookup(key);
    if (!pPage || pPage->evictionState == CachePage::EvictionState::Retiring) {
      return;
    }

    assert(pPage->refcnt);
    pPage->refcnt--;
    releasedPage = pPage;
    shouldEvict = !pPage->refcnt;
  }

#if THREADS
  m_EvictionWaiters.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(releasedPage));
#endif

  // Thread creation can reschedule, so it must happen after dropping the
  // cache lock. Eviction rechecks the refcount if the page is pinned again.
  if (shouldEvict) {
    CacheManager::instance().addCacheRequest(this, true, CacheConstants::PleaseEvict, key);
  }
}

size_t Cache::trim(size_t count) {
  if (!ensureUsable("trim")) {
    return 0;
  }

  if (!count)
    return 0;

  size_t nPages = 0;

  // Attempt an LRU compact.
  size_t n = 0;
  while ((nPages < count) && ((n = lruEvict(true)) > 0)) {
    nPages += n;
  }

  return nPages;
}

void Cache::sync(uintptr_t key, bool async) {
  if (!ensureUsable("sync")) {
    return;
  }

#if THREADS
  TerminationDeferral terminationDeferral;
#endif
  if (!m_Callback)
    return;

  uintptr_t location = 0;
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  writeback_admission_hook_t admissionHook = nullptr;
  void* admissionHookMeta = nullptr;
#endif
  {
    LockGuard<Spinlock> guard(m_Lock);

    if (!m_PageFilter.contains(key)) {
      return;
    }

    CachePage* pPage = m_Pages.lookup(key);
    if (!pPage || pPage->evictionState == CachePage::EvictionState::Draining ||
        pPage->evictionState == CachePage::EvictionState::Retiring) {
      return;
    }

    ++pPage->refcnt;
    location = pPage->location;
    promotePage(pPage);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    admissionHook = m_WritebackAdmissionHook;
    admissionHookMeta = m_WritebackAdmissionHookMeta;
#endif
  }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  if (admissionHook) {
    admissionHook(this, key, admissionHookMeta);
  }
#endif

  if (async) {
    CacheManager::instance().addCacheRequest(this, true, CacheConstants::WriteBack, key, location,
                                             true);
  } else {
    uint64_t result = CacheManager::instance().addCacheRequest(
        this, false, CacheConstants::WriteBack, key, location, true);
    if (result != 2) {
      WARNING("Cache: writeback failed in sync");
    }
  }
}

void Cache::triggerChecksum(uintptr_t key) {
  if (!ensureUsable("triggerChecksum")) {
    return;
  }

  LockGuard<Spinlock> guard(m_Lock);

  if (!m_PageFilter.contains(key)) {
    return;
  }

  CachePage* pPage = m_Pages.lookup(key);
  if (!pPage) {
    return;
  }

  calculateChecksum(pPage);
}

void Cache::timer(uint64_t delta) {
  if (!ensureUsable("timer")) {
    return;
  }

#if THREADS
  TerminationDeferral terminationDeferral;
#endif
  {
    LockGuard<Spinlock> guard(m_Lock);
    const uint64_t maximum = ~static_cast<uint64_t>(0);
    m_Nanoseconds = delta > (maximum - m_Nanoseconds) ? maximum : m_Nanoseconds + delta;
    if (LIKELY(m_Nanoseconds < (CACHE_WRITEBACK_PERIOD * 1000000ULL))) {
      return;
    }
    if (UNLIKELY(m_Callback == 0)) {
      return;
    }
    if (UNLIKELY(m_bInCritical == 1)) {
      // Missed - don't repeatedly scan while the cache is changing.
      m_Nanoseconds = 0;
      return;
    }
    m_Nanoseconds = 0;
  }

  // Select and mark one page while holding the cache lock, then enqueue it
  // after dropping the lock. Restarting the scan is intentional: the state
  // transition prevents the same page from being selected twice.
  while (true) {
    bool queueWriteback = false;
    uintptr_t key = 0;
    uintptr_t location = 0;
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    writeback_admission_hook_t admissionHook = nullptr;
    void* admissionHookMeta = nullptr;
#endif
    {
      LockGuard<Spinlock> guard(m_Lock);
      if (!m_Callback || m_bInCritical == 1) {
        return;
      }

      for (Tree<uintptr_t, CachePage*>::Iterator it = m_Pages.begin(); it != m_Pages.end(); ++it) {
        CachePage* page = it.value();
        if (page->evictionState != CachePage::EvictionState::None) {
          continue;
        }
        if (page->status == CachePage::Editing) {
          continue;
        }
        if (page->status == CachePage::EditTransition) {
          promotePage(page);
          page->status = CachePage::ChecksumStable;
          continue;
        }
        if (page->status == CachePage::ChecksumChanging) {
          if (!verifyChecksum(page, true)) {
            continue;
          }
          page->status = CachePage::ChecksumStable;
        } else if (page->status == CachePage::ChecksumStable) {
          if (!verifyChecksum(page, true)) {
            page->status = CachePage::ChecksumChanging;
          }
          continue;
        } else {
          ERROR("Unknown page status!");
          continue;
        }

        promotePage(page);
        ++page->refcnt;
        key = it.key();
        location = page->location;
        queueWriteback = true;
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        admissionHook = m_WritebackAdmissionHook;
        admissionHookMeta = m_WritebackAdmissionHookMeta;
#endif
        break;
      }
    }

    if (!queueWriteback) {
      return;
    }

    NOTICE("** writeback @" << Hex << key);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    if (admissionHook) {
      admissionHook(this, key, admissionHookMeta);
    }
#endif
    CacheManager::instance().addCacheRequest(this, true, CacheConstants::WriteBack, key, location,
                                             true);
  }
}

void Cache::setCallback(Cache::writeback_t newCallback, void* meta) {
  if (static_cast<size_t>(m_ShutdownState) != 0) {
    FATAL("Cache callback installation requires an active Cache");
    return;
  }

  LockGuard<Spinlock> guard(m_Lock);
  if (!newCallback) {
    FATAL("Cache callbacks cannot be cleared after publication");
    return;
  }
  if (m_Callback) {
    FATAL("Cache callbacks are immutable after installation");
    return;
  }
  if (m_Pages.count()) {
    FATAL("Cache callbacks must be installed before inserting pages");
    return;
  }
  m_Callback = newCallback;
  m_CallbackMeta = meta;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void Cache::setWritebackAdmissionHookForTest(writeback_admission_hook_t hook, void* meta) {
  LockGuard<Spinlock> guard(m_Lock);
  m_WritebackAdmissionHook = hook;
  m_WritebackAdmissionHookMeta = meta;
}
#endif

uint64_t Cache::executeRequest(uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5,
                               uint64_t p6, uint64_t p7, uint64_t p8) {
  // Eviction request?
  if (static_cast<CacheConstants::CallbackCause>(p2) == CacheConstants::PleaseEvict) {
    evict(p3);
    return 1;
  }

  writeback_t callback = nullptr;
  void* callbackMeta = nullptr;
  {
    LockGuard<Spinlock> guard(m_Lock);
    callback = m_Callback;
    callbackMeta = m_CallbackMeta;
  }
  if (!callback) {
    if (p5) {
      // sync() transferred this pin to the request.
      release(p3);
    }
    return 0;
  }

  // sync() transfers a pin to its request before dropping the cache lock.
  // Timer-driven requests acquire their pin here.
  if (!p5 && !pin(p3)) {
    return 0;
  }

#if SUPERDEBUG
  NOTICE("Cache: writeback for off=" << p3 << " @" << p3 << "!");
#endif
  callback(static_cast<CacheConstants::CallbackCause>(p2), p3, p4, callbackMeta);
#if SUPERDEBUG
  NOTICE_NOLOCK("Cache: writeback for off=" << p3 << " @" << p3 << " complete!");
#endif

  // Unpin page, writeback complete
  release(p3);

  return 2;
}

size_t Cache::lruEvict(bool force) {
#if STANDALONE_CACHE
  return 0;
#else
  // Do we have memory pressure - do we need to do an LRU eviction?
  if (!force && PhysicalMemoryManager::instance().freePageCount() >=
                    MemoryPressureManager::getLowWatermark()) {
    return 0;
  }

  uintptr_t key = 0;
  {
    LockGuard<Spinlock> guard(m_Lock);
    if (!(m_pLruHead && m_pLruTail)) {
      return 0;
    }
    key = m_pLruTail->key;
  }

  if (evict(key)) {
    return 1;
  }

  {
    LockGuard<Spinlock> guard(m_Lock);
    CachePage* page = nullptr;
    if (m_PageFilter.contains(key)) {
      page = m_Pages.lookup(key);
    }
    if (page && page->evictionState == CachePage::EvictionState::None) {
      // Avoid repeatedly selecting a pinned page under pressure.
      promotePage(page);
    }
  }

  return 0;
#endif
}

void Cache::linkPage(CachePage* pPage) {
  pPage->pPrev = 0;
  pPage->pNext = m_pLruHead;
  if (m_pLruHead)
    m_pLruHead->pPrev = pPage;
  m_pLruHead = pPage;
  if (!m_pLruTail)
    m_pLruTail = m_pLruHead;
}

void Cache::promotePage(CachePage* pPage) {
  unlinkPage(pPage);
  linkPage(pPage);
}

void Cache::unlinkPage(CachePage* pPage) {
  if (pPage->pPrev)
    pPage->pPrev->pNext = pPage->pNext;
  if (pPage->pNext)
    pPage->pNext->pPrev = pPage->pPrev;
  if (pPage == m_pLruTail)
    m_pLruTail = pPage->pPrev;
  if (pPage == m_pLruHead)
    m_pLruHead = pPage->pNext;
}

void Cache::calculateChecksum(CachePage* pPage) {
  void* buffer = reinterpret_cast<void*>(pPage->location);
  checksum(buffer, CachePageSize, pPage->checksum);
}

bool Cache::verifyChecksum(CachePage* pPage, bool replace) {
  void* buffer = reinterpret_cast<void*>(pPage->location);

  uint64_t new_checksum[2];
  checksum(buffer, CachePageSize, new_checksum);

  bool result = pPage->checkZeroChecksum() || pPage->checkChecksum(new_checksum);
  if (replace) {
    pPage->checksum[0] = new_checksum[0];
    pPage->checksum[1] = new_checksum[1];
  }

  return result;
}

void Cache::checksum(const void* data, size_t len, uint64_t out[2]) {
  MurmurHash3_x64_128(data, len, 0, out);
}

void Cache::markEditing(uintptr_t key, size_t length) {
  if (!ensureUsable("markEditing")) {
    return;
  }

  LockGuard<Spinlock> guard(m_Lock);

  if (length % CachePageSize) {
    WARNING("Cache::markEditing called with a length that isn't page-aligned");
    length &= ~CachePageMask;
  }

  if (!length) {
    length = CachePageSize;
  }

  size_t nPages = length / CachePageSize;

  for (size_t page = 0; page < nPages; page++) {
    if (!m_PageFilter.contains(key + (page * CachePageSize))) {
      continue;
    }

    CachePage* pPage = m_Pages.lookup(key + (page * CachePageSize));
    if (!pPage) {
      continue;
    }

    pPage->status = CachePage::Editing;
  }
}

void Cache::markNoLongerEditing(uintptr_t key, size_t length) {
  if (!ensureUsable("markNoLongerEditing")) {
    return;
  }

  LockGuard<Spinlock> guard(m_Lock);

  if (length % CachePageSize) {
    WARNING("Cache::markEditing called with a length that isn't page-aligned");
    length &= ~CachePageMask;
  }

  if (!length) {
    length = CachePageSize;
  }

  size_t nPages = length / CachePageSize;

  for (size_t page = 0; page < nPages; page++) {
    if (!m_PageFilter.contains(key + (page * CachePageSize))) {
      continue;
    }

    CachePage* pPage = m_Pages.lookup(key + (page * CachePageSize));
    if (!pPage) {
      continue;
    }

    pPage->status = CachePage::EditTransition;

    // We have to checksum here as a write could happen between now and the
    // actual handling of the EditTransition, which would lead to some pages
    // potentially failing to complete a writeback (not good).
    calculateChecksum(pPage);
  }
}

CachePageGuard::CachePageGuard(Cache& cache, uintptr_t location)
    : m_Cache(cache), m_Location(location) {}

CachePageGuard::~CachePageGuard() {
  m_Cache.release(m_Location);
}

bool Cache::CachePage::checkChecksum(uint64_t other[2]) const {
  return checksum[0] == other[0] && checksum[1] == other[1];
}

bool Cache::CachePage::checkZeroChecksum() const {
  return checksum[0] == 0 && checksum[1] == 0;
}
