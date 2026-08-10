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
#include "pedigree/kernel/process/MemoryPressureManager.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#if THREADS
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#endif
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

MemoryPressureManager MemoryPressureManager::m_Instance;

namespace {
const void* currentCallbackOwner() {
#if THREADS
  Thread* thread = Processor::information().getCurrentThread();
  if (thread) {
    return static_cast<const void*>(thread);
  }
#endif
  return static_cast<const void*>(&Processor::information());
}
}  // namespace

MemoryPressureHandler::MemoryPressureHandler()
    : m_pPrevious(nullptr),
      m_pNext(nullptr),
      m_Priority(0),
      m_RegistrationSequence(0),
      m_bRegistered(false),
      m_bRemoving(false)
#if THREADS
      ,
      m_CallbacksInFlight(0),
      m_pCallbackOwner(nullptr),
      m_CallbackWaiters()
#endif
{
}

MemoryPressureHandler::~MemoryPressureHandler() = default;

bool MemoryPressureManager::compact() {
#if THREADS
  if (!Processor::information().getCurrentThread() || !Processor::getInterrupts()) {
    // Recovery callbacks may block and may allocate through subsystems
    // whose outer spinlock triggered this pressure pass. Running them in
    // atomic context would turn allocation failure into a lock inversion.
    return false;
  }
#endif

  // A callback may own stack-backed lifetime state. Terminal requests can
  // interrupt its work, but cannot skip retirement of the callback and
  // global compact pins below.
  TerminationDeferral terminationDeferral;
  const void* owner = currentCallbackOwner();

  // The compact predicate and its wakeup share a WaitQueue guard. This keeps
  // concurrent allocators from missing the pass which should replenish
  // memory without allocating any waiter storage.
#if THREADS
  while (true) {
    auto compactGuard = m_CompactWaiters.acquire();
    if (m_bCompacting.compareAndSwap(false, true)) {
      __atomic_store_n(&m_pCompactOwner, owner, __ATOMIC_RELEASE);
      break;
    }

    if (__atomic_load_n(&m_pCompactOwner, __ATOMIC_ACQUIRE) == owner) {
      return false;
    }

    const WaitQueue::WakeReason compactWake = compactGuard.waitForCompletion(
        WaitQueue::Channel(this), Thread::CallbackDrain, reinterpret_cast<uintptr_t>(this));
    (void)compactWake;
  }
#else
  if (!m_bCompacting.compareAndSwap(false, true)) {
    return false;
  }
  __atomic_store_n(&m_pCompactOwner, owner, __ATOMIC_RELEASE);
#endif

  size_t registrationLimit = 0;
  {
    LockGuard<Spinlock> guard(m_Lock);
    registrationLimit = m_NextRegistrationSequence;
  }

  bool releasedPages = false;
  size_t priority = 0;
  size_t registrationCursor = 0;
  while (priority < MAX_MEMPRESSURE_PRIORITY) {
    MemoryPressureHandler* handler = nullptr;
    {
      LockGuard<Spinlock> guard(m_Lock);
      for (MemoryPressureHandler* candidate = m_Handlers[priority]; candidate;
           candidate = candidate->m_pNext) {
        if (candidate->m_RegistrationSequence > registrationCursor &&
            candidate->m_RegistrationSequence <= registrationLimit) {
          handler = candidate;
          registrationCursor = candidate->m_RegistrationSequence;
          break;
        }
      }

#if THREADS
      if (handler) {
        auto callbackGuard = handler->m_CallbackWaiters.acquire();
        assert(!handler->m_CallbacksInFlight);
        handler->m_CallbacksInFlight = 1;
        handler->m_pCallbackOwner = currentCallbackOwner();
      }
#endif
    }

    if (!handler) {
      ++priority;
      registrationCursor = 0;
      continue;
    }

    const char* description = handler->getMemoryPressureDescription();
    NOTICE("Compact: " << (description ? description : "<unnamed memory-pressure handler>"));
    releasedPages = handler->compact();
    if (releasedPages) {
      NOTICE("  -> pages released!");
    } else {
      NOTICE("  -> no pages released.");
    }

#if THREADS
    {
      auto callbackGuard = handler->m_CallbackWaiters.acquire();
      assert(handler->m_CallbacksInFlight == 1);
      handler->m_CallbacksInFlight = 0;
      handler->m_pCallbackOwner = nullptr;
      callbackGuard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(handler));
    }
#endif

    if (releasedPages) {
      break;
    }
  }

#if THREADS
  {
    auto compactGuard = m_CompactWaiters.acquire();
    __atomic_store_n(&m_pCompactOwner, nullptr, __ATOMIC_RELEASE);
    const bool cleared = m_bCompacting.compareAndSwap(true, false);
    assert(cleared);
    compactGuard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(this));
  }
#else
  __atomic_store_n(&m_pCompactOwner, nullptr, __ATOMIC_RELEASE);
  const bool cleared = m_bCompacting.compareAndSwap(true, false);
  assert(cleared);
#endif
  return releasedPages;
}

bool MemoryPressureManager::compactingForCurrentExecution() const {
  return m_bCompacting &&
         __atomic_load_n(&m_pCompactOwner, __ATOMIC_ACQUIRE) == currentCallbackOwner();
}

MemoryPressureManager::MemoryPressureManager()
    : m_Lock(false),
      m_Handlers(),
      m_HandlerTails(),
      m_NextRegistrationSequence(0),
      m_bCompacting(false),
      m_pCompactOwner(nullptr)
#if THREADS
      ,
      m_CompactWaiters()
#endif
{
}

MemoryPressureManager::~MemoryPressureManager() = default;

void MemoryPressureManager::registerHandler(size_t prio, MemoryPressureHandler* pHandler) {
  if (!pHandler) {
    FATAL("Cannot register a null memory-pressure handler.");
  }

  if (prio >= MAX_MEMPRESSURE_PRIORITY) {
    prio = MAX_MEMPRESSURE_PRIORITY - 1;
  }

  LockGuard<Spinlock> guard(m_Lock);
#if THREADS
  auto callbackGuard = pHandler->m_CallbackWaiters.acquire();
#endif
  if (pHandler->m_bRegistered || pHandler->m_bRemoving) {
    FATAL("Memory-pressure handler registered more than once.");
  }

#if THREADS
  if (pHandler->m_CallbacksInFlight) {
    FATAL("Memory-pressure handler registered during callback removal.");
  }
#endif

  pHandler->m_Priority = prio;
  pHandler->m_RegistrationSequence = ++m_NextRegistrationSequence;
  pHandler->m_pPrevious = m_HandlerTails[prio];
  pHandler->m_pNext = nullptr;
  pHandler->m_bRegistered = true;

  if (m_HandlerTails[prio]) {
    m_HandlerTails[prio]->m_pNext = pHandler;
  } else {
    m_Handlers[prio] = pHandler;
  }
  m_HandlerTails[prio] = pHandler;
}

void MemoryPressureManager::removeHandler(MemoryPressureHandler* pHandler) {
  if (!pHandler) {
    return;
  }

  bool needsDrain = false;
  {
    LockGuard<Spinlock> guard(m_Lock);

#if THREADS
    auto callbackGuard = pHandler->m_CallbackWaiters.acquire();
    if (pHandler->m_CallbacksInFlight && pHandler->m_pCallbackOwner == currentCallbackOwner()) {
      FATAL(
          "A memory-pressure callback cannot synchronously remove "
          "itself.");
    }
#endif

    if (pHandler->m_bRegistered) {
      const size_t priority = pHandler->m_Priority;
      if (pHandler->m_pPrevious) {
        pHandler->m_pPrevious->m_pNext = pHandler->m_pNext;
      } else {
        m_Handlers[priority] = pHandler->m_pNext;
      }
      if (pHandler->m_pNext) {
        pHandler->m_pNext->m_pPrevious = pHandler->m_pPrevious;
      } else {
        m_HandlerTails[priority] = pHandler->m_pPrevious;
      }

      pHandler->m_pPrevious = nullptr;
      pHandler->m_pNext = nullptr;
      pHandler->m_bRegistered = false;
      pHandler->m_bRemoving = true;
      needsDrain = true;
    } else {
      if (pHandler->m_bRemoving) {
        FATAL(
            "Concurrent removal of one memory-pressure handler is "
            "not permitted.");
      }
    }
  }

  if (!needsDrain) {
    return;
  }

#if THREADS
  const bool canBlock = Processor::information().getCurrentThread() && Processor::getInterrupts();
  while (true) {
    auto callbackGuard = pHandler->m_CallbackWaiters.acquire();
    const bool callbacksInFlight = pHandler->m_CallbacksInFlight;
    if (!callbacksInFlight) {
      break;
    }

    if (!canBlock) {
      FATAL(
          "Cannot drain a live memory-pressure callback from atomic "
          "context.");
    }

    const WaitQueue::WakeReason callbackWake = callbackGuard.waitForCompletion(
        WaitQueue::Channel(pHandler), Thread::CallbackDrain, reinterpret_cast<uintptr_t>(pHandler));
    (void)callbackWake;
  }

  {
    LockGuard<Spinlock> guard(m_Lock);
    assert(pHandler->m_bRemoving);
    assert(!pHandler->m_bRegistered);
    pHandler->m_bRemoving = false;
  }
#else
  {
    LockGuard<Spinlock> guard(m_Lock);
    if (!pHandler->m_bRegistered) {
      pHandler->m_bRemoving = false;
    }
  }
#endif
}
