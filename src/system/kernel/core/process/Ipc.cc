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
#include "pedigree/kernel/process/Completion.h"
#include "pedigree/kernel/process/Ipc.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/MemoryPool.h"
#include "pedigree/kernel/utilities/RadixTree.h"
#include "pedigree/kernel/utilities/Result.h"

using namespace Ipc;

#define MEMPOOL_BUFF_SIZE 4096
#define MEMPOOL_BASE_SIZE 1024  /// \todo Tune.

static MemoryPool __ipc_mempool("IPC Message Pool");

static RadixTree<IpcEndpoint*> __endpoints;

class Ipc::IpcEndpoint::IpcCompletion {
 public:
  explicit IpcCompletion(bool asynchronous);

  MUST_USE_RESULT bool wait();
  void complete();

 private:
  ~IpcCompletion();
  static void discardWait(void* context);
  void releaseReference();

  Completion m_Completion;
  Atomic<size_t> m_References;
};

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
namespace {
struct HostedIpcInterruptContext {
  HostedIpcInterruptContext(IpcEndpoint* endpoint, Thread* thread)
      : endpoint(endpoint),
        thread(thread),
        hookCalls(0),
        hookFailures(0),
        eventWakes(0),
        rescues(0) {}

  IpcEndpoint* endpoint;
  Thread* thread;
  size_t hookCalls;
  size_t hookFailures;
  size_t eventWakes;
  size_t rescues;
};

HostedIpcInterruptContext* g_HostedIpcInterruptContext = nullptr;
}  // namespace

EXPORTED_PUBLIC bool Ipc::runHostedIpcInterruptionRegression() {
  Thread* thread = Processor::information().getCurrentThread();
  if (!thread) {
    ERROR("HOSTED-WAIT-TEST: FAIL ipc-interruption: no current thread");
    return false;
  }

  IpcEndpoint* endpoint = new IpcEndpoint(MakeConstantString("Hosted IPC interruption"));
  HostedIpcInterruptContext context(endpoint, thread);

  g_HostedIpcInterruptContext = &context;
  WaitQueue::setBeforeBlockHook(
      [](WaitQueue* queue, Thread* waiter, const WaitQueue::Channel& channel, size_t debugState) {
        HostedIpcInterruptContext* hookContext = g_HostedIpcInterruptContext;
        if (!hookContext || debugState != Thread::SemWait) {
          return;
        }

        ++hookContext->hookCalls;
        if (waiter != hookContext->thread) {
          ++hookContext->hookFailures;
          return;
        }

        if (hookContext->hookCalls == 1) {
          if (queue->wakeOne(WaitQueue::WakeReason::Event, channel)) {
            ++hookContext->eventWakes;
          } else {
            ++hookContext->hookFailures;
          }
        } else if (hookContext->hookCalls == 2) {
          // Keep the unfixed path from hanging the suite after it
          // incorrectly re-enters the semaphore wait.
          ++hookContext->rescues;
          if (!hookContext->endpoint->pushMessage(nullptr, true)) {
            ++hookContext->hookFailures;
          }
        } else {
          ++hookContext->hookFailures;
        }
      });
  IpcMessage* message = endpoint->getMessage(true);
  WaitQueue::setBeforeBlockHook(nullptr);
  g_HostedIpcInterruptContext = nullptr;

  const bool passed = !message && context.hookCalls == 1 && context.hookFailures == 0 &&
                      context.eventWakes == 1 && context.rescues == 0;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS ipc-interruption");
  } else {
    ERROR("HOSTED-WAIT-TEST: FAIL ipc-interruption: calls="
          << context.hookCalls << " failures=" << context.hookFailures << " event-wakes="
          << context.eventWakes << " rescues=" << context.rescues << " message=" << message);
  }
  return passed;
}
#endif

IpcEndpoint::IpcCompletion::IpcCompletion(bool asynchronous)
    : m_Completion(), m_References(asynchronous ? 1 : 2) {}

IpcEndpoint::IpcCompletion::~IpcCompletion() = default;

bool IpcEndpoint::IpcCompletion::wait() {
  Thread::StackDiscardScope discardScope(&IpcCompletion::discardWait, this);
  bool result = m_Completion.wait();
  discardScope.disarm();
  releaseReference();
  return result;
}

void IpcEndpoint::IpcCompletion::complete() {
  m_Completion.complete();
  releaseReference();
}

void IpcEndpoint::IpcCompletion::discardWait(void* context) {
  reinterpret_cast<IpcCompletion*>(context)->releaseReference();
}

void IpcEndpoint::IpcCompletion::releaseReference() {
  if ((m_References -= 1) == 0) {
    delete this;
  }
}

IpcEndpoint* Ipc::getEndpoint(String& name) {
  RadixTree<IpcEndpoint*>::LookupType result = __endpoints.lookup(name);
  return result.hasValue() ? result.value() : nullptr;
}

void Ipc::createEndpoint(String& name) {
  if (__endpoints.lookup(name).hasValue())
    return;
  __endpoints.insert(name, new IpcEndpoint(name));
}

void Ipc::removeEndpoint(String& name) {
  if (!__endpoints.lookup(name).hasValue())
    return;
  __endpoints.remove(name);
}

bool Ipc::send(IpcEndpoint* pEndpoint, IpcMessage* pMessage, bool bAsync) {
  if (!(pEndpoint && pMessage))
    return false;

  IpcEndpoint::IpcCompletion* pCompletion = pEndpoint->pushMessage(pMessage, bAsync);
  if (!pCompletion)
    return false;

  // Block if we're allowed to.
  if (!bAsync) {
    return pCompletion->wait();
  }

  return true;
}

bool Ipc::recv(IpcEndpoint* pEndpoint, IpcMessage** pMessage, bool bAsync) {
  if (!(pEndpoint && pMessage))
    return false;

  IpcMessage* pMsg = pEndpoint->getMessage(!bAsync);
  if (pMsg) {
    /// > 4 KB messages only use this form of IPC as a synchronisation
    /// method, they actually communicate via reads and writes to their
    /// shared block of memory.
    *pMessage = new IpcMessage(*pMsg);

    return true;
  }

  return false;
}

IpcEndpoint::IpcCompletion* IpcEndpoint::pushMessage(IpcMessage* pMessage, bool bAsync) {
  LockGuard<Mutex> guard(m_QueueLock);

  QueuedMessage* p = new QueuedMessage;
  p->pMessage = pMessage;
  p->pCompletion = new IpcCompletion(bAsync);

  m_Queue.pushBack(p);
  m_QueueSize.release();

  return p->pCompletion;
}

IpcMessage* IpcEndpoint::getMessage(bool bBlock) {
  // Handle the case where m_QueueSize acquire() returns but the queue is
  // already emptied by the time we acquire the Mutex.
  QueuedMessage* p = 0;
  while (p == 0) {
    bool b = m_QueueSize.tryAcquire();
    if (!b) {
      if (!bBlock)
        return 0;
      else if (!m_QueueSize.acquire()) {
        return nullptr;
      }
    }

    m_QueueLock.acquire();
    p = m_Queue.popFront();
    m_QueueLock.release();
  }

  IpcMessage* pReturn = p->pMessage;

  p->pCompletion->complete();
  delete p;

  return pReturn;
}

Ipc::IpcMessage::IpcMessage() : nPages(1), m_vAddr(0), m_pMemRegion(0) {
  allocatePoolBuffer();
}

void Ipc::IpcMessage::allocatePoolBuffer() {
  if (!__ipc_mempool.initialised()) {
    if (!__ipc_mempool.initialise(MEMPOOL_BASE_SIZE, MEMPOOL_BUFF_SIZE)) {
      ERROR("IpcMessage: memory pool could not be initialised.");
      return;
    }
  }

  // Allocate the message.
  uintptr_t msg = __ipc_mempool.allocate();
  if (msg) {
    // Remap to user read/write.
    Processor::information().getVirtualAddressSpace().setFlags(reinterpret_cast<void*>(msg),
                                                               VirtualAddressSpace::Write);

    m_vAddr = msg;
  } else {
    ERROR("IpcMessage: no memory available.");
    return;
  }
}

Ipc::IpcMessage::IpcMessage(size_t nBytes, uintptr_t regionHandle)
    : nPages(0), m_vAddr(0), m_pMemRegion(0) {
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  if (nBytes > (~size_t{0} - (pageSize - 1))) {
    ERROR("IpcMessage: requested size overflows the page count.");
    return;
  }
  nPages = (nBytes + pageSize - 1) / pageSize;

  if (nBytes < MEMPOOL_BUFF_SIZE) {
    nPages = 1;
    allocatePoolBuffer();
  } else {
    MemoryRegion* pRegion = reinterpret_cast<MemoryRegion*>(regionHandle);
    if (!pRegion) {
      // Need to allocate RAM for this space.
      m_pMemRegion = new MemoryRegion("IPC Message");
      if (!PhysicalMemoryManager::instance().allocateRegion(*m_pMemRegion, nPages,
                                                            PhysicalMemoryManager::continuous,
                                                            VirtualAddressSpace::Write)) {
        delete m_pMemRegion;
        m_pMemRegion = 0;

        ERROR("IpcMessage: region allocation failed.");
      }
    } else {
      // Need to remap the given region into this address space, if it
      // isn't mapped in already.
      void* pAddress = pRegion->virtualAddress();
      physical_uintptr_t phys = pRegion->physicalAddress();

      VirtualAddressSpace& va = Processor::information().getVirtualAddressSpace();
      if (va.isMapped(pAddress)) {
        size_t ignore = 0;
        physical_uintptr_t map_phys = 0;
        va.getMapping(pAddress, map_phys, ignore);

        // This works because we ask for continuous physical memory.
        // Even if we didn't, it would still be a valid check. We would
        // just have to verify a few more physical pages to be 100%
        // certain.
        if (phys == map_phys) {
          m_pMemRegion = pRegion;
          return;
        }
      }

      // Create the region.
      m_pMemRegion = new MemoryRegion("IPC Message");
      if (!PhysicalMemoryManager::instance().allocateRegion(*m_pMemRegion, nPages,
                                                            PhysicalMemoryManager::continuous,
                                                            VirtualAddressSpace::Write, phys)) {
        delete m_pMemRegion;
        m_pMemRegion = 0;

        ERROR("IpcMessage: region allocation (via handle) failed.");
      }
    }
  }
}

Ipc::IpcMessage::~IpcMessage() {
  /// \todo Problem: an endpoint might still have this message in a queue.
  /// We should notify it that we're now dead.
  if (m_pMemRegion) {
    delete m_pMemRegion;
  } else if (m_vAddr) {
    __ipc_mempool.free(m_vAddr);
  }
}

void* Ipc::IpcMessage::getBuffer() {
  if (m_vAddr)
    return reinterpret_cast<void*>(m_vAddr);
  else if (m_pMemRegion)
    return m_pMemRegion->virtualAddress();
  else
    return 0;
}

void* Ipc::IpcMessage::getHandle() {
  return m_pMemRegion;
}
