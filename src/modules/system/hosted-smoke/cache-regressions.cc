/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Cache.h"

namespace
{
bool checkNamed(bool condition, const char *test, const char *detail)
{
    if (condition)
    {
        return true;
    }

    ERROR(
        "HOSTED-WAIT-TEST: FAIL " << test << ": "
        << detail);
    return false;
}

bool check(bool condition, const char *detail)
{
    return checkNamed(condition, "cache-callback-lifetime", detail);
}

bool waitUntilQueued(Thread *thread, size_t debugState)
{
    const Time::Timestamp deadline =
        Time::getTicks() + (500 * Time::Multiplier::Millisecond);
    while (Time::getTicks() < deadline)
    {
        Thread::WaitDebugInfo info = {};
        uintptr_t debugAddress = 0;
        if (
            thread->getWaitDebugInfo(info) && info.queue && info.queued &&
            thread->getDebugState(debugAddress) == debugState)
        {
            return true;
        }
        Scheduler::instance().yield();
    }
    return false;
}

struct CacheLifetimeContext
{
    CacheLifetimeContext()
        : cache(nullptr), callbackEntered(0), allowCallbackReturn(0),
          callbackCalls(0), evictionCalls(0), reentrantPins(0),
          deleteReturned(0)
    {
    }

    Cache *cache;
    Semaphore callbackEntered;
    Semaphore allowCallbackReturn;
    Atomic<size_t> callbackCalls;
    Atomic<size_t> evictionCalls;
    Atomic<size_t> reentrantPins;
    Atomic<size_t> deleteReturned;
};

void cacheCallback(
    CacheConstants::CallbackCause cause, uintptr_t loc, uintptr_t,
    void *parameter)
{
    CacheLifetimeContext *context =
        reinterpret_cast<CacheLifetimeContext *>(parameter);
    if (cause == CacheConstants::Eviction)
    {
        context->evictionCalls += 1;
    }
    else if (cause == CacheConstants::WriteBack)
    {
        uintptr_t page = context->cache->lookup(loc);
        if (page)
        {
            context->reentrantPins += 1;
            context->cache->release(loc);
        }
    }
    const size_t call = (context->callbackCalls += 1);
    if (call == 1)
    {
        context->callbackEntered.release();
        const bool released =
            context->allowCallbackReturn.acquireForCompletion();
        (void) released;
    }
}

int deleteCache(void *parameter)
{
    CacheLifetimeContext *context =
        reinterpret_cast<CacheLifetimeContext *>(parameter);
    delete context->cache;
    context->deleteReturned += 1;
    return 0;
}

bool callbackLifetime()
{
    CacheLifetimeContext context;
    context.cache = new Cache;
    context.cache->setCallback(cacheCallback, &context);

    constexpr uintptr_t Key = 0xCA7E000;
    const uintptr_t page = context.cache->insert(Key);
    if (!check(page != 0, "could not create the test cache page"))
    {
        delete context.cache;
        return false;
    }

    context.cache->markNoLongerEditing(Key);
    context.cache->triggerChecksum(Key);
    reinterpret_cast<uint8_t *>(page)[0] ^= 0xA5;
    context.cache->sync(Key, true);

    if (!check(
            context.callbackEntered.acquire(1, 2),
            "the queued writeback callback did not start"))
    {
        // Keep cleanup safe if the callback crossed the timeout boundary.
        context.allowCallbackReturn.release();
        delete context.cache;
        return false;
    }

    Thread *deleter = new Thread(
        Scheduler::instance().getKernelProcess(), deleteCache, &context,
        nullptr, false, true);
    deleter->setName("hosted Cache callback-drain deleter");

    const bool drainPublished =
        waitUntilQueued(deleter, Thread::CallbackDrain);
    const bool callbackPinnedObject = context.deleteReturned == 0;

    context.allowCallbackReturn.release();
    const bool joined = deleter->join();

    const bool passed =
        check(
            drainPublished,
            "destruction did not publish its callback-drain wait") &&
        check(
            callbackPinnedObject,
            "Cache destruction returned while its callback was active") &&
        check(joined, "the Cache deleter did not become reapable") &&
        check(
            context.deleteReturned == 1,
            "Cache destruction did not complete exactly once") &&
        check(
            context.callbackCalls == 3,
            "writeback and eviction callbacks did not execute exactly once") &&
        check(
            context.reentrantPins == 2,
            "dirty writeback could not safely re-enter the Cache") &&
        check(
            context.evictionCalls == 1,
            "Cache destruction did not reclaim the inserted page");

    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS cache-callback-lifetime");
    }
    return passed;
}

struct QueuedLifetimeContext
{
    QueuedLifetimeContext()
        : target(nullptr), blockerEntered(0), allowBlockerReturn(0),
          blockerCalls(0), targetCalls(0), deleteReturned(0)
    {
    }

    Cache *target;
    Semaphore blockerEntered;
    Semaphore allowBlockerReturn;
    Atomic<size_t> blockerCalls;
    Atomic<size_t> targetCalls;
    Atomic<size_t> deleteReturned;
};

void blockerCallback(
    CacheConstants::CallbackCause, uintptr_t, uintptr_t, void *parameter)
{
    QueuedLifetimeContext *context =
        reinterpret_cast<QueuedLifetimeContext *>(parameter);
    if ((context->blockerCalls += 1) == 1)
    {
        context->blockerEntered.release();
        const bool released =
            context->allowBlockerReturn.acquireForCompletion();
        (void) released;
    }
}

void queuedTargetCallback(
    CacheConstants::CallbackCause, uintptr_t, uintptr_t, void *parameter)
{
    QueuedLifetimeContext *context =
        reinterpret_cast<QueuedLifetimeContext *>(parameter);
    context->targetCalls += 1;
}

int deleteQueuedCache(void *parameter)
{
    QueuedLifetimeContext *context =
        reinterpret_cast<QueuedLifetimeContext *>(parameter);
    delete context->target;
    context->deleteReturned += 1;
    return 0;
}

bool queuedRequestLifetime()
{
    QueuedLifetimeContext context;
    Cache blocker;
    blocker.setCallback(blockerCallback, &context);

    constexpr uintptr_t BlockerKey = 0xCA7E010;
    constexpr uintptr_t TargetKey = 0xCA7E020;
    if (!checkNamed(
            blocker.insert(BlockerKey) != 0, "cache-queued-lifetime",
            "could not create the worker-blocking page"))
    {
        return false;
    }
    blocker.markNoLongerEditing(BlockerKey);
    blocker.sync(BlockerKey, true);
    if (!checkNamed(
            context.blockerEntered.acquire(1, 2),
            "cache-queued-lifetime",
            "the blocking Cache callback did not start"))
    {
        context.allowBlockerReturn.release();
        return false;
    }

    context.target = new Cache;
    context.target->setCallback(queuedTargetCallback, &context);
    if (!checkNamed(
            context.target->insert(TargetKey) != 0,
            "cache-queued-lifetime",
            "could not create the queued target page"))
    {
        context.allowBlockerReturn.release();
        delete context.target;
        return false;
    }
    context.target->markNoLongerEditing(TargetKey);
    context.target->sync(TargetKey, true);

    Thread *deleter = new Thread(
        Scheduler::instance().getKernelProcess(), deleteQueuedCache, &context,
        nullptr, false, true);
    deleter->setName("hosted queued Cache lease deleter");

    const bool queuedLeasePublished =
        waitUntilQueued(deleter, Thread::CallbackDrain);
    const bool queuedRequestPinnedObject = context.deleteReturned == 0;

    context.allowBlockerReturn.release();
    const bool joined = deleter->join();

    const bool passed =
        checkNamed(
            queuedLeasePublished, "cache-queued-lifetime",
            "destruction did not wait for a queued request lease") &&
        checkNamed(
            queuedRequestPinnedObject, "cache-queued-lifetime",
            "a queued request did not pin its Cache") &&
        checkNamed(
            joined && context.deleteReturned == 1,
            "cache-queued-lifetime",
            "queued Cache destruction did not complete") &&
        checkNamed(
            context.targetCalls == 2, "cache-queued-lifetime",
            "the queued writeback and final eviction did not both execute");

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS cache-queued-lifetime");
    }
    return passed;
}

bool emptyAndReuse()
{
    Cache cache;
    constexpr uintptr_t FirstKey = 0xCA7E100;
    constexpr uintptr_t SecondKey = 0xCA7E200;

    const uintptr_t firstPage = cache.insert(FirstKey);
    if (!checkNamed(
            firstPage != 0, "cache-empty-reuse",
            "could not create the first no-callback page"))
    {
        return false;
    }

    cache.markNoLongerEditing(FirstKey);
    cache.triggerChecksum(FirstKey);
    reinterpret_cast<uint8_t *>(firstPage)[0] ^= 0x5A;
    cache.empty();

    const uintptr_t secondPage = cache.insert(SecondKey);
    const bool reused =
        checkNamed(
            secondPage != 0, "cache-empty-reuse",
            "could not insert after emptying a dirty Cache") &&
        checkNamed(
            cache.exists(SecondKey, 4096), "cache-empty-reuse",
            "the replacement page was not published");
    cache.empty();

    if (reused)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS cache-empty-reuse");
    }
    return reused;
}

struct RetirementContext
{
    RetirementContext()
        : cache(nullptr), evictionEntered(0), allowEvictionReturn(0),
          evictionCalls(0), evictReturned(0), insertReturned(0),
          replacementPage(0)
    {
    }

    Cache *cache;
    Semaphore evictionEntered;
    Semaphore allowEvictionReturn;
    Atomic<size_t> evictionCalls;
    Atomic<size_t> evictReturned;
    Atomic<size_t> insertReturned;
    uintptr_t replacementPage;
};

void retirementCallback(
    CacheConstants::CallbackCause cause, uintptr_t, uintptr_t,
    void *parameter)
{
    RetirementContext *context =
        reinterpret_cast<RetirementContext *>(parameter);
    if (
        cause == CacheConstants::Eviction &&
        (context->evictionCalls += 1) == 1)
    {
        context->evictionEntered.release();
        const bool released =
            context->allowEvictionReturn.acquireForCompletion();
        (void) released;
    }
}

int evictRetirementPage(void *parameter)
{
    RetirementContext *context =
        reinterpret_cast<RetirementContext *>(parameter);
    context->evictReturned += context->cache->evict(0xCA7E300) ? 1 : 2;
    return 0;
}

int insertRetirementReplacement(void *parameter)
{
    RetirementContext *context =
        reinterpret_cast<RetirementContext *>(parameter);
    context->replacementPage = context->cache->insert(0xCA7E300);
    context->insertReturned += 1;
    return 0;
}

bool retirementPublication()
{
    RetirementContext context;
    Cache cache;
    context.cache = &cache;
    cache.setCallback(retirementCallback, &context);

    constexpr uintptr_t Key = 0xCA7E300;
    const uintptr_t originalPage = cache.insert(Key);
    if (!checkNamed(
            originalPage != 0, "cache-retirement-publication",
            "could not create the original page"))
    {
        return false;
    }
    cache.markNoLongerEditing(Key);

    Thread *evictor = new Thread(
        Scheduler::instance().getKernelProcess(), evictRetirementPage,
        &context, nullptr, false, true);
    evictor->setName("hosted Cache retirement evictor");

    const bool callbackEntered =
        context.evictionEntered.acquire(1, 2);
    if (!callbackEntered)
    {
        context.allowEvictionReturn.release();
        evictor->join();
        cache.empty();
        return checkNamed(
            false, "cache-retirement-publication",
            "the eviction callback did not publish retirement");
    }

    const bool retiringPinRejected = !cache.pin(Key);
    const bool retiringLookupRejected = cache.lookup(Key) == 0;

    Thread *inserter = new Thread(
        Scheduler::instance().getKernelProcess(), insertRetirementReplacement,
        &context, nullptr, false, true);
    inserter->setName("hosted Cache same-key replacement");

    const bool insertWaitPublished =
        waitUntilQueued(inserter, Thread::CallbackDrain);
    const bool replacementBlocked = context.insertReturned == 0;

    context.allowEvictionReturn.release();
    const bool evictorJoined = evictor->join();
    const bool inserterJoined = inserter->join();

    const bool passed =
        checkNamed(
            retiringPinRejected && retiringLookupRejected,
            "cache-retirement-publication",
            "a retiring page remained available to a new consumer") &&
        checkNamed(
            insertWaitPublished, "cache-retirement-publication",
            "same-key insertion did not wait for retirement publication") &&
        checkNamed(
            replacementBlocked, "cache-retirement-publication",
            "same-key insertion returned the retiring page") &&
        checkNamed(
            evictorJoined && context.evictReturned == 1,
            "cache-retirement-publication",
            "the original eviction did not complete successfully") &&
        checkNamed(
            inserterJoined && context.insertReturned == 1 &&
                context.replacementPage != 0,
            "cache-retirement-publication",
            "the replacement insertion did not complete") &&
        checkNamed(
            cache.exists(Key, 4096), "cache-retirement-publication",
            "the replacement page was invalidated by the old callback");

    cache.empty();
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS cache-retirement-publication");
    }
    return passed;
}

struct DiscardEditingContext
{
    DiscardEditingContext() : writebacks(0), evictions(0)
    {
    }

    Atomic<size_t> writebacks;
    Atomic<size_t> evictions;
};

void discardEditingCallback(
    CacheConstants::CallbackCause cause, uintptr_t, uintptr_t, void *parameter)
{
    DiscardEditingContext *context =
        reinterpret_cast<DiscardEditingContext *>(parameter);
    if (cause == CacheConstants::WriteBack)
    {
        context->writebacks += 1;
    }
    else if (cause == CacheConstants::Eviction)
    {
        context->evictions += 1;
    }
}

bool failedPublicationDiscard()
{
    constexpr uintptr_t Key = 0xCA7E400;
    Cache cache;
    DiscardEditingContext context;
    cache.setCallback(discardEditingCallback, &context);

    const uintptr_t failedPage = cache.insert(Key);
    const bool discarded =
        failedPage != 0 && cache.discardEditing(Key);
    const bool removed = !cache.exists(Key, 4096);
    const bool suppressedWriteback =
        context.writebacks == 0 && context.evictions == 1;

    const uintptr_t pinnedPage = cache.insert(Key);
    const bool pinned = pinnedPage != 0 && cache.pin(Key);
    const bool rejectedPinned = pinned && !cache.discardEditing(Key);
    if (pinned)
    {
        cache.release(Key);
    }
    const bool discardedAfterRelease =
        rejectedPinned && cache.discardEditing(Key);

    const uintptr_t publishedPage = cache.insert(Key);
    if (publishedPage)
    {
        cache.markNoLongerEditing(Key);
    }
    const bool rejectedPublished =
        publishedPage != 0 && !cache.discardEditing(Key);
    cache.empty();

    const bool passed =
        checkNamed(
            discarded && removed, "cache-failed-publication-discard",
            "an unpinned Editing page was not synchronously removed") &&
        checkNamed(
            suppressedWriteback, "cache-failed-publication-discard",
            "discarding failed data invoked backing-store writeback") &&
        checkNamed(
            discardedAfterRelease, "cache-failed-publication-discard",
            "discard removed a pinned page or could not remove it after release") &&
        checkNamed(
            rejectedPublished, "cache-failed-publication-discard",
            "discard removed a page after successful publication");

    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS cache-failed-publication-discard");
    }
    return passed;
}

bool rangeExistence()
{
    constexpr uintptr_t Key = 0xCA7E500;
    constexpr uintptr_t ProbeKey = 0xCA80500;
    constexpr uintptr_t SecondProbeKey = 0xCA86500;
    constexpr size_t Length = 3 * 4096;
    Cache cache;

    // Reserve and return a known six-page allocator extent. After a rejected
    // overlap, the same extent must still split into the same two halves.
    // The old partial-publication path leaked the skipped virtual page, which
    // forced the second half to be allocated elsewhere.
    const uintptr_t allocatorExtent = cache.insert(ProbeKey, 2 * Length);
    const bool allocatorProbeReady = allocatorExtent != 0;
    cache.empty();

    const uintptr_t pages = cache.insert(Key, Length);
    const bool completeRange =
        pages != 0 && cache.exists(Key, Length);
    bool alreadyExisted = false;
    const uintptr_t reused =
        cache.insert(Key, Length, &alreadyExisted);
    const bool completeRangeReused =
        alreadyExisted && reused == pages;
    const bool removedInterior = cache.discardEditing(Key + 4096);
    const bool missingInteriorRejected =
        removedInterior && !cache.exists(Key, Length);
    cache.empty();

    const uintptr_t interior = cache.insert(Key + 4096);
    bool overlapExisted = true;
    const uintptr_t overlappingRange =
        cache.insert(Key, Length, &overlapExisted);
    const bool overlapRejectedBeforeAllocation =
        interior != 0 && !overlappingRange && !overlapExisted &&
        cache.exists(Key + 4096, 4096) &&
        !cache.exists(Key, 4096) &&
        !cache.exists(Key + (2 * 4096), 4096) &&
        !cache.exists(Key, Length);
    cache.empty();

    const uintptr_t firstHalf = cache.insert(ProbeKey, Length);
    const uintptr_t secondHalf = cache.insert(SecondProbeKey, Length);
    const bool allocatorAndPageAccountingBalanced =
        allocatorProbeReady && firstHalf == allocatorExtent &&
        secondHalf == allocatorExtent + Length &&
        cache.exists(ProbeKey, Length) &&
        cache.exists(SecondProbeKey, Length);
    cache.empty();

    const bool passed =
        checkNamed(
            completeRange && completeRangeReused, "cache-range-existence",
            "a complete contiguous cache range was not detected or reused") &&
        checkNamed(
            missingInteriorRejected, "cache-range-existence",
            "a range with a missing interior page was reported complete") &&
        checkNamed(
            overlapRejectedBeforeAllocation, "cache-range-existence",
            "an interior overlap partially allocated or published a range") &&
        checkNamed(
            allocatorAndPageAccountingBalanced, "cache-range-existence",
            "a rejected overlap leaked cache VA or page accounting");
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS cache-range-existence");
    }
    return passed;
}
}  // namespace

bool runHostedCacheRegressions()
{
    return callbackLifetime() && queuedRequestLifetime() && emptyAndReuse() &&
           retirementPublication() && failedPublicationDiscard() &&
           rangeExistence();
}
