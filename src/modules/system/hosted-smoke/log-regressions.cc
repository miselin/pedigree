/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/lib.h"

namespace
{
constexpr size_t Attempts = 10000;
constexpr char SnapshotA[] = "hosted-log-snapshot-a";
constexpr char SnapshotB[] = "hosted-log-snapshot-b";

bool check(bool condition, const char *detail)
{
    if (condition)
    {
        return true;
    }

    ERROR("HOSTED-WAIT-TEST: FAIL log-callback-lifetime: " << detail);
    return false;
}

struct RemovalContext;

class BlockingLogger : public Log::LogCallback
{
  public:
    explicit BlockingLogger(RemovalContext &context) : m_Context(context)
    {
    }

    void callback(const LogCord &, bool) override;

  private:
    RemovalContext &m_Context;
};

struct RemovalWorker
{
    RemovalContext *context;
};

struct RemovalContext
{
    RemovalContext()
        : logger(*this), phase(0), callbackCalls(0), callbackAfterRemoval(0),
          removerStarted(0), removerReturned(0), removersObservedWaiting(0),
          failures(0), removerA(nullptr), removerB(nullptr)
    {
        workerA.context = this;
        workerB.context = this;
    }

    BlockingLogger logger;
    Atomic<size_t> phase;
    Atomic<size_t> callbackCalls;
    Atomic<size_t> callbackAfterRemoval;
    Atomic<size_t> removerStarted;
    Atomic<size_t> removerReturned;
    Atomic<size_t> removersObservedWaiting;
    Atomic<size_t> failures;
    Thread *removerA;
    Thread *removerB;
    RemovalWorker workerA;
    RemovalWorker workerB;
};

RemovalContext *g_RemovalContext = nullptr;

void BlockingLogger::callback(const LogCord &, bool)
{
    m_Context.callbackCalls += 1;
    if (m_Context.removerReturned)
    {
        m_Context.callbackAfterRemoval += 1;
    }
}

bool removerWaiting(Thread *thread, Log::LogCallback *callback)
{
    Thread::WaitDebugInfo info = {};
    uintptr_t debugAddress = 0;
    return thread && thread->getWaitDebugInfo(info) && info.queue &&
           info.channelOwner && info.queued &&
           thread->getDebugState(debugAddress) == Thread::CallbackDrain &&
           debugAddress == reinterpret_cast<uintptr_t>(callback);
}

void callbackPinHook(Log::LogCallback *callback)
{
    RemovalContext *context = g_RemovalContext;
    if (
        !context || callback != &context->logger ||
        !context->phase.compareAndSwap(0, 1))
    {
        return;
    }

    for (size_t attempt = 0; attempt < Attempts; ++attempt)
    {
        if (
            context->removerStarted == static_cast<size_t>(2) &&
            removerWaiting(context->removerA, callback) &&
            removerWaiting(context->removerB, callback))
        {
            context->removersObservedWaiting += 1;
            break;
        }
        Scheduler::instance().yield();
    }

    if (!context->removersObservedWaiting)
    {
        context->failures += 1;
    }
    context->phase = 2;
}

int removePinnedCallback(void *parameter)
{
    RemovalWorker *worker = reinterpret_cast<RemovalWorker *>(parameter);
    RemovalContext *context = worker->context;
    for (
        size_t attempt = 0;
        attempt < Attempts && context->phase == static_cast<size_t>(0);
        ++attempt)
    {
        Scheduler::instance().yield();
    }

    if (context->phase == static_cast<size_t>(0))
    {
        context->failures += 1;
        return 1;
    }

    context->removerStarted += 1;
    if (!Log::instance().removeCallback(&context->logger))
    {
        context->failures += 1;
    }
    context->removerReturned += 1;
    return 0;
}

class SelfRemovingLogger : public Log::LogCallback
{
  public:
    SelfRemovingLogger() : calls(0), deferred(0)
    {
    }

    void callback(const LogCord &, bool) override
    {
        calls += 1;
        if (!Log::instance().removeCallback(this))
        {
            deferred += 1;
        }
    }

    Atomic<size_t> calls;
    Atomic<size_t> deferred;
};

bool callbackLifetime()
{
    RemovalContext context;
    if (!Log::instance().installCallback(&context.logger, true))
    {
        return check(false, "the test callback could not be registered");
    }

    Process *process = Scheduler::instance().getKernelProcess();
    context.removerA = new Thread(
        process, removePinnedCallback, &context.workerA, nullptr, false, true);
    context.removerB = new Thread(
        process, removePinnedCallback, &context.workerB, nullptr, false, true);
    context.removerA->setName("hosted log callback remover A");
    context.removerB->setName("hosted log callback remover B");

    g_RemovalContext = &context;
    Log::setCallbackPinHook(callbackPinHook);
    NOTICE("hosted-log-callback-drain");

    const bool joinedA = context.removerA->join();
    const bool joinedB = context.removerB->join();
    Log::setCallbackPinHook(nullptr);
    g_RemovalContext = nullptr;

    NOTICE("hosted-log-callback-after-removal");
    Log::instance().removeCallback(&context.logger);

    SelfRemovingLogger selfRemoving;
    const bool selfInstalled =
        Log::instance().installCallback(&selfRemoving, true);
    NOTICE("hosted-log-self-removal");
    NOTICE("hosted-log-after-self-removal");
    Log::instance().removeCallback(&selfRemoving);

    bool passed = true;
    passed &= check(
        joinedA && joinedB && context.failures == 0,
        "concurrent callback removers did not complete cleanly");
    passed &= check(
        context.removersObservedWaiting == 1 &&
            context.removerReturned == 2,
        "both external removers did not join the same callback drain");
    passed &= check(
        context.callbackCalls == 1 && context.callbackAfterRemoval == 0,
        "a callback ran after synchronous removal returned");
    passed &= check(
        selfInstalled && selfRemoving.calls == 1 &&
            selfRemoving.deferred == 1,
        "self-removal did not close admission without deadlocking");

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS log-callback-lifetime");
    }
    return passed;
}

struct SnapshotContext;

class SnapshotLogger : public Log::LogCallback
{
  public:
    explicit SnapshotLogger(SnapshotContext &context) : m_Context(context)
    {
    }

    void callback(const LogCord &cord, bool) override;

  private:
    SnapshotContext &m_Context;
};

struct SnapshotContext
{
    SnapshotContext()
        : logger(*this), phase(0), sawA(0), sawB(0), unknown(0),
          worker(nullptr)
    {
    }

    SnapshotLogger logger;
    Atomic<size_t> phase;
    Atomic<size_t> sawA;
    Atomic<size_t> sawB;
    Atomic<size_t> unknown;
    Thread *worker;
};

SnapshotContext *g_SnapshotContext = nullptr;

bool contains(const String &message, const char *needle, size_t length)
{
    if (message.length() < length)
    {
        return false;
    }

    for (size_t i = 0; i <= (message.length() - length); ++i)
    {
        if (StringCompareN(message.cstr() + i, needle, length) == 0)
        {
            return true;
        }
    }
    return false;
}

void SnapshotLogger::callback(const LogCord &cord, bool)
{
    const String message = cord.toString();
    if (contains(message, SnapshotA, sizeof(SnapshotA) - 1))
    {
        m_Context.sawA += 1;
    }
    else if (contains(message, SnapshotB, sizeof(SnapshotB) - 1))
    {
        m_Context.sawB += 1;
    }
    else
    {
        m_Context.unknown += 1;
    }
}

void entrySnapshotHook(const Log::LogEntry &entry)
{
    SnapshotContext *context = g_SnapshotContext;
    if (
        !context || !(entry.str == SnapshotA) ||
        !context->phase.compareAndSwap(0, 1))
    {
        return;
    }

    for (
        size_t attempt = 0;
        attempt < Attempts && context->phase != static_cast<size_t>(2);
        ++attempt)
    {
        Scheduler::instance().yield();
    }
}

int writeSecondSnapshot(void *parameter)
{
    SnapshotContext *context =
        reinterpret_cast<SnapshotContext *>(parameter);
    for (
        size_t attempt = 0;
        attempt < Attempts && context->phase != static_cast<size_t>(1);
        ++attempt)
    {
        Scheduler::instance().yield();
    }
    if (context->phase != static_cast<size_t>(1))
    {
        return 1;
    }

    NOTICE(SnapshotB);
    context->phase = 2;
    return 0;
}

bool entrySnapshotIsolation()
{
    SnapshotContext context;
    context.worker = new Thread(
        Scheduler::instance().getKernelProcess(), writeSecondSnapshot,
        &context, nullptr, false, true);
    context.worker->setName("hosted concurrent log writer");
    if (!Log::instance().installCallback(&context.logger, true))
    {
        context.phase = 1;
        context.worker->join();
        return check(false, "the snapshot callback could not be registered");
    }

    g_SnapshotContext = &context;
    Log::setEntrySnapshotHook(entrySnapshotHook);
    NOTICE(SnapshotA);
    const bool joined = context.worker->join();
    Log::setEntrySnapshotHook(nullptr);
    g_SnapshotContext = nullptr;
    Log::instance().removeCallback(&context.logger);

    const bool passed = check(
        joined && context.phase == static_cast<size_t>(2) &&
            context.sawA == 1 && context.sawB == 1 &&
            context.unknown == 0,
        "concurrent writers did not retain distinct entry snapshots");
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS log-entry-snapshot");
    }
    return passed;
}
}  // namespace

bool runHostedLogRegressions()
{
    return callbackLifetime() && entrySnapshotIsolation();
}
