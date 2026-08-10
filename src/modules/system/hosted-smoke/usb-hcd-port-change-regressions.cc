/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"

#include "modules/drivers/common/usb-hcd/PortChangeRequest.h"
#include "modules/system/usb/UsbHub.h"

namespace {
bool check(bool condition, const char* detail) {
  if (condition) {
    return true;
  }

  ERROR("HOSTED-WAIT-TEST: FAIL usb-hcd-port-change-publication: " << detail);
  return false;
}

class HostedConnectionChangeHub final : public UsbHub {
 public:
  using Suppression = UsbHub::ConnectionChangeSuppression;

  HostedConnectionChangeHub()
      : UsbHub(),
        replayTotal(0),
        publicationClosing(false),
        closedReplayNoops(0),
        rootGenerationBase(100),
        replayHoldPort(16),
        replayEntered(0),
        allowReplay(0) {
    for (size_t i = 0; i < 16; ++i) {
      replays[i] = 0;
    }
  }

  explicit HostedConnectionChangeHub(Device* device)
      : UsbHub(device),
        replayTotal(0),
        publicationClosing(false),
        closedReplayNoops(0),
        rootGenerationBase(100),
        replayHoldPort(16),
        replayEntered(0),
        allowReplay(0) {
    for (size_t i = 0; i < 16; ++i) {
      replays[i] = 0;
    }
  }

  void attach(UsbHub* upstream, uint8_t upstreamPort) {
    attachToUpstreamHub(upstream, upstream->rootConnectionForChild(upstreamPort));
  }

  UsbHub* root() const {
    return rootHub();
  }

  bool suppress(size_t port, Suppression& suppression) {
    return suppressConnectionChanges(port, suppression);
  }

  bool defer(size_t port) {
    return deferConnectionChangeIfSuppressed(port);
  }

  void resetReplays() {
    replayTotal = 0;
    publicationClosing = false;
    closedReplayNoops = 0;
    for (size_t i = 0; i < 16; ++i) {
      replays[i] = 0;
    }
  }

  void addTransferToTransaction(uintptr_t, bool, UsbPid, uintptr_t, size_t) override {}

  uintptr_t createTransaction(UsbEndpoint) override {
    return 0;
  }

  bool doAsync(uintptr_t, void (*)(uintptr_t, ssize_t), uintptr_t) override {
    return false;
  }

  void cancelAsyncAndDrain(uintptr_t, void (*)(uintptr_t, ssize_t), uintptr_t) override {}

  bool addInterruptInHandler(UsbEndpoint, uintptr_t, uint16_t, void (*)(uintptr_t, ssize_t),
                             UsbInterruptInHandle&, uintptr_t) override {
    return false;
  }

  bool portReset(uint8_t, bool) override {
    return true;
  }

  Atomic<size_t> replayTotal;
  Atomic<size_t> replays[16];
  Atomic<bool> publicationClosing;
  Atomic<size_t> closedReplayNoops;
  Atomic<size_t> rootGenerationBase;
  Atomic<size_t> replayHoldPort;
  Semaphore replayEntered;
  Semaphore allowReplay;

 protected:
  size_t currentRootPortGeneration(size_t port) const override {
    return port + rootGenerationBase;
  }

  bool cancelInterruptInAndDrain(const UsbInterruptInToken&, void (*)(uintptr_t, ssize_t),
                                 uintptr_t, bool) override {
    panic("hosted port-change hub unexpectedly owned an interrupt subscription");
    return false;
  }

  void replaySuppressedConnectionChange(size_t port) override {
    if (publicationClosing) {
      closedReplayNoops += 1;
      return;
    }
    if (replayHoldPort == port) {
      replayEntered.release();
      const bool released = allowReplay.acquireForCompletion();
      (void)released;
      replayHoldPort = 16;
    }
    replayTotal += 1;
    if (port < 16) {
      replays[port] += 1;
    }
  }
};

struct ReplayOrderingContext {
  ReplayOrderingContext(HostedConnectionChangeHub* hub, size_t port,
                        HostedConnectionChangeHub::Suppression* endingSuppression)
      : hub(hub),
        port(port),
        endingSuppression(endingSuppression),
        acquireWaitEntered(0),
        releaseFinished(false),
        acquisitionFinished(false),
        acquisitionSucceeded(false) {}

  HostedConnectionChangeHub* hub;
  size_t port;
  HostedConnectionChangeHub::Suppression* endingSuppression;
  Semaphore acquireWaitEntered;
  Atomic<bool> releaseFinished;
  Atomic<bool> acquisitionFinished;
  Atomic<bool> acquisitionSucceeded;
};

ReplayOrderingContext* g_ReplayOrderingContext = nullptr;

void connectionChangeReplayWait(UsbHub* hub, size_t port) {
  ReplayOrderingContext* context = g_ReplayOrderingContext;
  if (context && context->hub == hub && context->port == port) {
    context->acquireWaitEntered.release();
  }
}

int releaseSuppressionForReplay(void* parameter) {
  auto* context = reinterpret_cast<ReplayOrderingContext*>(parameter);
  context->endingSuppression->reset();
  context->releaseFinished = true;
  return 0;
}

int acquireSuppressionBehindReplay(void* parameter) {
  auto* context = reinterpret_cast<ReplayOrderingContext*>(parameter);
  HostedConnectionChangeHub::Suppression suppression;
  context->acquisitionSucceeded = context->hub->suppress(context->port, suppression);
  context->acquisitionFinished = true;
  return 0;
}

struct SuppressionHolderContext {
  SuppressionHolderContext(HostedConnectionChangeHub* hub, size_t port)
      : hub(hub), port(port), entered(0), release(0), acquired(false) {}

  HostedConnectionChangeHub* hub;
  size_t port;
  Semaphore entered;
  Semaphore release;
  Atomic<bool> acquired;
};

int holdConnectionChangeSuppression(void* parameter) {
  auto* context = reinterpret_cast<SuppressionHolderContext*>(parameter);
  HostedConnectionChangeHub::Suppression suppression;
  context->acquired = context->hub->suppress(context->port, suppression);
  context->entered.release();
  if (context->acquired) {
    const bool released = context->release.acquireForCompletion();
    (void)released;
  }
  return 0;
}

bool suppressAndReturnEarly(HostedConnectionChangeHub& hub, size_t port) {
  HostedConnectionChangeHub::Suppression suppression;
  if (!hub.suppress(port, suppression)) {
    return false;
  }
  return hub.defer(port);
}

struct SuppressionBoundaryContext {
  SuppressionBoundaryContext(HostedConnectionChangeHub* hub, size_t port)
      : hub(hub),
        port(port),
        hookEntered(0),
        allowCompareExchange(0),
        hookCalls(0),
        deferred(true) {}

  HostedConnectionChangeHub* hub;
  size_t port;
  Semaphore hookEntered;
  Semaphore allowCompareExchange;
  Atomic<size_t> hookCalls;
  Atomic<bool> deferred;
};

SuppressionBoundaryContext* g_SuppressionBoundaryContext = nullptr;

void holdBeforeConnectionChangePendingCas(UsbHub* hub, size_t port, size_t observedState) {
  SuppressionBoundaryContext* context = g_SuppressionBoundaryContext;
  if (!context || context->hub != hub || context->port != port) {
    return;
  }

  context->hookCalls += 1;
  (void)observedState;
  context->hookEntered.release();
  const bool released = context->allowCompareExchange.acquireForCompletion();
  (void)released;
}

int raceSuppressionFinalRelease(void* parameter) {
  auto* context = reinterpret_cast<SuppressionBoundaryContext*>(parameter);
  context->deferred = context->hub->defer(context->port);
  return 0;
}

bool runConnectionChangeSuppressionRegressions() {
  Device controllerDevice;
  HostedConnectionChangeHub hub(&controllerDevice);
  bool passed = true;

  HostedConnectionChangeHub downstream;
  HostedConnectionChangeHub nested;
  downstream.attach(&hub, 5);
  hub.rootGenerationBase = 200;
  nested.attach(&downstream, 7);
  const auto rootConnection = hub.rootConnectionForChild(5);
  const auto downstreamConnection = downstream.rootConnectionForChild(9);
  const auto nestedConnection = nested.rootConnectionForChild(11);
  passed &= check(hub.root() == &hub && downstream.root() == &hub && nested.root() == &hub,
                  "nested hubs did not retain their root-controller association");
  passed &= check(rootConnection.port == 5 && rootConnection.generation == 205 &&
                      downstreamConnection.port == 5 && downstreamConnection.generation == 105 &&
                      nestedConnection.port == 5 && nestedConnection.generation == 105,
                  "nested hubs did not preserve root-port generation provenance");

  HostedConnectionChangeHub::Suppression outer;
  HostedConnectionChangeHub::Suppression inner;
  passed &= check(hub.suppress(0, outer) && hub.suppress(0, inner) && hub.defer(0) &&
                      hub.defer(0) && hub.defer(0),
                  "nested root-port suppression did not coalesce observations");
  inner.reset();
  passed &=
      check(hub.replayTotal == 0, "an inner root-port suppression release replayed too early");
  outer.reset();
  passed &= check(hub.replayTotal == 1 && hub.replays[0] == 1,
                  "nested root-port suppression did not replay exactly once");

  hub.resetReplays();
  HostedConnectionChangeHub::Suppression firstPort;
  HostedConnectionChangeHub::Suppression secondPort;
  passed &= check(
      hub.suppress(1, firstPort) && hub.suppress(2, secondPort) && hub.defer(1) && hub.defer(2),
      "independent root-port suppression could not retain observations");
  firstPort.reset();
  passed &= check(hub.replays[1] == 1 && hub.replays[2] == 0,
                  "releasing one root port replayed another port");
  secondPort.reset();
  passed &= check(hub.replayTotal == 2 && hub.replays[2] == 1,
                  "independent root-port observations did not replay independently");

  hub.resetReplays();
  passed &= check(suppressAndReturnEarly(hub, 3) && hub.replayTotal == 1 && hub.replays[3] == 1,
                  "early return leaked a root-port suppression lease");
  HostedConnectionChangeHub::Suppression reacquired;
  passed &= check(hub.suppress(3, reacquired),
                  "an early-return lease left its root port permanently suppressed");
  reacquired.reset();

  hub.resetReplays();
  HostedConnectionChangeHub::Suppression threadOverlap;
  passed &= check(hub.suppress(4, threadOverlap),
                  "same-port overlap could not acquire its first suppression");
  SuppressionHolderContext holderContext(&hub, 4);
  Thread* holder =
      new Thread(Scheduler::instance().getKernelProcess(), holdConnectionChangeSuppression,
                 &holderContext, nullptr, false, true);
  holder->setName("hosted USB suppression holder");
  const bool holderEntered = holderContext.entered.acquireForCompletion();
  passed &= check(holderEntered && holderContext.acquired && hub.defer(4),
                  "two-thread same-port suppression did not overlap");
  threadOverlap.reset();
  passed &=
      check(hub.replayTotal == 0, "same-port overlap replayed before the second thread released");
  holderContext.release.release();
  passed &= check(holder->join() && hub.replayTotal == 1 && hub.replays[4] == 1,
                  "same-port overlap did not replay at the outermost release");

  hub.resetReplays();
  HostedConnectionChangeHub::Suppression boundarySuppression;
  const bool boundaryAcquired = hub.suppress(5, boundarySuppression);
  SuppressionBoundaryContext boundaryContext(&hub, 5);
  bool boundaryEntered = false;
  bool releaseWonWithoutReplay = false;
  bool boundaryJoined = false;
  if (boundaryAcquired) {
    g_SuppressionBoundaryContext = &boundaryContext;
    UsbHub::setConnectionChangePendingHookForTest(holdBeforeConnectionChangePendingCas);
    Thread* boundaryReader =
        new Thread(Scheduler::instance().getKernelProcess(), raceSuppressionFinalRelease,
                   &boundaryContext, nullptr, false, true);
    boundaryReader->setName("hosted USB suppression boundary reader");
    boundaryEntered = boundaryContext.hookEntered.acquireForCompletion();
    boundarySuppression.reset();
    releaseWonWithoutReplay = hub.replayTotal == 0;
    boundaryContext.allowCompareExchange.release();
    boundaryJoined = boundaryReader->join();
    UsbHub::setConnectionChangePendingHookForTest(nullptr);
    g_SuppressionBoundaryContext = nullptr;
  }
  passed &=
      check(boundaryAcquired && boundaryEntered && releaseWonWithoutReplay && boundaryJoined &&
                boundaryContext.hookCalls == 1 && !boundaryContext.deferred && hub.replayTotal == 0,
            "a reader behind final release acknowledged an unreplayed change");

  hub.resetReplays();
  HostedConnectionChangeHub::Suppression replaySuppression;
  const bool replayAcquired = hub.suppress(6, replaySuppression);
  const bool replayDeferred = replayAcquired && hub.defer(6);
  hub.replayHoldPort = 6;
  ReplayOrderingContext replayContext(&hub, 6, &replaySuppression);
  bool replayStarted = false;
  bool acquisitionWaited = false;
  bool replayNotOvertaken = false;
  bool releaseJoined = false;
  bool acquisitionJoined = false;
  if (replayAcquired && replayDeferred) {
    Thread* releaser =
        new Thread(Scheduler::instance().getKernelProcess(), releaseSuppressionForReplay,
                   &replayContext, nullptr, false, true);
    releaser->setName("hosted USB suppression replay publisher");
    replayStarted = hub.replayEntered.acquireForCompletion();

    g_ReplayOrderingContext = &replayContext;
    UsbHub::setConnectionChangeReplayWaitHookForTest(connectionChangeReplayWait);
    Thread* acquirer =
        new Thread(Scheduler::instance().getKernelProcess(), acquireSuppressionBehindReplay,
                   &replayContext, nullptr, false, true);
    acquirer->setName("hosted USB suppression replay waiter");
    acquisitionWaited = replayContext.acquireWaitEntered.acquireForCompletion();
    replayNotOvertaken = !replayContext.releaseFinished && !replayContext.acquisitionFinished &&
                         hub.replayTotal == 0;
    hub.allowReplay.release();
    releaseJoined = releaser->joinForCompletion();
    acquisitionJoined = acquirer->joinForCompletion();
    UsbHub::setConnectionChangeReplayWaitHookForTest(nullptr);
    g_ReplayOrderingContext = nullptr;
  }
  passed &=
      check(replayAcquired && replayDeferred && replayStarted && acquisitionWaited &&
                replayNotOvertaken && releaseJoined && acquisitionJoined &&
                replayContext.releaseFinished && replayContext.acquisitionFinished &&
                replayContext.acquisitionSucceeded && hub.replayTotal == 1 && hub.replays[6] == 1,
            "a new same-port suppression overtook retained-change replay");

  hub.resetReplays();
  HostedConnectionChangeHub::Suppression closingSuppression;
  const bool closingAcquired = hub.suppress(7, closingSuppression);
  const bool closingDeferred = closingAcquired && hub.defer(7);
  hub.publicationClosing = true;
  closingSuppression.reset();
  HostedConnectionChangeHub::Suppression afterClosedReplay;
  const bool reacquiredAfterClose = hub.suppress(7, afterClosedReplay);
  afterClosedReplay.reset();
  passed &= check(closingAcquired && closingDeferred && reacquiredAfterClose &&
                      hub.replayTotal == 0 && hub.replays[7] == 0 && hub.closedReplayNoops == 1,
                  "suppression release after publication closure did not no-op and "
                  "clear replay state");
  hub.publicationClosing = false;

  HostedConnectionChangeHub::Suppression invalid;
  passed &= check(!hub.suppress(16, invalid) && !invalid && !hub.defer(16),
                  "out-of-range root port entered suppression state");

  if (passed) {
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "usb-hcd-port-change-suppression-state");
  }
  return passed;
}

class HostedUsbPortQueue final : public RequestQueue {
 public:
  enum Controller {
    Ehci,
    Ohci,
    Uhci,
  };

  HostedUsbPortQueue()
      : RequestQueue(MakeConstantString("Hosted USB port changes")),
        workerEntered(0),
        holdEntered(0),
        releaseHold(0),
        executions(0),
        suppressed(0),
        failures(0),
        cancellations(0) {
    seen[Ehci] = 0;
    seen[Ohci] = 0;
    seen[Uhci] = 0;
  }

  ~HostedUsbPortQueue() override {
    destroy();
  }

  void setMaximum(size_t maximum) {
    m_nMaxAsyncRequests = maximum;
  }

  Thread* workerThread() const {
    return m_pThread;
  }

  Semaphore workerEntered;
  Semaphore holdEntered;
  Semaphore releaseHold;
  Atomic<size_t> executions;
  Atomic<size_t> suppressed;
  Atomic<size_t> failures;
  Atomic<size_t> cancellations;
  Atomic<uint64_t> seen[3];

 protected:
  uint64_t executeRequest(uint64_t controller, uint64_t port, uint64_t publication,
                          uint64_t signalWorker, uint64_t holdWorker, uint64_t returnEarly,
                          uint64_t, uint64_t generation) override {
    auto* request = reinterpret_cast<UsbHcd::PortChangeRequest*>(publication);
    if (signalWorker) {
      workerEntered.release();
    }

    PublicationCompletion completion(*request, generation);
    if (!completion) {
      suppressed += 1;
      return 0;
    }
    if (holdWorker && generation == 1) {
      holdEntered.release();
      const bool released = releaseHold.acquireForCompletion();
      if (!released) {
        failures += 1;
      }
    }
    if (returnEarly) {
      return 0;
    }
    if (controller > Uhci || port >= 64) {
      failures += 1;
    } else {
      const uint64_t bit = 1ULL << port;
      seen[controller] |= bit;
    }
    executions += 1;
    return 0;
  }

  void cancelRequest(const Request& request) override {
    auto* publication = reinterpret_cast<UsbHcd::PortChangeRequest*>(request.p3);
    publication->cancel(request.p8);
    cancellations += 1;
  }

 private:
  using PublicationCompletion = UsbHcd::PortChangeRequest::Completion;
};

struct AcknowledgeWaitContext {
  AcknowledgeWaitContext(UsbHcd::PortChangeRequest* request, Thread* worker, size_t generation)
      : request(request),
        worker(worker),
        generation(generation),
        hookCalls(0),
        hookFailures(0),
        wakeBeforeBlock(0) {}

  UsbHcd::PortChangeRequest* request;
  Thread* worker;
  size_t generation;
  Atomic<size_t> hookCalls;
  Atomic<size_t> hookFailures;
  Atomic<size_t> wakeBeforeBlock;
};

AcknowledgeWaitContext* g_AcknowledgeWaitContext = nullptr;

void acknowledgeBeforeBlockHook(WaitQueue* queue, Thread* thread, const WaitQueue::Channel& channel,
                                size_t debugState) {
  AcknowledgeWaitContext* context = g_AcknowledgeWaitContext;
  if (!context || thread != context->worker || channel.owner != context->request) {
    return;
  }

  context->hookCalls += 1;
  Thread::WaitDebugInfo wait = {};
  uintptr_t debugAddress = 0;
  const bool validPublication = queue && !channel.value && debugState == Thread::CallbackDrain &&
                                thread->getWaitDebugInfo(wait) && wait.queue == queue &&
                                wait.channelOwner == channel.owner &&
                                wait.channelValue == channel.value && wait.queued &&
                                wait.reason == WaitQueue::WakeReason::Waiting &&
                                thread->getDebugState(debugAddress) == Thread::CallbackDrain &&
                                debugAddress == reinterpret_cast<uintptr_t>(context->request);

  // Always release the worker so a failed assertion cannot strand the
  // RequestQueue thread inside the test.
  context->request->acknowledge(context->generation);

  Thread::WaitDebugInfo signalledWait = {};
  if (validPublication && thread->getWaitDebugInfo(signalledWait) && signalledWait.queue == queue &&
      signalledWait.channelOwner == channel.owner && signalledWait.channelValue == channel.value &&
      signalledWait.queued && signalledWait.reason == WaitQueue::WakeReason::Signalled) {
    context->wakeBeforeBlock += 1;
  } else {
    context->hookFailures += 1;
  }
}

struct DestroyContext {
  explicit DestroyContext(HostedUsbPortQueue* queue) : queue(queue), finished(0) {}

  HostedUsbPortQueue* queue;
  Atomic<size_t> finished;
};

int destroyQueue(void* parameter) {
  auto* context = reinterpret_cast<DestroyContext*>(parameter);
  context->queue->destroy();
  context->finished += 1;
  return 0;
}

bool allIdle(UsbHcd::PortChangeRequest* requests, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (!requests[i].isIdle()) {
      return false;
    }
  }
  return true;
}
bool runPortChangePublicationRegressions() {
  using Publication = UsbHcd::PortChangeRequest;
  using Result = Publication::Result;

  bool passed = true;
  passed &= check(UsbHcd::EhciRootPortCount == 15 && UsbHcd::OhciRootPortCount == 15 &&
                      UsbHcd::UhciRootPortCount == 8 && UsbHcd::validEhciRootPortCount(15) &&
                      !UsbHcd::validEhciRootPortCount(16) && !UsbHcd::validOhciRootPortCount(0) &&
                      UsbHcd::validOhciRootPortCount(15) && !UsbHcd::validOhciRootPortCount(16) &&
                      UsbHcd::validUhciRootPortCount(8) && !UsbHcd::validUhciRootPortCount(9),
                  "root-port token bounds do not match controller limits");

  constexpr uint16_t Status = 0x123f;
  constexpr uint16_t ChangeMask = 0x2a;
  constexpr uint16_t Acknowledge = 0x02;
  const uint16_t w1c = UsbHcd::selectiveW1cValue(Status, ChangeMask, Acknowledge);
  passed &=
      check((w1c & ChangeMask) == Acknowledge && (w1c & ~ChangeMask) == (Status & ~ChangeMask),
            "selective W1C echoed an unrelated change bit");
  passed &= check(Publication::canAcknowledge(Result::Accepted) &&
                      Publication::canAcknowledge(Result::Coalesced) &&
                      !Publication::canAcknowledge(Result::TokenBusy) &&
                      !Publication::canAcknowledge(Result::QueueFull) &&
                      !Publication::canAcknowledge(Result::QueueStopped) &&
                      !Publication::canAcknowledge(Result::InvalidPriority),
                  "a rejected publication was treated as safe to ACK");

  UsbHcd::DeferredPortChanges deferred;
  deferred.defer(0, 1);
  deferred.defer(2, 4);
  deferred.defer(0, 3);
  passed &= check(
      !deferred.empty() && deferred.release(0) == 3 && deferred.release(2) == 4 && deferred.empty(),
      "mixed port publication passes lost a deferred generation");

  HostedUsbPortQueue queue;
  Publication recovered;
  passed &= check(recovered.configure(queue, 0, HostedUsbPortQueue::Ehci, 0,
                                      reinterpret_cast<uintptr_t>(&recovered)),
                  "port token configuration was rejected");
  const Publication::Observation stopped = recovered.observe();
  passed &= check(
      stopped.result == Result::QueueStopped && stopped.generation == 1 && !recovered.isIdle(),
      "stopped queue discarded the pending hardware observation");

  queue.initialise();
  queue.setMaximum(0);
  const Publication::Observation reservedAdmission = recovered.observe();
  passed &= check(reservedAdmission.result == Result::Accepted && reservedAdmission.generation == 2,
                  "preallocated port token depended on allocation admission");
  if (Publication::canAcknowledge(reservedAdmission.result)) {
    recovered.acknowledge(reservedAdmission.generation);
  }
  passed &= check(queue.drain() && recovered.isIdle() && !recovered.hasPublicationFailure(),
                  "over-capacity port token did not drain");
  queue.setMaximum(256);
  queue.executions = 0;
  queue.seen[HostedUsbPortQueue::Ehci] = 0;

  Publication invalid;
  passed &= check(!invalid.configure(queue, REQUEST_QUEUE_NUM_PRIORITIES),
                  "invalid priority configured a port token");

  Publication ordered;
  passed &= check(ordered.configure(queue, 0, HostedUsbPortQueue::Ehci, 0,
                                    reinterpret_cast<uintptr_t>(&ordered), 1, 1),
                  "ordered port token configuration was rejected");

  AcknowledgeWaitContext acknowledgeWait(&ordered, queue.workerThread(), 1);
  g_AcknowledgeWaitContext = &acknowledgeWait;
  WaitQueue::setBeforeBlockHook(acknowledgeBeforeBlockHook);
  const Publication::Observation first = ordered.observe();
  const bool workerEntered = first.result == Result::Accepted && queue.workerEntered.acquire();
  constexpr size_t AcknowledgeWaitAttempts = 10000;
  for (size_t i = 0; i < AcknowledgeWaitAttempts && !acknowledgeWait.wakeBeforeBlock &&
                     !acknowledgeWait.hookFailures;
       ++i) {
    Scheduler::instance().yield();
  }
  if (!acknowledgeWait.wakeBeforeBlock) {
    ordered.acknowledge(first.generation);
  }
  WaitQueue::setBeforeBlockHook(nullptr);
  g_AcknowledgeWaitContext = nullptr;

  passed &= check(workerEntered, "worker did not reach the pre-ACK publication barrier");
  const bool waitQueueAcknowledgementPassed =
      first.generation == 1 && acknowledgeWait.hookCalls == 1 &&
      acknowledgeWait.hookFailures == 0 && acknowledgeWait.wakeBeforeBlock == 1;
  passed &= check(waitQueueAcknowledgementPassed,
                  "hardware ACK did not signal the published pre-block waiter");
  if (waitQueueAcknowledgementPassed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-hcd-port-change-waitqueue-ack");
  }
  passed &=
      check(queue.holdEntered.acquire(), "acknowledged worker did not reach the execution hold");
  const Publication::Observation second = ordered.observe();
  const Publication::Observation third = ordered.observe();
  passed &= check(second.result == Result::Coalesced && third.result == Result::Coalesced &&
                      queue.executions == 0,
                  "active port observations did not coalesce behind the worker");
  queue.releaseHold.release();
  passed &= check(queue.workerEntered.acquire() && queue.executions == 1,
                  "follow-up worker crossed a deferred controller ACK");
  ordered.acknowledge(third.generation);
  passed &= check(queue.drain() && ordered.isIdle() && queue.executions == 2 &&
                      !ordered.hasPublicationFailure(),
                  "latest coalesced generation was not consumed exactly once");
  queue.seen[HostedUsbPortQueue::Ehci] = 0;
  queue.executions = 0;

  Publication earlyReturn;
  passed &= check(earlyReturn.configure(queue, 0, HostedUsbPortQueue::Uhci, 0,
                                        reinterpret_cast<uintptr_t>(&earlyReturn), 0, 0, 1),
                  "early-return port token configuration was rejected");
  const Publication::Observation early = earlyReturn.observe();
  passed &= check(early.result == Result::Accepted, "early-return publication was rejected");
  earlyReturn.acknowledge(early.generation);
  passed &=
      check(queue.drain() && earlyReturn.isIdle(), "early worker return retained its port token");

  Publication ehci[UsbHcd::EhciRootPortCount];
  Publication ohci[UsbHcd::OhciRootPortCount];
  Publication uhci[UsbHcd::UhciRootPortCount];

  for (size_t i = 0; i < UsbHcd::EhciRootPortCount; ++i) {
    passed &= check(ehci[i].configure(queue, 0, HostedUsbPortQueue::Ehci, i,
                                      reinterpret_cast<uintptr_t>(&ehci[i])),
                    "EHCI token configuration was rejected");
    const Publication::Observation observation = ehci[i].observe();
    passed &= check(observation.result == Result::Accepted, "EHCI token was rejected");
    if (Publication::canAcknowledge(observation.result)) {
      ehci[i].acknowledge(observation.generation);
    }
  }
  for (size_t i = 0; i < UsbHcd::OhciRootPortCount; ++i) {
    passed &= check(ohci[i].configure(queue, 0, HostedUsbPortQueue::Ohci, i,
                                      reinterpret_cast<uintptr_t>(&ohci[i])),
                    "OHCI token configuration was rejected");
    const Publication::Observation observation = ohci[i].observe();
    passed &= check(observation.result == Result::Accepted, "OHCI token was rejected");
    if (Publication::canAcknowledge(observation.result)) {
      ohci[i].acknowledge(observation.generation);
    }
  }
  for (size_t i = 0; i < UsbHcd::UhciRootPortCount; ++i) {
    passed &= check(uhci[i].configure(queue, 0, HostedUsbPortQueue::Uhci, i,
                                      reinterpret_cast<uintptr_t>(&uhci[i])),
                    "UHCI token configuration was rejected");
    const Publication::Observation observation = uhci[i].observe();
    passed &= check(observation.result == Result::Accepted, "UHCI token was rejected");
    if (Publication::canAcknowledge(observation.result)) {
      uhci[i].acknowledge(observation.generation);
    }
  }

  const uint64_t ehciMask = (1ULL << UsbHcd::EhciRootPortCount) - 1;
  const uint64_t ohciMask = (1ULL << UsbHcd::OhciRootPortCount) - 1;
  const uint64_t uhciMask = (1ULL << UsbHcd::UhciRootPortCount) - 1;
  passed &= check(queue.drain() && queue.seen[HostedUsbPortQueue::Ehci] == ehciMask &&
                      queue.seen[HostedUsbPortQueue::Ohci] == ohciMask &&
                      queue.seen[HostedUsbPortQueue::Uhci] == uhciMask && queue.failures == 0 &&
                      queue.executions == UsbHcd::EhciRootPortCount + UsbHcd::OhciRootPortCount +
                                              UsbHcd::UhciRootPortCount &&
                      allIdle(ehci, UsbHcd::EhciRootPortCount) &&
                      allIdle(ohci, UsbHcd::OhciRootPortCount) &&
                      allIdle(uhci, UsbHcd::UhciRootPortCount),
                  "controller port tokens did not map and complete exactly once");
  queue.destroy();

  HostedUsbPortQueue stopQueue;
  stopQueue.initialise();
  Publication unacknowledged;
  passed &= check(unacknowledged.configure(stopQueue, 0, HostedUsbPortQueue::Ehci, 0,
                                           reinterpret_cast<uintptr_t>(&unacknowledged), 1),
                  "stop-wait port token configuration was rejected");
  const Publication::Observation waiting = unacknowledged.observe();
  const bool stopWorkerEntered =
      waiting.result == Result::Accepted && stopQueue.workerEntered.acquire();
  bool stopWaitPublished = false;
  for (size_t i = 0; i < 10000 && !stopWaitPublished; ++i) {
    Thread::WaitDebugInfo wait = {};
    uintptr_t debugAddress = 0;
    stopWaitPublished =
        stopQueue.workerThread()->getWaitDebugInfo(wait) && wait.queue &&
        wait.channelOwner == &unacknowledged && !wait.channelValue && wait.queued &&
        wait.reason == WaitQueue::WakeReason::Waiting &&
        stopQueue.workerThread()->getDebugState(debugAddress) == Thread::CallbackDrain &&
        debugAddress == reinterpret_cast<uintptr_t>(&unacknowledged);
    if (!stopWaitPublished) {
      Scheduler::instance().yield();
    }
  }
  passed &= check(stopWorkerEntered && stopWaitPublished,
                  "worker did not enter its unacknowledged generation wait");
  const Publication::Observation stopFollowUp = unacknowledged.observe();
  const bool stopFollowUpRetained =
      stopFollowUp.result == Result::Coalesced && stopFollowUp.generation == 2;
  passed &=
      check(stopFollowUpRetained, "stop-wait port token did not retain its pending follow-up");
  unacknowledged.stopAfterQuiesce();
  const bool stopWakePassed = stopQueue.drain() && unacknowledged.isIdle() &&
                              stopQueue.suppressed == 1 && !stopQueue.workerEntered.tryAcquire() &&
                              !unacknowledged.hasPublicationFailure();
  passed &= check(stopWakePassed, "stop did not release an unacknowledged active worker");
  if (stopWorkerEntered && stopWaitPublished && stopFollowUpRetained && stopWakePassed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-hcd-port-change-waitqueue-stop");
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "usb-hcd-port-change-stop-suppresses-republish");
  }
  stopQueue.destroy();

  HostedUsbPortQueue cancellationQueue;
  cancellationQueue.initialise();
  Publication blocker;
  Publication cancelled;
  passed &= check(blocker.configure(cancellationQueue, 0, HostedUsbPortQueue::Ehci, 0,
                                    reinterpret_cast<uintptr_t>(&blocker), 0, 1) &&
                      cancelled.configure(cancellationQueue, 0, HostedUsbPortQueue::Ohci, 0,
                                          reinterpret_cast<uintptr_t>(&cancelled)),
                  "cancellation port token configuration was rejected");
  const Publication::Observation blocking = blocker.observe();
  passed &= check(blocking.result == Result::Accepted, "cancellation blocker was rejected");
  blocker.acknowledge(blocking.generation);
  passed &= check(cancellationQueue.holdEntered.acquire(),
                  "cancellation blocker did not enter the worker");
  const Publication::Observation cancellation = cancelled.observe();
  passed &= check(cancellation.result == Result::Accepted, "cancellation target was rejected");
  cancelled.acknowledge(cancellation.generation);
  const Publication::Observation cancellationFollowUp = cancelled.observe();
  passed &= check(cancellationFollowUp.result == Result::Coalesced,
                  "cancellation target did not retain a coalesced follow-up");

  blocker.stopAfterQuiesce();
  cancelled.stopAfterQuiesce();
  DestroyContext destroyContext(&cancellationQueue);
  Thread* destroyer = new Thread(Scheduler::instance().getKernelProcess(), destroyQueue,
                                 &destroyContext, nullptr, false, true);
  destroyer->setName("hosted USB port-change destroy regression");
  while (cancellationQueue.getLifecycleState() != RequestQueue::LifecycleState::Stopping) {
    Scheduler::instance().yield();
  }
  cancellationQueue.releaseHold.release();
  passed &= check(destroyer->join() && destroyContext.finished == 1 && blocker.isIdle() &&
                      cancelled.isIdle() && cancellationQueue.cancellations == 1 &&
                      !cancelled.hasPublicationFailure(),
                  "destroy did not cancel and release a queued port token");

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-hcd-port-change-publication");
  }
  return passed;
}
}  // namespace

bool runHostedUsbHcdPortChangeRegressions() {
  // Keep these two stack-heavy fixtures sequential. Nesting the suppression
  // fixture under the publication fixture exhausts the bounded kernel stack
  // in ASan hosted builds before either fixture reports its result.
  const bool suppressionPassed = runConnectionChangeSuppressionRegressions();
  const bool publicationPassed = runPortChangePublicationRegressions();
  return suppressionPassed && publicationPassed;
}
