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
#include "pedigree/kernel/utilities/ProducerConsumer.h"
#include "pedigree/kernel/utilities/RequestQueue.h"

#include "modules/Module.h"

namespace {
class HandoffQueue : public RequestQueue {
 public:
  HandoffQueue()
      : RequestQueue(MakeConstantString("QEMU release handoff")),
        releaseEntered(0),
        allowReleaseReturn(0),
        releaseCalls(0),
        executions(0),
        token(released, this) {}

  ~HandoffQueue() override {
    destroy();
  }

  static void released(void* context) {
    HandoffQueue* queue = reinterpret_cast<HandoffQueue*>(context);
    if ((queue->releaseCalls += 1) == 1) {
      queue->releaseEntered.release();
      if (!queue->allowReleaseReturn.acquireForCompletion()) {
        FATAL("QEMU RequestQueue release callback was interrupted");
      }
    }
  }

  uint64_t executeRequest(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                          uint64_t) override {
    executions += 1;
    return 42;
  }

  Semaphore releaseEntered;
  Semaphore allowReleaseReturn;
  Atomic<size_t> releaseCalls;
  Atomic<size_t> executions;
  PreallocatedRequest token;
};

struct ClaimPause {
  ClaimPause() : entered(0), release(0), claimCalls(0) {}

  Semaphore entered;
  Semaphore release;
  Atomic<size_t> claimCalls;
};

struct PublishContext {
  PublishContext(HandoffQueue* requestQueue)
      : queue(requestQueue), result(RequestQueue::PreallocatedPublishResult::QueueStopped) {}

  HandoffQueue* queue;
  RequestQueue::PreallocatedPublishResult result;
};

void pauseClaimedPublication(void* context) {
  ClaimPause* pause = reinterpret_cast<ClaimPause*>(context);
  if ((pause->claimCalls += 1) == 1) {
    pause->entered.release();
    if (!pause->release.acquireForCompletion()) {
      FATAL("QEMU RequestQueue publisher pause was interrupted");
    }
  }
}

class BlockingConsumer : public ProducerConsumer {
 public:
  BlockingConsumer() : entered(0), release(0), returned(0) {}

  ~BlockingConsumer() override {
    shutdown();
  }

  bool start() {
    return initialise();
  }

  void shutdown() {
    destroy();
  }

  Semaphore entered;
  Semaphore release;
  Atomic<size_t> returned;

 private:
  void consume(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
               uint64_t) override {
    entered.release();
    if (!release.acquireForCompletion()) {
      FATAL("QEMU ProducerConsumer callback release was interrupted");
    }
    returned += 1;
  }
};

struct ConsumerDestroyContext {
  explicit ConsumerDestroyContext(BlockingConsumer* producerConsumer)
      : consumer(producerConsumer), finished(0) {}

  BlockingConsumer* consumer;
  Atomic<size_t> finished;
};

int destroyConsumer(void* context) {
  ConsumerDestroyContext* destroy = reinterpret_cast<ConsumerDestroyContext*>(context);
  destroy->consumer->shutdown();
  destroy->finished += 1;
  return 0;
}

int publishHandoff(void* context) {
  PublishContext* publication = reinterpret_cast<PublishContext*>(context);
  publication->result =
      publication->queue->republishPreallocatedWhileReleasing(publication->queue->token, 0, 2);
  return 0;
}

void testProducerConsumerTeardown() {
  NOTICE("QEMU-CONCURRENCY-TEST: BEGIN producerconsumer-teardown-completion");

  BlockingConsumer consumer;
  if (!consumer.start()) {
    FATAL("QEMU ProducerConsumer worker did not start");
  }
  consumer.produce(1);
  if (!consumer.entered.acquireForCompletion()) {
    FATAL("QEMU ProducerConsumer callback did not enter");
  }

  ConsumerDestroyContext destroy(&consumer);
  Thread* destroyer = new Thread(Scheduler::instance().getKernelProcess(), destroyConsumer,
                                 &destroy, nullptr, false, true);
  destroyer->setName("QEMU ProducerConsumer destroyer");

  bool joinPublished = false;
  for (size_t attempt = 0; attempt < 4096; ++attempt) {
    uintptr_t address = 0;
    if (destroyer->getStatus() == Thread::Sleeping &&
        destroyer->getDebugState(address) == Thread::Joining) {
      joinPublished = true;
      break;
    }
    Scheduler::instance().yield();
  }
  if (!joinPublished) {
    FATAL("QEMU ProducerConsumer destroyer did not join its worker");
  }

  destroyer->setUnwindState(Thread::TerminateThread);
  bool teardownAbandoned = false;
  for (size_t attempt = 0; attempt < 4096; ++attempt) {
    if (destroyer->getStatus() == Thread::AwaitingJoin) {
      teardownAbandoned = true;
      break;
    }
    Scheduler::instance().yield();
  }
  if (teardownAbandoned) {
    FATAL("QEMU ProducerConsumer teardown abandoned its worker join");
  }

  consumer.release.release();
  if (!destroyer->joinForCompletion() || destroy.finished.value() != static_cast<size_t>(1) ||
      consumer.returned.value() != static_cast<size_t>(1)) {
    FATAL("QEMU ProducerConsumer teardown did not drain its worker");
  }

  NOTICE("QEMU-CONCURRENCY-TEST: PASS producerconsumer-teardown-completion");
}

bool entry() {
  testProducerConsumerTeardown();

  NOTICE("QEMU-CONCURRENCY-TEST: BEGIN requestqueue-release-handoff");

  HandoffQueue queue;
  ClaimPause pause;
  queue.initialise();

  const RequestQueue::PreallocatedPublishResult initial =
      queue.publishPreallocated(queue.token, 0, 1);
  if (initial != RequestQueue::PreallocatedPublishResult::Accepted ||
      !queue.releaseEntered.acquireForCompletion()) {
    FATAL("QEMU RequestQueue handoff setup failed");
  }
  queue.setAfterPreallocatedClaimHookForTest(pauseClaimedPublication, &pause);

  PublishContext publication(&queue);
  Thread* publisher = new Thread(Scheduler::instance().getKernelProcess(), publishHandoff,
                                 &publication, nullptr, false, true, true);
  publisher->setName("QEMU RequestQueue handoff publisher");
  if (!publisher->start() || !pause.entered.acquireForCompletion()) {
    FATAL("QEMU RequestQueue handoff publisher did not claim its token");
  }

  queue.allowReleaseReturn.release();

  if (!queue.addAsyncRequest(0, 3)) {
    FATAL("QEMU RequestQueue handoff progress probe was rejected");
  }

  bool independentProgress = false;
  for (size_t attempt = 0; attempt < 4096; ++attempt) {
    if (queue.executions.value() >= 2) {
      independentProgress = true;
      break;
    }
    Scheduler::instance().yield();
  }

  pause.release.release();

  const bool drained = queue.drain();
  const bool joined = publisher->joinForCompletion();
  queue.setAfterPreallocatedClaimHookForTest(nullptr, nullptr);
  queue.destroy();

  if (!independentProgress || !drained || !joined ||
      publication.result != RequestQueue::PreallocatedPublishResult::Accepted ||
      queue.executions.value() != 3 || queue.releaseCalls.value() != 2 ||
      !queue.token.isAvailable()) {
    FATAL("QEMU RequestQueue handoff blocked independent queue progress");
  }

  NOTICE("QEMU-CONCURRENCY-TEST: PASS requestqueue-release-handoff");
  return true;
}

void exit() {}
}  // namespace

MODULE_INFO("concurrency-smoke", &entry, &exit);
