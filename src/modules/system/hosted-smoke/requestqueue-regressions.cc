/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/RoundRobin.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/SignalEvent.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/RequestQueue.h"

#include "modules/drivers/common/ata/AtaController.h"

namespace {
constexpr size_t RequestQueueSignalNumber = 11;
constexpr size_t WaitAttempts = 10000;

Atomic<size_t> g_RequestQueueSignalCalls(0);

void hostedRequestQueueSignalHandler(size_t) {
  g_RequestQueueSignalCalls += 1;
}

bool check(bool condition, const char* detail) {
  if (condition) {
    return true;
  }

  ERROR("HOSTED-WAIT-TEST: FAIL requestqueue-lifecycle: " << detail);
  return false;
}

bool waitUntilQueued(Thread* thread, size_t debugState) {
  for (size_t attempt = 0; attempt < WaitAttempts; ++attempt) {
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

class HostedRequestQueue : public RequestQueue {
 public:
  enum Operation {
    Sum = 1,
    SelfSubmit,
    SelfSubmitInner,
    SelfHalt,
    HoldWorker,
    CancelQueued,
    CancelLifecycleProbe,
    PreallocatedHold,
    Record,
    RecordHold,
  };

  HostedRequestQueue()
      : RequestQueue(MakeConstantString("Hosted wait regression")),
        executions(0),
        cancellations(0),
        queuedCancellations(0),
        comparisons(0),
        recordedCount(0),
        recordFailures(0),
        selfHaltRejections(0),
        cancelHaltRejections(0),
        cancelResumeRejections(0),
        cancelPublicationRejections(0),
        cancelPreallocatedRejections(0),
        holdStarted(0),
        releaseHold(0),
        matchEqualPayload(false) {}

  ~HostedRequestQueue() override {
    destroy();
  }

  Atomic<size_t> executions;
  Atomic<size_t> cancellations;
  Atomic<size_t> queuedCancellations;
  Atomic<size_t> comparisons;
  Atomic<size_t> recordedCount;
  Atomic<size_t> recordFailures;
  Atomic<size_t> selfHaltRejections;
  Atomic<size_t> cancelHaltRejections;
  Atomic<size_t> cancelResumeRejections;
  Atomic<size_t> cancelPublicationRejections;
  Atomic<size_t> cancelPreallocatedRejections;
  PreallocatedRequest cancelPreallocated;
  uint64_t recorded[16] = {};
  Semaphore holdStarted;
  Semaphore releaseHold;

  size_t requestWaiterCount() {
    return m_RequestQueueWaiters.waiterCount();
  }

  Thread* workerThread() {
    auto guard = m_RequestQueueWaiters.acquire();
    return m_pThread;
  }

  void setMaxAsyncRequests(size_t maximum) {
    m_nMaxAsyncRequests = maximum;
  }

  void setMatchEqualPayload(bool match) {
    matchEqualPayload = match;
  }

 protected:
  uint64_t executeRequest(uint64_t p1, uint64_t p2, uint64_t p3, uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t) override {
    executions += 1;

    switch (p1) {
      case Sum:
      case SelfSubmitInner:
        return p2 + p3;
      case SelfSubmit:
        return addRequest(0, SelfSubmitInner, p2, p3);
      case SelfHalt:
        if (!halt()) {
          selfHaltRejections += 1;
          return p2;
        }
        return 0;
      case HoldWorker:
      case PreallocatedHold:
        holdStarted.release();
        return releaseHold.acquire() ? p2 : 0;
      case Record:
      case RecordHold: {
        const size_t index = (recordedCount += 1) - 1;
        if (index < (sizeof(recorded) / sizeof(recorded[0]))) {
          recorded[index] = p2;
        } else {
          recordFailures += 1;
        }
        if (p1 == RecordHold) {
          holdStarted.release();
          if (!releaseHold.acquireForCompletion()) {
            return 0;
          }
        }
        return p2;
      }
      default:
        return 0;
    }
  }

  void cancelRequest(const Request& request) override {
    cancellations += 1;
    if (request.p1 == CancelQueued || request.p1 == CancelLifecycleProbe) {
      queuedCancellations += 1;
    }
    if (request.p1 == CancelLifecycleProbe) {
      if (!halt()) {
        cancelHaltRejections += 1;
      }
      if (!resume()) {
        cancelResumeRejections += 1;
      }
      if (addRequest(0, CancelLifecycleProbe) == 0) {
        cancelPublicationRejections += 1;
      }
      if (addAsyncRequest(0, CancelLifecycleProbe) == 0) {
        cancelPublicationRejections += 1;
      }
      if (publishPreallocated(cancelPreallocated, 0, CancelLifecycleProbe) ==
              PreallocatedPublishResult::QueueStopped &&
          cancelPreallocated.isAvailable()) {
        cancelPreallocatedRejections += 1;
      }
    }
  }

  bool compareRequests(const Request& a, const Request& b) override {
    comparisons += 1;
    return matchEqualPayload && a.p1 == b.p1 && a.p2 == b.p2 && a.p3 == b.p3 && a.p4 == b.p4 &&
           a.p5 == b.p5 && a.p6 == b.p6 && a.p7 == b.p7 && a.p8 == b.p8;
  }

 private:
  bool matchEqualPayload;
};

class AtaRequestIdentityProbe : public AtaController {
 public:
  using AtaController::canCoalesceCanonicalRequests;
};

class HostedAtaRequestQueue : public RequestQueue {
 public:
  HostedAtaRequestQueue()
      : RequestQueue(MakeConstantString("Hosted ATA request identity")),
        executions(0),
        firstStarted(0),
        releaseFirst(0) {}

  ~HostedAtaRequestQueue() override {
    destroy();
  }

  Atomic<size_t> executions;
  Semaphore firstStarted;
  Semaphore releaseFirst;

 protected:
  uint64_t executeRequest(uint64_t, uint64_t, uint64_t location, uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t) override {
    const size_t execution = executions += 1;
    if (execution == 1) {
      firstStarted.release();
      if (!releaseFirst.acquireForCompletion()) {
        return 0;
      }
    }
    return location;
  }

  bool compareRequests(const Request& a, const Request& b) override {
    return AtaRequestIdentityProbe::canCoalesceCanonicalRequests(a.p1, a.p2, a.p3, b.p1, b.p2,
                                                                 b.p3);
  }
};

bool ataRequestIdentityRegression() {
  constexpr uint64_t DiskA = 0xA7A;
  constexpr uint64_t DiskB = 0xB7B;
  constexpr uint64_t FirstPartitionPage = 0x600;
  constexpr uint64_t SecondPartitionPage = FirstPartitionPage + 0x1000;
  constexpr uint64_t SecondPartitionExtent = FirstPartitionPage + (128 * 1024);

  HostedAtaRequestQueue writeQueue;
  writeQueue.initialise();
  bool passed = writeQueue.addAsyncRequest(0, SCSI_REQUEST_WRITE, DiskA, FirstPartitionPage) == 1 &&
                writeQueue.firstStarted.acquire();
  const bool repeatedWriteAccepted =
      writeQueue.addAsyncRequest(0, SCSI_REQUEST_WRITE, DiskA, FirstPartitionPage) == 1;
  const bool distinctWritePageAccepted =
      writeQueue.addAsyncRequest(0, SCSI_REQUEST_WRITE, DiskA, SecondPartitionPage) == 1;
  writeQueue.releaseFirst.release();
  const bool writesDrained = writeQueue.drain();
  writeQueue.destroy();
  passed &= repeatedWriteAccepted && distinctWritePageAccepted && writesDrained &&
            writeQueue.executions == 3;

  HostedAtaRequestQueue syncQueue;
  syncQueue.initialise();
  passed &= syncQueue.addAsyncRequest(0, SCSI_REQUEST_SYNC, DiskA, FirstPartitionPage) == 1 &&
            syncQueue.firstStarted.acquire();
  const bool repeatedSyncAccepted =
      syncQueue.addAsyncRequest(0, SCSI_REQUEST_SYNC, DiskA, FirstPartitionPage) == 1;
  syncQueue.releaseFirst.release();
  const bool syncsDrained = syncQueue.drain();
  syncQueue.destroy();
  passed &= repeatedSyncAccepted && syncsDrained && syncQueue.executions == 2;

  HostedAtaRequestQueue readQueue;
  readQueue.initialise();
  passed &= readQueue.addAsyncRequest(0, SCSI_REQUEST_READ, DiskA, FirstPartitionPage) == 1 &&
            readQueue.firstStarted.acquire();
  const bool repeatedReadCoalesced =
      readQueue.addAsyncRequest(0, SCSI_REQUEST_READ, DiskA, FirstPartitionPage) == 0;
  const bool distinctExtentAccepted =
      readQueue.addAsyncRequest(0, SCSI_REQUEST_READ, DiskA, SecondPartitionExtent) == 1;
  const bool differentTypeAccepted =
      readQueue.addAsyncRequest(0, SCSI_REQUEST_WRITE, DiskA, FirstPartitionPage) == 1;
  const bool differentDiskAccepted =
      readQueue.addAsyncRequest(0, SCSI_REQUEST_READ, DiskB, FirstPartitionPage) == 1;
  readQueue.releaseFirst.release();
  const bool readsDrained = readQueue.drain();
  readQueue.destroy();
  passed &= repeatedReadCoalesced && distinctExtentAccepted && differentTypeAccepted &&
            differentDiskAccepted && readsDrained && readQueue.executions == 4;

  if (!check(passed, "ATA request identity dropped write or sync work, or merged distinct reads")) {
    return false;
  }

  NOTICE("HOSTED-WAIT-TEST: PASS ata-request-identity");
  return true;
}

bool watchdogProgressRegression() {
  HostedRequestQueue queue;
  queue.initialise();

  bool passed = queue.addAsyncRequest(0, HostedRequestQueue::HoldWorker, 42) == 1 &&
                queue.holdStarted.acquire();
  passed &= queue.addAsyncRequest(0, HostedRequestQueue::Sum, 20, 22) == 1;
  const RequestQueue::OverrunStatus transient = queue.sampleOverrunForTest();

  queue.releaseHold.release();
  const bool firstDrained = queue.drain();
  const RequestQueue::OverrunStatus cleared = queue.sampleOverrunForTest();

  passed &= queue.addAsyncRequest(0, HostedRequestQueue::HoldWorker, 42) == 1 &&
            queue.holdStarted.acquire();
  passed &= queue.addAsyncRequest(0, HostedRequestQueue::Sum, 19, 23) == 1;
  const RequestQueue::OverrunStatus baseline = queue.sampleOverrunForTest();
  const RequestQueue::OverrunStatus stalled = queue.sampleOverrunForTest();

  queue.releaseHold.release();
  const bool secondDrained = queue.drain();
  queue.destroy();

  passed &= transient == RequestQueue::OverrunStatus::Armed && firstDrained &&
            cleared == RequestQueue::OverrunStatus::Clear &&
            baseline == RequestQueue::OverrunStatus::Armed &&
            stalled == RequestQueue::OverrunStatus::Stalled && secondDrained;
  if (!check(passed,
             "the watchdog confused transient admission with a full "
             "no-progress interval")) {
    return false;
  }

  NOTICE("HOSTED-WAIT-TEST: PASS requestqueue-watchdog-progress");
  return true;
}

struct HeldRequestContext {
  explicit HeldRequestContext(HostedRequestQueue* queue)
      : queue(queue), finished(0), result(~0ULL) {}

  HostedRequestQueue* queue;
  Atomic<size_t> finished;
  uint64_t result;
};

struct CompletionRequeueContext {
  explicit CompletionRequeueContext(Thread* target)
      : target(target), requeues(0), hookFailures(0) {}

  Thread* target;
  Atomic<size_t> requeues;
  Atomic<size_t> hookFailures;
};

CompletionRequeueContext* g_CompletionRequeueContext = nullptr;

void completionRequeueHook(WaitQueue* queue, Thread* thread, const WaitQueue::Channel& channel,
                           size_t debugState) {
  CompletionRequeueContext* context = g_CompletionRequeueContext;
  if (!context || thread != context->target) {
    return;
  }

  if (!queue || channel.owner || channel.value || debugState != Thread::CondWait) {
    context->hookFailures += 1;
  }
  context->requeues += 1;
}

int submitHeldRequest(void* parameter) {
  HeldRequestContext* context = reinterpret_cast<HeldRequestContext*>(parameter);
  context->result = context->queue->addRequest(0, HostedRequestQueue::HoldWorker, 42);
  context->finished += 1;
  return 0;
}

bool completionBarrierInterruption(HostedRequestQueue& queue, bool terminal) {
  HeldRequestContext request(&queue);
  Thread* caller = new Thread(Scheduler::instance().getKernelProcess(), submitHeldRequest, &request,
                              nullptr, false, true);
  if (terminal) {
    caller->setName("hosted RequestQueue terminal caller");
  } else {
    caller->setName("hosted RequestQueue signal caller");
  }

  const bool workerHeld = queue.holdStarted.acquire();
  const bool initiallyQueued = waitUntilQueued(caller, Thread::CondWait);

  CompletionRequeueContext requeue(caller);
  g_CompletionRequeueContext = &requeue;
  WaitQueue::setBeforeBlockHook(completionRequeueHook);

  bool injected = true;
  if (terminal) {
    caller->setUnwindState(Thread::TerminateThread);
  } else {
    g_RequestQueueSignalCalls = 0;
    SignalEvent* event =
        new SignalEvent(reinterpret_cast<uintptr_t>(&hostedRequestQueueSignalHandler),
                        RequestQueueSignalNumber, ~0UL, 0, true, true);
    injected = caller->sendEvent(event);
    if (!injected) {
      delete event;
    }
  }

  for (size_t attempt = 0; attempt < WaitAttempts && !requeue.requeues && !request.finished;
       ++attempt) {
    Scheduler::instance().yield();
  }

  WaitQueue::setBeforeBlockHook(nullptr);
  g_CompletionRequeueContext = nullptr;

  const bool deferred = requeue.requeues == 1 && requeue.hookFailures == 0 && request.finished == 0;
  const bool signalDelivered = terminal || g_RequestQueueSignalCalls == 1;

  queue.releaseHold.release();
  const bool joined = caller->join();

  return check(workerHeld && initiallyQueued && injected && deferred && signalDelivered && joined &&
                   request.finished == 1 && request.result == 42,
               terminal ? "terminal teardown escaped a synchronous request completion"
                        : "a signal returned a synchronous request before completion");
}

struct RequestQueueDestroyContext {
  explicit RequestQueueDestroyContext(HostedRequestQueue* queue) : queue(queue), finished(0) {}

  HostedRequestQueue* queue;
  Atomic<size_t> finished;
};

struct RequestQueueDrainContext {
  explicit RequestQueueDrainContext(HostedRequestQueue* queue)
      : queue(queue), finished(0), result(false) {}

  HostedRequestQueue* queue;
  Atomic<size_t> finished;
  Atomic<size_t> result;
};

struct QueuedRequestContext {
  explicit QueuedRequestContext(HostedRequestQueue* queue)
      : queue(queue), waiter(nullptr), published(0), hookFailures(0), finished(0), result(~0ULL) {}

  HostedRequestQueue* queue;
  Thread* waiter;
  Atomic<size_t> published;
  Atomic<size_t> hookFailures;
  Atomic<size_t> finished;
  uint64_t result;
};

QueuedRequestContext* g_QueuedRequestContext = nullptr;

void queuedRequestWaitHook(WaitQueue* queue, Thread* thread, const WaitQueue::Channel& channel,
                           size_t debugState) {
  QueuedRequestContext* context = g_QueuedRequestContext;
  if (!context) {
    return;
  }

  if (thread != context->waiter) {
    return;
  }

  if (!queue || channel.owner || channel.value || debugState != Thread::CondWait) {
    context->hookFailures += 1;
  }
  context->published += 1;
}

int submitQueuedRequest(void* parameter) {
  QueuedRequestContext* context = reinterpret_cast<QueuedRequestContext*>(parameter);
  context->waiter = Processor::information().getCurrentThread();
  context->result = context->queue->addRequest(0, HostedRequestQueue::CancelLifecycleProbe);
  context->finished += 1;
  return 0;
}

int destroyRequestQueue(void* parameter) {
  RequestQueueDestroyContext* context = reinterpret_cast<RequestQueueDestroyContext*>(parameter);
  context->queue->destroy();
  context->finished += 1;
  return 0;
}

int drainRequestQueue(void* parameter) {
  RequestQueueDrainContext* context = reinterpret_cast<RequestQueueDrainContext*>(parameter);
  context->result = context->queue->drain();
  context->finished += 1;
  return 0;
}

struct RequestQueueHaltContext {
  explicit RequestQueueHaltContext(HostedRequestQueue* queue)
      : queue(queue), finished(0), result(false) {}

  HostedRequestQueue* queue;
  Atomic<size_t> finished;
  Atomic<size_t> result;
};

int haltRequestQueue(void* parameter) {
  auto* context = reinterpret_cast<RequestQueueHaltContext*>(parameter);
  context->result = context->queue->halt() ? 1 : 0;
  context->finished += 1;
  return 0;
}

struct PublicationPauseContext {
  PublicationPauseContext() : calls(0), failures(0), entered(0), release(0) {}

  Atomic<size_t> calls;
  Atomic<size_t> failures;
  Semaphore entered;
  Semaphore release;
};

void pauseFirstPublication(void* parameter) {
  auto* context = reinterpret_cast<PublicationPauseContext*>(parameter);
  if ((context->calls += 1) != 1) {
    return;
  }

  context->entered.release();
  if (!context->release.acquireForCompletion()) {
    context->failures += 1;
  }
}

struct PreallocatedPublicationContext {
  PreallocatedPublicationContext(HostedRequestQueue* queue,
                                 RequestQueue::PreallocatedRequest* request, size_t priority,
                                 uint64_t operation, uint64_t value)
      : queue(queue),
        request(request),
        priority(priority),
        operation(operation),
        value(value),
        result(RequestQueue::PreallocatedPublishResult::QueueStopped),
        finished(0) {}

  HostedRequestQueue* queue;
  RequestQueue::PreallocatedRequest* request;
  size_t priority;
  uint64_t operation;
  uint64_t value;
  RequestQueue::PreallocatedPublishResult result;
  Atomic<size_t> finished;
};

int publishPreallocatedRequest(void* parameter) {
  auto* context = reinterpret_cast<PreallocatedPublicationContext*>(parameter);
  context->result = context->queue->publishPreallocated(*context->request, context->priority,
                                                        context->operation, context->value);
  context->finished += 1;
  return 0;
}

struct AsyncPublicationContext {
  AsyncPublicationContext(HostedRequestQueue* queue, size_t priority, uint64_t operation,
                          uint64_t value)
      : queue(queue),
        priority(priority),
        operation(operation),
        value(value),
        result(0),
        finished(0) {}

  HostedRequestQueue* queue;
  size_t priority;
  uint64_t operation;
  uint64_t value;
  uint64_t result;
  Atomic<size_t> finished;
};

int publishAsyncRequest(void* parameter) {
  auto* context = reinterpret_cast<AsyncPublicationContext*>(parameter);
  context->result =
      context->queue->addAsyncRequest(context->priority, context->operation, context->value);
  context->finished += 1;
  return 0;
}

struct PreallocatedReleaseContext {
  PreallocatedReleaseContext(HostedRequestQueue* queue, bool requeueOnce, bool holdFirst = false,
                             bool probeOrdinaryPublication = false)
      : queue(queue),
        request(nullptr),
        requeueOnce(requeueOnce),
        holdFirst(holdFirst),
        probeOrdinaryPublication(probeOrdinaryPublication),
        callbacks(0),
        requeues(0),
        ordinaryPublicationRejections(0),
        failures(0),
        callbackEntered(0),
        releaseCallback(0) {}

  HostedRequestQueue* queue;
  RequestQueue::PreallocatedRequest* request;
  bool requeueOnce;
  bool holdFirst;
  bool probeOrdinaryPublication;
  Atomic<size_t> callbacks;
  Atomic<size_t> requeues;
  Atomic<size_t> ordinaryPublicationRejections;
  Atomic<size_t> failures;
  Semaphore callbackEntered;
  Semaphore releaseCallback;
};

void preallocatedRequestReleased(void* parameter) {
  auto* context = reinterpret_cast<PreallocatedReleaseContext*>(parameter);
  const size_t callback = (context->callbacks += 1);
  if (!context->request || context->request->isAvailable()) {
    context->failures += 1;
  }

  if (context->holdFirst && callback == 1) {
    context->callbackEntered.release();
    if (!context->releaseCallback.acquireForCompletion()) {
      context->failures += 1;
      return;
    }
  }

  if (context->probeOrdinaryPublication) {
    RequestQueue::PreallocatedRequest ordinary;
    if (context->queue->publishPreallocated(ordinary, 0, HostedRequestQueue::Sum, 20, 22) ==
            RequestQueue::PreallocatedPublishResult::QueueStopped &&
        ordinary.isAvailable()) {
      context->ordinaryPublicationRejections += 1;
    } else {
      context->failures += 1;
    }
  }

  if (context->request && context->requeueOnce && context->requeues.compareAndSwap(0, 1)) {
    if (context->queue->republishPreallocatedWhileReleasing(*context->request, 0,
                                                            HostedRequestQueue::Sum, 20, 22) !=
        RequestQueue::PreallocatedPublishResult::Accepted) {
      context->failures += 1;
    }
  }
}

struct ReleaseDestroyReentryContext {
  explicit ReleaseDestroyReentryContext(HostedRequestQueue* queue)
      : queue(queue),
        callbacks(0),
        failures(0),
        haltRejections(0),
        resumeRejections(0),
        allocationRejections(0),
        preallocatedRejections(0),
        entered(0),
        release(0) {}

  HostedRequestQueue* queue;
  RequestQueue::PreallocatedRequest ordinary;
  Atomic<size_t> callbacks;
  Atomic<size_t> failures;
  Atomic<size_t> haltRejections;
  Atomic<size_t> resumeRejections;
  Atomic<size_t> allocationRejections;
  Atomic<size_t> preallocatedRejections;
  Semaphore entered;
  Semaphore release;
};

void releaseDuringDestroy(void* parameter) {
  auto* context = reinterpret_cast<ReleaseDestroyReentryContext*>(parameter);
  context->callbacks += 1;
  context->entered.release();
  if (!context->release.acquireForCompletion()) {
    context->failures += 1;
    return;
  }

  if (!context->queue->halt()) {
    context->haltRejections += 1;
  }
  if (!context->queue->resume()) {
    context->resumeRejections += 1;
  }
  if (context->queue->addRequest(0, HostedRequestQueue::Sum, 20, 22) == 0 &&
      context->queue->addAsyncRequest(0, HostedRequestQueue::Sum, 20, 22) == 0) {
    context->allocationRejections += 1;
  }
  if (context->queue->publishPreallocated(context->ordinary, 0, HostedRequestQueue::Sum, 20, 22) ==
          RequestQueue::PreallocatedPublishResult::QueueStopped &&
      context->ordinary.isAvailable()) {
    context->preallocatedRejections += 1;
  }
}

bool predicateDoorbellRegression() {
  using Result = RequestQueue::PreallocatedPublishResult;

  HostedRequestQueue queue;
  RequestQueue::PreallocatedRequest request;
  queue.initialise();

  // Consume initialise()'s readiness notification so the publication below
  // is the only possible source of the observed doorbell.
  PerProcessorScheduler::serviceCurrentIrqWorkDoorbellForTest();
  const size_t waitersBefore = queue.requestWaiterCount();
  const bool interrupts = Processor::getInterrupts();
  Processor::setInterrupts(false);
  const Result accepted = queue.publishPreallocated(request, 0, HostedRequestQueue::Sum, 20, 22);
  const Result busy = queue.publishPreallocated(request, 0, HostedRequestQueue::Sum, 19, 23);
  const bool doorbellPending = PerProcessorScheduler::currentIrqWorkDoorbellPendingForTest();
  const bool deferred = queue.executions == 0;
  const size_t waitersAfterPublication = queue.requestWaiterCount();
  Processor::setInterrupts(interrupts);

  PerProcessorScheduler::serviceCurrentIrqWorkDoorbellForTest();
  const bool drained = queue.drain();
  const size_t executions = queue.executions;
  const bool available = request.isAvailable();
  queue.destroy();

  const bool passed =
      check(waitersBefore == 0 && waitersAfterPublication == 0 && accepted == Result::Accepted &&
                busy == Result::TokenBusy && doorbellPending && deferred && drained &&
                executions == 1 && available,
            "IF=0 publication touched a WaitQueue or escaped the predicate "
            "doorbell");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS requestqueue-predicate-doorbell");
  }
  return passed;
}

bool preallocatedDuplicateDomainRegression() {
  using Result = RequestQueue::PreallocatedPublishResult;

  HostedRequestQueue queue;
  RequestQueue::PreallocatedRequest preallocated;
  queue.setMatchEqualPayload(true);
  queue.initialise();

  bool passed = queue.addAsyncRequest(0, HostedRequestQueue::HoldWorker, 42) == 1 &&
                queue.holdStarted.acquire();

  const bool interrupts = Processor::getInterrupts();
  Processor::setInterrupts(false);
  const Result preallocatedAccepted =
      queue.publishPreallocated(preallocated, 1, HostedRequestQueue::Record, 77, 88);
  Processor::setInterrupts(interrupts);

  const size_t comparisonsBefore = queue.comparisons;
  const bool allocatedAccepted = queue.addAsyncRequest(1, HostedRequestQueue::Record, 77, 88) == 1;
  const size_t comparisonsAfter = queue.comparisons;

  queue.releaseHold.release();
  const bool drained = queue.drain();
  const bool executedBoth =
      queue.recordedCount == 2 && queue.recorded[0] == 77 && queue.recorded[1] == 77;
  queue.destroy();

  passed &= preallocatedAccepted == Result::Accepted && allocatedAccepted &&
            comparisonsBefore == 0 && comparisonsAfter == 0 && drained && executedBoth &&
            preallocated.isAvailable() && queue.cancellations == 0 && queue.executions == 3;
  passed = check(passed,
                 "allocation-backed duplicate comparison entered the preallocated "
                 "token "
                 "coalescing domain");
  if (passed) {
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "requestqueue-preallocated-duplicate-domain");
  }
  return passed;
}

bool intakeOrderingRegression() {
  using Result = RequestQueue::PreallocatedPublishResult;

  HostedRequestQueue queue;
  RequestQueue::PreallocatedRequest firstPreallocated;
  RequestQueue::PreallocatedRequest secondPreallocated;
  queue.initialise();

  bool passed = queue.addAsyncRequest(0, HostedRequestQueue::HoldWorker, 42) == 1 &&
                queue.holdStarted.acquire();
  passed &= queue.addAsyncRequest(3, HostedRequestQueue::Record, 30) == 1;
  passed &= queue.addAsyncRequest(1, HostedRequestQueue::Record, 10) == 1;

  const bool interrupts = Processor::getInterrupts();
  Processor::setInterrupts(false);
  const Result firstAccepted =
      queue.publishPreallocated(firstPreallocated, 1, HostedRequestQueue::Record, 11);
  Processor::setInterrupts(interrupts);

  passed &= queue.addAsyncRequest(1, HostedRequestQueue::Record, 12) == 1;
  Processor::setInterrupts(false);
  const Result secondAccepted =
      queue.publishPreallocated(secondPreallocated, 1, HostedRequestQueue::Record, 13);
  Processor::setInterrupts(interrupts);

  passed &= queue.addAsyncRequest(2, HostedRequestQueue::Record, 20) == 1;
  passed &= queue.addAsyncRequest(0, HostedRequestQueue::Record, 1) == 1;

  queue.releaseHold.release();
  const bool drained = queue.drain();
  const uint64_t expected[] = {1, 10, 11, 12, 13, 20, 30};
  bool orderMatches = queue.recordedCount == (sizeof(expected) / sizeof(expected[0]));
  if (orderMatches) {
    for (size_t i = 0; i < (sizeof(expected) / sizeof(expected[0])); ++i) {
      orderMatches &= queue.recorded[i] == expected[i];
    }
  }

  passed &= firstAccepted == Result::Accepted && secondAccepted == Result::Accepted && drained &&
            orderMatches && queue.recordFailures == 0 && queue.executions == 8 &&
            firstPreallocated.isAvailable() && secondPreallocated.isAvailable();
  queue.destroy();

  passed = check(passed, "the shared intake lost same-priority FIFO or strict priority order");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS requestqueue-intake-ordering");
  }
  return passed;
}

bool haltRetentionRegression() {
  using Result = RequestQueue::PreallocatedPublishResult;

  HostedRequestQueue queue;
  RequestQueue::PreallocatedRequest retained;
  RequestQueue::PreallocatedRequest rejected;
  queue.initialise();

  bool passed = queue.addAsyncRequest(0, HostedRequestQueue::HoldWorker, 42) == 1 &&
                queue.holdStarted.acquire();
  passed &= queue.addAsyncRequest(1, HostedRequestQueue::Record, 40) == 1;

  const bool interrupts = Processor::getInterrupts();
  Processor::setInterrupts(false);
  const Result retainedResult =
      queue.publishPreallocated(retained, 1, HostedRequestQueue::Record, 41);
  Processor::setInterrupts(interrupts);

  RequestQueueHaltContext haltContext(&queue);
  Thread* halter = new Thread(Scheduler::instance().getKernelProcess(), haltRequestQueue,
                              &haltContext, nullptr, false, true);
  halter->setName("hosted RequestQueue halt retention regression");

  bool stopping = false;
  for (size_t attempt = 0; attempt < WaitAttempts; ++attempt) {
    stopping = queue.getLifecycleState() == RequestQueue::LifecycleState::Stopping;
    if (stopping) {
      break;
    }
    Scheduler::instance().yield();
  }

  Result rejectedResult = Result::Accepted;
  if (stopping) {
    Processor::setInterrupts(false);
    rejectedResult = queue.publishPreallocated(rejected, 1, HostedRequestQueue::Record, 42);
    Processor::setInterrupts(interrupts);
  }
  const bool retainedBeforeRelease = queue.recordedCount == 0 && !retained.isAvailable();
  queue.releaseHold.release();
  const bool halterJoined = halter->joinForCompletion();
  const bool haltFinished = haltContext.finished == 1;
  const bool haltSucceeded = haltContext.result == 1;
  const bool stopped = queue.getLifecycleState() == RequestQueue::LifecycleState::Stopped;
  const bool retainedWhileStopped = queue.recordedCount == 0 && !retained.isAvailable();

  const bool resumed = queue.resume();
  const bool drained = resumed && queue.drain();
  const bool resumedInOrder =
      queue.recordedCount == 2 && queue.recorded[0] == 40 && queue.recorded[1] == 41;
  queue.destroy();

  passed &= check(retainedResult == Result::Accepted,
                  "halt retention setup did not admit preallocated work");
  passed &= check(stopping, "halt did not publish the Stopping lifecycle state");
  passed &= check(rejectedResult == Result::QueueStopped && rejected.isAvailable(),
                  "halt did not close preallocated admission");
  passed &= check(retainedBeforeRelease,
                  "accepted work ran or was released before the active worker exited");
  passed &= check(halterJoined, "halt regression thread could not be joined");
  passed &= check(haltFinished, "halt regression thread did not return exactly once");
  passed &= check(haltSucceeded, "halt rejected its non-worker caller");
  passed &= check(stopped, "successful halt did not publish Stopped");
  passed &= check(retainedWhileStopped,
                  "halt executed or released accepted work instead of retaining it");
  passed &= check(resumed, "halted queue rejected resume");
  passed &= check(drained, "resumed queue did not drain retained work");
  passed &=
      check(resumedInOrder, "resumed queue did not execute retained work in publication order");
  passed &= check(retained.isAvailable(), "resumed preallocated work did not release its token");
  passed &= check(queue.recordFailures == 0 && queue.executions == 3,
                  "halt retention executed an unexpected request set");
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS requestqueue-halt-retention");
  }
  return passed;
}

bool rejectedPublisherDestroyWaitRegression() {
  using Result = RequestQueue::PreallocatedPublishResult;

  HostedRequestQueue queue;
  RequestQueue::PreallocatedRequest request;
  PublicationPauseContext pause;
  queue.setAfterPreallocatedAdmissionHookForTest(pauseFirstPublication, &pause);

  PreallocatedPublicationContext publication(&queue, &request, 1, HostedRequestQueue::Record, 16);
  Thread* publisher = new Thread(Scheduler::instance().getKernelProcess(),
                                 publishPreallocatedRequest, &publication, nullptr, false, true);
  publisher->setName("hosted RequestQueue rejected publication pause");
  const bool paused = pause.entered.acquire();

  RequestQueueDestroyContext destroyContext(&queue);
  Thread* destroyer = new Thread(Scheduler::instance().getKernelProcess(), destroyRequestQueue,
                                 &destroyContext, nullptr, false, true);
  destroyer->setName("hosted RequestQueue stopped publication drain regression");

  bool drainWaitObserved = false;
  for (size_t attempt = 0; attempt < WaitAttempts; ++attempt) {
    drainWaitObserved = queue.publisherDrainRetriesForTest() != 0;
    if (drainWaitObserved) {
      break;
    }
    Scheduler::instance().yield();
  }

  const bool destroyWaited = drainWaitObserved && !publication.finished &&
                             !destroyContext.finished && request.isAvailable();
  pause.release.release();
  const bool publisherJoined = publisher->join();
  const bool destroyerJoined = destroyer->join();
  queue.setAfterPreallocatedAdmissionHookForTest(nullptr, nullptr);

  const bool passed = check(
      paused && destroyWaited && publisherJoined && destroyerJoined && publication.finished == 1 &&
          publication.result == Result::QueueStopped && destroyContext.finished == 1 &&
          request.isAvailable() && pause.calls == 1 && pause.failures == 0 &&
          queue.executions == 0 && queue.cancellations == 0 && queue.recordedCount == 0,
      "a never-started queue outran a rejected publisher in its closed-gate "
      "lifetime count");
  if (passed) {
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "requestqueue-rejected-publication-close-wait");
  }
  return passed;
}

bool preallocatedAdmissionCloseWaitRegression() {
  using Result = RequestQueue::PreallocatedPublishResult;

  HostedRequestQueue queue;
  RequestQueue::PreallocatedRequest request;
  PublicationPauseContext pause;
  queue.initialise();
  queue.setAfterPreallocatedAdmissionHookForTest(pauseFirstPublication, &pause);

  PreallocatedPublicationContext publication(&queue, &request, 1, HostedRequestQueue::Record, 17);
  Thread* publisher = new Thread(Scheduler::instance().getKernelProcess(),
                                 publishPreallocatedRequest, &publication, nullptr, false, true);
  publisher->setName("hosted RequestQueue admitted publication pause");
  const bool paused = pause.entered.acquire();

  RequestQueueDestroyContext destroyContext(&queue);
  Thread* destroyer = new Thread(Scheduler::instance().getKernelProcess(), destroyRequestQueue,
                                 &destroyContext, nullptr, false, true);
  destroyer->setName("hosted RequestQueue publication drain regression");

  bool stopping = false;
  for (size_t attempt = 0; attempt < WaitAttempts; ++attempt) {
    stopping = queue.getLifecycleState() == RequestQueue::LifecycleState::Stopping;
    if (stopping) {
      break;
    }
    Scheduler::instance().yield();
  }

  const bool destroyWaited =
      stopping && !publication.finished && !destroyContext.finished && request.isAvailable();
  pause.release.release();
  const bool publisherJoined = publisher->join();
  const bool destroyerJoined = destroyer->join();
  queue.setAfterPreallocatedAdmissionHookForTest(nullptr, nullptr);

  const bool passed = check(
      paused && destroyWaited && publisherJoined && destroyerJoined && publication.finished == 1 &&
          publication.result == Result::Accepted && destroyContext.finished == 1 &&
          request.isAvailable() && pause.calls == 1 && pause.failures == 0 &&
          queue.executions == 0 && queue.cancellations == 1 && queue.recordedCount == 0,
      "destroy outran an admitted preallocated publisher paused before its "
      "closed-gate observation");
  if (passed) {
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "requestqueue-preallocated-admission-close-wait");
  }
  return passed;
}

bool intakeTransientPublicationRegression() {
  using Result = RequestQueue::PreallocatedPublishResult;

  HostedRequestQueue queue;
  RequestQueue::PreallocatedRequest preallocated;
  PublicationPauseContext pause;
  queue.initialise();

  bool passed = queue.addAsyncRequest(0, HostedRequestQueue::HoldWorker, 42) == 1 &&
                queue.holdStarted.acquire();
  passed &= queue.addAsyncRequest(3, HostedRequestQueue::Record, 30) == 1;
  queue.setAfterIntakeExchangeHookForTest(pauseFirstPublication, &pause);

  PreallocatedPublicationContext first(&queue, &preallocated, 1, HostedRequestQueue::RecordHold,
                                       10);
  Thread* preallocatedPublisher =
      new Thread(Scheduler::instance().getKernelProcess(), publishPreallocatedRequest, &first,
                 nullptr, false, true);
  preallocatedPublisher->setName("hosted RequestQueue incomplete MPSC publisher");
  const bool paused = pause.entered.acquire();

  AsyncPublicationContext second(&queue, 1, HostedRequestQueue::Record, 11);
  Thread* ordinaryPublisher = new Thread(Scheduler::instance().getKernelProcess(),
                                         publishAsyncRequest, &second, nullptr, false, true);
  ordinaryPublisher->setName("hosted RequestQueue transient duplicate scanner");

  queue.releaseHold.release();
  bool bothRetried = false;
  for (size_t attempt = 0; attempt < WaitAttempts; ++attempt) {
    bothRetried = queue.workerTransientRetriesForTest() && queue.guardedTransientRetriesForTest();
    if (bothRetried) {
      break;
    }
    Scheduler::instance().yield();
  }

  const bool noOvertake = queue.recordedCount == 0 && !first.finished && !second.finished;
  pause.release.release();
  const bool preallocatedJoined = preallocatedPublisher->join();
  const bool firstHeld = queue.holdStarted.acquire();
  const bool ordinaryJoined = ordinaryPublisher->join();
  queue.setAfterIntakeExchangeHookForTest(nullptr, nullptr);

  const bool firstStillLeads = firstHeld && queue.recordedCount == 1 && queue.recorded[0] == 10;
  queue.releaseHold.release();
  const bool drained = queue.drain();
  const uint64_t expected[] = {10, 11, 30};
  bool orderMatches = queue.recordedCount == (sizeof(expected) / sizeof(expected[0]));
  if (orderMatches) {
    for (size_t i = 0; i < (sizeof(expected) / sizeof(expected[0])); ++i) {
      orderMatches &= queue.recorded[i] == expected[i];
    }
  }
  queue.destroy();

  passed &= paused && bothRetried && noOvertake && preallocatedJoined && ordinaryJoined &&
            first.result == Result::Accepted && first.finished == 1 && second.result == 1 &&
            second.finished == 1 && firstStillLeads && drained && orderMatches &&
            preallocated.isAvailable() && pause.calls == 2 && pause.failures == 0 &&
            queue.executions == 4 && queue.cancellations == 0 && queue.recordFailures == 0;
  passed = check(passed,
                 "an incomplete MPSC link was treated as empty or lost priority/FIFO "
                 "ordering");
  if (passed) {
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "requestqueue-intake-transient-publication");
  }
  return passed;
}

bool preallocatedRequestRegressions() {
  using Result = RequestQueue::PreallocatedPublishResult;

  bool passed = true;
  {
    HostedRequestQueue queue;
    PreallocatedReleaseContext releaseContext(&queue, false, true);
    RequestQueue::PreallocatedRequest released(preallocatedRequestReleased, &releaseContext);
    releaseContext.request = &released;
    queue.setMaxAsyncRequests(0);
    queue.initialise();

    passed &= check(queue.republishPreallocatedWhileReleasing(
                        released, 0, HostedRequestQueue::Sum) == Result::TokenBusy &&
                        released.isAvailable(),
                    "an idle token accepted release-only republication");
    passed &= check(
        queue.publishPreallocated(released, 0, HostedRequestQueue::Sum, 19, 23) == Result::Accepted,
        "release-callback request was not admitted");
    passed &= check(releaseContext.callbackEntered.acquire(),
                    "release callback did not enter its drain handoff");

    RequestQueueDrainContext drainContext(&queue);
    Thread* drainer = new Thread(Scheduler::instance().getKernelProcess(), drainRequestQueue,
                                 &drainContext, nullptr, false, true);
    drainer->setName("hosted preallocated release drain regression");
    const bool drainWaitPublished = waitUntilQueued(drainer, Thread::CallbackDrain);
    passed &= check(drainWaitPublished && drainContext.finished == 0,
                    "drain observed a transient empty queue during release");

    passed &= check(queue.republishPreallocatedWhileReleasing(released, 0, HostedRequestQueue::Sum,
                                                              20, 22) == Result::Accepted &&
                        !released.isAvailable(),
                    "a producer could not win the final release handoff");
    releaseContext.releaseCallback.release();
    passed &= check(drainer->join() && drainContext.result && released.isAvailable() &&
                        releaseContext.callbacks == 2 && releaseContext.requeues == 0 &&
                        releaseContext.failures == 0 && queue.executions == 2,
                    "producer-assisted release work escaped drain");
    queue.destroy();
  }

  {
    HostedRequestQueue queue;
    PreallocatedReleaseContext releaseContext(&queue, true, false, true);
    RequestQueue::PreallocatedRequest released(preallocatedRequestReleased, &releaseContext);
    releaseContext.request = &released;
    queue.setMaxAsyncRequests(0);
    queue.initialise();

    passed &= check(queue.publishPreallocated(released, 0, HostedRequestQueue::Sum, 19, 23) ==
                            Result::Accepted &&
                        queue.drain() && released.isAvailable() && releaseContext.callbacks == 2 &&
                        releaseContext.requeues == 1 &&
                        releaseContext.ordinaryPublicationRejections == 2 &&
                        releaseContext.failures == 0 && queue.executions == 2,
                    "release callback did not reject ordinary publication while "
                    "preserving the explicit Releasing-token handoff");
    queue.destroy();
  }

  {
    HostedRequestQueue queue;
    RequestQueue::PreallocatedRequest request;
    RequestQueue::PreallocatedRequest capacity;

    passed &= check(queue.publishPreallocated(request, 0, HostedRequestQueue::Sum, 20, 22) ==
                            Result::QueueStopped &&
                        request.isAvailable(),
                    "a stopped queue claimed a preallocated token");
    passed &= check(queue.publishPreallocated(request, REQUEST_QUEUE_NUM_PRIORITIES,
                                              HostedRequestQueue::Sum) == Result::InvalidPriority &&
                        request.isAvailable(),
                    "an invalid preallocated priority claimed its token");

    queue.initialise();
    const bool interrupts = Processor::getInterrupts();
    const size_t comparisonsBefore = queue.comparisons;
    Processor::setInterrupts(false);
    const Result accepted = queue.publishPreallocated(request, 0, HostedRequestQueue::Sum, 20, 22);
    const Result busy = queue.publishPreallocated(request, 0, HostedRequestQueue::Sum, 19, 23);
    const bool deferred = queue.executions == 0;
    Processor::setInterrupts(interrupts);
    PerProcessorScheduler::serviceCurrentIrqWorkDoorbellForTest();

    passed &= check(accepted == Result::Accepted && busy == Result::TokenBusy && deferred &&
                        static_cast<size_t>(queue.comparisons) == comparisonsBefore,
                    "preallocated IF=0 enqueue ran early or called virtual duplicate "
                    "detection");
    passed &= check(queue.drain() && request.isAvailable() && queue.executions == 1,
                    "executed preallocated work did not release its token");

    passed &= check(queue.publishPreallocated(request, 0, HostedRequestQueue::PreallocatedHold,
                                              42) == Result::Accepted &&
                        queue.holdStarted.acquire(),
                    "preallocated hold request was not admitted");
    passed &= check(queue.publishPreallocated(request, 0, HostedRequestQueue::PreallocatedHold,
                                              42) == Result::TokenBusy,
                    "a published preallocated token was admitted twice");
    queue.setMaxAsyncRequests(1);
    const size_t cancellationsBeforeCapacity = queue.cancellations;
    passed &= check(
        queue.publishPreallocated(capacity, 0, HostedRequestQueue::Sum, 1, 2) == Result::Accepted &&
            !capacity.isAvailable() &&
            queue.addAsyncRequest(0, HostedRequestQueue::Sum, 3, 4) == 0 &&
            queue.cancellations == cancellationsBeforeCapacity + 1,
        "preallocated work depended on allocation backlog capacity");
    queue.setMaxAsyncRequests(256);
    queue.releaseHold.release();
    passed &= check(
        queue.drain() && request.isAvailable() && capacity.isAvailable() && queue.executions == 3,
        "over-capacity preallocated work did not drain exactly once");

    passed &= check(
        queue.publishPreallocated(capacity, 0, HostedRequestQueue::Sum, 19, 23) == Result::Accepted,
        "a drained preallocated token was not admitted again");
    passed &= check(queue.drain() && capacity.isAvailable() && queue.executions == 4,
                    "a drained preallocated token could not be reused");
    queue.destroy();
  }

  {
    HostedRequestQueue queue;
    PreallocatedReleaseContext releaseContext(&queue, false);
    RequestQueue::PreallocatedRequest cancelled(preallocatedRequestReleased, &releaseContext);
    releaseContext.request = &cancelled;
    queue.initialise();
    passed &= check(queue.addAsyncRequest(0, HostedRequestQueue::HoldWorker, 42) == 1 &&
                        queue.holdStarted.acquire(),
                    "teardown setup did not hold the worker");
    passed &= check(queue.publishPreallocated(cancelled, 0, HostedRequestQueue::CancelQueued) ==
                        Result::Accepted,
                    "teardown setup did not queue the preallocated token");

    RequestQueueDestroyContext destroyContext(&queue);
    Thread* destroyer = new Thread(Scheduler::instance().getKernelProcess(), destroyRequestQueue,
                                   &destroyContext, nullptr, false, true);
    destroyer->setName("hosted preallocated request destroy regression");
    while (queue.getLifecycleState() != RequestQueue::LifecycleState::Stopping) {
      Scheduler::instance().yield();
    }
    queue.releaseHold.release();

    passed &= check(destroyer->join() && destroyContext.finished == 1 && cancelled.isAvailable() &&
                        queue.cancellations == 1 && queue.queuedCancellations == 1 &&
                        releaseContext.callbacks == 1 && releaseContext.failures == 0,
                    "destroy did not cancel and release queued preallocated work");
    passed &= check(
        queue.publishPreallocated(cancelled, 0, HostedRequestQueue::Sum) == Result::QueueStopped &&
            cancelled.isAvailable(),
        "a stopped queue retained a reused preallocated token");
  }

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS requestqueue-preallocated-token");
  }
  return passed;
}

bool releaseCallbackDestroyReentryRegression() {
  using Result = RequestQueue::PreallocatedPublishResult;

  HostedRequestQueue queue;
  ReleaseDestroyReentryContext releaseContext(&queue);
  RequestQueue::PreallocatedRequest released(releaseDuringDestroy, &releaseContext);
  queue.initialise();

  const bool accepted =
      queue.publishPreallocated(released, 0, HostedRequestQueue::Sum, 20, 22) == Result::Accepted;
  const bool callbackEntered = accepted && releaseContext.entered.acquire();

  RequestQueueDestroyContext destroyContext(&queue);
  Thread* destroyer = new Thread(Scheduler::instance().getKernelProcess(), destroyRequestQueue,
                                 &destroyContext, nullptr, false, true);
  destroyer->setName("hosted RequestQueue release callback destroy regression");

  bool stopping = false;
  for (size_t attempt = 0; attempt < WaitAttempts; ++attempt) {
    stopping = queue.getLifecycleState() == RequestQueue::LifecycleState::Stopping;
    if (stopping) {
      break;
    }
    Scheduler::instance().yield();
  }

  const bool destroyWaitedForRelease = stopping && !destroyContext.finished;
  releaseContext.release.release();
  const bool destroyerJoined = destroyer->joinForCompletion();

  const bool passed =
      check(callbackEntered && destroyWaitedForRelease && destroyerJoined &&
                destroyContext.finished == 1 && releaseContext.callbacks == 1 &&
                releaseContext.failures == 0 && releaseContext.haltRejections == 1 &&
                releaseContext.resumeRejections == 1 && releaseContext.allocationRejections == 1 &&
                releaseContext.preallocatedRejections == 1 && released.isAvailable() &&
                releaseContext.ordinary.isAvailable() && queue.executions == 1 &&
                queue.cancellations == 0 &&
                queue.getLifecycleState() == RequestQueue::LifecycleState::Destroyed,
            "a worker release callback re-entered queue lifecycle or publication "
            "while destroy waited for it");
  if (passed) {
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "requestqueue-release-callback-destroy-reentry");
  }
  return passed;
}
}  // namespace

bool runHostedRequestQueueRegressions() {
  Thread* current = Processor::information().getCurrentThread();
  if (!check(RoundRobin::runHostedIntrusiveQueueRegressions(current),
             "intrusive scheduler ready-queue invariants failed")) {
    return false;
  }
  NOTICE("HOSTED-WAIT-TEST: PASS scheduler-intrusive-ready-queue");

  if (!ataRequestIdentityRegression()) {
    return false;
  }

  if (!preallocatedRequestRegressions()) {
    return false;
  }

  if (!releaseCallbackDestroyReentryRegression()) {
    return false;
  }

  if (!predicateDoorbellRegression()) {
    return false;
  }

  if (!preallocatedDuplicateDomainRegression()) {
    return false;
  }

  if (!intakeOrderingRegression()) {
    return false;
  }

  if (!haltRetentionRegression()) {
    return false;
  }

  if (!rejectedPublisherDestroyWaitRegression()) {
    return false;
  }

  if (!preallocatedAdmissionCloseWaitRegression()) {
    return false;
  }

  if (!intakeTransientPublicationRegression()) {
    return false;
  }

  if (!watchdogProgressRegression()) {
    return false;
  }

  {
    HostedRequestQueue activeRequestQueue;
    activeRequestQueue.initialise();
    const bool accepted =
        activeRequestQueue.addAsyncRequest(0, HostedRequestQueue::HoldWorker, 42) == 1;
    const bool active = accepted && activeRequestQueue.holdStarted.acquire();

    const Time::Timestamp deadline =
        Time::getTicks() + Time::Multiplier::Second + (100 * Time::Multiplier::Millisecond);
    while (Time::getTicks() < deadline) {
      Scheduler::instance().yield();
    }

    activeRequestQueue.releaseHold.release();
    const bool drained = activeRequestQueue.drain();
    activeRequestQueue.destroy();
    if (!check(accepted && active && drained &&
                   activeRequestQueue.executions == static_cast<size_t>(1),
               "an active request was misclassified as queued backlog")) {
      return false;
    }
    NOTICE("HOSTED-WAIT-TEST: PASS requestqueue-active-not-backlog");
  }

  {
    HostedRequestQueue ownedWorkerQueue;
    ownedWorkerQueue.initialise();
    Thread* idleWorker = ownedWorkerQueue.workerThread();
    if (idleWorker) {
      idleWorker->setUnwindState(Thread::TerminateThread);
    }
    const bool idleTerminalSafe =
        idleWorker && ownedWorkerQueue.addRequest(0, HostedRequestQueue::Sum, 20, 22) == 42;

    const bool halted = ownedWorkerQueue.halt();
    const bool resumed = ownedWorkerQueue.resume();
    Thread* activeWorker = ownedWorkerQueue.workerThread();
    const bool held =
        ownedWorkerQueue.addAsyncRequest(0, HostedRequestQueue::HoldWorker, 42) == 1 &&
        ownedWorkerQueue.holdStarted.acquire();
    if (activeWorker) {
      activeWorker->setUnwindState(Thread::TerminateThread);
    }
    ownedWorkerQueue.releaseHold.release();
    const bool activeTerminalSafe =
        held && activeWorker &&
        ownedWorkerQueue.addRequest(0, HostedRequestQueue::Sum, 19, 23) == 42;
    ownedWorkerQueue.destroy();

    if (!check(idleTerminalSafe && halted && resumed && activeTerminalSafe,
               "queue-owned worker termination orphaned work")) {
      return false;
    }
    NOTICE("HOSTED-WAIT-TEST: PASS requestqueue-worker-terminal-ownership");
  }

  HostedRequestQueue queue;
  bool passed = true;

  passed &= check(queue.addRequest(0, HostedRequestQueue::Sum, 1, 2) == 0,
                  "a stopped queue accepted work before initialise");

  queue.initialise();
  passed &= check(queue.getLifecycleState() == RequestQueue::LifecycleState::Accepting,
                  "initialise did not start the worker");
  passed &= check(
      queue.addRequest(REQUEST_QUEUE_NUM_PRIORITIES - 1, HostedRequestQueue::Sum, 40, 2) == 42,
      "synchronous completion returned the wrong result");
  passed &=
      check(queue.addRequest(REQUEST_QUEUE_NUM_PRIORITIES, HostedRequestQueue::Sum, 1, 2) == 0,
            "an invalid priority was accepted");

  passed &= check(queue.halt(), "halt rejected its non-worker caller");
  passed &= check(queue.getLifecycleState() == RequestQueue::LifecycleState::Stopped,
                  "halt did not join the worker");
  passed &= check(queue.addRequest(0, HostedRequestQueue::Sum, 3, 4) == 0,
                  "a halted queue accepted work");

  passed &= check(queue.resume(), "resume rejected a halted queue");
  passed &= check(queue.addRequest(0, HostedRequestQueue::Sum, 20, 22) == 42,
                  "the resumed worker did not complete synchronous work");
  passed &= check(queue.addRequest(0, HostedRequestQueue::SelfSubmit, 19, 23) == 42,
                  "worker self-submission did not execute inline");
  passed &= check(queue.addRequest(0, HostedRequestQueue::SelfHalt, 77) == 77 &&
                      queue.selfHaltRejections == 1 &&
                      queue.getLifecycleState() == RequestQueue::LifecycleState::Accepting &&
                      queue.addRequest(0, HostedRequestQueue::Sum, 20, 22) == 42,
                  "worker self-halt did not reject without stopping the queue");

  passed &= completionBarrierInterruption(queue, false);
  passed &= completionBarrierInterruption(queue, true);

  passed &= check(queue.addAsyncRequest(0, HostedRequestQueue::HoldWorker, 42) == 1,
                  "the drain test request was not accepted");
  passed &= check(queue.holdStarted.acquire(), "the drain test worker did not enter its request");
  RequestQueueDrainContext drainContext(&queue);
  Thread* drainer = new Thread(Scheduler::instance().getKernelProcess(), drainRequestQueue,
                               &drainContext, nullptr, false, true);
  drainer->setName("hosted RequestQueue drain regression");
  const bool drainWaitPublished = waitUntilQueued(drainer, Thread::CallbackDrain);
  const bool drainBlocked = drainContext.finished == 0;
  queue.releaseHold.release();
  passed &= check(drainer->join() && drainWaitPublished && drainBlocked &&
                      drainContext.finished == 1 && drainContext.result,
                  "drain returned before all published work completed");

  passed &= check(queue.addAsyncRequest(0, HostedRequestQueue::HoldWorker, 42) == 1,
                  "the blocking worker request was not accepted");
  passed &= check(queue.holdStarted.acquire(), "the worker did not enter its held request");

  QueuedRequestContext queuedContext(&queue);
  g_QueuedRequestContext = &queuedContext;
  WaitQueue::setBeforeBlockHook(queuedRequestWaitHook);
  Thread* submitter = new Thread(Scheduler::instance().getKernelProcess(), submitQueuedRequest,
                                 &queuedContext, nullptr, false, true);
  submitter->setName("hosted RequestQueue queued caller");

  while (queuedContext.published < 1) {
    Scheduler::instance().yield();
  }
  WaitQueue::setBeforeBlockHook(nullptr);
  g_QueuedRequestContext = nullptr;

  RequestQueueDestroyContext destroyContext(&queue);
  Thread* destroyer = new Thread(Scheduler::instance().getKernelProcess(), destroyRequestQueue,
                                 &destroyContext, nullptr, false, true);
  destroyer->setName("hosted RequestQueue destroy regression");

  while (queue.getLifecycleState() != RequestQueue::LifecycleState::Stopping) {
    Scheduler::instance().yield();
  }
  queue.releaseHold.release();

  passed &= check(destroyer->join(), "the concurrent destroy worker could not be joined");
  passed &= check(submitter->join(), "the cancelled synchronous caller could not be joined");
  passed &= check(destroyContext.finished == 1, "destroy did not complete exactly once");
  passed &= check(queuedContext.published == 1 && queuedContext.hookFailures == 0,
                  "the queued synchronous caller did not publish one completion wait");
  passed &= check(queuedContext.finished == 1 && queuedContext.result == 0,
                  "destroy did not wake and reject the queued synchronous caller");
  passed &=
      check(queue.cancelHaltRejections == 1 && queue.cancelResumeRejections == 1 &&
                queue.cancelPublicationRejections == 2 && queue.cancelPreallocatedRejections == 1 &&
                queue.cancelPreallocated.isAvailable(),
            "destroy cancellation did not reject recursive lifecycle entry");

  queue.destroy();
  passed &= check(queue.getLifecycleState() == RequestQueue::LifecycleState::Destroyed,
                  "destroy was not idempotent");
  RequestQueue::PreallocatedRequest postDestroy;
  const bool resumeRejected = !queue.resume();
  const bool interrupts = Processor::getInterrupts();
  Processor::setInterrupts(false);
  const RequestQueue::PreallocatedPublishResult postDestroyResult =
      queue.publishPreallocated(postDestroy, 0, HostedRequestQueue::Sum, 20, 22);
  Processor::setInterrupts(interrupts);
  passed &= check(resumeRejected &&
                      queue.getLifecycleState() == RequestQueue::LifecycleState::Destroyed &&
                      queue.addRequest(0, HostedRequestQueue::Sum, 20, 22) == 0 &&
                      postDestroyResult == RequestQueue::PreallocatedPublishResult::QueueStopped &&
                      postDestroy.isAvailable(),
                  "destroyed queue reopened publication");
  queue.destroy();
  passed &= check(queue.getLifecycleState() == RequestQueue::LifecycleState::Destroyed,
                  "repeated destroy changed the terminal lifecycle state");
  passed &= check(queue.executions == 10, "the worker executed an unexpected number of requests");
  passed &= check(queue.cancellations == 5, "rejected requests did not release their payloads");
  passed &= check(queue.queuedCancellations == 1,
                  "destroy did not cancel the queued synchronous request");

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS requestqueue-lifecycle");
  }
  return passed;
}
