/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/utilities/Cache.h"
#include "pedigree/kernel/utilities/assert.h"

#if THREADS
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#endif

static constexpr size_t CachePageSize = TargetInfo::getPageSize();

struct Cache::PreparedDiscard::Entry {
  CachePage* page;
  size_t references;
};

Cache::PreparedDiscard::PreparedDiscard(Cache& cache)
    : m_TerminationDeferral(), m_Cache(cache), m_Entries(), m_Count(0), m_Committed(false) {}

Cache::PreparedDiscard::~PreparedDiscard() {
  if (m_Committed) {
    return;
  }
  for (size_t i = 0; i < m_Count; ++i) {
    CachePage* page = m_Entries.get()[i].page;
    {
      LockGuard<Spinlock> guard(m_Cache.m_Lock);
      assert(page->evictionState == CachePage::EvictionState::Draining);
      page->evictionState = CachePage::EvictionState::None;
    }
#if THREADS
    m_Cache.m_EvictionWaiters.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(page));
#endif
  }
}

bool Cache::PreparedDiscard::writeback(retirement_writeback_t callback, void* context) {
  if (m_Committed || !callback)
    return false;
  {
    LockGuard<Spinlock> guard(m_Cache.m_Lock);
    for (size_t i = 0; i < m_Count; ++i) {
      CachePage* page = m_Entries.get()[i].page;
      if (m_Entries.get()[i].references || page->refcnt != 1 || page->writebackPins ||
          page->callbackActive || page->evictionState != CachePage::EvictionState::Draining)
        return false;
    }
  }
  bool succeeded = true;
  for (size_t i = 0; i < m_Count; ++i) {
    CachePage* page = m_Entries.get()[i].page;
    {
      LockGuard<Spinlock> guard(m_Cache.m_Lock);
      page->callbackActive = true;
#if THREADS
      page->callbackOwner = Processor::information().getCurrentThread();
#endif
      // A later device-cache flush may fail even after this write succeeds.
      // Rollback must leave every submitted page eligible for another write.
      m_Cache.recordMutation(page);
    }
    const bool written = callback(page->key, page->location, context);
    if (!written)
      succeeded = false;
    {
      LockGuard<Spinlock> guard(m_Cache.m_Lock);
      page->writebackFailed = page->writebackFailed || !written;
      page->callbackActive = false;
#if THREADS
      page->callbackOwner = nullptr;
#endif
      m_Cache.updateWritebackIndex(page);
    }
#if THREADS
    m_Cache.m_EvictionWaiters.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(page));
#endif
  }
  return succeeded;
}

void Cache::PreparedDiscard::commit() {
  assert(!m_Committed);
  for (size_t i = 0; i < m_Count; ++i) {
    CachePage* page = m_Entries.get()[i].page;
    {
      LockGuard<Spinlock> guard(m_Cache.m_Lock);
      assert(page->evictionState == CachePage::EvictionState::Draining);
      assert(page->refcnt == 1 && !page->writebackPins);
      page->evictionState = CachePage::EvictionState::Retiring;
    }
    // The bytes are intentionally discarded. Eviction callbacks retire only
    // external indexes; running writeback could recreate truncated storage.
    const bool retired = m_Cache.finishRetirement(page, m_Cache.m_Callback, m_Cache.m_CallbackMeta);
    assert(retired);
  }
  m_Committed = true;
}

Cache::DiscardStatus Cache::prepareDiscardFrom(uintptr_t cutoff, const DiscardReference* references,
                                               size_t count,
                                               UniquePointer<PreparedDiscard>& result) {
  result.reset();
  if ((count && !references) || cutoff % CachePageSize) {
    return DiscardStatus::Invalid;
  }
  for (size_t i = 0; i < count; ++i) {
    if (references[i].key % CachePageSize || references[i].references == ~size_t(0) ||
        (i && references[i - 1].key >= references[i].key)) {
      return DiscardStatus::Invalid;
    }
  }
#if THREADS
  TerminationDeferral terminationDeferral;
#endif
  auto plan = UniquePointer<PreparedDiscard>::adopt(new PreparedDiscard(*this));
  if (!plan) {
    return DiscardStatus::NoMemory;
  }
#if THREADS
  if (!m_ManagerOperations.tryAcquire(plan.get()->m_Lease)) {
    return DiscardStatus::Closed;
  }
#endif
  constexpr size_t MaximumPages = 65536;
  size_t pages = 0;
  {
    LockGuard<Spinlock> guard(m_Lock);
    if (static_cast<size_t>(m_ShutdownState)) {
      return DiscardStatus::Closed;
    }
    uintptr_t key = 0;
    CachePage* page = nullptr;
    uintptr_t cursor = cutoff;
    while (m_Pages.lowerBound(cursor, key, page)) {
      if (key == ~uintptr_t(0) || ++pages > MaximumPages) {
        return DiscardStatus::NoMemory;
      }
      cursor = key + 1;
    }
  }
  if (pages) {
    plan.get()->m_Entries = UniqueArray<PreparedDiscard::Entry>::allocate(pages);
    if (!plan.get()->m_Entries) {
      return DiscardStatus::NoMemory;
    }
  }

  DiscardStatus status = DiscardStatus::Ready;
  {
    LockGuard<Spinlock> guard(m_Lock);
    uintptr_t cursor = cutoff;
    uintptr_t key = 0;
    CachePage* page = nullptr;
    size_t referenceIndex = 0;
    while (referenceIndex < count && references[referenceIndex].key < cutoff) {
      ++referenceIndex;
    }
    while (m_Pages.lowerBound(cursor, key, page)) {
      if (plan.get()->m_Count == pages || key == ~uintptr_t(0)) {
        status = DiscardStatus::Busy;
        break;
      }
      cursor = key + 1;
      while (referenceIndex < count && references[referenceIndex].key < key) {
        if (references[referenceIndex++].references) {
          status = DiscardStatus::Invalid;
          break;
        }
      }
      if (status != DiscardStatus::Ready) {
        break;
      }
      size_t expected = 0;
      if (referenceIndex < count && references[referenceIndex].key == key) {
        expected = references[referenceIndex++].references;
      }
      if (page->evictionState != CachePage::EvictionState::None ||
          page->status == CachePage::Editing || page->writebackPins > page->refcnt ||
          page->refcnt - page->writebackPins != 1 + expected) {
        status = DiscardStatus::Busy;
        break;
      }
      page->evictionState = CachePage::EvictionState::Draining;
      plan.get()->m_Entries.get()[plan.get()->m_Count++] = {page, expected};
    }
    while (status == DiscardStatus::Ready && referenceIndex < count) {
      if (references[referenceIndex++].references) {
        status = DiscardStatus::Invalid;
      }
    }
  }
  if (status != DiscardStatus::Ready) {
    return status;
  }

  for (size_t i = 0; i < plan.get()->m_Count; ++i) {
    const auto& entry = plan.get()->m_Entries.get()[i];
#if THREADS
    while (true) {
      auto waitGuard = m_EvictionWaiters.acquire();
      {
        LockGuard<Spinlock> guard(m_Lock);
        if (!entry.page->writebackPins) {
          assert(entry.page->refcnt == 1 + entry.references);
          break;
        }
      }
      const auto reason = waitGuard.waitForCompletion(WaitQueue::Channel(entry.page),
                                                      Thread::CallbackDrain, entry.page->key);
      if (reason == WaitQueue::WakeReason::Unwinding ||
          reason == WaitQueue::WakeReason::Terminating) {
        FATAL("Prepared cache discard interrupted during callback drain");
      }
    }
#else
    if (entry.page->writebackPins) {
      return DiscardStatus::Busy;
    }
#endif
  }
  result = pedigree_std::move(plan);
  return DiscardStatus::Ready;
}

void Cache::releaseWriteback(uintptr_t key) {
  CachePage* page = nullptr;
  bool shouldEvict = false;
  {
    LockGuard<Spinlock> guard(m_Lock);
    page = m_Pages.lookup(key);
    assert(page && page->writebackPins && page->refcnt);
    --page->writebackPins;
    --page->refcnt;
    shouldEvict = !page->refcnt;
  }
#if THREADS
  m_EvictionWaiters.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(page));
#endif
  if (shouldEvict) {
    CacheManager::instance().addCacheRequest(this, true, CacheConstants::PleaseEvict, key);
  }
}
