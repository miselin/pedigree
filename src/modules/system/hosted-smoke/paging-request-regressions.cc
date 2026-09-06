/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/utilities/RequestQueue.h"

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS && THREADS
namespace {
class Queue final : public RequestQueue {
 public:
  Queue() : RequestQueue(MakeConstantString("paging completion fixture")), entered(0), release(0) {}
  ~Queue() override {
    destroy();
  }
  Semaphore entered;
  Semaphore release;
  Atomic<size_t> failures{0};
  Atomic<size_t> cancellations{0};

 protected:
  uint64_t executeRequest(uint64_t hold, uint64_t token, uint64_t, uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t) override {
    if (canWaitForCompletion() ||
        (token && waitForPreallocated(*reinterpret_cast<PreallocatedRequest*>(token))))
      failures += 1;
    if (hold) {
      entered.release();
      if (!release.acquireForCompletion(1, 10))
        failures += 1;
    }
    return 42;
  }
  void cancelRequest(const Request& request) override {
    cancellations += 1;
    if (canWaitForCompletion() ||
        (request.p2 && waitForPreallocated(*reinterpret_cast<PreallocatedRequest*>(request.p2))))
      failures += 1;
  }
};
struct Wait {
  Queue& queue;
  RequestQueue::PreallocatedRequest& token;
  Semaphore done{0};
  bool result = false;
  static int run(void* opaque) {
    auto& self = *static_cast<Wait*>(opaque);
    self.result = self.queue.waitForPreallocated(self.token);
    self.done.release();
    return 0;
  }
};
struct ReleaseGate {
  Queue& queue;
  RequestQueue::PreallocatedRequest* token = nullptr;
  Semaphore entered{0};
  Semaphore release{0};
  size_t failures = 0;
  static void released(void* opaque) {
    auto& self = *static_cast<ReleaseGate*>(opaque);
    if (self.queue.canWaitForCompletion() || self.queue.waitForPreallocated(*self.token))
      ++self.failures;
    self.entered.release();
    // This controlled fixture gate exposes the token's Releasing interval.
    if (!self.release.acquireForCompletion(1, 10))
      ++self.failures;
  }
};
bool queued(Thread* thread) {
  for (size_t attempts = 0; attempts < 10000; ++attempts) {
    Thread::WaitDebugInfo info{};
    if (thread->getWaitDebugInfo(info) && info.queued)
      return true;
    Scheduler::instance().yield();
  }
  return false;
}
struct Destroy {
  Queue& queue;
  Semaphore done{0};
  static int run(void* opaque) {
    auto& self = *static_cast<Destroy*>(opaque);
    self.queue.destroy();
    self.done.release();
    return 0;
  }
};
Thread* worker(Thread::ThreadStartFunc function, void* context) {
  return new Thread(Scheduler::instance().getKernelProcess(), function, context, nullptr, false,
                    true);
}
}  // namespace

bool runHostedPagingRequestRegressions() {
  using Result = RequestQueue::PreallocatedPublishResult;
  bool passed = true;
  {
    Queue queue;
    ReleaseGate release{queue};
    RequestQueue::PreallocatedRequest token(ReleaseGate::released, &release);
    release.token = &token;
    queue.initialise();
    passed &= queue.publishPreallocated(token, 0, 1, reinterpret_cast<uintptr_t>(&token)) ==
              Result::Accepted;
    const bool entered = queue.entered.acquireForCompletion(1, 10);
    Wait waiter{queue, token};
    Thread* thread = worker(Wait::run, &waiter);
    const bool beforeCompletion = queued(thread) && !waiter.done.tryAcquire();
    queue.release.release();
    const bool releasing = release.entered.acquireForCompletion(1, 10);
    const bool duringRelease = !token.isAvailable() && !waiter.done.tryAcquire();
    release.release.release();
    const bool completed = waiter.done.acquireForCompletion(1, 10);
    passed &= entered && beforeCompletion && releasing && duringRelease && completed;
    passed &= thread->joinForCompletion() && waiter.result && token.isAvailable();
    passed &= queue.waitForPreallocated(token);  // Completion preceded this admission.
    passed &= !queue.failures && !release.failures;
    queue.destroy();
  }
  {
    Queue queue;
    RequestQueue::PreallocatedRequest blocker;
    RequestQueue::PreallocatedRequest cancelled;
    queue.initialise();
    passed &= queue.publishPreallocated(blocker, 0, 1, reinterpret_cast<uintptr_t>(&blocker)) ==
              Result::Accepted;
    passed &= queue.entered.acquireForCompletion(1, 10);
    passed &= queue.publishPreallocated(cancelled, 0, 0, reinterpret_cast<uintptr_t>(&cancelled)) ==
              Result::Accepted;
    Wait waiter{queue, cancelled};
    Thread* waiting = worker(Wait::run, &waiter);
    passed &= queued(waiting);
    Destroy destroy{queue};
    Thread* destroying = worker(Destroy::run, &destroy);
    for (size_t attempts = 0;
         attempts < 10000 && queue.getLifecycleState() != RequestQueue::LifecycleState::Stopping;
         ++attempts)
      Scheduler::instance().yield();
    passed &= queue.getLifecycleState() == RequestQueue::LifecycleState::Stopping;
    queue.release.release();
    passed &= destroy.done.acquireForCompletion(1, 10) && waiter.done.acquireForCompletion(1, 10);
    passed &= destroying->joinForCompletion() && waiting->joinForCompletion();
    passed &= waiter.result && cancelled.isAvailable() && blocker.isAvailable();
    passed &= queue.cancellations == 1 && !queue.failures;
  }
  if (passed)
    NOTICE("HOSTED-STORAGE-PAGING: PASS request-completion-retirement");
  else
    ERROR("HOSTED-STORAGE-PAGING: FAIL request-completion-retirement");
  return passed;
}
#endif
