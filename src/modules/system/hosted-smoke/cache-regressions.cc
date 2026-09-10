/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Cache.h"

namespace {
constexpr size_t PageSize = TargetInfo::getPageSize();

bool checkNamed(bool condition, const char* test, const char* detail) {
  if (condition) {
    return true;
  }

  ERROR("HOSTED-WAIT-TEST: FAIL " << test << ": " << detail);
  return false;
}

bool check(bool condition, const char* detail) {
  return checkNamed(condition, "cache-callback-lifetime", detail);
}

bool waitUntilQueued(Thread* thread, size_t debugState) {
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (Time::getTicks() < deadline) {
    Thread::WaitDebugInfo info = {};
    uintptr_t debugAddress = 0;
    if (thread->getWaitDebugInfo(info) && info.queue && info.queued &&
        thread->getDebugState(debugAddress) == debugState) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}

bool waitUntilQueuedAt(Thread* thread, size_t debugState, uintptr_t debugAddress) {
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (Time::getTicks() < deadline) {
    Thread::WaitDebugInfo info = {};
    uintptr_t address = 0;
    if (thread->getWaitDebugInfo(info) && info.queue && info.queued &&
        thread->getDebugState(address) == debugState && address == debugAddress) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}

struct CacheLifetimeContext {
  CacheLifetimeContext()
      : cache(nullptr),
        callbackEntered(0),
        allowCallbackReturn(0),
        callbackCalls(0),
        evictionCalls(0),
        reentrantPins(0),
        deleteReturned(0) {}

  Cache* cache;
  Semaphore callbackEntered;
  Semaphore allowCallbackReturn;
  Atomic<size_t> callbackCalls;
  Atomic<size_t> evictionCalls;
  Atomic<size_t> reentrantPins;
  Atomic<size_t> deleteReturned;
};

bool cacheCallback(CacheConstants::CallbackCause cause, uintptr_t loc, uintptr_t, void* parameter) {
  CacheLifetimeContext* context = reinterpret_cast<CacheLifetimeContext*>(parameter);
  if (cause == CacheConstants::Eviction) {
    context->evictionCalls += 1;
  } else if (cause == CacheConstants::WriteBack) {
    uintptr_t page = context->cache->lookup(loc);
    if (page) {
      context->reentrantPins += 1;
      context->cache->release(loc);
    }
  }
  const size_t call = (context->callbackCalls += 1);
  if (call == 1) {
    context->callbackEntered.release();
    const bool released = context->allowCallbackReturn.acquireForCompletion();
    (void)released;
  }
  return true;
}

int deleteCache(void* parameter) {
  CacheLifetimeContext* context = reinterpret_cast<CacheLifetimeContext*>(parameter);
  delete context->cache;
  context->deleteReturned += 1;
  return 0;
}

bool callbackLifetime() {
  CacheLifetimeContext context;
  context.cache = new Cache;
  context.cache->setCallback(cacheCallback, &context);

  constexpr uintptr_t Key = 0xCA7E000;
  const uintptr_t page = context.cache->insert(Key);
  if (!check(page != 0, "could not create the test cache page")) {
    delete context.cache;
    return false;
  }

  context.cache->markNoLongerEditing(Key);
  context.cache->triggerChecksum(Key);
  reinterpret_cast<uint8_t*>(page)[0] ^= 0xA5;
  context.cache->sync(Key, true);

  if (!check(context.callbackEntered.acquire(1, 2),
             "the queued writeback callback did not start")) {
    // Keep cleanup safe if the callback crossed the timeout boundary.
    context.allowCallbackReturn.release();
    delete context.cache;
    return false;
  }

  Thread* deleter = new Thread(Scheduler::instance().getKernelProcess(), deleteCache, &context,
                               nullptr, false, true);
  deleter->setName("hosted Cache callback-drain deleter");

  const bool drainPublished = waitUntilQueued(deleter, Thread::CallbackDrain);
  const bool callbackPinnedObject = context.deleteReturned == 0;

  context.allowCallbackReturn.release();
  const bool joined = deleter->join();

  const bool passed =
      check(drainPublished, "destruction did not publish its callback-drain wait") &&
      check(callbackPinnedObject, "Cache destruction returned while its callback was active") &&
      check(joined, "the Cache deleter did not become reapable") &&
      check(context.deleteReturned == 1, "Cache destruction did not complete exactly once") &&
      check(context.callbackCalls == 2,
            "writeback and eviction callbacks did not execute exactly once") &&
      check(context.reentrantPins == 1, "dirty writeback could not safely re-enter the Cache") &&
      check(context.evictionCalls == 1, "Cache destruction did not reclaim the inserted page");

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS cache-callback-lifetime");
  }
  return passed;
}

struct QueuedLifetimeContext {
  QueuedLifetimeContext()
      : target(nullptr),
        blockerEntered(0),
        allowBlockerReturn(0),
        blockerCalls(0),
        targetCalls(0),
        deleteReturned(0) {}

  Cache* target;
  Semaphore blockerEntered;
  Semaphore allowBlockerReturn;
  Atomic<size_t> blockerCalls;
  Atomic<size_t> targetCalls;
  Atomic<size_t> deleteReturned;
};

bool blockerCallback(CacheConstants::CallbackCause, uintptr_t, uintptr_t, void* parameter) {
  QueuedLifetimeContext* context = reinterpret_cast<QueuedLifetimeContext*>(parameter);
  if ((context->blockerCalls += 1) == 1) {
    context->blockerEntered.release();
    const bool released = context->allowBlockerReturn.acquireForCompletion();
    (void)released;
  }
  return true;
}

bool queuedTargetCallback(CacheConstants::CallbackCause, uintptr_t, uintptr_t, void* parameter) {
  QueuedLifetimeContext* context = reinterpret_cast<QueuedLifetimeContext*>(parameter);
  context->targetCalls += 1;
  return true;
}

int deleteQueuedCache(void* parameter) {
  QueuedLifetimeContext* context = reinterpret_cast<QueuedLifetimeContext*>(parameter);
  delete context->target;
  context->deleteReturned += 1;
  return 0;
}

bool queuedRequestLifetime() {
  QueuedLifetimeContext context;
  Cache blocker;
  blocker.setCallback(blockerCallback, &context);

  constexpr uintptr_t BlockerKey = 0xCA7E010;
  constexpr uintptr_t TargetKey = 0xCA7E020;
  if (!checkNamed(blocker.insert(BlockerKey) != 0, "cache-queued-lifetime",
                  "could not create the worker-blocking page")) {
    return false;
  }
  blocker.markNoLongerEditing(BlockerKey);
  blocker.sync(BlockerKey, true);
  if (!checkNamed(context.blockerEntered.acquire(1, 2), "cache-queued-lifetime",
                  "the blocking Cache callback did not start")) {
    context.allowBlockerReturn.release();
    return false;
  }

  context.target = new Cache;
  context.target->setCallback(queuedTargetCallback, &context);
  if (!checkNamed(context.target->insert(TargetKey) != 0, "cache-queued-lifetime",
                  "could not create the queued target page")) {
    context.allowBlockerReturn.release();
    delete context.target;
    return false;
  }
  context.target->markNoLongerEditing(TargetKey);
  context.target->sync(TargetKey, true);

  Thread* deleter = new Thread(Scheduler::instance().getKernelProcess(), deleteQueuedCache,
                               &context, nullptr, false, true);
  deleter->setName("hosted queued Cache lease deleter");

  const bool queuedLeasePublished = waitUntilQueued(deleter, Thread::CallbackDrain);
  const bool queuedRequestPinnedObject = context.deleteReturned == 0;

  context.allowBlockerReturn.release();
  const bool joined = deleter->join();

  const bool passed = checkNamed(queuedLeasePublished, "cache-queued-lifetime",
                                 "destruction did not wait for a queued request lease") &&
                      checkNamed(queuedRequestPinnedObject, "cache-queued-lifetime",
                                 "a queued request did not pin its Cache") &&
                      checkNamed(joined && context.deleteReturned == 1, "cache-queued-lifetime",
                                 "queued Cache destruction did not complete") &&
                      checkNamed(context.targetCalls == 2, "cache-queued-lifetime",
                                 "the queued writeback and final eviction did not both execute");

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS cache-queued-lifetime");
  }
  return passed;
}

bool emptyAndReuse() {
  Cache cache;
  constexpr uintptr_t FirstKey = 0xCA7E100;
  constexpr uintptr_t SecondKey = 0xCA7E200;

  const uintptr_t firstPage = cache.insert(FirstKey);
  if (!checkNamed(firstPage != 0, "cache-empty-reuse",
                  "could not create the first no-callback page")) {
    return false;
  }

  cache.markNoLongerEditing(FirstKey);
  cache.triggerChecksum(FirstKey);
  reinterpret_cast<uint8_t*>(firstPage)[0] ^= 0x5A;
  cache.empty();

  const uintptr_t secondPage = cache.insert(SecondKey);
  const bool reused = checkNamed(secondPage != 0, "cache-empty-reuse",
                                 "could not insert after emptying a dirty Cache") &&
                      checkNamed(cache.exists(SecondKey, PageSize), "cache-empty-reuse",
                                 "the replacement page was not published");
  cache.empty();

  if (reused) {
    NOTICE("HOSTED-WAIT-TEST: PASS cache-empty-reuse");
  }
  return reused;
}

struct RetirementContext {
  RetirementContext()
      : cache(nullptr),
        evictionEntered(0),
        allowEvictionReturn(0),
        evictionCalls(0),
        evictReturned(0),
        insertReturned(0),
        replacementPage(0) {}

  Cache* cache;
  Semaphore evictionEntered;
  Semaphore allowEvictionReturn;
  Atomic<size_t> evictionCalls;
  Atomic<size_t> evictReturned;
  Atomic<size_t> insertReturned;
  uintptr_t replacementPage;
};

bool retirementCallback(CacheConstants::CallbackCause cause, uintptr_t, uintptr_t,
                        void* parameter) {
  RetirementContext* context = reinterpret_cast<RetirementContext*>(parameter);
  if (cause == CacheConstants::Eviction && (context->evictionCalls += 1) == 1) {
    context->evictionEntered.release();
    const bool released = context->allowEvictionReturn.acquireForCompletion();
    (void)released;
  }
  return true;
}

int evictRetirementPage(void* parameter) {
  RetirementContext* context = reinterpret_cast<RetirementContext*>(parameter);
  context->evictReturned += context->cache->evict(0xCA7E300) ? 1 : 2;
  return 0;
}

int insertRetirementReplacement(void* parameter) {
  RetirementContext* context = reinterpret_cast<RetirementContext*>(parameter);
  context->replacementPage = context->cache->insert(0xCA7E300);
  context->insertReturned += 1;
  return 0;
}

bool retirementPublication() {
  RetirementContext context;
  Cache cache;
  context.cache = &cache;
  cache.setCallback(retirementCallback, &context);

  constexpr uintptr_t Key = 0xCA7E300;
  const uintptr_t originalPage = cache.insert(Key);
  if (!checkNamed(originalPage != 0, "cache-retirement-publication",
                  "could not create the original page")) {
    return false;
  }
  cache.markNoLongerEditing(Key);

  Thread* evictor = new Thread(Scheduler::instance().getKernelProcess(), evictRetirementPage,
                               &context, nullptr, false, true);
  evictor->setName("hosted Cache retirement evictor");

  const bool callbackEntered = context.evictionEntered.acquire(1, 2);
  if (!callbackEntered) {
    context.allowEvictionReturn.release();
    evictor->join();
    cache.empty();
    return checkNamed(false, "cache-retirement-publication",
                      "the eviction callback did not publish retirement");
  }

  const bool retiringPinRejected = !cache.pin(Key);
  const bool retiringLookupRejected = cache.lookup(Key) == 0;

  Thread* inserter = new Thread(Scheduler::instance().getKernelProcess(),
                                insertRetirementReplacement, &context, nullptr, false, true);
  inserter->setName("hosted Cache same-key replacement");

  const bool insertWaitPublished = waitUntilQueued(inserter, Thread::CallbackDrain);
  const bool replacementBlocked = context.insertReturned == 0;

  context.allowEvictionReturn.release();
  const bool evictorJoined = evictor->join();
  const bool inserterJoined = inserter->join();

  const bool passed =
      checkNamed(retiringPinRejected && retiringLookupRejected, "cache-retirement-publication",
                 "a retiring page remained available to a new consumer") &&
      checkNamed(insertWaitPublished, "cache-retirement-publication",
                 "same-key insertion did not wait for retirement publication") &&
      checkNamed(replacementBlocked, "cache-retirement-publication",
                 "same-key insertion returned the retiring page") &&
      checkNamed(evictorJoined && context.evictReturned == 1, "cache-retirement-publication",
                 "the original eviction did not complete successfully") &&
      checkNamed(inserterJoined && context.insertReturned == 1 && context.replacementPage != 0,
                 "cache-retirement-publication", "the replacement insertion did not complete") &&
      checkNamed(cache.exists(Key, PageSize), "cache-retirement-publication",
                 "the replacement page was invalidated by the old callback");

  cache.empty();
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS cache-retirement-publication");
  }
  return passed;
}

struct DiscardEditingContext {
  DiscardEditingContext() : writebacks(0), evictions(0) {}

  Atomic<size_t> writebacks;
  Atomic<size_t> evictions;
};

bool discardEditingCallback(CacheConstants::CallbackCause cause, uintptr_t, uintptr_t,
                            void* parameter) {
  DiscardEditingContext* context = reinterpret_cast<DiscardEditingContext*>(parameter);
  if (cause == CacheConstants::WriteBack) {
    context->writebacks += 1;
  } else if (cause == CacheConstants::Eviction) {
    context->evictions += 1;
  }
  return true;
}

bool failedPublicationDiscard() {
  constexpr uintptr_t Key = 0xCA7E400;
  Cache cache;
  DiscardEditingContext context;
  cache.setCallback(discardEditingCallback, &context);

  const uintptr_t failedPage = cache.insert(Key);
  const bool discarded = failedPage != 0 && cache.discardEditing(Key);
  const bool removed = !cache.exists(Key, PageSize);
  const bool suppressedWriteback = context.writebacks == 0 && context.evictions == 1;

  const uintptr_t pinnedPage = cache.insert(Key);
  const bool pinned = pinnedPage != 0 && cache.pin(Key);
  const bool rejectedPinned = pinned && !cache.discardEditing(Key);
  if (pinned) {
    cache.release(Key);
  }
  const bool discardedAfterRelease = rejectedPinned && cache.discardEditing(Key);

  const uintptr_t publishedPage = cache.insert(Key);
  if (publishedPage) {
    cache.markNoLongerEditing(Key);
  }
  const bool rejectedPublished = publishedPage != 0 && !cache.discardEditing(Key);
  cache.empty();

  const bool passed =
      checkNamed(discarded && removed, "cache-failed-publication-discard",
                 "an unpinned Editing page was not synchronously removed") &&
      checkNamed(suppressedWriteback, "cache-failed-publication-discard",
                 "discarding failed data invoked backing-store writeback") &&
      checkNamed(discardedAfterRelease, "cache-failed-publication-discard",
                 "discard removed a pinned page or could not remove it after release") &&
      checkNamed(rejectedPublished, "cache-failed-publication-discard",
                 "discard removed a page after successful publication");

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS cache-failed-publication-discard");
  }
  return passed;
}

struct RetirePublicationContext {
  RetirePublicationContext()
      : cache(nullptr),
        key(0),
        page(0),
        admissionEntered(0),
        allowPublication(0),
        callbackEntered(0),
        allowCallbackReturn(0),
        admissionCalls(0),
        queuedCallbacks(0),
        queuedCallbackFinished(0),
        evictionCalls(0),
        retireCallbacks(0),
        retireSawQueuedCompletion(0),
        retireArgumentsValid(0),
        syncReturned(0),
        retireReturned(0),
        retireSucceeded(0) {}

  Cache* cache;
  uintptr_t key;
  uintptr_t page;
  Semaphore admissionEntered;
  Semaphore allowPublication;
  Semaphore callbackEntered;
  Semaphore allowCallbackReturn;
  Atomic<size_t> admissionCalls;
  Atomic<size_t> queuedCallbacks;
  Atomic<size_t> queuedCallbackFinished;
  Atomic<size_t> evictionCalls;
  Atomic<size_t> retireCallbacks;
  Atomic<size_t> retireSawQueuedCompletion;
  Atomic<size_t> retireArgumentsValid;
  Atomic<size_t> syncReturned;
  Atomic<size_t> retireReturned;
  Atomic<size_t> retireSucceeded;
};

void retireAdmissionHook(Cache* cache, uintptr_t key, void* parameter) {
  RetirePublicationContext* context = reinterpret_cast<RetirePublicationContext*>(parameter);
  if (cache == context->cache && key == context->key && (context->admissionCalls += 1) == 1) {
    context->admissionEntered.release();
    const bool released = context->allowPublication.acquireForCompletion();
    (void)released;
  }
}

bool retireQueuedCallback(CacheConstants::CallbackCause cause, uintptr_t, uintptr_t,
                          void* parameter) {
  RetirePublicationContext* context = reinterpret_cast<RetirePublicationContext*>(parameter);
  if (cause == CacheConstants::Eviction) {
    context->evictionCalls += 1;
  } else if (cause == CacheConstants::WriteBack && (context->queuedCallbacks += 1) == 1) {
    context->callbackEntered.release();
    const bool released = context->allowCallbackReturn.acquireForCompletion();
    (void)released;
    context->queuedCallbackFinished = 1;
  }
  return true;
}

bool retireSynchronousCallback(uintptr_t key, uintptr_t page, void* parameter) {
  RetirePublicationContext* context = reinterpret_cast<RetirePublicationContext*>(parameter);
  context->retireCallbacks += 1;
  context->retireSawQueuedCompletion = context->queuedCallbackFinished;
  context->retireArgumentsValid = key == context->key && page == context->page;
  return true;
}

int publishRetireWriteback(void* parameter) {
  RetirePublicationContext* context = reinterpret_cast<RetirePublicationContext*>(parameter);
  context->cache->sync(context->key, true);
  context->syncReturned += 1;
  return 0;
}

int retirePublishedWriteback(void* parameter) {
  RetirePublicationContext* context = reinterpret_cast<RetirePublicationContext*>(parameter);
  if (context->cache->retireWriteback(context->key, retireSynchronousCallback, context)) {
    context->retireSucceeded += 1;
  }
  context->retireReturned += 1;
  return 0;
}

bool retirePrepublicationWriteback() {
  constexpr uintptr_t Key = 0xCA7E500;
  RetirePublicationContext context;
  Cache cache;
  context.cache = &cache;
  context.key = Key;
  cache.setCallback(retireQueuedCallback, &context);

  context.page = cache.insert(Key);
  if (!checkNamed(context.page != 0, "cache-retire-prepublication",
                  "could not create the test page")) {
    return false;
  }
  cache.markNoLongerEditing(Key);
  cache.startAtomic();
  cache.setWritebackAdmissionHookForTest(retireAdmissionHook, &context);

  Thread* producer = new Thread(Scheduler::instance().getKernelProcess(), publishRetireWriteback,
                                &context, nullptr, false, true);
  producer->setName("hosted Cache paused writeback producer");
  const bool admissionPaused = context.admissionEntered.acquire(1, 2);
  if (!admissionPaused) {
    context.allowPublication.release();
    context.allowCallbackReturn.release();
    producer->join();
    cache.setWritebackAdmissionHookForTest(nullptr, nullptr);
    cache.endAtomic();
    cache.empty();
    return checkNamed(false, "cache-retire-prepublication",
                      "sync did not pause after publishing its page reference");
  }

  Thread* retirer = new Thread(Scheduler::instance().getKernelProcess(), retirePublishedWriteback,
                               &context, nullptr, false, true);
  retirer->setName("hosted Cache writeback retirer");
  const bool drainPublished = waitUntilQueuedAt(retirer, Thread::CallbackDrain, Key);
  const bool blockedBeforePublication = context.retireReturned == 0;
  if (drainPublished) {
    cache.sync(Key, true);
  }
  const bool drainingSyncRejected = context.admissionCalls == 1;

  context.allowPublication.release();
  const bool queuedCallbackEntered = context.callbackEntered.acquire(1, 2);
  const bool blockedThroughCallback = context.retireReturned == 0;
  context.allowCallbackReturn.release();

  const bool producerJoined = producer->join();
  const bool retirerJoined = retirer->join();
  cache.setWritebackAdmissionHookForTest(nullptr, nullptr);
  cache.endAtomic();

  const bool passed =
      checkNamed(drainPublished, "cache-retire-prepublication",
                 "retirement did not publish its exact per-key CallbackDrain wait") &&
      checkNamed(blockedBeforePublication, "cache-retire-prepublication",
                 "retirement returned before a visible writeback was queued") &&
      checkNamed(drainingSyncRejected, "cache-retire-prepublication",
                 "sync admitted another writeback while retirement was draining") &&
      checkNamed(queuedCallbackEntered && blockedThroughCallback, "cache-retire-prepublication",
                 "retirement did not wait for the queued callback to finish") &&
      checkNamed(producerJoined && retirerJoined, "cache-retire-prepublication",
                 "writeback producer or retirer did not become reapable") &&
      checkNamed(
          context.syncReturned == 1 && context.retireReturned == 1 && context.retireSucceeded == 1,
          "cache-retire-prepublication", "retirement did not complete exactly once") &&
      checkNamed(context.queuedCallbacks == 1 && context.queuedCallbackFinished == 1 &&
                     context.retireCallbacks == 1 && context.retireSawQueuedCompletion == 1,
                 "cache-retire-prepublication",
                 "the synchronous retirement callback overtook queued writeback") &&
      checkNamed(context.retireArgumentsValid == 1 && context.evictionCalls == 1,
                 "cache-retire-prepublication",
                 "retirement callback arguments or final eviction were incorrect") &&
      checkNamed(!cache.exists(Key, PageSize) && cache.lookup(Key) == 0,
                 "cache-retire-prepublication", "the successful page remained published");

  if (cache.exists(Key, PageSize)) {
    cache.empty();
  }
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS cache-retire-prepublication");
  }
  return passed;
}

struct DiscardPublicationContext {
  explicit DiscardPublicationContext(bool cancelPlan)
      : publication(), prepared(0), allowCompletion(0), completed(0), cancel(cancelPlan) {}

  RetirePublicationContext publication;
  Semaphore prepared;
  Semaphore allowCompletion;
  Atomic<size_t> completed;
  bool cancel;
};

int preparePublishedDiscard(void* parameter) {
  auto* context = reinterpret_cast<DiscardPublicationContext*>(parameter);
  auto& publication = context->publication;
  const Cache::DiscardReference reference = {publication.key, 1};
  UniquePointer<Cache::PreparedDiscard> plan;
  const auto status = publication.cache->prepareDiscardFrom(publication.key, &reference, 1, plan);
  if (status == Cache::DiscardStatus::Ready && plan) {
    publication.retireSucceeded += 1;
  }
  publication.retireReturned += 1;
  context->prepared.release();
  const bool released = context->allowCompletion.acquireForCompletion();
  (void)released;
  if (plan && !context->cancel) {
    plan.get()->commit();
  }
  // The plan retains a thread-owned termination deferral through commit or rollback.
  plan.reset();
  context->completed += 1;
  return 0;
}

bool discardPrefixUsable(Cache& cache, uintptr_t key, uintptr_t expected) {
  const uintptr_t lookup = cache.lookup(key);
  const bool pinned = cache.pin(key);
  bool unchanged = lookup && lookup == expected && pinned;
  if (lookup) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(lookup);
    for (size_t n = 0; n < PageSize; ++n) {
      unchanged = unchanged && bytes[n] == static_cast<uint8_t>(n ^ 0x6d);
    }
    cache.release(key);
  }
  if (pinned) {
    cache.release(key);
  }
  return unchanged;
}

bool preparedDiscardPublication(bool cancel) {
  constexpr uintptr_t PrefixKey = 0xCB000000;
  constexpr uintptr_t Key = PrefixKey + PageSize;
  const char* test = cancel ? "cache-discard-cancel" : "cache-discard-prepublication";
  DiscardPublicationContext context(cancel);
  auto& publication = context.publication;
  Cache cache;
  publication.cache = &cache;
  publication.key = Key;
  cache.setCallback(retireQueuedCallback, &publication);
  cache.startAtomic();
  const uintptr_t prefix = cache.insert(PrefixKey);
  publication.page = cache.insert(Key);
  bool ownerPinned = publication.page && cache.pin(Key);
  if (!prefix || !ownerPinned) {
    if (ownerPinned) {
      cache.release(Key);
    }
    publication.allowCallbackReturn.release();
    cache.empty();
    cache.endAtomic();
    return checkNamed(false, test, "could not create and pin the test cache pages");
  }
  for (size_t n = 0; n < PageSize; ++n) {
    reinterpret_cast<uint8_t*>(prefix)[n] = static_cast<uint8_t>(n ^ 0x6d);
    reinterpret_cast<uint8_t*>(publication.page)[n] = static_cast<uint8_t>(n ^ 0xb2);
  }
  cache.markNoLongerEditing(PrefixKey);
  cache.markNoLongerEditing(Key);
  cache.setWritebackAdmissionHookForTest(retireAdmissionHook, &publication);
  Thread* producer = new Thread(Scheduler::instance().getKernelProcess(), publishRetireWriteback,
                                &publication, nullptr, false, true);
  producer->setName("hosted Cache discard writeback producer");
  if (!publication.admissionEntered.acquire(1, 2)) {
    publication.allowPublication.release();
    publication.allowCallbackReturn.release();
    producer->join();
    cache.setWritebackAdmissionHookForTest(nullptr, nullptr);
    cache.release(Key);
    cache.empty();
    cache.endAtomic();
    return checkNamed(false, test, "writeback did not pause after publishing its page pin");
  }

  Thread* preparer = new Thread(Scheduler::instance().getKernelProcess(), preparePublishedDiscard,
                                &context, nullptr, false, true);
  preparer->setName("hosted Cache prepared discard");
  const bool drainPublished = waitUntilQueuedAt(preparer, Thread::CallbackDrain, Key);
  const bool blockedBeforePublication = publication.retireReturned == 0;
  const uintptr_t unexpectedLookup = drainPublished ? cache.lookup(Key) : 0;
  const bool unexpectedPin = drainPublished && cache.pin(Key);
  const bool syncRejected = drainPublished && !cache.sync(Key, true);
  const bool admissionRejected =
      syncRejected && !unexpectedLookup && !unexpectedPin && publication.admissionCalls == 1;
  if (unexpectedLookup) {
    cache.release(Key);
  }
  if (unexpectedPin) {
    cache.release(Key);
  }
  const bool prefixDuringDrain = discardPrefixUsable(cache, PrefixKey, prefix);
  publication.allowPublication.release();
  const bool callbackEntered = publication.callbackEntered.acquire(1, 2);
  const bool blockedThroughCallback = callbackEntered &&
                                      waitUntilQueuedAt(preparer, Thread::CallbackDrain, Key) &&
                                      publication.retireReturned == 0;
  publication.allowCallbackReturn.release();
  const bool readyWithOwnerPin = context.prepared.acquire(1, 2) &&
                                 publication.retireReturned == 1 &&
                                 publication.retireSucceeded == 1;
  if (readyWithOwnerPin) {
    reinterpret_cast<uint8_t*>(publication.page)[0] = 0xa7;
    cache.markDirty(Key);
  }
  if (!cancel || !readyWithOwnerPin) {
    cache.release(Key);
    ownerPinned = false;
  }
  context.allowCompletion.release();
  const bool producerJoined = producer->join();
  const bool preparerJoined = preparer->join();
  cache.setWritebackAdmissionHookForTest(nullptr, nullptr);

  bool completedCorrectly = false;
  if (cancel && readyWithOwnerPin) {
    const uintptr_t restored = cache.lookup(Key);
    const bool pinRestored = cache.pin(Key);
    completedCorrectly = restored == publication.page && pinRestored &&
                         reinterpret_cast<const uint8_t*>(publication.page)[0] == 0xa7 &&
                         publication.queuedCallbacks == 1 && publication.evictionCalls == 0;
    if (restored) {
      cache.release(Key);
    }
    if (pinRestored) {
      cache.release(Key);
    }
    cache.release(Key);
    ownerPinned = false;
    const bool evicted = cache.evict(Key);
    completedCorrectly = completedCorrectly && evicted && publication.queuedCallbacks == 2 &&
                         publication.evictionCalls == 1;
  } else if (!cancel) {
    completedCorrectly = publication.queuedCallbacks == 1 && publication.evictionCalls == 1;
  }
  const bool suffixRemoved = !cache.exists(Key, PageSize);
  const bool prefixAfterCompletion = discardPrefixUsable(cache, PrefixKey, prefix);
  const bool passed =
      checkNamed(drainPublished && blockedBeforePublication, test,
                 "prepare did not wait at its exact CallbackDrain key before queue publication") &&
      checkNamed(admissionRejected && prefixDuringDrain, test,
                 "draining suffix accepted a new consumer or blocked the retained prefix") &&
      checkNamed(blockedThroughCallback, test, "prepare returned before queued writeback ended") &&
      checkNamed(readyWithOwnerPin, test, "prepare did not become Ready with its owner pin held") &&
      checkNamed(producerJoined && preparerJoined && context.completed == 1 &&
                     publication.syncReturned == 1 && publication.queuedCallbackFinished == 1,
                 test, "producer or prepared-discard worker failed to complete exactly once") &&
      checkNamed(completedCorrectly && suffixRemoved, test,
                 cancel ? "rollback lost admission or dirty data needed by ordinary eviction"
                        : "discard wrote dirty bytes back or failed to evict exactly once") &&
      checkNamed(prefixAfterCompletion, test, "completion changed or removed the retained prefix");
  if (ownerPinned) {
    cache.release(Key);
  }
  cache.empty();
  cache.endAtomic();
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS " << test);
  }
  return passed;
}

struct RejectedWritebackContext {
  RejectedWritebackContext()
      : publication(),
        producerFinished(0),
        shutdownFinished(0),
        accepted(0),
        shutdownSucceeded(0) {}

  RetirePublicationContext publication;
  Semaphore producerFinished;
  Semaphore shutdownFinished;
  Atomic<size_t> accepted;
  Atomic<size_t> shutdownSucceeded;
};

int publishRejectedWriteback(void* parameter) {
  auto* context = reinterpret_cast<RejectedWritebackContext*>(parameter);
  auto& publication = context->publication;
  context->accepted = publication.cache->sync(publication.key, true) ? 1 : 0;
  publication.syncReturned += 1;
  context->producerFinished.release();
  return 0;
}

int shutdownRejectedWriteback(void* parameter) {
  auto* context = reinterpret_cast<RejectedWritebackContext*>(parameter);
  context->shutdownSucceeded = context->publication.cache->shutdown() ? 1 : 0;
  context->shutdownFinished.release();
  return 0;
}

bool rejectedLastWritebackPin() {
  constexpr uintptr_t Key = 0xCC000000;
  constexpr const char* Test = "cache-rejected-last-writeback";
  RejectedWritebackContext context;
  auto& publication = context.publication;
  Cache cache;
  CacheManager& manager = CacheManager::instance();
  publication.cache = &cache;
  publication.key = Key;
  cache.setCallback(retireQueuedCallback, &publication);
  // Keep timer publication quiesced through this cache's terminal shutdown.
  cache.startAtomic();
  publication.page = cache.insert(Key);
  if (!checkNamed(publication.page != 0, Test, "could not create the test page")) {
    publication.allowCallbackReturn.release();
    return false;
  }
  reinterpret_cast<uint8_t*>(publication.page)[0] = 0xa7;
  cache.markNoLongerEditing(Key);
  cache.setWritebackAdmissionHookForTest(retireAdmissionHook, &publication);
  Thread* producer = new Thread(Scheduler::instance().getKernelProcess(), publishRejectedWriteback,
                                &context, nullptr, false, true);
  producer->setName("hosted Cache rejected writeback producer");
  const bool admissionPaused = publication.admissionEntered.acquire(1, 2);
  if (admissionPaused) {
    cache.release(Key);
  }
  const bool halted = admissionPaused && manager.halt();
  const bool stopped =
      halted && manager.getLifecycleState() == RequestQueue::LifecycleState::Stopped;
  publication.allowCallbackReturn.release();
  publication.allowPublication.release();
  const bool producerFinished = context.producerFinished.acquire(1, 2);
  if (!producerFinished) {
    const bool resumed = manager.resume();
    (void)resumed;
    checkNamed(false, Test, "rejected writeback producer did not finish");
    FATAL("Cache rejection fixture retained a live producer");
  }
  const bool producerJoined = producer->join();
  const bool rejected = context.accepted == 0 && publication.syncReturned == 1 &&
                        publication.queuedCallbacks == 0 && publication.evictionCalls == 0;
  cache.setWritebackAdmissionHookForTest(nullptr, nullptr);
  const bool resumed = manager.resume();
  if (!resumed && (!manager.halt() || !manager.resume())) {
    checkNamed(false, Test, "could not restore the CacheManager worker");
    FATAL("Cache rejection fixture could not resume CacheManager");
  }

  Thread* shutdown = new Thread(Scheduler::instance().getKernelProcess(), shutdownRejectedWriteback,
                                &context, nullptr, false, true);
  shutdown->setName("hosted Cache rejected-writeback shutdown");
  if (!context.shutdownFinished.acquire(1, 2)) {
    checkNamed(false, Test, "shutdown did not drain the rejected request's cache lease");
    FATAL("Cache rejection fixture retained a live shutdown worker");
  }
  const bool shutdownJoined = shutdown->join();
  const bool unmapped = !VirtualAddressSpace::getKernelAddressSpace().isMapped(
      reinterpret_cast<void*>(publication.page));
  const bool passed =
      checkNamed(admissionPaused && stopped, Test,
                 "writeback was not paused with its last pin before manager halt") &&
      checkNamed(producerFinished && producerJoined && rejected, Test,
                 "the stopped queue executed or retained the rejected writeback") &&
      checkNamed(resumed, Test, "the CacheManager worker did not resume") &&
      checkNamed(shutdownJoined && context.shutdownSucceeded == 1 &&
                     publication.queuedCallbacks == 1 && publication.evictionCalls == 1 && unmapped,
                 Test, "terminal shutdown did not write back and remove the abandoned page");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS " << Test);
  }
  return passed;
}

struct RetireContractContext {
  RetireContractContext()
      : cache(nullptr),
        key(0),
        page(0),
        shouldSucceed(0),
        callbacks(0),
        argumentsValid(0),
        retireReturned(0),
        retireSucceeded(0) {}

  Cache* cache;
  uintptr_t key;
  uintptr_t page;
  Atomic<size_t> shouldSucceed;
  Atomic<size_t> callbacks;
  Atomic<size_t> argumentsValid;
  Atomic<size_t> retireReturned;
  Atomic<size_t> retireSucceeded;
};

bool retireContractCallback(uintptr_t key, uintptr_t page, void* parameter) {
  RetireContractContext* context = reinterpret_cast<RetireContractContext*>(parameter);
  context->callbacks += 1;
  context->argumentsValid = key == context->key && page == context->page;
  return static_cast<size_t>(context->shouldSucceed) != 0;
}

int retirePinnedWriteback(void* parameter) {
  RetireContractContext* context = reinterpret_cast<RetireContractContext*>(parameter);
  if (context->cache->retireWriteback(context->key, retireContractCallback, context)) {
    context->retireSucceeded += 1;
  }
  context->retireReturned += 1;
  return 0;
}

bool retireWritebackContract() {
  constexpr uintptr_t EditingKey = 0xCA7E600;
  constexpr uintptr_t RetryKey = 0xCA7E700;
  constexpr uintptr_t PinnedKey = 0xCA7E800;
  constexpr uintptr_t MissingKey = 0xCA7E900;
  Cache cache;

  RetireContractContext editing;
  editing.cache = &cache;
  editing.key = EditingKey;
  editing.page = cache.insert(EditingKey);
  editing.shouldSucceed = 1;
  const bool editingRejected =
      editing.page && !cache.retireWriteback(EditingKey, retireContractCallback, &editing) &&
      editing.callbacks == 0 && cache.exists(EditingKey, PageSize);
  const bool editingDiscarded = editingRejected && cache.discardEditing(EditingKey);

  RetireContractContext retry;
  retry.cache = &cache;
  retry.key = RetryKey;
  retry.page = cache.insert(RetryKey);
  if (retry.page) {
    cache.markNoLongerEditing(RetryKey);
  }
  const bool failureKeptPage = retry.page &&
                               !cache.retireWriteback(RetryKey, retireContractCallback, &retry) &&
                               retry.callbacks == 1 && cache.lookup(RetryKey) == retry.page;
  if (failureKeptPage) {
    cache.release(RetryKey);
  }
  retry.shouldSucceed = 1;
  const bool retryRetired =
      failureKeptPage && cache.retireWriteback(RetryKey, retireContractCallback, &retry) &&
      retry.callbacks == 2 && retry.argumentsValid == 1 && !cache.exists(RetryKey, PageSize);

  RetireContractContext pinned;
  pinned.cache = &cache;
  pinned.key = PinnedKey;
  pinned.page = cache.insert(PinnedKey);
  if (pinned.page) {
    cache.markNoLongerEditing(PinnedKey);
  }
  pinned.shouldSucceed = 1;
  const bool pinnedReady = pinned.page && cache.pin(PinnedKey);
  Thread* retirer = nullptr;
  if (pinnedReady) {
    retirer = new Thread(Scheduler::instance().getKernelProcess(), retirePinnedWriteback, &pinned,
                         nullptr, false, true);
    retirer->setName("hosted Cache pinned-page retirer");
  }
  const bool pinDrainPublished =
      retirer && waitUntilQueuedAt(retirer, Thread::CallbackDrain, PinnedKey);
  const uintptr_t unexpectedLookup = pinDrainPublished ? cache.lookup(PinnedKey) : 0;
  const bool unexpectedPin = pinDrainPublished && cache.pin(PinnedKey);
  const bool newConsumersRejected =
      pinDrainPublished && pinned.retireReturned == 0 && !unexpectedLookup && !unexpectedPin;
  if (unexpectedLookup) {
    cache.release(PinnedKey);
  }
  if (unexpectedPin) {
    cache.release(PinnedKey);
  }
  if (pinnedReady) {
    cache.release(PinnedKey);
  }
  const bool pinnedJoined = retirer && retirer->join();
  const bool pinnedRetired = pinnedJoined && pinned.retireReturned == 1 &&
                             pinned.retireSucceeded == 1 && pinned.callbacks == 1 &&
                             pinned.argumentsValid == 1 && !cache.exists(PinnedKey, PageSize);

  RetireContractContext missing;
  missing.cache = &cache;
  missing.key = MissingKey;
  missing.shouldSucceed = 1;
  const bool missingSucceeded =
      cache.retireWriteback(MissingKey, retireContractCallback, &missing) && missing.callbacks == 0;

  const bool passed =
      checkNamed(editingDiscarded, "cache-retire-contract",
                 "retirement invoked writeback for an Editing page") &&
      checkNamed(retryRetired, "cache-retire-contract",
                 "failed writeback did not preserve a retryable page") &&
      checkNamed(pinDrainPublished && newConsumersRejected && pinnedRetired,
                 "cache-retire-contract",
                 "retirement did not drain the old pin while rejecting new consumers") &&
      checkNamed(missingSucceeded, "cache-retire-contract",
                 "retiring a missing page invoked the callback or failed");

  cache.empty();
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS cache-retire-contract");
  }
  return passed;
}

struct SyncAllContext {
  SyncAllContext()
      : cache(nullptr),
        lower(nullptr),
        entered(0),
        allowReturn(0),
        writes(0),
        blockWriteback(false),
        retirementSucceeds(false),
        nestedSucceeded(0) {}

  Cache* cache;
  Cache* lower;
  Semaphore entered;
  Semaphore allowReturn;
  Atomic<size_t> writes;
  bool blockWriteback;
  bool retirementSucceeds;
  Atomic<size_t> nestedSucceeded;
};

bool syncAllCallback(CacheConstants::CallbackCause cause, uintptr_t, uintptr_t, void* parameter) {
  auto* context = static_cast<SyncAllContext*>(parameter);
  if (cause != CacheConstants::WriteBack) {
    return true;
  }
  const size_t write = (context->writes += 1);
  if (context->blockWriteback && write == 1) {
    context->entered.release();
    const bool released = context->allowReturn.acquireForCompletion();
    (void)released;
  }
  if (context->lower) {
    const bool succeeded = context->lower->syncAll();
    context->nestedSucceeded = succeeded ? 1 : 0;
    return succeeded;
  }
  return true;
}

bool syncAllRetirementCallback(uintptr_t, uintptr_t, void* parameter) {
  auto* context = static_cast<SyncAllContext*>(parameter);
  context->entered.release();
  const bool released = context->allowReturn.acquireForCompletion();
  (void)released;
  return context->retirementSucceeds;
}

struct SyncAllCall {
  enum Kind { All, QueuedPage, Retire };
  SyncAllCall(SyncAllContext& state, uintptr_t cacheKey, Kind operation)
      : context(state), key(cacheKey), kind(operation), done(0), result(0) {}
  SyncAllContext& context;
  uintptr_t key;
  Kind kind;
  Semaphore done;
  Atomic<size_t> result;
};

int syncAllWorker(void* parameter) {
  auto* call = static_cast<SyncAllCall*>(parameter);
  bool succeeded = false;
  if (call->kind == SyncAllCall::All) {
    succeeded = call->context.cache->syncAll();
  } else if (call->kind == SyncAllCall::QueuedPage) {
    succeeded = call->context.cache->sync(call->key, false);
  } else {
    succeeded =
        call->context.cache->retireWriteback(call->key, syncAllRetirementCallback, &call->context);
  }
  call->result = succeeded ? 1 : 0;
  call->done.release();
  return 0;
}

bool syncAllJoinsCallback() {
  constexpr uintptr_t Key = 0xCA7EA00;
  SyncAllContext context;
  Cache cache;
  context.cache = &cache;
  context.blockWriteback = true;
  cache.setCallback(syncAllCallback, &context);
  if (!cache.insert(Key)) {
    return false;
  }
  cache.markNoLongerEditing(Key);
  SyncAllCall first(context, Key, SyncAllCall::All);
  Thread* writer = new Thread(Scheduler::instance().getKernelProcess(), syncAllWorker, &first,
                              nullptr, false, true);
  const bool entered = context.entered.acquire(1, 2);
  if (!entered) {
    context.allowReturn.release();
    writer->join();
    return checkNamed(false, "cache-sync-all", "direct callback did not start");
  }

  SyncAllCall queued(context, Key, SyncAllCall::QueuedPage);
  Thread* producer = new Thread(Scheduler::instance().getKernelProcess(), syncAllWorker, &queued,
                                nullptr, false, true);
  const bool queueCompleted = queued.done.acquire(1, 2);
  const bool queueRejected = queueCompleted && queued.result == 0;
  SyncAllCall second(context, Key, SyncAllCall::All);
  Thread* joiner = new Thread(Scheduler::instance().getKernelProcess(), syncAllWorker, &second,
                              nullptr, false, true);
  const bool joinedCallback = waitUntilQueuedAt(joiner, Thread::CallbackDrain, Key);
  context.allowReturn.release();
  const bool writerJoined = writer->join();
  const bool producerJoined = producer->join();
  const bool joinerJoined = joiner->join();
  const bool passed =
      checkNamed(queueRejected && producerJoined, "cache-sync-all",
                 "the CacheManager worker blocked behind a direct callback") &&
      checkNamed(joinedCallback && writerJoined && joinerJoined && first.result == 1 &&
                     second.result == 1 && context.writes == 2,
                 "cache-sync-all", "synchronous drain did not join an active callback");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS cache-sync-all-callback");
  }
  return passed;
}

bool syncAllJoinsRetirement(bool succeeds) {
  constexpr uintptr_t Key = 0xCA7EB00;
  SyncAllContext context;
  Cache cache;
  context.cache = &cache;
  context.retirementSucceeds = succeeds;
  cache.setCallback(syncAllCallback, &context);
  if (!cache.insert(Key)) {
    return false;
  }
  cache.markNoLongerEditing(Key);
  SyncAllCall retirement(context, Key, SyncAllCall::Retire);
  Thread* retirer = new Thread(Scheduler::instance().getKernelProcess(), syncAllWorker, &retirement,
                               nullptr, false, true);
  const bool entered = context.entered.acquire(1, 2);
  if (!entered) {
    context.allowReturn.release();
    retirer->join();
    return checkNamed(false, "cache-sync-all", "retirement callback did not start");
  }
  SyncAllCall sync(context, Key, SyncAllCall::All);
  Thread* joiner = new Thread(Scheduler::instance().getKernelProcess(), syncAllWorker, &sync,
                              nullptr, false, true);
  const bool joinedRetirement = waitUntilQueuedAt(joiner, Thread::CallbackDrain, Key);
  context.allowReturn.release();
  const bool retirerJoined = retirer->join();
  const bool joinerJoined = joiner->join();
  const bool passed = checkNamed(
      joinedRetirement && retirerJoined && joinerJoined && sync.result == 1 &&
          retirement.result == (succeeds ? 1U : 0U) && context.writes == (succeeds ? 0U : 1U) &&
          cache.exists(Key, PageSize) != succeeds,
      "cache-sync-all", "drain did not join retirement or retry its retained failure");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS cache-sync-all-retirement-" << (succeeds ? "success" : "retry"));
  }
  return passed;
}

bool syncAllFromCacheManager() {
  constexpr uintptr_t Key = 0xCA7EC00;
  SyncAllContext lowerContext;
  Cache lower;
  lowerContext.cache = &lower;
  lower.setCallback(syncAllCallback, &lowerContext);
  SyncAllContext upperContext;
  Cache upper;
  upperContext.cache = &upper;
  upperContext.lower = &lower;
  upper.setCallback(syncAllCallback, &upperContext);
  if (!lower.insert(Key) || !upper.insert(Key)) {
    return false;
  }
  lower.markNoLongerEditing(Key);
  upper.markNoLongerEditing(Key);
  const bool passed = checkNamed(
      upper.sync(Key, false) && upperContext.nestedSucceeded == 1 && lowerContext.writes == 1,
      "cache-sync-all", "a CacheManager callback could not drain an independent lower cache");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS cache-sync-all-nested");
  }
  return passed;
}

bool rangeExistence() {
  constexpr uintptr_t Key = 0xCA7E500;
  constexpr size_t Length = 3 * PageSize;
  constexpr uintptr_t ProbeKey = Key + (8 * PageSize);
  constexpr uintptr_t SecondProbeKey = ProbeKey + (4 * PageSize);
  Cache cache;

  // Reserve and return a known six-page allocator extent. After a rejected
  // overlap, the same extent must still split into the same two halves.
  // The old partial-publication path leaked the skipped virtual page, which
  // forced the second half to be allocated elsewhere.
  const uintptr_t allocatorExtent = cache.insert(ProbeKey, 2 * Length);
  const bool allocatorProbeReady = allocatorExtent != 0;
  cache.empty();

  const uintptr_t pages = cache.insert(Key, Length);
  const bool completeRange = pages != 0 && cache.exists(Key, Length);
  bool alreadyExisted = false;
  const uintptr_t reused = cache.insert(Key, Length, &alreadyExisted);
  const bool completeRangeReused = alreadyExisted && reused == pages;
  const bool removedInterior = cache.discardEditing(Key + PageSize);
  const bool missingInteriorRejected = removedInterior && !cache.exists(Key, Length);
  cache.empty();

  const uintptr_t interior = cache.insert(Key + PageSize);
  bool overlapExisted = true;
  const uintptr_t overlappingRange = cache.insert(Key, Length, &overlapExisted);
  const bool overlapRejectedBeforeAllocation =
      interior != 0 && !overlappingRange && !overlapExisted &&
      cache.exists(Key + PageSize, PageSize) && !cache.exists(Key, PageSize) &&
      !cache.exists(Key + (2 * PageSize), PageSize) && !cache.exists(Key, Length);
  cache.empty();

  const uintptr_t firstHalf = cache.insert(ProbeKey, Length);
  const uintptr_t secondHalf = cache.insert(SecondProbeKey, Length);
  const bool allocatorAndPageAccountingBalanced =
      allocatorProbeReady && firstHalf == allocatorExtent &&
      secondHalf == allocatorExtent + Length && cache.exists(ProbeKey, Length) &&
      cache.exists(SecondProbeKey, Length);
  cache.empty();

  const bool passed = checkNamed(completeRange && completeRangeReused, "cache-range-existence",
                                 "a complete contiguous cache range was not detected or reused") &&
                      checkNamed(missingInteriorRejected, "cache-range-existence",
                                 "a range with a missing interior page was reported complete") &&
                      checkNamed(overlapRejectedBeforeAllocation, "cache-range-existence",
                                 "an interior overlap partially allocated or published a range") &&
                      checkNamed(allocatorAndPageAccountingBalanced, "cache-range-existence",
                                 "a rejected overlap leaked cache VA or page accounting");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS cache-range-existence");
  }
  return passed;
}

bool strictRangeGeometry() {
  constexpr uintptr_t InsertKey = 0xCA7F000;
  constexpr uintptr_t PublishKey = InsertKey + (4 * PageSize);
  Cache cache;

  bool alreadyExisted = true;
  const uintptr_t invalid = cache.insert(InsertKey, PageSize + 1, &alreadyExisted);
  const bool insertionRejected = !invalid && !alreadyExisted && !cache.exists(InsertKey, PageSize);

  const uintptr_t editing = cache.insert(InsertKey);
  cache.markNoLongerEditing(InsertKey, PageSize + 1);
  const bool invalidPublishLeftEditing = editing && cache.discardEditing(InsertKey);

  const uintptr_t published = cache.insert(PublishKey);
  cache.markNoLongerEditing(PublishKey);
  cache.markEditing(PublishKey, PageSize + 1);
  const bool invalidEditLeftPublished = published && !cache.discardEditing(PublishKey);

  cache.empty();
  const bool passed =
      checkNamed(insertionRejected, "cache-range-geometry",
                 "a partial target-page insertion was truncated instead of rejected") &&
      checkNamed(invalidPublishLeftEditing, "cache-range-geometry",
                 "an invalid publish range changed the first cache page") &&
      checkNamed(invalidEditLeftPublished, "cache-range-geometry",
                 "an invalid edit range changed the first cache page");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS cache-range-geometry");
  }
  return passed;
}

struct TimerWritebackContext {
  Cache* cache = nullptr;
  uintptr_t key = 0;
  Semaphore admissionEntered{0};
  Semaphore allowPublication{0};
  Semaphore callbackEntered{0};
  Semaphore allowCallbackReturn{0};
  Atomic<size_t> admissions{0};
  Atomic<size_t> callbacks{0};
  uint8_t lastWritten = 0;
  bool failFirst = false;
};

void timerWritebackAdmission(Cache*, uintptr_t, void* parameter) {
  auto& context = *static_cast<TimerWritebackContext*>(parameter);
  if ((context.admissions += 1) == 1) {
    context.cache->startAtomic();
    context.admissionEntered.release();
    const bool released = context.allowPublication.acquireForCompletion();
    (void)released;
  }
}

bool timerWritebackCallback(CacheConstants::CallbackCause cause, uintptr_t, uintptr_t page,
                            void* parameter) {
  if (cause != CacheConstants::WriteBack) {
    return true;
  }
  auto& context = *static_cast<TimerWritebackContext*>(parameter);
  const size_t call = (context.callbacks += 1);
  context.lastWritten = *reinterpret_cast<uint8_t*>(page);
  if (call == 1) {
    context.callbackEntered.release();
    const bool released = context.allowCallbackReturn.acquireForCompletion();
    (void)released;
  }
  return !(context.failFirst && call == 1);
}

void tickWriteback(Cache& cache) {
  cache.endAtomic();
  cache.timer(CACHE_WRITEBACK_PERIOD * 1000000ULL);
  cache.startAtomic();
}

int publishTimerWriteback(void* parameter) {
  auto& context = *static_cast<TimerWritebackContext*>(parameter);
  tickWriteback(*context.cache);
  return 0;
}

bool timerWritebackCoalescing(bool failFirst, bool mutateDuringWriteback) {
  const char* test = failFirst ? "cache-timer-pending-failure"
                              : (mutateDuringWriteback ? "cache-timer-pending-mutation"
                                                       : "cache-timer-pending-success");
  TimerWritebackContext context;
  context.failFirst = failFirst;
  context.key = 0xCA7F800;
  Cache cache;
  context.cache = &cache;
  cache.startAtomic();
  cache.setCallback(timerWritebackCallback, &context);
  const uintptr_t page = cache.insert(context.key);
  if (!checkNamed(page != 0, test, "could not create the test page")) {
    return false;
  }
  *reinterpret_cast<uint8_t*>(page) = 0x57;
  cache.markNoLongerEditing(context.key);
  tickWriteback(cache);
  cache.markDirty(context.key);
  cache.setWritebackAdmissionHookForTest(timerWritebackAdmission, &context);

  // A separate synchronous request fences the worker after each released
  // callback, including checksum publication and writeback-pin retirement.
  Cache fence;
  fence.startAtomic();
  fence.setCallback(
      [](CacheConstants::CallbackCause, uintptr_t, uintptr_t, void*) { return true; }, nullptr);
  const uintptr_t fencePage = fence.insert(0);
  if (!checkNamed(fencePage != 0, test, "could not create the worker fence")) {
    cache.setWritebackAdmissionHookForTest(nullptr, nullptr);
    context.allowCallbackReturn.release();
    return false;
  }
  fence.markNoLongerEditing(0);

  Thread* producer = new Thread(Scheduler::instance().getKernelProcess(), publishTimerWriteback,
                                &context, nullptr, false, true);
  producer->setName("hosted Cache paused timer producer");
  const bool admissionPaused = context.admissionEntered.acquire(1, 2);
  if (admissionPaused) {
    for (size_t i = 0; i < 3; ++i) {
      tickWriteback(cache);
    }
  }
  const bool onePendingAdmission = context.admissions == 1;
  context.allowPublication.release();
  const bool producerJoined = producer->join();
  const bool callbackPaused = context.callbackEntered.acquire(1, 2);
  if (callbackPaused) {
    for (size_t i = 0; i < 3; ++i) {
      tickWriteback(cache);
    }
    if (mutateDuringWriteback) {
      *reinterpret_cast<uint8_t*>(page) = 0xA6;
    }
  }
  const bool oneActiveAdmission = context.admissions == 1;
  context.allowCallbackReturn.release();
  const bool firstDrained = fence.sync(0, false);
  const bool oneInitialCallback = context.callbacks == 1;

  // Failure is immediately retryable; a mutation must first be detected by
  // the checksum scan. Drain each epoch so queued work cannot mask a retry.
  bool retriesDrained = true;
  for (size_t i = 0; i < 3; ++i) {
    tickWriteback(cache);
    retriesDrained = fence.sync(0, false) && retriesDrained;
  }
  const size_t expected = failFirst || mutateDuringWriteback ? 2 : 1;
  const bool expectedCallbacks = context.callbacks == expected && context.admissions == expected;
  const bool expectedBytes = context.lastWritten == (mutateDuringWriteback ? 0xA6 : 0x57);
  cache.setWritebackAdmissionHookForTest(nullptr, nullptr);
  const bool reclaimed = cache.empty();

  const bool passed =
      checkNamed(admissionPaused && callbackPaused && producerJoined, test,
                 "writeback did not reach both controlled publication phases") &&
      checkNamed(onePendingAdmission && oneActiveAdmission, test,
                 "timer admitted duplicate work while writeback was pending or active") &&
      checkNamed(firstDrained && oneInitialCallback && retriesDrained, test,
                 "worker did not drain exactly one initial writeback") &&
      checkNamed(expectedCallbacks && expectedBytes, test,
                 "completion lost a mutation, failed to retry, or wrote clean data again") &&
      checkNamed(reclaimed, test, "completed writeback retained a page pin");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS " << test);
  }
  return passed;
}

struct ExplicitCacheCall {
  enum Kind { Sync, Lookup, Redirty };
  ExplicitCacheCall(TimerWritebackContext& state, Kind operation)
      : context(state), kind(operation) {}
  TimerWritebackContext& context;
  Kind kind;
  uintptr_t page = 0;
  Semaphore done{0};
  Atomic<size_t> result{0};
};

int explicitCacheWorker(void* parameter) {
  auto& call = *static_cast<ExplicitCacheCall*>(parameter);
  Cache& cache = *call.context.cache;
  const uintptr_t key = call.context.key;
  bool succeeded = false;
  if (call.kind == ExplicitCacheCall::Sync) {
    succeeded = cache.syncAll();
  } else if (call.kind == ExplicitCacheCall::Lookup) {
    // Transfer the lookup pin to the test so it can verify eviction refusal.
    succeeded = cache.lookupStable(key, call.page, true);
  } else {
    call.page = cache.lookup(key);
    if (call.page) {
      *reinterpret_cast<uint8_t*>(call.page) = 0xA6;
      cache.markDirty(key);
      cache.release(key);
      succeeded = true;
    }
  }
  call.result = succeeded ? 1 : 0;
  call.done.release();
  return 0;
}

bool explicitWritebackThreading(bool redirty) {
  const char* test = redirty ? "cache-explicit-concurrent-redirty" : "cache-explicit-lookup-wakeup";
  TimerWritebackContext context;
  context.key = 0xCA7F900;
  Cache cache;
  context.cache = &cache;
  cache.startAtomic();
  cache.setDirtyTracking(Cache::DirtyTracking::Explicit);
  cache.setCallback(timerWritebackCallback, &context);
  const uintptr_t page = cache.insert(context.key);
  if (!checkNamed(page != 0, test, "could not create the test page")) {
    return false;
  }
  *reinterpret_cast<uint8_t*>(page) = 0x57;
  cache.markNoLongerEditing(context.key);
  cache.markDirty(context.key);

  ExplicitCacheCall first(context, ExplicitCacheCall::Sync);
  Thread* writer = new Thread(Scheduler::instance().getKernelProcess(), explicitCacheWorker, &first,
                              nullptr, false, true);
  writer->setName("hosted Cache explicit paused writer");
  const bool callbackPaused = context.callbackEntered.acquire(1, 2);
  if (!callbackPaused) {
    context.allowCallbackReturn.release();
    writer->joinForCompletion();
    return checkNamed(false, test, "explicit dirty callback did not start");
  }

  ExplicitCacheCall second(context,
                           redirty ? ExplicitCacheCall::Redirty : ExplicitCacheCall::Lookup);
  Thread* peer = new Thread(Scheduler::instance().getKernelProcess(), explicitCacheWorker, &second,
                            nullptr, false, true);
  peer->setName(String(redirty ? "hosted Cache concurrent explicit mutation"
                               : "hosted Cache stable lookup"));
  const bool concurrentPhase = redirty
                                   ? second.done.acquire(1, 2)
                                   : waitUntilQueuedAt(peer, Thread::CallbackDrain, context.key);
  context.allowCallbackReturn.release();
  const bool writerJoined = writer->joinForCompletion();
  bool peerCompleted = concurrentPhase;
  if (!redirty) {
    peerCompleted = second.done.acquire(1, 2);
    if (!peerCompleted) {
      // A lost completion wake must fail the test without stranding its worker.
      // The cache and page remain alive, and the writer has left the callback.
      Thread::WaitDebugInfo wait = {};
      uintptr_t address = 0;
      if (peer->getWaitDebugInfo(wait) && wait.queue && wait.queued &&
          peer->getDebugState(address) == Thread::CallbackDrain && address == context.key) {
        wait.queue->wakeAll(WaitQueue::WakeReason::Signalled,
                            WaitQueue::Channel(wait.channelOwner, wait.channelValue));
      }
    }
  }
  const bool peerJoined = peer->joinForCompletion();
  const bool firstWrite =
      first.result == 1 && context.callbacks == 1 && context.lastWritten == 0x57;
  const bool samePage = second.result == 1 && second.page == page;
  bool lookupPinned = true;
  if (!redirty) {
    lookupPinned = samePage && !cache.evict(context.key);
    if (second.page) {
      cache.release(context.key);
    }
  }

  const bool nextSynced = cache.sync(context.key, false);
  const size_t expectedWrites = redirty ? 2 : 1;
  const bool latestWritten =
      context.callbacks == expectedWrites && context.lastWritten == (redirty ? 0xA6 : 0x57);
  const bool cleanSynced = cache.syncAll();
  const bool stayedClean = context.callbacks == expectedWrites;
  const bool reclaimed = cache.empty();
  const bool passed =
      checkNamed(concurrentPhase && peerCompleted && writerJoined && peerJoined, test,
                 redirty ? "mutation did not finish while the callback was paused"
                         : "stable lookup did not publish and complete its callback-drain wait") &&
      checkNamed(firstWrite && samePage && lookupPinned, test,
                 "initial write or the concurrent page pin changed identity") &&
      checkNamed(nextSynced && latestWritten && cleanSynced && stayedClean, test,
                 "callback completion lost a dirty generation or rewrote a clean page") &&
      checkNamed(reclaimed, test, "completed workers retained a page pin");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS " << test);
  }
  return passed;
}
}  // namespace

bool runHostedCacheDiscardRegressions() {
  return preparedDiscardPublication(false) && preparedDiscardPublication(true) &&
         rejectedLastWritebackPin();
}

bool runHostedCacheSyncRegressions() {
  return syncAllJoinsCallback() && syncAllJoinsRetirement(true) && syncAllJoinsRetirement(false) &&
         syncAllFromCacheManager();
}

bool runHostedCacheTimerRegressions() {
  return timerWritebackCoalescing(false, false) && timerWritebackCoalescing(true, false) &&
         timerWritebackCoalescing(false, true);
}

bool runHostedCacheRegressions() {
  return callbackLifetime() && queuedRequestLifetime() && emptyAndReuse() &&
         retirementPublication() && failedPublicationDiscard() && retirePrepublicationWriteback() &&
         runHostedCacheDiscardRegressions() && retireWritebackContract() && rangeExistence() &&
         strictRangeGeometry() && runHostedCacheSyncRegressions() && runHostedCacheTimerRegressions() &&
         explicitWritebackThreading(false) && explicitWritebackThreading(true);
}
