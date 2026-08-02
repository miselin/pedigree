/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "modules/drivers/common/usb-hcd/CallbackDelivery.h"
#include "modules/drivers/common/usb-hcd/TransferCompletion.h"
#include "modules/system/usb/Usb.h"
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/utility.h"

namespace
{
using Completion = UsbHcd::TransferCompletion;
using DeliveryQueue = UsbHcd::CallbackDeliveryQueue;

bool check(bool condition, const char *test, const char *detail)
{
    if (condition)
        return true;

    ERROR("HOSTED-WAIT-TEST: FAIL " << test << ": " << detail);
    return false;
}

class TransferLifecycleModel
{
  public:
    static constexpr size_t SlotCount = 2;

    struct SlotCleanup
    {
        SlotCleanup() : controller(nullptr), slot(0)
        {
        }

        TransferLifecycleModel *controller;
        size_t slot;
    };

    struct Slot
    {
        Slot()
            : completion(), accepted(), linked(false), dmaOwned(false),
              reclaimed(false), cleanup()
        {
        }

        Completion completion;
        OperationBarrier::Lease accepted;
        bool linked;
        bool dmaOwned;
        bool reclaimed;
        SlotCleanup cleanup;
    };

    TransferLifecycleModel()
        : m_Lock(), m_Submissions(), m_Cancellations(), m_Accepted(),
          m_Deliveries(), m_Slots(), m_Closing(false), m_Shutdown(false),
          m_DmaViolations(0)
    {
        for (size_t i = 0; i < SlotCount; ++i)
        {
            m_Slots[i].cleanup.controller = this;
            m_Slots[i].cleanup.slot = i;
        }
    }

    ~TransferLifecycleModel()
    {
        assert(m_Shutdown);
    }

    bool submit(
        size_t slotIndex, Completion::Callback callback, uintptr_t parameter,
        Semaphore *commitEntered = nullptr, Semaphore *allowCommit = nullptr)
    {
        OperationBarrier::Lease submission;
        if (!m_Submissions.tryAcquire(submission))
            return false;

        LockGuard<Mutex> guard(m_Lock);
        if (commitEntered)
            commitEntered->release();
        if (allowCommit)
        {
            const bool allowed = allowCommit->acquireForCompletion();
            (void) allowed;
        }

        if (m_Closing || slotIndex >= SlotCount)
            return false;

        Slot &slot = m_Slots[slotIndex];
        if (slot.completion.state() != Completion::State::Idle)
            return false;

        OperationBarrier::Lease accepted;
        if (!m_Accepted.tryAcquire(accepted))
            return false;

        // Active is published last, while cancellation is excluded by this
        // same lock. It can therefore never describe an unlinked DMA object.
        slot.accepted = pedigree_std::move(accepted);
        slot.linked = true;
        slot.dmaOwned = true;
        slot.reclaimed = false;
        slot.completion.arm(callback, parameter, m_Deliveries.nextGeneration());
        return true;
    }

    bool captureNatural(size_t slotIndex, ssize_t result)
    {
        LockGuard<Mutex> guard(m_Lock);
        if (slotIndex >= SlotCount)
            return false;
        return m_Slots[slotIndex].completion.captureNatural(result);
    }

    bool
    cancel(size_t slotIndex, Completion::Callback callback, uintptr_t parameter)
    {
        OperationBarrier::Lease cancellation;
        if (!m_Cancellations.tryAcquire(cancellation))
            return false;

        DeliveryQueue::Record *record = nullptr;
        DeliveryQueue::Key drainKey = {0, 0};
        bool drain = false;
        {
            LockGuard<Mutex> guard(m_Lock);
            if (slotIndex >= SlotCount)
                return false;

            Slot &slot = m_Slots[slotIndex];
            Completion::Claim claim;
            const Completion::CancellationDisposition disposition =
                slot.completion.claimCancellation(
                    callback, parameter, -TransactionError, claim);
            if (disposition == Completion::CancellationDisposition::NoMatch)
                return false;
            if (disposition ==
                Completion::CancellationDisposition::DrainPublished)
            {
                drainKey = {slotIndex, claim.generation};
                drain = true;
            }
            else
            {
                releaseDmaLocked(slot);
                record = createRecordLocked(slotIndex, claim);
                List<DeliveryQueue::Record *> batch;
                batch.pushBack(record);
                m_Deliveries.publish(batch);
            }
        }

        if (record)
            m_Deliveries.deliver(record);
        else if (drain)
            (void) m_Deliveries.drain(drainKey);
        return true;
    }

    bool shutdown()
    {
        m_Submissions.closeAndWait();
        m_Accepted.close();

        List<DeliveryQueue::Record *> batch;
        {
            LockGuard<Mutex> guard(m_Lock);
            m_Closing = true;
            for (size_t i = 0; i < SlotCount; ++i)
            {
                Slot &slot = m_Slots[i];
                Completion::Claim claim;
                if (!slot.completion.claimForTeardown(-TransactionError, claim))
                {
                    continue;
                }

                releaseDmaLocked(slot);
                batch.pushBack(createRecordLocked(i, claim));
            }
            if (batch.count())
                m_Deliveries.publish(batch);
        }

        while (batch.count())
            m_Deliveries.deliver(batch.popFront());

        m_Accepted.wait();
        (void) m_Deliveries.drainAll();
        m_Cancellations.closeAndWait();
        const bool passed = m_Deliveries.empty() &&
                            m_Accepted.isClosedAndDrained() && !m_DmaViolations;
        m_Shutdown = true;
        return passed;
    }

    bool linked(size_t slotIndex)
    {
        LockGuard<Mutex> guard(m_Lock);
        return slotIndex < SlotCount && m_Slots[slotIndex].linked;
    }

    bool reclaimed(size_t slotIndex)
    {
        LockGuard<Mutex> guard(m_Lock);
        return slotIndex < SlotCount && m_Slots[slotIndex].reclaimed;
    }

  private:
    static void reclaim(void *parameter)
    {
        auto *cleanup = reinterpret_cast<SlotCleanup *>(parameter);
        TransferLifecycleModel *controller = cleanup->controller;
        LockGuard<Mutex> guard(controller->m_Lock);
        Slot &slot = controller->m_Slots[cleanup->slot];
        if (slot.dmaOwned)
            controller->m_DmaViolations += 1;
        slot.linked = false;
        slot.reclaimed = true;
        slot.accepted = OperationBarrier::Lease();
    }

    void releaseDmaLocked(Slot &slot)
    {
        slot.linked = false;
        slot.dmaOwned = false;
    }

    DeliveryQueue::Record *
    createRecordLocked(size_t slotIndex, const Completion::Claim &claim)
    {
        Slot &slot = m_Slots[slotIndex];
        assert(!slot.dmaOwned);
        return m_Deliveries.create(
            {slotIndex, claim.generation}, claim.callback, claim.parameter,
            claim.result, reclaim, &slot.cleanup);
    }

    Mutex m_Lock;
    OperationBarrier m_Submissions;
    OperationBarrier m_Cancellations;
    OperationBarrier m_Accepted;
    DeliveryQueue m_Deliveries;
    Slot m_Slots[SlotCount];
    bool m_Closing;
    bool m_Shutdown;
    Atomic<size_t> m_DmaViolations;
};

struct ResultContext
{
    ResultContext() : calls(0), result(0)
    {
    }

    Atomic<size_t> calls;
    Atomic<ssize_t> result;
};

void recordResult(uintptr_t parameter, ssize_t result)
{
    auto *context = reinterpret_cast<ResultContext *>(parameter);
    context->result = result;
    context->calls += 1;
}

struct SubmissionRaceContext
{
    SubmissionRaceContext(TransferLifecycleModel *controller)
        : controller(controller), commitEntered(0), allowCommit(0), callback(),
          submitted(0), cancelled(0)
    {
    }

    TransferLifecycleModel *controller;
    Semaphore commitEntered;
    Semaphore allowCommit;
    ResultContext callback;
    Atomic<size_t> submitted;
    Atomic<size_t> cancelled;
};

int submitPausedTransfer(void *parameter)
{
    auto *context = reinterpret_cast<SubmissionRaceContext *>(parameter);
    if (context->controller->submit(
            0, recordResult, reinterpret_cast<uintptr_t>(&context->callback),
            &context->commitEntered, &context->allowCommit))
    {
        context->submitted += 1;
    }
    return 0;
}

int cancelPausedTransfer(void *parameter)
{
    auto *context = reinterpret_cast<SubmissionRaceContext *>(parameter);
    if (context->controller->cancel(
            0, recordResult, reinterpret_cast<uintptr_t>(&context->callback)))
    {
        context->cancelled += 1;
    }
    return 0;
}

bool waitUntilSleeping(Thread *thread)
{
    for (size_t attempt = 0; attempt < 10000; ++attempt)
    {
        if (thread->getStatus() == Thread::Sleeping)
            return true;
        Scheduler::instance().yield();
    }
    return false;
}

bool cancellationCannotObserveHalfPublishedTransfer()
{
    TransferLifecycleModel controller;
    SubmissionRaceContext context(&controller);
    Process *kernelProcess = Scheduler::instance().getKernelProcess();
    Thread *submission = new Thread(
        kernelProcess, submitPausedTransfer, &context, nullptr, false, true);
    const bool commitEntered = context.commitEntered.acquireForCompletion();
    Thread *cancellation = new Thread(
        kernelProcess, cancelPausedTransfer, &context, nullptr, false, true);
    const bool cancellationBlocked = waitUntilSleeping(cancellation);
    const bool callbackRanEarly =
        static_cast<size_t>(context.callback.calls) != 0;

    context.allowCommit.release();
    const bool submissionJoined = submission->joinForCompletion();
    const bool cancellationJoined = cancellation->joinForCompletion();
    const bool shutdown = controller.shutdown();

    const bool passed = check(
        commitEntered && cancellationBlocked && !callbackRanEarly &&
            submissionJoined && cancellationJoined && context.submitted == 1 &&
            context.cancelled == 1 && context.callback.calls == 1 &&
            context.callback.result == -TransactionError &&
            controller.reclaimed(0) && !controller.linked(0) && shutdown,
        "usb-transfer-atomic-publication",
        "cancellation observed Active before the transfer was fully linked");
    if (passed)
        NOTICE("HOSTED-WAIT-TEST: PASS usb-transfer-atomic-publication");
    return passed;
}

bool teardownPreservesNaturalAndFailsActive()
{
    TransferLifecycleModel controller;
    ResultContext natural;
    ResultContext active;
    const bool naturalSubmitted = controller.submit(
        0, recordResult, reinterpret_cast<uintptr_t>(&natural));
    const bool activeSubmitted = controller.submit(
        1, recordResult, reinterpret_cast<uintptr_t>(&active));
    const bool captured = controller.captureNatural(0, 2718);
    const bool shutdown = controller.shutdown();

    const bool passed = check(
        naturalSubmitted && activeSubmitted && captured && shutdown &&
            natural.calls == 1 && natural.result == 2718 && active.calls == 1 &&
            active.result == -TransactionError && controller.reclaimed(0) &&
            controller.reclaimed(1),
        "usb-transfer-teardown-results",
        "teardown lost a natural result or stranded an active obligation");
    if (passed)
        NOTICE("HOSTED-WAIT-TEST: PASS usb-transfer-teardown-results");
    return passed;
}

struct NestedCancelContext
{
    NestedCancelContext(
        TransferLifecycleModel *controller, ResultContext *target)
        : controller(controller), target(target), calls(0), cancelReturned(0)
    {
    }

    TransferLifecycleModel *controller;
    ResultContext *target;
    Atomic<size_t> calls;
    Atomic<size_t> cancelReturned;
};

void cancelSecondFromFirst(uintptr_t parameter, ssize_t result)
{
    auto *context = reinterpret_cast<NestedCancelContext *>(parameter);
    if (result != -TransactionError)
        return;
    context->calls += 1;
    if (context->controller->cancel(
            1, recordResult, reinterpret_cast<uintptr_t>(context->target)))
    {
        context->cancelReturned += 1;
    }
}

bool teardownBatchSupportsNestedCancellation()
{
    TransferLifecycleModel controller;
    ResultContext second;
    NestedCancelContext first(&controller, &second);
    const bool firstSubmitted = controller.submit(
        0, cancelSecondFromFirst, reinterpret_cast<uintptr_t>(&first));
    const bool secondSubmitted = controller.submit(
        1, recordResult, reinterpret_cast<uintptr_t>(&second));
    const bool shutdown = controller.shutdown();

    const bool passed = check(
        firstSubmitted && secondSubmitted && shutdown && first.calls == 1 &&
            first.cancelReturned == 1 && second.calls == 1 &&
            second.result == -TransactionError && controller.reclaimed(0) &&
            controller.reclaimed(1),
        "usb-transfer-teardown-nested-cancel",
        "a teardown callback could not synchronously drain a later callback");
    if (passed)
        NOTICE("HOSTED-WAIT-TEST: PASS usb-transfer-teardown-nested-cancel");
    return passed;
}
}  // namespace

bool runHostedUsbTransferLifecycleRegressions()
{
    return cancellationCannotObserveHalfPublishedTransfer() &&
           teardownPreservesNaturalAndFailsActive() &&
           teardownBatchSupportsNestedCancellation();
}
