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
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/SyscallHandler.h"
#include "pedigree/kernel/processor/SyscallManager.h"
#include "pedigree/kernel/processor/Syscalls.h"
#include "pedigree/kernel/utilities/ProducerConsumer.h"
#include "pedigree/kernel/utilities/RequestQueue.h"

#include "modules/Module.h"

extern bool runNetworkFilterConcurrencyRegressions();
extern bool runAnonymousMemoryRegionRegression();
extern bool runSlamAllocatorConcurrencyRegression();
extern bool runTlbShootdownConcurrencyRegression();

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

struct ReciprocalSyscallContext;

class ReciprocalSyscallHandler : public SyscallHandler {
 public:
  ReciprocalSyscallHandler(ReciprocalSyscallContext& context, bool first)
      : m_Context(context), m_First(first) {}

  uintptr_t syscall(SyscallState&) override;

 private:
  ReciprocalSyscallContext& m_Context;
  bool m_First;
};

struct ReciprocalSyscallContext {
  ReciprocalSyscallContext()
      : first(*this, true),
        second(*this, false),
        entered(0),
        rejections(0),
        resetReturns(0),
        failures(0),
        firstProcessor(static_cast<size_t>(-1)),
        secondProcessor(static_cast<size_t>(-1)) {}

  ReciprocalSyscallHandler first;
  ReciprocalSyscallHandler second;
  SyscallManager::Registration firstRegistration;
  SyscallManager::Registration secondRegistration;
  Atomic<size_t> entered;
  Atomic<size_t> rejections;
  Atomic<size_t> resetReturns;
  Atomic<size_t> failures;
  Atomic<size_t> firstProcessor;
  Atomic<size_t> secondProcessor;
};

uintptr_t ReciprocalSyscallHandler::syscall(SyscallState&) {
  if (m_First) {
    m_Context.firstProcessor = Processor::id();
  } else {
    m_Context.secondProcessor = Processor::id();
  }
  m_Context.entered += 1;
  for (size_t attempt = 0; attempt < 65536 && m_Context.entered != static_cast<size_t>(2);
       ++attempt) {
    Scheduler::instance().yield();
  }
  if (m_Context.entered != static_cast<size_t>(2)) {
    m_Context.failures += 1;
    return 0;
  }

  SyscallManager::Registration& peer =
      m_First ? m_Context.secondRegistration : m_Context.firstRegistration;
  if (!peer.reset() && peer) {
    m_Context.rejections += 1;
  } else {
    m_Context.failures += 1;
  }
  m_Context.resetReturns += 1;
  for (size_t attempt = 0; attempt < 65536 && m_Context.resetReturns != static_cast<size_t>(2);
       ++attempt) {
    Scheduler::instance().yield();
  }
  if (m_Context.resetReturns != static_cast<size_t>(2)) {
    m_Context.failures += 1;
  }
  return m_First ? 0x71 : 0x72;
}

struct ReciprocalSyscallInvocation {
  Service_t service;
  uintptr_t result;
};

int invokeReciprocalSyscall(void* context) {
  ReciprocalSyscallInvocation* invocation = reinterpret_cast<ReciprocalSyscallInvocation*>(context);
  return SyscallManager::instance().dispatchHandlerForTest(invocation->service, invocation->result)
             ? 0
             : 1;
}

void testSyscallReciprocalUnregister() {
  NOTICE("QEMU-CONCURRENCY-TEST: BEGIN syscall-reciprocal-unregister-smp");

  SyscallManager& manager = SyscallManager::instance();
  ReciprocalSyscallContext context;
  if (!manager.registerSyscallHandler(TUI, &context.first, context.firstRegistration) ||
      !manager.registerSyscallHandler(native, &context.second, context.secondRegistration)) {
    if (context.firstRegistration) {
      context.firstRegistration.reset();
    }
    FATAL("QEMU syscall reciprocal handlers could not register");
  }

  ReciprocalSyscallInvocation first = {TUI, 0};
  ReciprocalSyscallInvocation second = {native, 0};
  Process* process = Scheduler::instance().getKernelProcess();
  Thread* firstThread =
      new Thread(process, invokeReciprocalSyscall, &first, nullptr, false, false, true);
  Thread* secondThread =
      new Thread(process, invokeReciprocalSyscall, &second, nullptr, false, false, true);
  firstThread->setName("QEMU syscall reciprocal callback A");
  secondThread->setName("QEMU syscall reciprocal callback B");
  if (!firstThread->start() || !secondThread->start()) {
    FATAL("QEMU syscall reciprocal workers did not start");
  }

  const bool firstJoined = firstThread->joinForCompletion();
  const bool secondJoined = secondThread->joinForCompletion();
  const bool registrationsPreserved = context.firstRegistration && context.secondRegistration;
  const bool firstRetired = context.firstRegistration.reset();
  const bool secondRetired = context.secondRegistration.reset();
  if (!firstJoined || !secondJoined || first.result != 0x71 || second.result != 0x72 ||
      context.entered != static_cast<size_t>(2) || context.rejections != static_cast<size_t>(2) ||
      context.resetReturns != static_cast<size_t>(2) || context.failures ||
      context.firstProcessor == context.secondProcessor || !registrationsPreserved ||
      !firstRetired || !secondRetired) {
    FATAL("QEMU syscall reciprocal unregister did not preserve external cleanup ownership");
  }

  NOTICE("QEMU-CONCURRENCY-TEST: syscall reciprocal cpus="
         << Dec << static_cast<size_t>(context.firstProcessor) << "/"
         << static_cast<size_t>(context.secondProcessor));
  NOTICE("QEMU-CONCURRENCY-TEST: PASS syscall-reciprocal-unregister-smp");
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
  testSyscallReciprocalUnregister();
  NOTICE("QEMU-CONCURRENCY-TEST: BEGIN network-filter-reciprocal-removal-smp");
  if (!runNetworkFilterConcurrencyRegressions()) {
    FATAL("QEMU NetworkFilter reciprocal-removal regression failed");
  }
  testProducerConsumerTeardown();

  if (!runAnonymousMemoryRegionRegression()) {
    FATAL("QEMU anonymous MemoryRegion regression failed");
  }
  if (!runTlbShootdownConcurrencyRegression()) {
    FATAL("QEMU shared-kernel TLB shootdown regression failed");
  }
  if (!runSlamAllocatorConcurrencyRegression()) {
    FATAL("QEMU SLAM allocator concurrency regression failed");
  }

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

  const bool joined = publisher->joinForCompletion();
  const bool drained = queue.drain();
  queue.setAfterPreallocatedClaimHookForTest(nullptr, nullptr);
  queue.destroy();

  if (!independentProgress || !drained || !joined ||
      publication.result != RequestQueue::PreallocatedPublishResult::Accepted ||
      queue.executions.value() != 3 || queue.releaseCalls.value() != 2 ||
      !queue.token.isAvailable()) {
    FATAL("QEMU RequestQueue handoff blocked independent queue progress");
  }

  NOTICE("QEMU-CONCURRENCY-TEST: PASS requestqueue-release-handoff");
  NOTICE("QEMU-CONCURRENCY-TEST: BEGIN shutdown-worker-drain-smp");
  return true;
}

void exit() {}
}  // namespace

MODULE_INFO("concurrency-smoke", &entry, &exit);
