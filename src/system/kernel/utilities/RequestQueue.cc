/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/RequestQueue.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/new"

class Process;

static_assert(__atomic_always_lock_free(sizeof(size_t), nullptr),
              "RequestQueue preallocated-publication words must be lock-free");
static_assert(__atomic_always_lock_free(sizeof(PerProcessorScheduler*), nullptr),
              "RequestQueue preallocated-publication pointers must be lock-free");

/**
 * Marks a derived RequestQueue callback on the current Thread state stack.
 *
 * The cleanup record is needed because a terminal path can abandon a callback
 * stack without executing this scope's destructor. Nested event states inherit
 * the head pointer, so they cannot re-enter the same queue around an outer
 * callback either.
 */
#if defined(PEDIGREE_BUILDUTILS)
class RequestQueueCallbackScope {
 public:
  RequestQueueCallbackScope(RequestQueue*, RequestQueue::PreallocatedRequest* = nullptr) {}

  static bool contains(const RequestQueue*) {
    return false;
  }

  static bool allowsReleaseHandoff(const RequestQueue*, const RequestQueue::PreallocatedRequest*) {
    return false;
  }
};
#else
class RequestQueueCallbackScope {
 public:
  RequestQueueCallbackScope(RequestQueue* queue,
                            RequestQueue::PreallocatedRequest* releasingToken = nullptr)
      : m_Queue(queue),
        m_ReleasingToken(releasingToken),
        m_Thread(nullptr),
        m_StateLevel(0),
        m_Previous(nullptr),
        m_Cleanup(),
        m_Active(false) {
    m_Thread = Processor::information().getCurrentThread();
    if (!m_Thread) {
      return;
    }

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    m_StateLevel = __atomic_load_n(&m_Thread->m_nStateLevel, __ATOMIC_ACQUIRE);
    m_Previous = m_Thread->m_StateLevels[m_StateLevel].m_pRequestQueueCallback;

    // Publish cleanup before this callback becomes visible to nested work.
    m_Thread->armAtomicStateCleanup(m_Cleanup, abandon, this);
    m_Active = true;
    m_Thread->m_StateLevels[m_StateLevel].m_pRequestQueueCallback = this;
    Processor::setInterrupts(interruptsWereEnabled);
  }

  ~RequestQueueCallbackScope() {
    if (!m_Active) {
      return;
    }

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    restore();
    m_Thread->disarmAtomicStateCleanup(m_Cleanup);
    Processor::setInterrupts(interruptsWereEnabled);
  }

  static bool contains(const RequestQueue* queue) {
    Thread* thread = Processor::information().getCurrentThread();
    if (!thread || !queue) {
      return false;
    }

    RequestQueueCallbackScope* scope =
        thread->m_StateLevels[thread->m_nStateLevel].m_pRequestQueueCallback;
    while (scope) {
      if (scope->m_Queue == queue) {
        return true;
      }
      scope = scope->m_Previous;
    }
    return false;
  }

  static bool allowsReleaseHandoff(const RequestQueue* queue,
                                   const RequestQueue::PreallocatedRequest* token) {
    Thread* thread = Processor::information().getCurrentThread();
    if (!thread || !queue || !token) {
      return false;
    }

    RequestQueueCallbackScope* scope =
        thread->m_StateLevels[thread->m_nStateLevel].m_pRequestQueueCallback;
    while (scope) {
      if (scope->m_Queue == queue && scope->m_ReleasingToken == token) {
        return true;
      }
      scope = scope->m_Previous;
    }
    return false;
  }

 private:
  RequestQueueCallbackScope(const RequestQueueCallbackScope&) = delete;
  RequestQueueCallbackScope& operator=(const RequestQueueCallbackScope&) = delete;

  static void abandon(void* context) {
    RequestQueueCallbackScope* scope = reinterpret_cast<RequestQueueCallbackScope*>(context);
    if (scope) {
      scope->restore();
    }
  }

  void restore() {
    if (!m_Active) {
      return;
    }
    if (m_StateLevel >= MAX_NESTED_EVENTS ||
        m_Thread->m_StateLevels[m_StateLevel].m_pRequestQueueCallback != this) {
      FATAL_NOLOCK("RequestQueue callback scope stack was corrupted.");
      return;
    }

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    m_Thread->m_StateLevels[m_StateLevel].m_pRequestQueueCallback = m_Previous;
    m_Active = false;
    Processor::setInterrupts(interruptsWereEnabled);
  }

  RequestQueue* m_Queue;
  RequestQueue::PreallocatedRequest* m_ReleasingToken;
  Thread* m_Thread;
  size_t m_StateLevel;
  RequestQueueCallbackScope* m_Previous;
  AtomicStateCleanupRecord m_Cleanup;
  bool m_Active;
};
#endif

RequestQueue::PreallocatedRequest::PreallocatedRequest() : PreallocatedRequest(nullptr, nullptr) {}

RequestQueue::PreallocatedRequest::PreallocatedRequest(ReleaseCallback releaseCallback,
                                                       void* releaseContext)
    : m_Request(0, true, 0, 0, 0, 0, 0, 0, 0, 0, this),
      m_State(Idle),
      m_ReleaseDepth(0),
      m_ReleaseCallback(releaseCallback),
      m_ReleaseContext(releaseContext) {}

RequestQueue::PreallocatedRequest::~PreallocatedRequest() {
  if (!isAvailable()) {
    FATAL("Destroying a published RequestQueue preallocated token.");
  }
}

bool RequestQueue::PreallocatedRequest::isAvailable() const {
  return static_cast<size_t>(m_State) == Idle && !m_ReleaseDepth;
}

RequestQueue::RequestQueue(const String& name)
    : m_IntakeLanes(),
      m_pActiveRequest(nullptr),
      m_State(static_cast<size_t>(LifecycleState::Stopped)),
#if THREADS
      m_LifecycleMutex(),
      m_RequestQueueWaiters(),
      m_pThread(nullptr),
      m_pWorkerScheduler(nullptr),
      m_bWorkerReady(0),
      m_bWorkerActive(0),
      m_WorkerProgressGeneration(0),
      m_pOverrunTimer(nullptr),
      m_PublicationState(PublicationClosed),
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      m_AfterPreallocatedAdmissionHook(nullptr),
      m_AfterPreallocatedAdmissionContext(nullptr),
      m_AfterIntakeExchangeHook(nullptr),
      m_AfterIntakeExchangeContext(nullptr),
      m_WorkerTransientRetries(0),
      m_GuardedTransientRetries(0),
      m_PublisherDrainRetries(0),
#endif
#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
      m_AfterPreallocatedClaimHook(nullptr),
      m_AfterPreallocatedClaimContext(nullptr),
#endif
#endif
      m_nMaxAsyncRequests(256),
      m_nAsyncRequests(0),
      m_nTotalRequests(0),
      m_nActiveRequests(0),
      m_Name(name.cstr(), name.length()) {
  for (size_t i = 0; i < REQUEST_QUEUE_NUM_PRIORITIES; ++i) {
    m_pRequestQueue[i] = nullptr;
    m_pRequestQueueTail[i] = nullptr;
  }

#if THREADS
  m_OverrunChecker.queue = this;
#endif
}

RequestQueue::~RequestQueue() {
#if THREADS
  bool active = false;
  {
    auto guard = m_RequestQueueWaiters.acquire();
    const LifecycleState state = static_cast<LifecycleState>(m_State.value());
    active = (state != LifecycleState::Stopped && state != LifecycleState::Destroyed) ||
             m_pThread || m_pOverrunTimer || m_nTotalRequests.value() ||
             m_PublicationState.value() != PublicationClosed;
  }
  if (active) {
    FATAL("RequestQueue '" << m_Name
                           << "' reached its base destructor while active; "
                              "the most-derived destructor must call "
                              "destroy().");
  }
#endif
}

void RequestQueue::initialise() {
  if (!resume()) {
    FATAL("Initialising a permanently destroyed RequestQueue");
  }
}

#if THREADS
bool RequestQueue::startWorker() {
  {
    auto guard = m_RequestQueueWaiters.acquire();
    const LifecycleState state = static_cast<LifecycleState>(m_State.value());
    if (state == LifecycleState::Accepting) {
      assert(m_pThread && m_bWorkerReady.value());
      assert(!(m_PublicationState.value() & PublicationClosed));
      return true;
    }

    if (state == LifecycleState::Destroyed) {
      return false;
    }
    if (state == LifecycleState::Stopping || m_pThread || m_bWorkerReady.value()) {
      ERROR("RequestQueue '" << m_Name << "' cannot start while stopping");
      return false;
    }
  }

  Process* process = Scheduler::instance().getKernelProcess();
  Thread* worker =
      new Thread(process, &trampoline, reinterpret_cast<void*>(this), nullptr, false, true, true);
  worker->setName("RequestQueue worker");
  if (!worker->setSchedulerReadyPredicate(workerReady, this)) {
    FATAL("RequestQueue '" << m_Name << "' could not install its ready predicate");
  }

  {
    auto guard = m_RequestQueueWaiters.acquire();
    assert(static_cast<LifecycleState>(m_State.value()) == LifecycleState::Stopped);
    assert(!m_pThread);
    assert(!m_bWorkerReady.value());
    assert(m_PublicationState.value() & PublicationClosed);
    m_OverrunChecker.resetBaselineLocked();
    m_pThread = worker;
    m_pWorkerScheduler = &Processor::information().getScheduler();
  }

  // The delayed worker cannot observe partially published queue state.
  if (!worker->start()) {
    FATAL("RequestQueue '" << m_Name << "' could not start its worker");
  }

  // A terminal request can retire a delayed Thread before its entry point
  // runs. Do not publish a usable queue until work() has installed the
  // queue-owned lifetime deferral.
  while (true) {
    auto guard = m_RequestQueueWaiters.acquire();
    if (m_bWorkerReady.value()) {
      break;
    }
    if (static_cast<LifecycleState>(m_State.value()) != LifecycleState::Stopped ||
        m_pThread != worker) {
      FATAL("RequestQueue '" << m_Name << "' lost its worker during startup");
    }
    const WaitQueue::WakeReason reason = guard.waitForCompletion(
        WaitQueue::Channel(this, 2), Thread::CondWait, reinterpret_cast<uintptr_t>(this));
    (void)reason;
  }

  while (true) {
    bool opened = false;
    {
      auto guard = m_RequestQueueWaiters.acquire();
      assert(m_pThread == worker && m_bWorkerReady.value());
      assert(static_cast<LifecycleState>(m_State.value()) == LifecycleState::Stopped);

      // A rejected hard producer briefly contributes to the closed
      // gate's low-bit count. Reopen only the exact closed-and-drained
      // state so its eventual decrement can never underflow a new
      // publication lifetime.
      opened = m_PublicationState.compareAndSwap(PublicationClosed, 0);
      if (opened) {
        // This is the final acceptance point. The worker, scheduler,
        // and lock-free readiness predicate are already published.
        m_State = static_cast<size_t>(LifecycleState::Accepting);
      }
    }

    if (opened) {
      break;
    }
    Scheduler::instance().yield();
  }

  m_pWorkerScheduler.value()->ringIrqWorkDoorbell();
  return true;
}

bool RequestQueue::stopWorker() {
  Thread* worker = nullptr;
  bool hadWorker = false;
  {
    auto guard = m_RequestQueueWaiters.acquire();
    worker = m_pThread;
    if (!worker) {
      closePreallocatedAdmission();
      if (static_cast<LifecycleState>(m_State.value()) != LifecycleState::Destroyed) {
        m_State = static_cast<size_t>(LifecycleState::Stopped);
      }
      m_bWorkerReady = 0;
      m_bWorkerActive = 0;
      m_pWorkerScheduler = nullptr;
    } else if (worker == Processor::information().getCurrentThread()) {
      ERROR("RequestQueue '" << m_Name << "' worker cannot halt itself");
      return false;
    } else {
      hadWorker = true;
      if (static_cast<LifecycleState>(m_State.value()) == LifecycleState::Accepting) {
        // Close before publishing Stopping, so every producer that can
        // still observe Accepting is either rejected or counted below.
        closePreallocatedAdmission();
        m_State = static_cast<size_t>(LifecycleState::Stopping);
      }
    }
  }

  if (!hadWorker) {
    // Even a closed queue admits rejected publishers into the low-bit
    // lifetime count long enough to observe the closed bit. Do not let a
    // never-started or already-stopped queue outrun one of those callers.
    waitForPreallocatedPublishers();
    return true;
  }

  PerProcessorScheduler* scheduler = m_pWorkerScheduler.value();
  if (scheduler) {
    scheduler->ringIrqWorkDoorbell();
  }

  waitForPreallocatedPublishers();

  if (!worker->joinForCompletion()) {
    ERROR("RequestQueue '" << m_Name << "' could not join its worker");
    return false;
  }

  {
    auto guard = m_RequestQueueWaiters.acquire();
    m_pThread = nullptr;
    m_pWorkerScheduler = nullptr;
    m_State = static_cast<size_t>(LifecycleState::Stopped);
    m_bWorkerReady = 0;
    m_bWorkerActive = 0;
  }
  return true;
}

bool RequestQueue::workerReady(void* context) {
  RequestQueue* queue = reinterpret_cast<RequestQueue*>(context);
  return !queue->m_bWorkerReady.value() || queue->m_bWorkerActive.value() ||
         queue->m_nTotalRequests.value() ||
         static_cast<LifecycleState>(queue->m_State.value()) != LifecycleState::Accepting;
}

void RequestQueue::closePreallocatedAdmission() {
  m_PublicationState |= PublicationClosed;
}

void RequestQueue::waitForPreallocatedPublishers() {
  while (m_PublicationState.value() & PublicationCountMask) {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    m_PublisherDrainRetries += 1;
#endif
    Scheduler::instance().yield();
  }
}
#endif

void RequestQueue::destroy() {
#if THREADS
  if (callbackActiveOnCurrentThread()) {
    FATAL("RequestQueue '" << m_Name << "' destroy re-entered a queue callback");
  }
  if (m_LifecycleMutex.isOwnedByCurrentThread()) {
    FATAL("RequestQueue '" << m_Name << "' destroy re-entered a lifecycle callback");
  }
  TerminationDeferral terminationDeferral;
  LockGuard<Mutex> lifecycleGuard(m_LifecycleMutex);
  if (!stopWorker()) {
    FATAL("RequestQueue '" << m_Name
                           << "' could not satisfy destroy's worker "
                              "drain contract");
  }

  if (m_pOverrunTimer) {
    if (!m_pOverrunTimer->unregisterHandler(&m_OverrunChecker)) {
      FATAL("RequestQueue '" << m_Name << "' could not drain its timer callback");
    }
    m_pOverrunTimer = nullptr;
  }

  Request* cancelled = nullptr;
  {
    auto guard = m_RequestQueueWaiters.acquire();
    assert(!m_pActiveRequest);
    for (size_t priority = 0; priority < REQUEST_QUEUE_NUM_PRIORITIES; ++priority) {
      if (!drainIntakeLocked(priority)) {
        FATAL("RequestQueue '" << m_Name
                               << "' retained an incomplete MPSC "
                                  "publication after producer drain");
      }

      Request* request = m_pRequestQueue[priority];
      m_pRequestQueue[priority] = nullptr;
      m_pRequestQueueTail[priority] = nullptr;

      while (request) {
        Request* next = request->m_Next;
        request->m_Next = cancelled;
        cancelled = request;
        request = next;
      }
    }
    m_nTotalRequests = 0;
    m_nAsyncRequests = 0;
    m_nActiveRequests = 0;
  }

  while (cancelled) {
    Request* next = cancelled->m_Next;
    cancelled->m_Next = nullptr;
    invokeCancelRequest(*cancelled);
    completeRequest(cancelled, 0, true);
    releaseRequest(cancelled);
    cancelled = next;
  }

  {
    auto guard = m_RequestQueueWaiters.acquire();
    closePreallocatedAdmission();
    m_State = static_cast<size_t>(LifecycleState::Destroyed);
  }
#endif
}

uint64_t RequestQueue::addRequest(size_t priority, uint64_t p1, uint64_t p2, uint64_t p3,
                                  uint64_t p4, uint64_t p5, uint64_t p6, uint64_t p7, uint64_t p8) {
  return addRequest(priority, RequestQueue::Block, p1, p2, p3, p4, p5, p6, p7, p8);
}

uint64_t RequestQueue::addRequest(size_t priority, ActionOnDuplicate action, uint64_t p1,
                                  uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5, uint64_t p6,
                                  uint64_t p7, uint64_t p8) {
#if THREADS
  if (callbackActiveOnCurrentThread() || m_LifecycleMutex.isOwnedByCurrentThread()) {
    return 0;
  }

  Request* candidate = new Request(priority, false, p1, p2, p3, p4, p5, p6, p7, p8);

  if (priority >= REQUEST_QUEUE_NUM_PRIORITIES) {
    ERROR("RequestQueue '" << m_Name << "' rejected invalid priority " << priority);
    discardRequest(candidate);
    return 0;
  }

  Request* request = nullptr;
  bool rejected = false;
  bool executeInline = false;
  while (true) {
    bool retry = false;
    {
      auto guard = m_RequestQueueWaiters.acquire();
      if (static_cast<LifecycleState>(m_State.value()) != LifecycleState::Accepting) {
        rejected = true;
      } else if (m_pThread == Processor::information().getCurrentThread()) {
        // A worker cannot wait for itself. Execute nested synchronous
        // work inline after dropping the queue guard.
        executeInline = true;
      } else if (action != NewRequest && !drainIntakeLocked(priority)) {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        m_GuardedTransientRetries += 1;
#endif
        // An allocation-backed predecessor can be hidden behind an
        // unfinished token link, so do not deduplicate around it.
        retry = true;
      } else {
        if (action != NewRequest) {
          request = findDuplicate(*candidate);
        }

        if (request) {
          if (action == ReturnImmediately) {
            rejected = true;
          } else {
            retainRequest(request);
          }
        } else {
          m_nTotalRequests += 1;
          publishRequest(candidate);
          request = candidate;
          candidate = nullptr;
        }
      }
    }

    if (!retry) {
      break;
    }
    Scheduler::instance().yield();
  }

  if (executeInline) {
    delete candidate;
    return executeRequest(p1, p2, p3, p4, p5, p6, p7, p8);
  }
  if (candidate) {
    discardRequest(candidate);
  }
  if (rejected) {
    return 0;
  }
  assert(request);
  return waitForRequest(request);
#else
  if (priority >= REQUEST_QUEUE_NUM_PRIORITIES) {
    ERROR("RequestQueue '" << m_Name << "' rejected invalid priority " << priority);
    Request* candidate = new Request(priority, false, p1, p2, p3, p4, p5, p6, p7, p8);
    discardRequest(candidate);
    return 0;
  }
  return executeRequest(p1, p2, p3, p4, p5, p6, p7, p8);
#endif
}

uint64_t RequestQueue::addAsyncRequest(size_t priority, uint64_t p1, uint64_t p2, uint64_t p3,
                                       uint64_t p4, uint64_t p5, uint64_t p6, uint64_t p7,
                                       uint64_t p8) {
  return addAsyncRequestInternal(priority, p1, p2, p3, p4, p5, p6, p7, p8);
}

RequestQueue::PreallocatedPublishResult RequestQueue::publishPreallocated(
    PreallocatedRequest& token, size_t priority, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4,
    uint64_t p5, uint64_t p6, uint64_t p7, uint64_t p8) {
  return publishPreallocatedRequest(token, PreallocatedRequest::Idle, priority, p1, p2, p3, p4, p5,
                                    p6, p7, p8);
}

RequestQueue::PreallocatedPublishResult RequestQueue::republishPreallocatedWhileReleasing(
    PreallocatedRequest& token, size_t priority, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4,
    uint64_t p5, uint64_t p6, uint64_t p7, uint64_t p8) {
  return publishPreallocatedRequest(token, PreallocatedRequest::Releasing, priority, p1, p2, p3, p4,
                                    p5, p6, p7, p8);
}

RequestQueue::PreallocatedPublishResult RequestQueue::publishPreallocatedRequest(
    PreallocatedRequest& token, PreallocatedRequest::State availableState, size_t priority,
    uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5, uint64_t p6, uint64_t p7,
    uint64_t p8) {
  if (priority >= REQUEST_QUEUE_NUM_PRIORITIES) {
    return PreallocatedPublishResult::InvalidPriority;
  }

  if (callbackActiveOnCurrentThread() &&
      (availableState != PreallocatedRequest::Releasing ||
       !RequestQueueCallbackScope::allowsReleaseHandoff(this, &token))) {
    return PreallocatedPublishResult::QueueStopped;
  }

  // The sole callback exception is its own Releasing token: this path only
  // hands that token back through the lock-free intake, performs no
  // allocation or lifecycle transition, and the closed admission gate below
  // still rejects it during shutdown.

#if THREADS
  const size_t admission = (m_PublicationState += 1);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  if (m_AfterPreallocatedAdmissionHook) {
    m_AfterPreallocatedAdmissionHook(m_AfterPreallocatedAdmissionContext);
  }
#endif
  if (admission & PublicationClosed) {
    m_PublicationState -= 1;
    return PreallocatedPublishResult::QueueStopped;
  }

  if (!token.m_State.compareAndSwap(availableState, PreallocatedRequest::Claimed)) {
    m_PublicationState -= 1;
    return PreallocatedPublishResult::TokenBusy;
  }
#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
  if (m_AfterPreallocatedClaimHook) {
    m_AfterPreallocatedClaimHook(m_AfterPreallocatedClaimContext);
  }
#endif
#else
  if (!token.m_State.compareAndSwap(availableState, PreallocatedRequest::Claimed)) {
    return PreallocatedPublishResult::TokenBusy;
  }
#endif

  Request* request = &token.m_Request;
  request->p1 = p1;
  request->p2 = p2;
  request->p3 = p3;
  request->p4 = p4;
  request->p5 = p5;
  request->p6 = p6;
  request->p7 = p7;
  request->p8 = p8;
  request->m_ReturnValue = 0;
#if THREADS
  request->m_References = 1;
#endif
  request->m_Next = nullptr;
  request->m_Intake.next = nullptr;
  request->m_Priority = priority;
  request->m_Asynchronous = true;
  request->m_Rejected = false;
  request->m_Completed = false;

#if !THREADS
  token.m_State = PreallocatedRequest::Published;
  executeRequest(p1, p2, p3, p4, p5, p6, p7, p8);
  releasePreallocatedRequest(request);
  return PreallocatedPublishResult::Accepted;
#else
  token.m_State = PreallocatedRequest::Published;
  // Readiness must be visible before the node can become consumable.
  m_nAsyncRequests += 1;
  m_nTotalRequests += 1;
  publishRequest(request);
  m_PublicationState -= 1;
  return PreallocatedPublishResult::Accepted;
#endif
}

uint64_t RequestQueue::addAsyncRequestInternal(size_t priority, uint64_t p1, uint64_t p2,
                                               uint64_t p3, uint64_t p4, uint64_t p5, uint64_t p6,
                                               uint64_t p7, uint64_t p8) {
#if !THREADS
  Request* request = new Request(priority, true, p1, p2, p3, p4, p5, p6, p7, p8);
  if (priority >= REQUEST_QUEUE_NUM_PRIORITIES) {
    ERROR("RequestQueue '" << m_Name << "' rejected invalid priority " << priority);
    discardRequest(request);
    return 0;
  }
  executeRequest(p1, p2, p3, p4, p5, p6, p7, p8);
  delete request;
  return 1;
#else
  if (callbackActiveOnCurrentThread() || m_LifecycleMutex.isOwnedByCurrentThread()) {
    return 0;
  }

  Request* request = new Request(priority, true, p1, p2, p3, p4, p5, p6, p7, p8);

  if (priority >= REQUEST_QUEUE_NUM_PRIORITIES) {
    ERROR("RequestQueue '" << m_Name << "' rejected invalid priority " << priority);
    discardRequest(request);
    return 0;
  }

  bool rejected = false;
  bool overloaded = false;
  while (true) {
    bool retry = false;
    {
      auto guard = m_RequestQueueWaiters.acquire();
      if (static_cast<LifecycleState>(m_State.value()) != LifecycleState::Accepting) {
        rejected = true;
      } else if (!drainIntakeLocked(priority)) {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        m_GuardedTransientRetries += 1;
#endif
        retry = true;
      } else if (findDuplicate(*request)) {
        rejected = true;
      } else if (m_nAsyncRequests.value() >= m_nMaxAsyncRequests) {
        rejected = true;
        overloaded = true;
      } else {
        m_nAsyncRequests += 1;
        m_nTotalRequests += 1;
        publishRequest(request);
      }
    }

    if (!retry) {
      break;
    }
    Scheduler::instance().yield();
  }

  if (overloaded) {
    ERROR("RequestQueue: '" << m_Name << "' is not keeping up with async requests");
    ERROR(" -> priority=" << priority << ", p1=" << Hex << p1 << ", p2=" << p2 << ", p3=" << p3
                          << ", p4=" << p4);
    ERROR(" -> p5=" << Hex << p5 << ", p6=" << p6 << ", p7=" << p7 << ", p8=" << p8);
  }
  if (rejected) {
    discardRequest(request);
    return 0;
  }
  return 1;
#endif
}

bool RequestQueue::halt() {
#if THREADS
  if (callbackActiveOnCurrentThread() || m_LifecycleMutex.isOwnedByCurrentThread()) {
    return false;
  }
  TerminationDeferral terminationDeferral;
  LockGuard<Mutex> lifecycleGuard(m_LifecycleMutex);
  return stopWorker();
#else
  return true;
#endif
}

bool RequestQueue::resume() {
#if THREADS
  if (callbackActiveOnCurrentThread() || m_LifecycleMutex.isOwnedByCurrentThread()) {
    return false;
  }
  TerminationDeferral terminationDeferral;
  LockGuard<Mutex> lifecycleGuard(m_LifecycleMutex);
  if (!startWorker()) {
    return false;
  }

  if (!m_pOverrunTimer) {
    Timer* timer = Machine::instance().getTimer();
    if (timer && timer->registerHandler(&m_OverrunChecker)) {
      m_pOverrunTimer = timer;
    }
  }
  return true;
#else
  return true;
#endif
}

RequestQueue::LifecycleState RequestQueue::getLifecycleState() {
  return static_cast<LifecycleState>(m_State.value());
}

bool RequestQueue::callbackActiveOnCurrentThread() const {
  return RequestQueueCallbackScope::contains(this);
}

bool RequestQueue::drain() {
#if THREADS
  if (callbackActiveOnCurrentThread()) {
    return false;
  }
  TerminationDeferral terminationDeferral;
  Thread* current = Processor::information().getCurrentThread();
  while (true) {
    auto guard = m_RequestQueueWaiters.acquire();
    if (!m_nTotalRequests.value()) {
      return true;
    }
    if (static_cast<LifecycleState>(m_State.value()) != LifecycleState::Accepting) {
      ERROR("RequestQueue '" << m_Name << "' cannot drain while it is stopping");
      return false;
    }
    if (m_pThread == current) {
      ERROR("RequestQueue '" << m_Name << "' worker cannot drain itself");
      return false;
    }

    const WaitQueue::WakeReason reason = guard.waitForCompletion(
        WaitQueue::Channel(this, 1), Thread::CallbackDrain, reinterpret_cast<uintptr_t>(this));
    (void)reason;
  }
#else
  return true;
#endif
}

int RequestQueue::trampoline(void* p) {
  RequestQueue* queue = reinterpret_cast<RequestQueue*>(p);
  return queue->work();
}

void RequestQueue::publishRequest(Request* request) {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  using TestAccess = IntrusiveMpscQueueTestAccess<IntakeNode, &IntakeNode::next>;
  IntakeLane& lane = m_IntakeLanes[request->m_Priority];
  const TestAccess::Publication publication =
      TestAccess::beginPush(lane.m_Queue, request->m_Intake);
  if (m_AfterIntakeExchangeHook) {
    m_AfterIntakeExchangeHook(m_AfterIntakeExchangeContext);
  }
  TestAccess::finishPush(lane.m_Queue, publication);
#else
  m_IntakeLanes[request->m_Priority].m_Queue.push(request->m_Intake);
#endif

#if THREADS
  PerProcessorScheduler* scheduler = m_pWorkerScheduler.value();
  if (scheduler) {
    scheduler->ringIrqWorkDoorbell();
  }
#endif
}

bool RequestQueue::drainIntakeLocked(size_t priority) {
  assert(priority < REQUEST_QUEUE_NUM_PRIORITIES);
  using PopResult = IntrusiveMpscQueue<IntakeNode, &IntakeNode::next>::PopResult;

  while (true) {
    IntakeNode* node = nullptr;
    const PopResult result = m_IntakeLanes[priority].m_Queue.pop(node);
    if (result == PopResult::Empty) {
      return true;
    }
    if (result == PopResult::Transient) {
      return false;
    }

    if (!node || !node->owner)
      return false;
    Request* request = node->owner;
    assert(request->m_Priority == priority);
    assert(!request->m_Next);
    if (m_pRequestQueueTail[priority]) {
      m_pRequestQueueTail[priority]->m_Next = request;
    } else {
      m_pRequestQueue[priority] = request;
    }
    m_pRequestQueueTail[priority] = request;
  }
}

RequestQueue::NextRequestResult RequestQueue::getNextRequest(Request*& out) {
  out = nullptr;
  using PopResult = IntrusiveMpscQueue<IntakeNode, &IntakeNode::next>::PopResult;

  for (size_t priority = 0; priority < REQUEST_QUEUE_NUM_PRIORITIES; ++priority) {
    Request* request = m_pRequestQueue[priority];
    if (request) {
      m_pRequestQueue[priority] = request->m_Next;
      if (!m_pRequestQueue[priority]) {
        m_pRequestQueueTail[priority] = nullptr;
      }
      request->m_Next = nullptr;
      out = request;
      return NextRequestResult::Item;
    }

    IntakeNode* node = nullptr;
    const PopResult result = m_IntakeLanes[priority].m_Queue.pop(node);
    if (result == PopResult::Transient) {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      m_WorkerTransientRetries += 1;
#endif
      // An unlinked request at this priority must not be overtaken by
      // work from a lower-priority lane.
      return NextRequestResult::Retry;
    }
    if (result == PopResult::Item) {
      if (!node || !node->owner)
        return NextRequestResult::Retry;
      request = node->owner;
      assert(request->m_Priority == priority);
      out = request;
      return NextRequestResult::Item;
    }
  }

  return NextRequestResult::Empty;
}

RequestQueue::Request* RequestQueue::findDuplicate(const Request& request) {
  if (m_pActiveRequest && !m_pActiveRequest->m_pPreallocatedOwner &&
      m_pActiveRequest->m_Priority == request.m_Priority &&
      compareRequests(*m_pActiveRequest, request)) {
    return m_pActiveRequest;
  }

  Request* queued = m_pRequestQueue[request.m_Priority];
  while (queued) {
    if (!queued->m_pPreallocatedOwner && compareRequests(*queued, request)) {
      return queued;
    }
    queued = queued->m_Next;
  }

  return nullptr;
}

void RequestQueue::completeRequest(Request* request, uint64_t returnValue, bool rejected) {
#if THREADS
  auto guard = request->m_Completion.acquire();
  assert(!request->m_Completed);
  request->m_ReturnValue = returnValue;
  request->m_Rejected = rejected;
  request->m_Completed = true;
  guard.wakeAll();
#else
  request->m_ReturnValue = returnValue;
  request->m_Rejected = rejected;
  request->m_Completed = true;
#endif
}

void RequestQueue::discardRequest(Request* request) {
  invokeCancelRequest(*request);
  delete request;
}

void RequestQueue::invokeCancelRequest(const Request& request) {
  RequestQueueCallbackScope callback(this);
  cancelRequest(request);
}

void RequestQueue::releasePreallocatedRequest(Request* request) {
  assert(request);
  PreallocatedRequest* owner = request->m_pPreallocatedOwner;
  assert(owner);
  assert(&owner->m_Request == request);
  assert(static_cast<size_t>(owner->m_State) == PreallocatedRequest::Published);
  owner->m_ReleaseDepth += 1;
  owner->m_State = PreallocatedRequest::Releasing;
  if (owner->m_ReleaseCallback) {
    RequestQueueCallbackScope callback(this, owner);
    owner->m_ReleaseCallback(owner->m_ReleaseContext);
  }

  while (true) {
    const size_t state = owner->m_State;
    if (state == PreallocatedRequest::Releasing) {
      if (owner->m_State.compareAndSwap(PreallocatedRequest::Releasing,
                                        PreallocatedRequest::Idle)) {
        break;
      }
      continue;
    }
    if (state == PreallocatedRequest::Claimed) {
      // The claimant now owns every remaining token transition. Waiting
      // for it here would make unrelated queue progress depend on the
      // scheduling latency of a producer on another CPU.
      break;
    }

    // Published is an asynchronous republication. Idle is a nested inline
    // republication when threading is disabled.
    assert(state == PreallocatedRequest::Published || state == PreallocatedRequest::Idle);
    break;
  }
  owner->m_ReleaseDepth -= 1;
}

#if THREADS
void RequestQueue::retainRequest(Request* request) {
  request->m_References += 1;
}

void RequestQueue::releaseRequest(Request* request) {
  if (request->m_pPreallocatedOwner) {
    releasePreallocatedRequest(request);
    return;
  }

  assert(static_cast<size_t>(request->m_References));
  if ((request->m_References -= 1) == 0) {
    delete request;
  }
}

uint64_t RequestQueue::waitForRequest(Request* request) {
  uint64_t result = 0;

  while (true) {
    auto guard = request->m_Completion.acquire();
    if (request->m_Completed) {
      if (!request->m_Rejected) {
        result = request->m_ReturnValue;
      }
      break;
    }

    // A synchronous request transfers payload lifetime to the queue until
    // execution completes. Signals and terminal teardown may wake this
    // thread, but neither can make that completion contract optional.
    WaitQueue::WakeReason reason = guard.waitForCompletion(WaitQueue::Channel(), Thread::CondWait,
                                                           reinterpret_cast<uintptr_t>(request));
    (void)reason;
  }

  releaseRequest(request);
  return result;
}
#endif

int RequestQueue::work() {
#if THREADS
  // The queue, not an unrelated terminal request, owns worker retirement.
  // This prevents an idle death from leaving Accepting with no worker and
  // prevents active executeRequest state from being abandoned.
  TerminationDeferral workerLifetime;
  {
    auto guard = m_RequestQueueWaiters.acquire();
    if (m_pThread != Processor::information().getCurrentThread() ||
        static_cast<LifecycleState>(m_State.value()) != LifecycleState::Stopped ||
        m_bWorkerReady.value()) {
      FATAL("RequestQueue '" << m_Name << "' worker entered with invalid state");
    }
    m_bWorkerReady = 1;
    guard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(this, 2));
  }

  while (true) {
    Request* request = nullptr;
    NextRequestResult next = NextRequestResult::Empty;
    m_bWorkerActive = 1;
    {
      auto guard = m_RequestQueueWaiters.acquire();
      const LifecycleState state = static_cast<LifecycleState>(m_State.value());
      if (state == LifecycleState::Stopping) {
        m_bWorkerReady = 0;
        m_bWorkerActive = 0;
        return 0;
      }
      if (state == LifecycleState::Stopped) {
        // startWorker() has not yet reached its final acceptance
        // point. Stay eligible long enough to publish readiness.
        m_bWorkerActive = 0;
      } else {
        next = getNextRequest(request);
        if (next == NextRequestResult::Item) {
          assert(request);
          assert(!m_pActiveRequest);
          m_pActiveRequest = request;
          m_nActiveRequests = 1;
          ++m_WorkerProgressGeneration;
        } else {
          m_bWorkerActive = 0;
        }
      }
    }

    if (next != NextRequestResult::Item) {
      // Empty workers remain published but scheduler-ineligible. Retry
      // leaves them eligible because the producer accounted before its
      // unfinished MPSC link became visible.
      Scheduler::instance().yield();
      continue;
    }

    assert(request);

    uint64_t result = executeRequest(request->p1, request->p2, request->p3, request->p4,
                                     request->p5, request->p6, request->p7, request->p8);
    completeRequest(request, result, false);

    {
      auto guard = m_RequestQueueWaiters.acquire();
      assert(m_pActiveRequest == request);
      m_pActiveRequest = nullptr;
      assert(m_nTotalRequests.value());
      if (request->m_Asynchronous) {
        assert(m_nAsyncRequests.value());
        m_nAsyncRequests -= 1;
      }
    }

    // Drop the queue's ownership after removing the request from every
    // location discoverable by duplicate detection. A preallocated-token
    // release callback may republish dependent work, so it runs before
    // the final drain predicate is observed.
    releaseRequest(request);

    {
      auto guard = m_RequestQueueWaiters.acquire();
      assert(m_nTotalRequests.value());
      const size_t remaining = (m_nTotalRequests -= 1);
      m_nActiveRequests = 0;
      if (!remaining) {
        guard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(this, 1));
      }
    }

    m_bWorkerActive = 0;
    Scheduler::instance().yield();
  }
#else
  return 0;
#endif
}

#if THREADS
void RequestQueue::RequestQueueOverrunChecker::resetBaselineLocked() {
  m_LastQueueSize = 0;
  m_LastProgressGeneration = queue->m_WorkerProgressGeneration;
  m_HasBacklogBaseline = false;
}

RequestQueue::OverrunStatus RequestQueue::RequestQueueOverrunChecker::sample(size_t& lastSize,
                                                                             size_t& currentSize) {
  auto guard = queue->m_RequestQueueWaiters.acquire();
  lastSize = m_LastQueueSize;
  const size_t total = queue->m_nTotalRequests.value();
  const size_t active = queue->m_nActiveRequests.value();
  assert(total >= active);
  currentSize = total - active;

  const size_t progress = queue->m_WorkerProgressGeneration;
  if (static_cast<LifecycleState>(queue->m_State.value()) != LifecycleState::Accepting ||
      !currentSize) {
    resetBaselineLocked();
    return OverrunStatus::Clear;
  }

  OverrunStatus status = OverrunStatus::Armed;
  if (m_HasBacklogBaseline) {
    if (progress == m_LastProgressGeneration && currentSize >= m_LastQueueSize) {
      status = OverrunStatus::Stalled;
    } else if (progress != m_LastProgressGeneration && currentSize > m_LastQueueSize) {
      status = OverrunStatus::Overloaded;
    }
  }

  m_LastQueueSize = currentSize;
  m_LastProgressGeneration = progress;
  m_HasBacklogBaseline = true;
  return status;
}

void RequestQueue::RequestQueueOverrunChecker::timer(uint64_t delta) {
  m_Tick += delta;
  if (m_Tick < Time::Multiplier::Second) {
    return;
  }
  m_Tick %= Time::Multiplier::Second;

  size_t lastSize = 0;
  size_t currentSize = 0;
  const OverrunStatus status = sample(lastSize, currentSize);
  if (status == OverrunStatus::Stalled) {
    FATAL("RequestQueue '" << queue->m_Name
                           << "' made no worker progress for a full watchdog interval with "
                           << currentSize << " queued requests!");
  } else if (status == OverrunStatus::Overloaded) {
    WARNING("RequestQueue '" << queue->m_Name << "' backlog grew from " << lastSize << " to "
                             << currentSize << " despite worker progress.");
  }
}
#endif

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
RequestQueue::OverrunStatus RequestQueue::sampleOverrunForTest() {
  size_t lastSize = 0;
  size_t currentSize = 0;
  return m_OverrunChecker.sample(lastSize, currentSize);
}
#endif
