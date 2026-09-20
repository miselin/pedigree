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
#include "pedigree/kernel/utilities/Vector.h"
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
      m_NextCacheId(1),
      m_TimerClock(),
      m_TrimDelta(0),
#if THREADS
      m_CachesLock(),
      m_pTrimThread(0),
      m_TrimWaiters(),
      m_bTrimRequested(false),
#endif
      m_bActive(false),
      m_pTimer(nullptr),
      m_TerminalState(0) {
}

CacheManager::~CacheManager() {
  stopPeriodicWork();

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

void CacheManager::stopPeriodicWork() {
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
}

bool CacheManager::shutdown() {
  const size_t state = m_TerminalState;
  if (state >= 2)
    return state == 2;
  if (!m_TerminalState.compareAndSwap(0, 1)) {
    FATAL("Concurrent CacheManager terminal shutdown is not permitted");
    return false;
  }
  TerminationDeferral lifetime;
  stopPeriodicWork();
  // Cancel queued writebacks and join active callbacks before flushing directly.
  // Lower storage queues and interrupts still service those synchronous writes.
  RequestQueue::destroy();

#if THREADS
  const uint64_t maximumId = cacheGenerationWatermark();
#else
  const uint64_t maximumId = m_NextCacheId - 1;
#endif
  size_t remainingPasses = 0;
  {
#if THREADS
    LockGuard<Mutex> guard(m_CachesLock);
#endif
    remainingPasses = m_Caches.count() + 1;
  }
  while (remainingPasses--) {
    bool succeeded = true;
    bool dirty = false;
    for (size_t scan = 0; scan < 2; ++scan) {
      uint64_t afterId = 0;
      Cache* cache = nullptr;
      uint64_t cacheId = 0;
      while (true) {
#if THREADS
        OperationBarrier::Lease lease;
        if (!acquireNextCache(afterId, maximumId, cache, cacheId, lease))
#else
        if (!findNextCache(afterId, maximumId, cache, cacheId))
#endif
          break;
        afterId = cacheId;
        if (!scan) {
          succeeded = cache->syncAllInternal(nullptr, nullptr, true) && succeeded;
        } else {
          LockGuard<Spinlock> guard(cache->m_Lock);
          if (cache->m_Callback) {
            for (auto page = cache->m_Pages.begin(); page != cache->m_Pages.end(); ++page)
              dirty |= page.value()->status == Cache::CachePage::Editing ||
                       cache->needsWriteback(page.value());
          }
        }
      }
    }
    if (!succeeded || !dirty) {
      m_TerminalState = succeeded ? 2 : 3;
      return succeeded;
    }
    // An upper cache can dirty a lower cache already visited in this pass.
    // A finite dependency chain settles within one pass per retained cache.
  }
  ERROR("CacheManager: terminal writeback did not settle");
  m_TerminalState = 3;
  return false;
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
#endif
  if (static_cast<size_t>(m_TerminalState)) {
    FATAL("Cache registered after terminal CacheManager shutdown began");
  }
  if (!m_NextCacheId) {
    FATAL("CacheManager exhausted its stable cache identity space");
  }
  pCache->m_ManagerId = m_NextCacheId++;
#if THREADS
  {
    auto timerGuard = m_TrimWaiters.acquire();
    pCache->m_ManagerTimerStamp = m_TimerClock;
  }
#else
  pCache->m_ManagerTimerStamp = m_TimerClock;
#endif
  m_Caches.insert(pCache->m_ManagerId, pCache);
}

void CacheManager::unregisterCache(Cache* pCache) {
#if THREADS
  {
    LockGuard<Mutex> guard(m_CachesLock);
#endif
    if (!pCache->m_ManagerId || m_Caches.lookup(pCache->m_ManagerId) != pCache) {
      FATAL("CacheManager could not unregister an unknown Cache");
    }
    m_Caches.remove(pCache->m_ManagerId);
    pCache->m_ManagerId = 0;
#if THREADS
  }
  pCache->m_ManagerOperations.closeAndWait();
#endif
}

bool CacheManager::trimAll(size_t count) {
  if (static_cast<size_t>(m_TerminalState))
    return false;
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
  uint64_t afterId = 0;
  const uint64_t maximumId = m_NextCacheId - 1;
  Cache* cache = nullptr;
  uint64_t cacheId = 0;
  while (count && findNextCache(afterId, maximumId, cache, cacheId)) {
    afterId = cacheId;
    size_t evicted = cache->trim(count);
    totalEvicted += evicted;
    count -= evicted;
  }
#endif

  return totalEvicted != 0;
}

void CacheManager::timer(uint64_t delta) {
  if (static_cast<size_t>(m_TerminalState))
    return;
  bool memoryPressure = false;
#if THREADS
  // Keep the pressure check at timer cadence without waking an idle worker.
  // Sample before taking the waiter lock, as physical allocators can trim caches.
  memoryPressure =
      PhysicalMemoryManager::instance().freePageCount() <= MemoryPressureManager::getLowWatermark();
#endif
  timerTick(delta, memoryPressure);
}

void CacheManager::timerTick(uint64_t delta, bool memoryPressure) {
#if THREADS
  auto guard = m_TrimWaiters.acquire();
#endif
  m_TimerClock.advance(delta);
  const uint64_t maximum = ~static_cast<uint64_t>(0);
  m_TrimDelta = delta > (maximum - m_TrimDelta) ? maximum : m_TrimDelta + delta;
#if THREADS
  if (!m_bTrimRequested && (memoryPressure || m_TrimDelta >= CACHE_WRITEBACK_PERIOD * 1000000ULL)) {
    // A running worker consumes this predicate before it can sleep again.
    m_bTrimRequested = true;
    guard.wakeOne(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(this));
  }
#else
  (void)memoryPressure;
  TimerStamp stamp;
  if (!takeTimerStamp(stamp))
    return;
  uint64_t afterId = 0;
  const uint64_t maximumId = m_NextCacheId - 1;
  Cache* cache = nullptr;
  uint64_t cacheId = 0;
  while (findNextCache(afterId, maximumId, cache, cacheId, true)) {
    afterId = cacheId;
    dispatchTimer(cache, stamp);
  }
#endif
}

void CacheManager::TimerStamp::advance(uint64_t delta) {
  const uint64_t previous = elapsed;
  elapsed += delta;
  if (elapsed < previous)
    ++wraps;
}

uint64_t CacheManager::TimerStamp::since(const TimerStamp& previous) const {
  if (wraps < previous.wraps || (wraps == previous.wraps && elapsed < previous.elapsed))
    return 0;
  if (wraps == previous.wraps || (wraps - previous.wraps == 1 && elapsed < previous.elapsed))
    return elapsed - previous.elapsed;
  return ~uint64_t{0};
}

bool CacheManager::takeTimerStamp(TimerStamp& stamp) {
  if (m_TrimDelta < CACHE_WRITEBACK_PERIOD * 1000000ULL)
    return false;
  // Keep subperiod deltas until a scan is due, then deliver the whole elapsed
  // interval once, including time accumulated while the worker was running.
  m_TrimDelta = 0;
  stamp = m_TimerClock;
  return true;
}

void CacheManager::dispatchTimer(Cache* cache, const TimerStamp& stamp) {
  const uint64_t delta = stamp.since(cache->m_ManagerTimerStamp);
  if (!delta)
    return;
  cache->m_ManagerTimerStamp = stamp;
  cache->timer(delta);
}

bool CacheManager::findNextCache(uint64_t afterId, uint64_t maximumId, Cache*& cache,
                                 uint64_t& cacheId, bool timersOnly) {
  while (afterId < maximumId && m_Caches.lowerBound(afterId + 1, cacheId, cache) &&
         cacheId <= maximumId) {
    if (!timersOnly || cache->needsPeriodicTimer())
      return true;
    // Retain the old timer stamp so late callback installation receives elapsed time.
    afterId = cacheId;
  }
  cache = nullptr;
  cacheId = 0;
  return false;
}

#if THREADS
bool CacheManager::acquireCache(Cache* cache, uint64_t& generation,
                                OperationBarrier::Lease& lease) {
  LockGuard<Mutex> guard(m_CachesLock);
  for (auto it = m_Caches.begin(); it != m_Caches.end(); ++it) {
    if (it.value() == cache) {
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
                                    uint64_t& cacheId, OperationBarrier::Lease& lease,
                                    bool timersOnly) {
  LockGuard<Mutex> guard(m_CachesLock);
  if (!findNextCache(afterId, maximumId, cache, cacheId, timersOnly)) {
    lease = OperationBarrier::Lease();
    return false;
  }

  if (!cache->m_ManagerOperations.tryAcquire(lease)) {
    FATAL("CacheManager found a closing Cache still registered");
  }
  return true;
}

uint64_t CacheManager::cacheGenerationWatermark() {
  LockGuard<Mutex> guard(m_CachesLock);
  return m_NextCacheId - 1;
}
#endif

uint64_t CacheManager::addCacheRequest(Cache* cache, bool asynchronous,
                                       CacheConstants::CallbackCause cause, uintptr_t key,
                                       uintptr_t location, bool transferredPin, bool onlyIfDirty,
                                       bool batch) {
  if (static_cast<size_t>(m_TerminalState)) {
    if (batch)
      cache->releaseBackgroundWriteback(reinterpret_cast<Cache::BackgroundWriteback*>(key));
    if (transferredPin)
      cache->releaseWriteback(key);
    return 0;
  }
#if THREADS
  // RequestQueue rejects these contexts before taking payload ownership.
  // In particular, last-reference cancellation can request another eviction.
  if (callbackActiveOnCurrentThread() || m_LifecycleMutex.isOwnedByCurrentThread()) {
    if (batch)
      cache->releaseBackgroundWriteback(reinterpret_cast<Cache::BackgroundWriteback*>(key));
    if (transferredPin)
      cache->releaseWriteback(key);
    return 0;
  }
  uint64_t generation = 0;
  OperationBarrier::Lease cacheLease;
  if (!acquireCache(cache, generation, cacheLease)) {
    if (batch)
      cache->releaseBackgroundWriteback(reinterpret_cast<Cache::BackgroundWriteback*>(key));
    if (transferredPin) {
      cache->releaseWriteback(key);
    }
    return 0;
  }

  CacheRequest* request = new CacheRequest(cache, pedigree_std::move(cacheLease));
  if (!request) {
    if (batch)
      cache->releaseBackgroundWriteback(reinterpret_cast<Cache::BackgroundWriteback*>(key));
    if (transferredPin) {
      cache->releaseWriteback(key);
    }
    return 0;
  }
  const uint64_t requestToken = reinterpret_cast<uint64_t>(request);
#else
  const uint64_t generation = 0;
  const uint64_t requestToken = 0;
#endif

  // p7 selects forced, conditional, or batch writeback. A batch owns its p3 payload.
  if (asynchronous) {
    return addAsyncRequest(1, reinterpret_cast<uint64_t>(cache), cause, key, location,
                           transferredPin ? 1 : 0, generation, batch ? 2 : (onlyIfDirty ? 1 : 0),
                           requestToken);
  }

  return addRequest(1, RequestQueue::NewRequest, reinterpret_cast<uint64_t>(cache), cause, key,
                    location, transferredPin ? 1 : 0, generation, batch ? 2 : (onlyIfDirty ? 1 : 0),
                    requestToken);
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
  for (auto it = m_Caches.begin(); it != m_Caches.end(); ++it) {
    if (it.value() == pCache) {
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
  if (request.p7 == 2)
    cacheRequest->cache->releaseBackgroundWriteback(
        reinterpret_cast<Cache::BackgroundWriteback*>(request.p3));
  if (request.p5) {
    cacheRequest->cache->releaseWriteback(request.p3);
  }
  delete cacheRequest;
#else
  if (request.p1 && request.p7 == 2)
    reinterpret_cast<Cache*>(request.p1)
        ->releaseBackgroundWriteback(reinterpret_cast<Cache::BackgroundWriteback*>(request.p3));
  if (request.p1 && request.p5) {
    Cache* cache = reinterpret_cast<Cache*>(request.p1);
    cache->releaseWriteback(request.p3);
  }
#endif
}

#if THREADS
void CacheManager::trimThread() {
  while (true) {
    TimerStamp stamp;
    bool timerDue = false;
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
      timerDue = takeTimerStamp(stamp);
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

    if (timerDue) {
      uint64_t afterId = 0;
      const uint64_t maximumId = cacheGenerationWatermark();
      while (true) {
        Cache* cache = nullptr;
        uint64_t cacheId = 0;
        OperationBarrier::Lease cacheLease;
        if (!acquireNextCache(afterId, maximumId, cache, cacheId, cacheLease, true)) {
          break;
        }

        afterId = cacheId;
        dispatchTimer(cache, stamp);
      }
    }
  }
}
#endif

Cache::Cache(size_t pageConstraints)
    : m_Pages(),
      m_WritebackPages(),
      m_DirtyTracking(DirtyTracking::Checksum),
      // Each inode owns a Cache. Keep first-page metadata small; a saturated
      // filter still falls back to the authoritative page tree.
      m_PageFilter(4096, 4),
      m_pLruHead(0),
      m_pLruTail(0),
      m_Lock(false),
#if THREADS
      m_EvictionWaiters(),
      m_ManagerOperations(),
#endif
      m_ManagerId(0),
      m_ManagerTimerStamp(),
      m_PeriodicTimerEnabled(false),
      m_Callback(0),
      m_BackgroundWriteback(nullptr),
      m_Nanoseconds(0),
      m_WritebackEpoch(0),
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

bool Cache::shutdown() {
  const size_t state = m_ShutdownState;
  if (state >= 2) {
    return state == 2;
  }
  if (!m_ShutdownState.compareAndSwap(0, 1)) {
    FATAL("Concurrent Cache shutdown is not permitted");
    return false;
  }

  // Removing registration closes queue-time admission. Every request already
  // published owns a manager-operation lease, so this waits for queued and
  // active callbacks before storage is touched.
  CacheManager::instance().unregisterCache(this);
  const bool succeeded = empty();
  if (!succeeded) {
    ERROR("Cache: backend teardown left unwritten pages resident");
  }
  m_ShutdownState = succeeded ? 2 : 3;
  return succeeded;
}

bool Cache::ensureUsable(const char* operation) const {
  if (static_cast<size_t>(m_ShutdownState) < 2) {
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

bool Cache::lookupStable(uintptr_t key, uintptr_t& location, bool wait) {
  location = 0;
  if (!ensureUsable("lookupStable"))
    return false;
#if THREADS
  TerminationDeferral terminationDeferral;
  OperationBarrier::Lease operation;
  if (!m_ManagerOperations.tryAcquire(operation))
    return false;
  Thread* currentThread = Processor::information().getCurrentThread();
  const bool canWait = wait && currentThread && !CacheManager::instance().callbackContext();
#else
  (void)wait;
#endif
  while (true) {
#if THREADS
    auto waitGuard = m_EvictionWaiters.acquire();
#endif
    CachePage* page = nullptr;
    {
      LockGuard<Spinlock> guard(m_Lock);
      page = m_PageFilter.contains(key) ? m_Pages.lookup(key) : nullptr;
      if (!page)
        return true;
      if (page->status == CachePage::Editing ||
          page->evictionState == CachePage::EvictionState::Draining || page->refcnt == ~size_t{0})
        return false;
      if (!page->callbackActive && page->evictionState == CachePage::EvictionState::None) {
        ++page->refcnt;
        location = page->location;
        promotePage(page);
        return true;
      }
#if THREADS
      if (!canWait || page->callbackOwner == currentThread)
#endif
        return false;
    }
#if THREADS
    const auto reason =
        waitGuard.waitForCompletion(WaitQueue::Channel(page), Thread::CallbackDrain, key);
    (void)reason;
#endif
  }
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
    updateWritebackIndex(pPage);
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
    if (alreadyExisted) {
      *alreadyExisted = false;
    }
    return 0;
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
      updateWritebackIndex(pPage);
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
  uint64_t submittedGeneration = 0;
  uint64_t submittedChecksum[2] = {};
  bool submittedChecksumTracking = false;

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
      dirty = callback && needsWriteback(page);
      submittedGeneration = page->mutationGeneration;
      submittedChecksumTracking = dirty && tracksChecksum(page);
      page->callbackActive = dirty;
#if THREADS
      page->callbackOwner = dirty ? Processor::information().getCurrentThread() : nullptr;
#endif
    }

    location = page->location;
  }

  // Backing-store I/O can block and may re-enter this Cache.
  if (submittedChecksumTracking)
    checksum(reinterpret_cast<const void*>(location), CachePageSize, submittedChecksum);
  if (dirty && !callback(CacheConstants::WriteBack, key, location, callbackMeta)) {
    {
      LockGuard<Spinlock> guard(m_Lock);
      page->writebackFailed = true;
      page->callbackActive = false;
#if THREADS
      page->callbackOwner = nullptr;
#endif
      page->evictionState = CachePage::EvictionState::None;
      updateWritebackIndex(page);
    }
#if THREADS
    m_EvictionWaiters.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(page));
#endif
    return false;
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

      page->callbackActive = false;
#if THREADS
      page->callbackOwner = nullptr;
#endif
      if (dirty) {
        page->writebackFailed = false;
        page->writtenGeneration = submittedGeneration;
        if (submittedChecksumTracking) {
          page->checksum[0] = submittedChecksum[0];
          page->checksum[1] = submittedChecksum[1];
        }
        if (page->status == CachePage::ChecksumChanging)
          page->status = CachePage::ChecksumStable;
      }
      // A callback or concurrent lookup may have pinned the page while
      // the cache lock was dropped. In that case, restore ordinary
      // admission.
      const size_t permittedReferences =
          (callback || mode == EvictionMode::DiscardBaseReference) ? 1 : 0;
      if (page->refcnt > permittedReferences ||
          (callback && (page->mutationGeneration != page->writtenGeneration ||
                        (dirty && needsWriteback(page))))) {
        page->evictionState = CachePage::EvictionState::None;
        pinnedAgain = true;
      } else {
        page->evictionState = CachePage::EvictionState::Retiring;
      }
      updateWritebackIndex(page);
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
#if THREADS
    {
      LockGuard<Spinlock> guard(m_Lock);
      page->callbackOwner = Processor::information().getCurrentThread();
    }
#endif
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
    if (page->writebackIndexed)
      m_WritebackPages.remove(key);
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

  {
    LockGuard<Spinlock> guard(m_Lock);
    page->callbackActive = true;
#if THREADS
    page->callbackOwner = Processor::information().getCurrentThread();
#endif
  }
  const bool writebackSucceeded = callback(key, page->location, meta);
  bool retire = false;
  bool wake = false;
  {
    LockGuard<Spinlock> guard(m_Lock);
    page->callbackActive = false;
#if THREADS
    page->callbackOwner = nullptr;
#endif
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
      page->writebackFailed = page->writebackFailed || !writebackSucceeded;
      page->evictionState = CachePage::EvictionState::None;
      updateWritebackIndex(page);
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

bool Cache::empty() {
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
          return true;
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
        return true;
      }
      key = it.key();
    }
#endif

    // Another caller can win the eviction race after the predicate check.
    // Restarting discovers either its in-progress state or the next page.
    if (!evict(key, EvictionMode::DiscardBaseReference)) {
      LockGuard<Spinlock> guard(m_Lock);
      CachePage* page = m_Pages.lookup(key);
      if (page && page->writebackFailed) {
        return false;
      }
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

bool Cache::sync(uintptr_t key, bool async) {
  if (!ensureUsable("sync")) {
    return false;
  }

#if THREADS
  TerminationDeferral terminationDeferral;
#endif
  if (!m_Callback)
    return true;

  uintptr_t location = 0;
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  writeback_admission_hook_t admissionHook = nullptr;
  void* admissionHookMeta = nullptr;
#endif
  {
    LockGuard<Spinlock> guard(m_Lock);

    if (!m_PageFilter.contains(key)) {
      return true;
    }

    CachePage* pPage = m_Pages.lookup(key);
    if (!pPage || pPage->evictionState == CachePage::EvictionState::Draining ||
        pPage->evictionState == CachePage::EvictionState::Retiring) {
      return false;
    }

    if (m_DirtyTracking == DirtyTracking::Explicit && pPage->status != CachePage::Editing &&
        !pPage->callbackActive && pPage->evictionState == CachePage::EvictionState::None &&
        !needsWriteback(pPage)) {
      return true;
    }

    // Preserve legacy forced writeback if queue admission fails.
    if (m_DirtyTracking == DirtyTracking::Checksum)
      recordMutation(pPage);

    ++pPage->refcnt;
    ++pPage->writebackPins;
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
    return CacheManager::instance().addCacheRequest(this, true, CacheConstants::WriteBack, key,
                                                    location, true) != 0;
  } else {
    uint64_t result = CacheManager::instance().addCacheRequest(
        this, false, CacheConstants::WriteBack, key, location, true);
    return result == 2;
  }
}

bool Cache::syncAll() {
  return syncAll(nullptr, nullptr);
}

bool Cache::syncAll(writeback_batch_t callback, void* metadata) {
  return syncAllInternal(callback, metadata, false);
}

bool Cache::syncAllInternal(writeback_batch_t callback, void* metadata, bool onlyIfDirty) {
#if THREADS
  TerminationDeferral terminationDeferral;
  OperationBarrier::Lease operation;
  if (!m_ManagerOperations.tryAcquire(operation)) {
    return false;
  }
  Thread* currentThread = Processor::information().getCurrentThread();
  const bool canWait = currentThread && !CacheManager::instance().callbackContext();
#endif
  struct Entry {
    uintptr_t key;
    uintptr_t location;
    bool pinned;
  };
  Vector<Entry> entries;
  auto& candidates = m_DirtyTracking == DirtyTracking::Explicit ? m_WritebackPages : m_Pages;
  bool snapshotted = false;
  // Allocation stays outside the cache lock; bounded retries avoid chasing
  // an indefinitely growing cache while holding the object's lifetime.
  for (size_t attempt = 0; attempt < 4 && !snapshotted; ++attempt) {
    size_t count = 0;
    {
      LockGuard<Spinlock> guard(m_Lock);
      if (static_cast<size_t>(m_ShutdownState)) {
        return false;
      }
      if (!m_Callback) {
        return true;
      }
      count = candidates.count();
    }
    if (!entries.tryReserve(count)) {
      return false;
    }
    {
      LockGuard<Spinlock> guard(m_Lock);
      if (candidates.count() > entries.size()) {
        continue;
      }
      for (auto it = candidates.begin(); it != candidates.end(); ++it) {
#if THREADS
        if ((it.value()->callbackOwner && it.value()->callbackOwner == currentThread) ||
            (!currentThread && it.value()->callbackActive)) {
#else
        if (it.value()->callbackActive) {
#endif
          return false;
        }
        CachePage* page = it.value();
        if (page->evictionState != CachePage::EvictionState::Draining &&
            page->evictionState != CachePage::EvictionState::Retiring &&
            (page->refcnt == ~size_t{0} || page->writebackPins == ~size_t{0})) {
          return false;
        }
      }
      for (auto it = candidates.begin(); it != candidates.end(); ++it) {
        CachePage* page = it.value();
        const bool pinned = page->evictionState != CachePage::EvictionState::Draining &&
                            page->evictionState != CachePage::EvictionState::Retiring;
        if (pinned) {
          ++page->refcnt;
          ++page->writebackPins;
        }
        const Entry entry = {page->key, page->location, pinned};
        entries.pushBack(entry);
      }
      snapshotted = true;
    }
  }
  if (!snapshotted) {
    return false;
  }

  bool succeeded = true;
  uintptr_t keys[MaxWritebackPages];
  size_t pending = 0;
  auto drain = [&] {
    if (!pending)
      return;
    succeeded = syncBatchInternal(keys, pending, callback, metadata, true) && succeeded;
    for (size_t n = 0; n < pending; ++n)
      releaseWriteback(keys[n]);
    pending = 0;
  };
  for (size_t i = 0; i < entries.count(); ++i) {
    Entry& entry = entries[i];
    // Draining pages cannot be pinned: their retirement waits for pins to
    // disappear. Join that operation, then retry if a failed page remains.
    while (!entry.pinned) {
#if THREADS
      auto waitGuard = m_EvictionWaiters.acquire();
#endif
      CachePage* page = nullptr;
      bool busy = false;
      {
        LockGuard<Spinlock> guard(m_Lock);
        page = m_Pages.lookup(entry.key);
        if (!page) {
          break;
        }
        busy = page->evictionState == CachePage::EvictionState::Draining ||
               page->evictionState == CachePage::EvictionState::Retiring;
        if (!busy) {
          if (page->refcnt == ~size_t{0} || page->writebackPins == ~size_t{0}) {
            succeeded = false;
            break;
          }
          ++page->refcnt;
          ++page->writebackPins;
          entry.location = page->location;
          entry.pinned = true;
        }
#if THREADS
        else if (!canWait || page->callbackOwner == currentThread) {
          succeeded = false;
          break;
        }
#endif
      }
      if (busy) {
#if THREADS
        const WaitQueue::WakeReason reason =
            waitGuard.waitForCompletion(WaitQueue::Channel(page), Thread::CallbackDrain, entry.key);
        (void)reason;
#else
        succeeded = false;
        break;
#endif
      }
    }
    if (entry.pinned) {
      if (callback) {
        keys[pending++] = entry.key;
        if (pending == MaxWritebackPages)
          drain();
      } else {
        const bool written = writebackPage(entry.key, entry.location, true, onlyIfDirty);
        succeeded = written && succeeded;
        releaseWriteback(entry.key);
      }
      entry.pinned = false;
    }
  }
  drain();
  return succeeded;
}

bool Cache::syncBatch(const uintptr_t* keys, size_t count, writeback_batch_t callback,
                      void* metadata) {
  return syncBatchInternal(keys, count, callback, metadata, false);
}

// syncAll owns snapshot pins before retirement can start draining these pages.
bool Cache::syncBatchInternal(const uintptr_t* keys, size_t count, writeback_batch_t callback,
                              void* metadata, bool snapshot) {
  if (!ensureUsable("syncBatch") || count > MaxWritebackPages || (count && (!keys || !callback))) {
    return false;
  }
  if (!count)
    return true;
  for (size_t i = 0; i < count; ++i) {
    for (size_t j = 0; j < i; ++j) {
      if (keys[i] == keys[j])
        return false;
    }
  }
#if THREADS
  TerminationDeferral terminationDeferral;
  OperationBarrier::Lease operation;
  if (!m_ManagerOperations.tryAcquire(operation))
    return false;
  Thread* currentThread = Processor::information().getCurrentThread();
  const bool canWait = currentThread && !CacheManager::instance().callbackContext();
#endif
  CachePage* pages[MaxWritebackPages] = {};
  WritebackPage writes[MaxWritebackPages] = {};
  uint64_t submittedChecksums[MaxWritebackPages][2] = {};
  uint64_t submittedGenerations[MaxWritebackPages] = {};
  bool submittedChecksumTracking[MaxWritebackPages] = {};
  size_t writeCount = 0;
  while (true) {
#if THREADS
    auto waitGuard = m_EvictionWaiters.acquire();
#endif
    CachePage* busy = nullptr;
    {
      LockGuard<Spinlock> guard(m_Lock);
      if (static_cast<size_t>(m_ShutdownState) || !m_Callback)
        return false;
      // Claim all pages together: two overlapping batches must never each own
      // a prefix while waiting for the other batch's remaining callbacks.
      for (size_t i = 0; i < count; ++i) {
        CachePage* page = m_Pages.lookup(keys[i]);
        if (!page || page->status == CachePage::Editing ||
            (page->evictionState == CachePage::EvictionState::Draining && !snapshot) ||
            page->evictionState == CachePage::EvictionState::Retiring ||
            page->refcnt == ~size_t{0} || page->writebackPins == ~size_t{0}) {
          return false;
        }
        if (page->callbackActive || page->evictionState == CachePage::EvictionState::WriteBack) {
#if THREADS
          if (!canWait || page->callbackOwner == currentThread)
            return false;
#else
          return false;
#endif
          busy = page;
        }
        pages[i] = page;
      }
      if (!busy) {
        writeCount = 0;
        for (size_t i = 0; i < count; ++i) {
          CachePage* page = pages[i];
          if ((snapshot || m_DirtyTracking == DirtyTracking::Explicit) && !needsWriteback(page))
            continue;
          ++page->refcnt;
          ++page->writebackPins;
          page->callbackActive = true;
#if THREADS
          page->callbackOwner = currentThread;
#endif
          pages[writeCount] = page;
          writes[writeCount] = {keys[i], page->location};
          submittedGenerations[writeCount] = page->mutationGeneration;
          submittedChecksumTracking[writeCount] = tracksChecksum(page);
          ++writeCount;
          updateWritebackIndex(page);
          promotePage(page);
        }
      }
    }
    if (!busy)
      break;
#if THREADS
    const auto reason = waitGuard.waitForCompletion(WaitQueue::Channel(busy), Thread::CallbackDrain,
                                                    reinterpret_cast<uintptr_t>(busy));
    (void)reason;
#endif
  }
  if (!writeCount)
    return true;
  for (size_t i = 0; i < writeCount; ++i) {
    if (submittedChecksumTracking[i])
      checksum(reinterpret_cast<const void*>(writes[i].location), CachePageSize,
               submittedChecksums[i]);
  }
  const bool succeeded = callback(writes, writeCount, metadata);
  {
    LockGuard<Spinlock> guard(m_Lock);
    for (size_t i = 0; i < writeCount; ++i) {
      CachePage* page = pages[i];
      page->writebackFailed = !succeeded;
      if (succeeded) {
        page->writtenGeneration = submittedGenerations[i];
        if (submittedChecksumTracking[i]) {
          page->checksum[0] = submittedChecksums[i][0];
          page->checksum[1] = submittedChecksums[i][1];
        }
        if (page->status == CachePage::ChecksumChanging)
          page->status = CachePage::ChecksumStable;
      }
      page->callbackActive = false;
#if THREADS
      page->callbackOwner = nullptr;
#endif
      updateWritebackIndex(page);
    }
  }
  for (size_t i = 0; i < writeCount; ++i) {
#if THREADS
    m_EvictionWaiters.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(pages[i]));
#endif
    releaseWriteback(writes[i].key);
  }
  return succeeded;
}

bool Cache::writebackPage(uintptr_t key, uintptr_t location, bool wait, bool onlyIfDirty) {
  CachePage* page = nullptr;
  writeback_t callback = nullptr;
  void* callbackMeta = nullptr;
  uint64_t submittedGeneration = 0;
  bool submittedChecksumTracking = false;
#if THREADS
  Thread* currentThread = Processor::information().getCurrentThread();
  const bool canWait = wait && currentThread && !CacheManager::instance().callbackContext();
#else
  (void)wait;
#endif
  while (true) {
#if THREADS
    auto waitGuard = m_EvictionWaiters.acquire();
#endif
    {
      LockGuard<Spinlock> guard(m_Lock);
      page = m_Pages.lookup(key);
      if (!page || page->location != location || !m_Callback) {
        return false;
      }
      if ((wait || onlyIfDirty || m_DirtyTracking == DirtyTracking::Explicit) &&
          page->status == CachePage::Editing) {
        return false;
      }
      if (!page->callbackActive && page->evictionState != CachePage::EvictionState::WriteBack) {
        // A durable batch can supersede a timer request already in the queue.
        // Legacy sync stays forced; explicit owners submit only known changes.
        if ((onlyIfDirty || m_DirtyTracking == DirtyTracking::Explicit) && !needsWriteback(page))
          return true;
        // A previously admitted writeback pin is allowed to finish while a
        // retirement waits in Draining for precisely these pins to disappear.
        page->callbackActive = true;
#if THREADS
        page->callbackOwner = currentThread;
#endif
        callback = m_Callback;
        callbackMeta = m_CallbackMeta;
        submittedGeneration = page->mutationGeneration;
        submittedChecksumTracking = tracksChecksum(page);
        updateWritebackIndex(page);
        break;
      }
#if THREADS
      if (!canWait || page->callbackOwner == currentThread) {
#endif
        return false;
#if THREADS
      }
#endif
    }
#if THREADS
    const WaitQueue::WakeReason reason =
        waitGuard.waitForCompletion(WaitQueue::Channel(page), Thread::CallbackDrain, key);
    (void)reason;
#endif
  }

  uint64_t submittedChecksum[2] = {};
  if (submittedChecksumTracking)
    checksum(reinterpret_cast<const void*>(location), CachePageSize, submittedChecksum);
  const bool succeeded = callback(CacheConstants::WriteBack, key, location, callbackMeta);
  {
    LockGuard<Spinlock> guard(m_Lock);
    page->writebackFailed = !succeeded;
    if (succeeded) {
      page->writtenGeneration = submittedGeneration;
      if (submittedChecksumTracking) {
        page->checksum[0] = submittedChecksum[0];
        page->checksum[1] = submittedChecksum[1];
      }
      // Otherwise the stable-checksum scan schedules this completed write again.
      if (page->status == CachePage::ChecksumChanging) {
        page->status = CachePage::ChecksumStable;
      }
    }
    page->callbackActive = false;
#if THREADS
    page->callbackOwner = nullptr;
#endif
    updateWritebackIndex(page);
  }
#if THREADS
  m_EvictionWaiters.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(page));
#endif
  return succeeded;
}

void Cache::markDirty(uintptr_t key) {
  if (!ensureUsable("markDirty")) {
    return;
  }
  LockGuard<Spinlock> guard(m_Lock);
  CachePage* page = m_Pages.lookup(key);
  if (page) {
    recordMutation(page);
  }
}

void Cache::markExternallyWritable(uintptr_t key) {
  if (!ensureUsable("markExternallyWritable"))
    return;
  LockGuard<Spinlock> guard(m_Lock);
  CachePage* page = m_Pages.lookup(key);
  if (!page || page->externallyWritable)
    return;
  const bool tracked = tracksChecksum(page);
  page->externallyWritable = true;
  if (!tracked)
    calculateChecksum(page);
  updateWritebackIndex(page);
}

bool Cache::beginMutableLoan(uintptr_t key) {
  if (!ensureUsable("beginMutableLoan"))
    return false;
  LockGuard<Spinlock> guard(m_Lock);
  CachePage* page = m_Pages.lookup(key);
  if (!page || page->evictionState == CachePage::EvictionState::Retiring ||
      page->mutableLoans == ~size_t{0})
    return false;

  const bool tracked = tracksChecksum(page);
  ++page->mutableLoans;
  if (!tracked) {
    calculateChecksum(page);
    // This callback captured no checksum before the writable alias existed.
    // Its older generation must not settle modifications made by this loan.
    if (page->callbackActive)
      recordMutation(page);
  }
  updateWritebackIndex(page);
  return true;
}

void Cache::endMutableLoan(uintptr_t key) {
  if (!ensureUsable("endMutableLoan"))
    return;
  LockGuard<Spinlock> guard(m_Lock);
  CachePage* page = m_Pages.lookup(key);
  assert(page && page->mutableLoans);
  if (!page || !page->mutableLoans)
    return;

  if (page->mutableLoans == 1) {
    // An active callback may publish its older checksum after tracking ends.
    // A newer mutation generation keeps that completion from losing changes.
    if (page->callbackActive ||
        (!page->writebackFailed && page->mutationGeneration == page->writtenGeneration &&
         !verifyChecksum(page)))
      recordMutation(page);
  }
  --page->mutableLoans;
  updateWritebackIndex(page);
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
    ++m_WritebackEpoch;
  }

  // Bound interrupt-disabled work to one page, including clean prefixes. A
  // copied key also lets callbacks mutate the tree without invalidating a scan.
  BackgroundWriteback* batch = nullptr;
  auto submitBatch = [&] {
    if (batch) {
      CacheManager::instance().addCacheRequest(this, true, CacheConstants::WriteBack,
                                               reinterpret_cast<uintptr_t>(batch), 0, false, true,
                                               true);
      batch = nullptr;
    }
  };
  uintptr_t nextKey = 0;
  auto& candidates = m_DirtyTracking == DirtyTracking::Explicit ? m_WritebackPages : m_Pages;
  bool finished = false;
  while (!finished) {
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
        break;
      }

      CachePage* page = nullptr;
      if (!candidates.lowerBound(nextKey, key, page)) {
        break;
      }
      finished = key == ~uintptr_t{0};
      if (!finished) {
        nextKey = key + 1;
      }
      if (page->writebackEpoch == m_WritebackEpoch) {
        continue;
      }
      page->writebackEpoch = m_WritebackEpoch;
      if (page->evictionState != CachePage::EvictionState::None) {
        continue;
      }
      // A queued write owns the retry until it completes. Rescanning it can
      // otherwise enqueue another write on every timer tick.
      if (page->writebackPins) {
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
      if (page->writebackFailed || page->mutationGeneration != page->writtenGeneration) {
        // A stable checksum cannot make an unsuccessful backend write clean.
      } else if (!tracksChecksum(page)) {
        continue;
      } else if (page->status == CachePage::ChecksumChanging) {
        if (!verifyChecksum(page, true)) {
          continue;
        }
        page->status = CachePage::ChecksumStable;
      } else if (page->status == CachePage::ChecksumStable) {
        if (!verifyChecksum(page, true)) {
          page->status = CachePage::ChecksumChanging;
          recordMutation(page);
        }
        continue;
      } else {
        ERROR("Unknown page status!");
        continue;
      }

      promotePage(page);
      ++page->refcnt;
      ++page->writebackPins;
      location = page->location;
      queueWriteback = true;
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      admissionHook = m_WritebackAdmissionHook;
      admissionHookMeta = m_WritebackAdmissionHookMeta;
#endif
    }

    if (!queueWriteback) {
      continue;
    }

#if defined(CACHE_TRACE_WRITEBACK) && CACHE_TRACE_WRITEBACK
    NOTICE("Cache " << Hex << reinterpret_cast<uintptr_t>(this) << ": queue writeback key=" << key
                    << ", page=" << location);
#endif
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    if (admissionHook) {
      admissionHook(this, key, admissionHookMeta);
    }
#endif
    if (m_BackgroundWriteback) {
      if (!batch)
        batch = new BackgroundWriteback;
      if (batch) {
        batch->keys[batch->count++] = key;
        if (batch->count == MaxWritebackPages)
          submitBatch();
        continue;
      }
    }
    CacheManager::instance().addCacheRequest(this, true, CacheConstants::WriteBack, key, location,
                                             true, true);
  }
  submitBatch();
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
  __atomic_store_n(&m_PeriodicTimerEnabled, true, __ATOMIC_RELEASE);
}

void Cache::setBackgroundWriteback(writeback_batch_t callback) {
  LockGuard<Spinlock> guard(m_Lock);
  if (static_cast<size_t>(m_ShutdownState) || !m_Callback || m_Pages.count() ||
      m_BackgroundWriteback || !callback) {
    FATAL("Background writeback must be installed before publishing cache pages");
    return;
  }
  m_BackgroundWriteback = callback;
}

void Cache::releaseBackgroundWriteback(BackgroundWriteback* batch) {
  for (size_t i = 0; i < batch->count; ++i)
    releaseWriteback(batch->keys[i]);
  delete batch;
}

void Cache::setDirtyTracking(DirtyTracking tracking) {
  if (!ensureUsable("setDirtyTracking"))
    return;
  LockGuard<Spinlock> guard(m_Lock);
  if (m_Pages.count()) {
    FATAL("Cache dirty tracking must be selected before inserting pages");
    return;
  }
  m_DirtyTracking = tracking;
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
  if (p7 == 2) {
    auto* batch = reinterpret_cast<BackgroundWriteback*>(p3);
    const bool succeeded =
        syncBatchInternal(batch->keys, batch->count, m_BackgroundWriteback, m_CallbackMeta, true);
    releaseBackgroundWriteback(batch);
    return succeeded ? 2 : 0;
  }

  // Eviction request?
  if (static_cast<CacheConstants::CallbackCause>(p2) == CacheConstants::PleaseEvict) {
    evict(p3);
    return 1;
  }

  // sync() transfers a pin to its request before dropping the cache lock.
  // Timer-driven requests acquire their pin here.
  if (!p5) {
    LockGuard<Spinlock> guard(m_Lock);
    CachePage* page = m_Pages.lookup(p3);
    if (!page || page->evictionState == CachePage::EvictionState::Draining ||
        page->evictionState == CachePage::EvictionState::Retiring) {
      return 0;
    }
    ++page->refcnt;
    ++page->writebackPins;
  }

  // Never block the shared worker behind a direct callback which may itself
  // submit work to CacheManager. A rejected request retains dirty data.
  const bool succeeded = writebackPage(p3, p4, false, p7 != 0);

  // Unpin page, writeback complete
  releaseWriteback(p3);

  return succeeded ? 2 : 0;
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
  if (!tracksChecksum(pPage))
    return;
  void* buffer = reinterpret_cast<void*>(pPage->location);
  checksum(buffer, CachePageSize, pPage->checksum);
}

bool Cache::verifyChecksum(CachePage* pPage, bool replace) {
  if (!tracksChecksum(pPage))
    return true;
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

bool Cache::tracksChecksum(const CachePage* page) const {
  return m_DirtyTracking == DirtyTracking::Checksum || page->externallyWritable ||
         page->mutableLoans;
}

bool Cache::needsWriteback(CachePage* page) {
  return page->writebackFailed || page->mutationGeneration != page->writtenGeneration ||
         !verifyChecksum(page);
}

void Cache::updateWritebackIndex(CachePage* page) {
  if (m_DirtyTracking != DirtyTracking::Explicit)
    return;
  const bool candidate = page->externallyWritable || page->mutableLoans || page->writebackFailed ||
                         page->mutationGeneration != page->writtenGeneration ||
                         page->status == CachePage::Editing || page->callbackActive;
  if (candidate && !page->writebackIndexed) {
    m_WritebackPages.insert(page->key, page);
    page->writebackIndexed = true;
  } else if (!candidate && page->writebackIndexed) {
    m_WritebackPages.remove(page->key);
    page->writebackIndexed = false;
  }
}

void Cache::recordMutation(CachePage* page) {
  ++page->mutationGeneration;
  if (page->mutationGeneration == page->writtenGeneration)
    ++page->mutationGeneration;
  updateWritebackIndex(page);
}

void Cache::checksum(const void* data, size_t len, uint64_t out[2]) {
  MurmurHash3_x64_128(data, len, 0, out);
}

void Cache::markEditing(uintptr_t key, size_t length) {
  if (!ensureUsable("markEditing")) {
    return;
  }

  if (length && (length % CachePageSize)) {
    WARNING("Cache::markEditing called with a length that isn't page-aligned");
    return;
  }

  if (!length) {
    length = CachePageSize;
  }

  LockGuard<Spinlock> guard(m_Lock);

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
    updateWritebackIndex(pPage);
  }
}

void Cache::markNoLongerEditing(uintptr_t key, size_t length) {
  if (!ensureUsable("markNoLongerEditing")) {
    return;
  }

  if (length && (length % CachePageSize)) {
    WARNING("Cache::markNoLongerEditing called with a length that isn't page-aligned");
    return;
  }

  if (!length) {
    length = CachePageSize;
  }

  LockGuard<Spinlock> guard(m_Lock);

  size_t nPages = length / CachePageSize;

  for (size_t page = 0; page < nPages; page++) {
    if (!m_PageFilter.contains(key + (page * CachePageSize))) {
      continue;
    }

    CachePage* pPage = m_Pages.lookup(key + (page * CachePageSize));
    if (!pPage) {
      continue;
    }

    pPage->status = tracksChecksum(pPage) ? CachePage::EditTransition : CachePage::ChecksumStable;

    // We have to checksum here as a write could happen between now and the
    // actual handling of the EditTransition, which would lead to some pages
    // potentially failing to complete a writeback (not good).
    calculateChecksum(pPage);
    updateWritebackIndex(pPage);
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
