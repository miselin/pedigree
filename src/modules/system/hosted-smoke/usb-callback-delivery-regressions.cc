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

#include "modules/drivers/common/usb-hcd/CallbackDelivery.h"
#include "modules/drivers/common/usb-hcd/TransferCompletion.h"
#include "modules/system/usb/Usb.h"

namespace {
using DeliveryQueue = UsbHcd::CallbackDeliveryQueue;

bool check(bool condition, const char* test, const char* detail) {
  if (condition)
    return true;

  ERROR("HOSTED-WAIT-TEST: FAIL " << test << ": " << detail);
  return false;
}

void countDestruction(void* parameter) {
  auto* destroyed = reinterpret_cast<Atomic<size_t>*>(parameter);
  *destroyed += 1;
}

struct PendingStealContext {
  PendingStealContext(DeliveryQueue* queue, const DeliveryQueue::Key& key)
      : queue(queue),
        pendingKey(key),
        sequence(0),
        firstOrder(0),
        stolenOrder(0),
        releaseOrder(0),
        resumedOrder(0),
        firstCalls(0),
        stolenCalls(0),
        drainSucceeded(0),
        failures(0) {}

  DeliveryQueue* queue;
  DeliveryQueue::Key pendingKey;
  Atomic<size_t> sequence;
  Atomic<size_t> firstOrder;
  Atomic<size_t> stolenOrder;
  Atomic<size_t> releaseOrder;
  Atomic<size_t> resumedOrder;
  Atomic<size_t> firstCalls;
  Atomic<size_t> stolenCalls;
  Atomic<size_t> drainSucceeded;
  Atomic<size_t> failures;
};

void stolenCallback(uintptr_t parameter, ssize_t result) {
  auto* context = reinterpret_cast<PendingStealContext*>(parameter);
  context->stolenCalls += 1;
  context->stolenOrder = context->sequence += 1;
  if (result != 22)
    context->failures += 1;
}

void releaseAfterStolenCallback(void* parameter) {
  auto* context = reinterpret_cast<PendingStealContext*>(parameter);
  context->releaseOrder = context->sequence += 1;
}

void firstCallback(uintptr_t parameter, ssize_t result) {
  auto* context = reinterpret_cast<PendingStealContext*>(parameter);
  context->firstCalls += 1;
  context->firstOrder = context->sequence += 1;
  if (result != 11)
    context->failures += 1;
  if (context->queue->drain(context->pendingKey))
    context->drainSucceeded += 1;
  context->resumedOrder = context->sequence += 1;
}

bool pendingRecordCanBeStolen() {
  DeliveryQueue queue;
  Atomic<size_t> destroyed(0);
  const DeliveryQueue::Key firstKey = {0x100, queue.nextGeneration()};
  const DeliveryQueue::Key pendingKey = {0x101, queue.nextGeneration()};
  PendingStealContext context(&queue, pendingKey);

  DeliveryQueue::Record* first =
      queue.create(firstKey, firstCallback, reinterpret_cast<uintptr_t>(&context), 11, nullptr,
                   nullptr, countDestruction, &destroyed);
  DeliveryQueue::Record* pending =
      queue.create(pendingKey, stolenCallback, reinterpret_cast<uintptr_t>(&context), 22,
                   releaseAfterStolenCallback, &context, countDestruction, &destroyed);
  List<DeliveryQueue::Record*> records;
  records.pushBack(first);
  records.pushBack(pending);
  queue.publish(records);

  queue.deliver(first);
  queue.deliver(pending);

  const bool passed = check(
      context.firstCalls == 1 && context.stolenCalls == 1 && context.drainSucceeded == 1 &&
          context.failures == 0 && context.firstOrder == 1 && context.stolenOrder == 2 &&
          context.releaseOrder == 3 && context.resumedOrder == 4 && destroyed == 2 && queue.empty(),
      "usb-callback-pending-steal",
      "a callback could not synchronously steal a later captured callback");
  if (passed)
    NOTICE("HOSTED-WAIT-TEST: PASS usb-callback-pending-steal");
  return passed;
}

struct SelfDrainContext {
  SelfDrainContext(DeliveryQueue* queue, const DeliveryQueue::Key& key)
      : queue(queue), key(key), calls(0), drained(0) {}

  DeliveryQueue* queue;
  DeliveryQueue::Key key;
  Atomic<size_t> calls;
  Atomic<size_t> drained;
};

void selfDrainingCallback(uintptr_t parameter, ssize_t) {
  auto* context = reinterpret_cast<SelfDrainContext*>(parameter);
  context->calls += 1;
  if (context->queue->drain(context->key))
    context->drained += 1;
}

bool runningRecordCanDrainItself() {
  DeliveryQueue queue;
  Atomic<size_t> destroyed(0);
  const DeliveryQueue::Key key = {0x200, queue.nextGeneration()};
  SelfDrainContext context(&queue, key);
  DeliveryQueue::Record* record =
      queue.create(key, selfDrainingCallback, reinterpret_cast<uintptr_t>(&context), 0, nullptr,
                   nullptr, countDestruction, &destroyed);
  List<DeliveryQueue::Record*> records;
  records.pushBack(record);
  queue.publish(records);
  queue.deliver(record);

  const bool passed = check(
      context.calls == 1 && context.drained == 1 && destroyed == 1 && queue.empty(),
      "usb-callback-self-drain", "a callback draining its own generation blocked or ran twice");
  if (passed)
    NOTICE("HOSTED-WAIT-TEST: PASS usb-callback-self-drain");
  return passed;
}

struct RunningDrainContext {
  RunningDrainContext(DeliveryQueue* queue, const DeliveryQueue::Key& key)
      : queue(queue),
        key(key),
        record(nullptr),
        callbackEntered(0),
        allowCallbackReturn(0),
        drainEntered(0),
        callbackCalls(0),
        deliveryReturned(0),
        drainReturned(0),
        drainSucceeded(0) {}

  DeliveryQueue* queue;
  DeliveryQueue::Key key;
  DeliveryQueue::Record* record;
  Semaphore callbackEntered;
  Semaphore allowCallbackReturn;
  Semaphore drainEntered;
  Atomic<size_t> callbackCalls;
  Atomic<size_t> deliveryReturned;
  Atomic<size_t> drainReturned;
  Atomic<size_t> drainSucceeded;
};

void blockingCallback(uintptr_t parameter, ssize_t) {
  auto* context = reinterpret_cast<RunningDrainContext*>(parameter);
  context->callbackCalls += 1;
  context->callbackEntered.release();
  const bool released = context->allowCallbackReturn.acquireForCompletion();
  (void)released;
}

int deliverRunningRecord(void* parameter) {
  auto* context = reinterpret_cast<RunningDrainContext*>(parameter);
  context->queue->deliver(context->record);
  context->deliveryReturned += 1;
  return 0;
}

int drainRunningRecord(void* parameter) {
  auto* context = reinterpret_cast<RunningDrainContext*>(parameter);
  context->drainEntered.release();
  if (context->queue->drain(context->key))
    context->drainSucceeded += 1;
  context->drainReturned += 1;
  return 0;
}

bool waitUntilSleeping(Thread* thread) {
  for (size_t attempt = 0; attempt < 10000; ++attempt) {
    if (thread->getStatus() == Thread::Sleeping)
      return true;
    Scheduler::instance().yield();
  }
  return false;
}

bool anotherThreadWaitsForRunningRecord() {
  DeliveryQueue queue;
  Atomic<size_t> destroyed(0);
  const DeliveryQueue::Key key = {0x300, queue.nextGeneration()};
  RunningDrainContext context(&queue, key);
  context.record = queue.create(key, blockingCallback, reinterpret_cast<uintptr_t>(&context), 0,
                                nullptr, nullptr, countDestruction, &destroyed);
  List<DeliveryQueue::Record*> records;
  records.pushBack(context.record);
  queue.publish(records);

  Process* kernelProcess = Scheduler::instance().getKernelProcess();
  Thread* delivery =
      new Thread(kernelProcess, deliverRunningRecord, &context, nullptr, false, true);
  delivery->setName("hosted USB callback delivery");
  const bool callbackEntered = context.callbackEntered.acquireForCompletion();

  Thread* drainer = new Thread(kernelProcess, drainRunningRecord, &context, nullptr, false, true);
  drainer->setName("hosted USB callback drainer");
  const bool drainEntered = context.drainEntered.acquireForCompletion();
  const bool drainBlocked = waitUntilSleeping(drainer);
  const bool returnedEarly = static_cast<size_t>(context.drainReturned) != 0;

  context.allowCallbackReturn.release();
  const bool deliveryJoined = delivery->join();
  const bool drainerJoined = drainer->join();

  const bool passed = check(callbackEntered && drainEntered && drainBlocked && !returnedEarly &&
                                deliveryJoined && drainerJoined && context.callbackCalls == 1 &&
                                context.deliveryReturned == 1 && context.drainReturned == 1 &&
                                context.drainSucceeded == 1 && destroyed == 1 && queue.empty(),
                            "usb-callback-running-drain",
                            "a cross-thread drain did not wait for the running callback");
  if (passed)
    NOTICE("HOSTED-WAIT-TEST: PASS usb-callback-running-drain");
  return passed;
}

bool producerWaitsForStolenRunningRecord() {
  DeliveryQueue queue;
  Atomic<size_t> destroyed(0);
  const DeliveryQueue::Key key = {0x350, queue.nextGeneration()};
  RunningDrainContext context(&queue, key);
  context.record = queue.create(key, blockingCallback, reinterpret_cast<uintptr_t>(&context), 0,
                                nullptr, nullptr, countDestruction, &destroyed);
  List<DeliveryQueue::Record*> records;
  records.pushBack(context.record);
  queue.publish(records);

  Process* kernelProcess = Scheduler::instance().getKernelProcess();
  Thread* drainer = new Thread(kernelProcess, drainRunningRecord, &context, nullptr, false, true);
  drainer->setName("hosted USB callback stealer");
  const bool drainEntered = context.drainEntered.acquireForCompletion();
  const bool callbackEntered = context.callbackEntered.acquireForCompletion();

  Thread* producer =
      new Thread(kernelProcess, deliverRunningRecord, &context, nullptr, false, true);
  producer->setName("hosted USB callback producer");
  const bool producerBlocked = waitUntilSleeping(producer);
  const bool returnedEarly = static_cast<size_t>(context.deliveryReturned) != 0;

  context.allowCallbackReturn.release();
  const bool drainerJoined = drainer->join();
  const bool producerJoined = producer->join();

  const bool passed = check(drainEntered && callbackEntered && producerBlocked && !returnedEarly &&
                                drainerJoined && producerJoined && context.callbackCalls == 1 &&
                                context.deliveryReturned == 1 && context.drainReturned == 1 &&
                                context.drainSucceeded == 1 && destroyed == 1 && queue.empty(),
                            "usb-callback-producer-drains-steal",
                            "the producer abandoned a callback stolen by another thread");
  if (passed)
    NOTICE("HOSTED-WAIT-TEST: PASS usb-callback-producer-drains-steal");
  return passed;
}

struct CountContext {
  CountContext() : calls(0) {}

  Atomic<size_t> calls;
};

void countCallback(uintptr_t parameter, ssize_t) {
  auto* context = reinterpret_cast<CountContext*>(parameter);
  context->calls += 1;
}

bool generationIsPartOfIdentity() {
  DeliveryQueue queue;
  Atomic<size_t> destroyed(0);
  CountContext context;
  const size_t generation = queue.nextGeneration();
  const DeliveryQueue::Key key = {0x400, generation};
  const DeliveryQueue::Key wrongGeneration = {0x400, generation + 1};
  DeliveryQueue::Record* record =
      queue.create(key, countCallback, reinterpret_cast<uintptr_t>(&context), 0, nullptr, nullptr,
                   countDestruction, &destroyed);
  List<DeliveryQueue::Record*> records;
  records.pushBack(record);
  queue.publish(records);

  const bool wrongDrained = queue.drain(wrongGeneration);
  queue.deliver(record);

  const bool passed = check(!wrongDrained && context.calls == 1 && destroyed == 1 && queue.empty(),
                            "usb-callback-generation-identity",
                            "a stale transaction generation matched a captured callback");
  if (passed)
    NOTICE("HOSTED-WAIT-TEST: PASS usb-callback-generation-identity");
  return passed;
}

bool allPendingRecordsCanBeDrained() {
  DeliveryQueue queue;
  Atomic<size_t> destroyed(0);
  CountContext context;
  List<DeliveryQueue::Record*> records;
  for (size_t i = 0; i < 3; ++i) {
    const DeliveryQueue::Key key = {0x500 + i, queue.nextGeneration()};
    records.pushBack(queue.create(key, countCallback, reinterpret_cast<uintptr_t>(&context), 0,
                                  nullptr, nullptr, countDestruction, &destroyed));
  }
  queue.publish(records);

  const size_t drained = queue.drainAll();
  const bool emptyAfterDrain = queue.empty();
  while (records.count())
    queue.deliver(records.popFront());

  const bool passed = check(
      drained == 3 && emptyAfterDrain && context.calls == 3 && destroyed == 3 && queue.empty(),
      "usb-callback-drain-all", "controller teardown could not drain every published callback");
  if (passed)
    NOTICE("HOSTED-WAIT-TEST: PASS usb-callback-drain-all");
  return passed;
}

bool recurringCancellationSuppressesPendingSamples() {
  DeliveryQueue queue;
  Atomic<size_t> destroyed(0);
  CountContext context;
  constexpr uintptr_t Transaction = 0x600;
  constexpr size_t Subscription = 0x61;
  constexpr size_t OtherSubscription = 0x62;
  List<DeliveryQueue::Record*> records;
  DeliveryQueue::Record* first = queue.create({Transaction, queue.nextGeneration(), Subscription},
                                              countCallback, reinterpret_cast<uintptr_t>(&context),
                                              0, nullptr, nullptr, countDestruction, &destroyed);
  DeliveryQueue::Record* second = queue.create({Transaction, queue.nextGeneration(), Subscription},
                                               countCallback, reinterpret_cast<uintptr_t>(&context),
                                               0, nullptr, nullptr, countDestruction, &destroyed);
  DeliveryQueue::Record* other = queue.create(
      {Transaction, queue.nextGeneration(), OtherSubscription}, countCallback,
      reinterpret_cast<uintptr_t>(&context), 0, nullptr, nullptr, countDestruction, &destroyed);
  records.pushBack(first);
  records.pushBack(second);
  records.pushBack(other);
  queue.publish(records);

  const bool cancelled = queue.cancelSubscription(Transaction, Subscription);
  const bool generationScoped = queue.activeCount() == 1;
  queue.deliver(first);
  queue.deliver(second);
  queue.deliver(other);

  const bool passed =
      check(cancelled && generationScoped && context.calls == 1 && destroyed == 3 && queue.empty(),
            "usb-callback-recurring-cancel",
            "recurring cancellation invoked a pending callback or consumed a reused generation");
  if (passed)
    NOTICE("HOSTED-WAIT-TEST: PASS usb-callback-recurring-cancel");
  return passed;
}

struct ReciprocalSubscriptionContext {
  ReciprocalSubscriptionContext(DeliveryQueue* first, DeliveryQueue* second)
      : first(first),
        second(second),
        callbacksEntered(0),
        cancellationsAttempted(0),
        resetRejected(0),
        failures(0) {}

  DeliveryQueue* first;
  DeliveryQueue* second;
  Atomic<size_t> callbacksEntered;
  Atomic<size_t> cancellationsAttempted;
  Atomic<size_t> resetRejected;
  Atomic<size_t> failures;
};

void firstReciprocalSubscriptionCallback(uintptr_t parameter, ssize_t) {
  auto* context = reinterpret_cast<ReciprocalSubscriptionContext*>(parameter);
  context->callbacksEntered += 1;
  while (context->callbacksEntered != static_cast<size_t>(2))
    Scheduler::instance().yield();
  if (!context->second->cancelSubscription(0x701, 0x72))
    context->resetRejected += 1;
  else
    context->failures += 1;
  context->cancellationsAttempted += 1;
  while (context->cancellationsAttempted != static_cast<size_t>(2))
    Scheduler::instance().yield();
}

void secondReciprocalSubscriptionCallback(uintptr_t parameter, ssize_t) {
  auto* context = reinterpret_cast<ReciprocalSubscriptionContext*>(parameter);
  context->callbacksEntered += 1;
  while (context->callbacksEntered != static_cast<size_t>(2))
    Scheduler::instance().yield();
  if (!context->first->cancelSubscription(0x700, 0x71))
    context->resetRejected += 1;
  else
    context->failures += 1;
  context->cancellationsAttempted += 1;
  while (context->cancellationsAttempted != static_cast<size_t>(2))
    Scheduler::instance().yield();
}

struct ReciprocalDeliveryThread {
  DeliveryQueue* queue;
  DeliveryQueue::Record* record;
};

int deliverReciprocalSubscription(void* parameter) {
  auto* delivery = reinterpret_cast<ReciprocalDeliveryThread*>(parameter);
  delivery->queue->deliver(delivery->record);
  return 0;
}

bool reciprocalSubscriptionCancellationDoesNotDeadlock() {
  DeliveryQueue first;
  DeliveryQueue second;
  ReciprocalSubscriptionContext context(&first, &second);
  DeliveryQueue::Record* firstRecord =
      first.create({0x700, first.nextGeneration(), 0x71}, firstReciprocalSubscriptionCallback,
                   reinterpret_cast<uintptr_t>(&context), 0);
  DeliveryQueue::Record* secondRecord =
      second.create({0x701, second.nextGeneration(), 0x72}, secondReciprocalSubscriptionCallback,
                    reinterpret_cast<uintptr_t>(&context), 0);
  List<DeliveryQueue::Record*> firstRecords;
  List<DeliveryQueue::Record*> secondRecords;
  firstRecords.pushBack(firstRecord);
  secondRecords.pushBack(secondRecord);
  first.publish(firstRecords);
  second.publish(secondRecords);

  ReciprocalDeliveryThread firstDelivery = {&first, firstRecord};
  ReciprocalDeliveryThread secondDelivery = {&second, secondRecord};
  Process* process = Scheduler::instance().getKernelProcess();
  Thread* firstThread =
      new Thread(process, deliverReciprocalSubscription, &firstDelivery, nullptr, false, true);
  Thread* secondThread =
      new Thread(process, deliverReciprocalSubscription, &secondDelivery, nullptr, false, true);
  firstThread->setName("hosted USB reciprocal callback one");
  secondThread->setName("hosted USB reciprocal callback two");
  const bool firstJoined = firstThread->join();
  const bool secondJoined = secondThread->join();

  const bool externallyRetired =
      first.cancelSubscription(0x700, 0x71) && second.cancelSubscription(0x701, 0x72);
  const bool passed =
      check(firstJoined && secondJoined && externallyRetired && context.callbacksEntered == 2 &&
                context.cancellationsAttempted == 2 && context.resetRejected == 2 &&
                context.failures == 0 && first.empty() && second.empty(),
            "usb-callback-reciprocal-cancel",
            "callbacks on separate queues waited on each other during cancellation");
  if (passed)
    NOTICE("HOSTED-WAIT-TEST: PASS usb-callback-reciprocal-cancel");
  return passed;
}

bool capturedCompletionHasOnePublisher() {
  UsbHcd::TransferCompletion completion;
  UsbHcd::TransferCompletion::Claim claim;
  UsbHcd::TransferCompletion::Claim duplicate;
  CountContext context;
  completion.arm(countCallback, reinterpret_cast<uintptr_t>(&context), 41);

  const bool captured = completion.captureNatural(73);
  const bool claimed = completion.claimCaptured(claim);
  const bool claimedTwice = completion.claimCaptured(duplicate);
  const bool teardownClaimed = completion.claimForTeardown(-TransactionError, duplicate);

  const bool passed = check(
      captured && claimed && !claimedTwice && !teardownClaimed && claim.callback == countCallback &&
          claim.parameter == reinterpret_cast<uintptr_t>(&context) && claim.generation == 41 &&
          claim.result == 73 && claim.reason == UsbHcd::TransferCompletion::Reason::Natural &&
          completion.state() == UsbHcd::TransferCompletion::State::PublicationClaimed,
      "usb-completion-captured-exactly-once",
      "a captured hardware result had more than one callback publisher");
  if (passed)
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "usb-completion-captured-exactly-once");
  return passed;
}

bool cancellationOwnsCompletion() {
  UsbHcd::TransferCompletion completion;
  UsbHcd::TransferCompletion::Claim claim;
  CountContext context;
  completion.arm(countCallback, reinterpret_cast<uintptr_t>(&context), 42);

  const auto cancellation = completion.claimCancellation(
      countCallback, reinterpret_cast<uintptr_t>(&context), -TransactionError, claim);
  const bool capturedAfterCancel = completion.captureNatural(12);
  const bool ordinaryClaimed = completion.claimCaptured(claim);
  const bool teardownClaimed = completion.claimForTeardown(-TransactionError, claim);

  const bool passed =
      check(cancellation == UsbHcd::TransferCompletion::CancellationDisposition::Claimed &&
                !capturedAfterCancel && !ordinaryClaimed && !teardownClaimed &&
                claim.callback == countCallback &&
                claim.parameter == reinterpret_cast<uintptr_t>(&context) &&
                claim.generation == 42 && claim.result == -TransactionError &&
                claim.reason == UsbHcd::TransferCompletion::Reason::Cancelled &&
                completion.state() == UsbHcd::TransferCompletion::State::PublicationClaimed,
            "usb-completion-cancellation-ownership",
            "completion ownership escaped a successful synchronous cancellation");
  if (passed)
    NOTICE("HOSTED-WAIT-TEST: PASS usb-completion-cancellation-ownership");
  return passed;
}

bool teardownTerminalizesActiveCompletion() {
  UsbHcd::TransferCompletion completion;
  UsbHcd::TransferCompletion::Claim claim;
  UsbHcd::TransferCompletion::Claim duplicate;
  CountContext context;
  completion.arm(countCallback, reinterpret_cast<uintptr_t>(&context), 43);

  const bool teardownClaimed = completion.claimForTeardown(-TransactionError, claim);
  const bool capturedAfterTeardown = completion.captureNatural(99);
  const auto cancellationAfterTeardown = completion.claimCancellation(
      countCallback, reinterpret_cast<uintptr_t>(&context), -TransactionError, duplicate);
  const bool claimedTwice = completion.claimForTeardown(-TransactionError, duplicate);

  const bool passed =
      check(teardownClaimed && !capturedAfterTeardown &&
                cancellationAfterTeardown ==
                    UsbHcd::TransferCompletion::CancellationDisposition::DrainPublished &&
                !claimedTwice && claim.generation == 43 && claim.result == -TransactionError &&
                claim.reason == UsbHcd::TransferCompletion::Reason::Teardown,
            "usb-completion-active-teardown",
            "teardown did not terminalize an accepted hardware obligation once");
  if (passed)
    NOTICE("HOSTED-WAIT-TEST: PASS usb-completion-active-teardown");
  return passed;
}

bool teardownPreservesCapturedResult() {
  UsbHcd::TransferCompletion completion;
  UsbHcd::TransferCompletion::Claim claim;
  UsbHcd::TransferCompletion::Claim duplicate;
  CountContext context;
  completion.arm(countCallback, reinterpret_cast<uintptr_t>(&context), 44);

  const bool captured = completion.captureNatural(1234);
  const bool teardownClaimed = completion.claimForTeardown(-TransactionError, claim);
  const bool ordinaryClaimed = completion.claimCaptured(duplicate);

  const bool passed =
      check(captured && teardownClaimed && !ordinaryClaimed && claim.generation == 44 &&
                claim.result == 1234 && claim.reason == UsbHcd::TransferCompletion::Reason::Natural,
            "usb-completion-captured-teardown",
            "teardown discarded or duplicated a captured hardware result");
  if (passed)
    NOTICE("HOSTED-WAIT-TEST: PASS usb-completion-captured-teardown");
  return passed;
}

bool cancellationPreservesCapturedResult() {
  UsbHcd::TransferCompletion completion;
  UsbHcd::TransferCompletion::Claim claim;
  CountContext context;
  completion.arm(countCallback, reinterpret_cast<uintptr_t>(&context), 45);

  const bool captured = completion.captureNatural(5678);
  const auto cancellation = completion.claimCancellation(
      countCallback, reinterpret_cast<uintptr_t>(&context), -TransactionError, claim);

  const bool passed = check(
      captured && cancellation == UsbHcd::TransferCompletion::CancellationDisposition::Claimed &&
          claim.generation == 45 && claim.result == 5678 &&
          claim.reason == UsbHcd::TransferCompletion::Reason::Natural,
      "usb-completion-cancel-preserves-natural",
      "cancellation replaced a hardware result which won the claim race");
  if (passed)
    NOTICE(
        "HOSTED-WAIT-TEST: PASS "
        "usb-completion-cancel-preserves-natural");
  return passed;
}

bool cancellationRequiresExactIdentity() {
  UsbHcd::TransferCompletion completion;
  UsbHcd::TransferCompletion::Claim claim;
  CountContext context;
  completion.arm(countCallback, reinterpret_cast<uintptr_t>(&context), 46);

  const auto wrongCallback = completion.claimCancellation(
      nullptr, reinterpret_cast<uintptr_t>(&context), -TransactionError, claim);
  const auto wrongParameter =
      completion.claimCancellation(countCallback, 0, -TransactionError, claim);
  const auto exact = completion.claimCancellation(
      countCallback, reinterpret_cast<uintptr_t>(&context), -TransactionError, claim);

  const bool passed = check(
      wrongCallback == UsbHcd::TransferCompletion::CancellationDisposition::NoMatch &&
          wrongParameter == UsbHcd::TransferCompletion::CancellationDisposition::NoMatch &&
          exact == UsbHcd::TransferCompletion::CancellationDisposition::Claimed &&
          claim.generation == 46,
      "usb-completion-cancel-identity", "a stale callback identity claimed a reused transfer slot");
  if (passed)
    NOTICE("HOSTED-WAIT-TEST: PASS usb-completion-cancel-identity");
  return passed;
}
}  // namespace

bool runHostedUsbCallbackDeliveryRegressions() {
  return pendingRecordCanBeStolen() && runningRecordCanDrainItself() &&
         anotherThreadWaitsForRunningRecord() && producerWaitsForStolenRunningRecord() &&
         generationIsPartOfIdentity() && allPendingRecordsCanBeDrained() &&
         recurringCancellationSuppressesPendingSamples() &&
         reciprocalSubscriptionCancellationDoesNotDeadlock() &&
         capturedCompletionHasOnePublisher() && cancellationOwnsCompletion() &&
         teardownTerminalizesActiveCompletion() && teardownPreservesCapturedResult() &&
         cancellationPreservesCapturedResult() && cancellationRequiresExactIdentity();
}
