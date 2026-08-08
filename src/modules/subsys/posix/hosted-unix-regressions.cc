/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "modules/subsys/posix/UnixFilesystem.h"
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/time/Time.h"

namespace
{
bool check(bool condition, const char *test, const char *detail)
{
    if (condition)
    {
        return true;
    }

    ERROR("HOSTED-WAIT-TEST: FAIL " << test << ": " << detail);
    return false;
}

bool waitUntilQueued(Thread *thread)
{
    const Time::Timestamp deadline =
        Time::getTicks() + (500 * Time::Multiplier::Millisecond);
    while (Time::getTicks() < deadline)
    {
        Thread::WaitDebugInfo info = {};
        uintptr_t debugAddress = 0;
        if (
            thread->getWaitDebugInfo(info) && info.queue && info.queued &&
            thread->getDebugState(debugAddress) == Thread::SemWait)
        {
            return true;
        }
        Scheduler::instance().yield();
    }
    return false;
}

struct PollWaitContext
{
    explicit PollWaitContext(Semaphore *readiness)
        : readiness(readiness), entered(0), woke(0)
    {
    }

    Semaphore *readiness;
    Atomic<size_t> entered;
    Atomic<size_t> woke;
};

int waitForPollReadiness(void *parameter)
{
    PollWaitContext *context =
        reinterpret_cast<PollWaitContext *>(parameter);
    context->entered += 1;
    if (context->readiness->acquireForCompletion())
    {
        context->woke += 1;
    }
    return 0;
}

Thread *startPollWaiter(PollWaitContext &context, const char *name)
{
    Thread *waiter = new Thread(
        Scheduler::instance().getKernelProcess(), waitForPollReadiness,
        &context, nullptr, false, true);
    waiter->setName(String(name));
    return waiter;
}

bool datagramPollSendWake()
{
    UnixSocket socket(
        String(), nullptr, nullptr, nullptr, UnixSocket::Datagram);
    Semaphore readiness(0, false);
    socket.addWaiter(&readiness, true, false);

    PollWaitContext context(&readiness);
    Thread *waiter =
        startPollWaiter(context, "hosted Unix datagram send poll");
    const bool queued = waitUntilQueued(waiter);

    char sent = 'd';
    const bool wrote =
        socket.writeBytewise(
            0, sizeof(sent), reinterpret_cast<uintptr_t>(&sent), false) ==
        sizeof(sent);
    const bool joined = waiter->join();

    char received = 0;
    String from;
    const bool read =
        socket.recvfrom(
            sizeof(received), reinterpret_cast<uintptr_t>(&received), false,
            from) == sizeof(received);

    const bool passed = check(
        context.entered == 1 && queued && wrote && joined &&
            context.woke == 1 && read && received == sent &&
            !readiness.tryAcquire(),
        "unix-datagram-poll-send",
        "a datagram did not wake exactly the waiter enrolled on its ring");
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS unix-datagram-poll-send");
    }
    return passed;
}

bool datagramPollCloseWake()
{
    UnixSocket socket(
        String(), nullptr, nullptr, nullptr, UnixSocket::Datagram);
    Semaphore readiness(0, false);
    socket.addWaiter(&readiness, true, false);

    PollWaitContext context(&readiness);
    Thread *waiter =
        startPollWaiter(context, "hosted Unix datagram close poll");
    const bool queued = waitUntilQueued(waiter);

    socket.unbind();
    const bool joined = waiter->join();

    Semaphore lateReadiness(0, false);
    socket.addWaiter(&lateReadiness, true, false);

    const bool passed = check(
        context.entered == 1 && queued && joined && context.woke == 1 &&
            socket.getState() == UnixSocket::Closed &&
            socket.select(false, 0) && !socket.select(true, 0) &&
            !readiness.tryAcquire() && lateReadiness.tryAcquire(),
        "unix-datagram-poll-close",
        "datagram close did not wake poll with persistent EOF readiness");
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS unix-datagram-poll-close");
    }
    return passed;
}

bool streamPollCloseWake()
{
    UnixSocket local(
        String(), nullptr, nullptr, nullptr, UnixSocket::Streaming);
    UnixSocket peer(
        String(), nullptr, nullptr, nullptr, UnixSocket::Streaming);
    const bool connected = local.bind(&peer);
    peer.acknowledgeBind();

    Semaphore readiness(0, false);
    local.addWaiter(&readiness, true, false);

    PollWaitContext context(&readiness);
    Thread *waiter =
        startPollWaiter(context, "hosted Unix stream close poll");
    const bool queued = waitUntilQueued(waiter);

    peer.unbind();
    const bool joined = waiter->join();

    Semaphore lateReadiness(0, false);
    local.addWaiter(&lateReadiness, true, false);

    const bool passed = check(
        connected && context.entered == 1 && queued && joined &&
            context.woke == 1 && local.getState() == UnixSocket::Closed &&
            local.select(false, 0) && !local.select(true, 0) &&
            !readiness.tryAcquire() && lateReadiness.tryAcquire() &&
            !lateReadiness.tryAcquire(),
        "unix-stream-poll-close",
        "stream close did not wake poll with persistent EOF readiness");
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS unix-stream-poll-close");
    }
    return passed;
}
}  // namespace

EXPORTED_PUBLIC bool runHostedUnixDatagramRegressions()
{
    return datagramPollSendWake() && datagramPollCloseWake() &&
           streamPollCloseWake();
}
