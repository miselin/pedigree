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
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/SyscallHandler.h"
#include "pedigree/kernel/processor/SyscallManager.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/new"

struct SyscallManager::HandlerSlot {
  HandlerSlot(SyscallHandler* handler, FastEntry entry) : handler(handler), entry(entry) {}

  SyscallHandler* const handler;
  const FastEntry entry;
};

SyscallManager::HandlerSlot SyscallManager::m_ClosingSlot(nullptr, nullptr);

SyscallManager::Registration::Registration()
    : m_pManager(nullptr), m_Service(serviceEnd), m_pSlot(nullptr) {}

SyscallManager::Registration::Registration(Registration&& other)
    : m_pManager(other.m_pManager), m_Service(other.m_Service), m_pSlot(other.m_pSlot) {
  other.m_pManager = nullptr;
  other.m_Service = serviceEnd;
  other.m_pSlot = nullptr;
}

SyscallManager::Registration::~Registration() {
  if (m_pManager && !reset()) {
    FATAL("Live syscall registration could not be retired.");
  }
}

SyscallManager::Registration& SyscallManager::Registration::operator=(Registration&& other) {
  if (this != &other) {
    if (m_pManager && !reset()) {
      FATAL("Syscall registration move could not retire ownership.");
    }
    m_pManager = other.m_pManager;
    m_Service = other.m_Service;
    m_pSlot = other.m_pSlot;
    other.m_pManager = nullptr;
    other.m_Service = serviceEnd;
    other.m_pSlot = nullptr;
  }
  return *this;
}

bool SyscallManager::Registration::closeAdmission() {
  return !m_pManager || m_pManager->closeHandler(*this);
}

bool SyscallManager::Registration::reset() {
  if (!m_pManager) {
    return true;
  }
  if (!m_pManager->unregisterHandler(*this)) {
    return false;
  }

  m_pManager = nullptr;
  m_Service = serviceEnd;
  m_pSlot = nullptr;
  return true;
}

SyscallManager::SyscallManager()
    : m_HandlerLock(),
      m_HandlerSlots(),
      m_Published()
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      ,
      m_HandlerPinHook(nullptr),
      m_PostSyscallHook(nullptr)
#endif
{
}

SyscallManager::~SyscallManager() = default;

uintptr_t SyscallManager::dispatchVirtual(SyscallHandler* handler, SyscallState& state) {
  return handler->syscall(state);
}

bool SyscallManager::inCallback() {
  Thread* thread = Processor::information().getCurrentThread();
  return thread && thread->getSyscallDispatchContext();
}

bool SyscallManager::registerHandler(Service_t service, SyscallHandler* handler,
                                     Registration& registration, FastEntry entry) {
  if (UNLIKELY(service >= serviceEnd) || !handler || registration) {
    return false;
  }

  auto* slot = new HandlerSlot(handler, entry ? entry : dispatchVirtual);
  if (!slot) {
    return false;
  }
  m_HandlerLock.acquire();
  if (m_HandlerSlots[service]) {
    m_HandlerLock.release();
    delete slot;
    return false;
  }

  m_HandlerSlots[service] = slot;
  registration.m_pManager = this;
  registration.m_Service = service;
  registration.m_pSlot = slot;
  __atomic_store_n(&m_Published[service], slot, __ATOMIC_SEQ_CST);
  m_HandlerLock.release();
  return true;
}

bool SyscallManager::closeHandler(Registration& registration) {
  if (registration.m_pManager != this || UNLIKELY(registration.m_Service >= serviceEnd) ||
      !registration.m_pSlot || inCallback()) {
    return false;
  }

  LockGuard<Spinlock> guard(m_HandlerLock);
  if (m_HandlerSlots[registration.m_Service] != registration.m_pSlot) {
    return false;
  }
  __atomic_store_n(&m_Published[registration.m_Service], nullptr, __ATOMIC_SEQ_CST);
  return true;
}

#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
bool SyscallManager::dispatchHandlerForTest(Service_t service, uintptr_t& result) {
  PostSyscallAction action;
  HandlerLease handler;
  if (!acquireHandler(service, handler, action)) {
    return false;
  }

  SyscallState state = {};
  result = dispatchHandler(handler, state);
  return action.kind == NoPostSyscallAction;
}
#endif

bool SyscallManager::unregisterHandler(Registration& registration) {
  if (registration.m_pManager != this || UNLIKELY(registration.m_Service >= serviceEnd) ||
      !registration.m_pSlot) {
    return false;
  }

  const bool canYield = !inCallback() && Processor::information().getCurrentThread() &&
                        Processor::executionContext() == ExecutionContext::WaitableThread;
  TerminationDeferral lifetime;
  HandlerSlot* slot = registration.m_pSlot;
  const Service_t service = registration.m_Service;
  Scheduler& scheduler = Scheduler::instance();
  if (!canYield) {
    // Take membership protection before publishing Closing: a reader may
    // already hold this lock, and must never spin while its remover needs it.
    RecursingLockGuard<Spinlock> registry(scheduler.m_SchedulerLock);
    LockGuard<Spinlock> guard(m_HandlerLock);
    if (m_HandlerSlots[service] != slot) {
      return false;
    }
    HandlerSlot* published =
        __atomic_exchange_n(&m_Published[service], &m_ClosingSlot, __ATOMIC_SEQ_CST);
    if (scheduler.hasActiveSyscallLocked(service)) {
      __atomic_store_n(&m_Published[service], published, __ATOMIC_SEQ_CST);
      return false;
    }
    __atomic_store_n(&m_Published[service], nullptr, __ATOMIC_SEQ_CST);
    m_HandlerSlots[service] = nullptr;
  } else {
    {
      LockGuard<Spinlock> guard(m_HandlerLock);
      if (m_HandlerSlots[service] != slot) {
        return false;
      }
      __atomic_store_n(&m_Published[service], nullptr, __ATOMIC_SEQ_CST);
    }

    // Closed publication prevents new callbacks. Scalar inspection under the
    // scheduler lock covers running and blocked Threads without borrowing stacks.
    while (true) {
      bool active;
      {
        RecursingLockGuard<Spinlock> registry(scheduler.m_SchedulerLock);
        active = scheduler.hasActiveSyscallLocked(service);
      }
      if (!active) {
        break;
      }
      scheduler.yield();
    }
    LockGuard<Spinlock> guard(m_HandlerLock);
    m_HandlerSlots[service] = nullptr;
  }
  delete slot;
  return true;
}

bool SyscallManager::acquireHandler(Service_t service, HandlerLease& lease,
                                    PostSyscallAction& action) {
  Thread* thread = Processor::information().getCurrentThread();
  if (UNLIKELY(service >= serviceEnd) || lease.m_pManager || !thread) {
    return false;
  }

  const bool interrupts = Processor::getInterrupts();
  Processor::setInterrupts(false);
  HandlerSlot* slot;
  while (true) {
    slot = __atomic_load_n(&m_Published[service], __ATOMIC_ACQUIRE);
    if (!slot) {
      Processor::setInterrupts(interrupts);
      return false;
    }
    if (slot == &m_ClosingSlot) {
      Processor::pause();
      continue;
    }

    size_t& admission = thread->m_ActiveSyscalls[service];
    const size_t count = __atomic_load_n(&admission, __ATOMIC_RELAXED);
    if (count == ~size_t(0)) {
      FATAL("Syscall admission nesting overflow.");
    }
    // This publication and reload share an SC order with unpublication and
    // the writer's scan. Either the scan sees us or we see the closed slot.
    __atomic_store_n(&admission, count + 1, __ATOMIC_SEQ_CST);
    slot = __atomic_load_n(&m_Published[service], __ATOMIC_SEQ_CST);
    if (slot && slot != &m_ClosingSlot) {
      break;
    }
    __atomic_store_n(&admission, count, __ATOMIC_RELEASE);
    if (!slot) {
      Processor::setInterrupts(interrupts);
      return false;
    }
  }

  lease.m_pManager = this;
  lease.m_Service = service;
  lease.m_pHandler = slot->handler;
  lease.m_Entry = slot->entry;
  lease.m_pThread = thread;
  lease.m_Previous = static_cast<HandlerLease*>(thread->getSyscallDispatchContext());
  lease.m_Action = &action;
  thread->registerFreshTerminationDeferral(lease.m_Cleanup, abandonedHandlerCleanup, &lease);
  thread->setSyscallDispatchContext(&lease);
  Processor::setInterrupts(interrupts);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  HandlerPinHook hook = __atomic_load_n(&m_HandlerPinHook, __ATOMIC_ACQUIRE);
  if (hook) {
    hook(service, lease.m_pHandler);
  }
#endif
  return true;
}

void SyscallManager::releaseHandler(HandlerLease& lease, bool normalReturn) {
  const bool interrupts = Processor::getInterrupts();
  Processor::setInterrupts(false);
  Thread* thread = lease.m_pThread;
  if (normalReturn) {
    if (thread != Processor::information().getCurrentThread()) {
      FATAL("Syscall lease released by a different Thread.");
    }
    thread->unregisterDeferredScope(lease.m_Cleanup);
  }
  auto* current = static_cast<HandlerLease*>(thread->getSyscallDispatchContext());
  if (current == &lease) {
    thread->setSyscallDispatchContext(lease.m_Previous);
  } else {
    // Nested exec can retire a lower state's lease while its own dispatch
    // remains active. Remove that ancestor before its action storage vanishes.
    while (current && current->m_Previous != &lease) {
      current = current->m_Previous;
    }
    if (!current) {
      FATAL("Syscall lease was absent from its Thread's dispatch chain.");
    }
    current->m_Previous = lease.m_Previous;
  }
  size_t& admission = thread->m_ActiveSyscalls[lease.m_Service];
  const size_t count = __atomic_load_n(&admission, __ATOMIC_RELAXED);
  if (!count) {
    FATAL("Syscall admission nesting underflow.");
  }
  lease.m_pManager = nullptr;
  lease.m_pHandler = nullptr;
  lease.m_pThread = nullptr;
  __atomic_store_n(&admission, count - 1, __ATOMIC_RELEASE);
  Processor::setInterrupts(interrupts);
}

void SyscallManager::abandonedHandlerCleanup(void* context) {
  HandlerLease* lease = reinterpret_cast<HandlerLease*>(context);
  if (lease && lease->m_pManager) {
    lease->m_pManager->releaseHandler(*lease, false);
  }
}

bool SyscallManager::requestPostSyscallAction(PostSyscallActionKind kind, intptr_t value,
                                              const ProcessorState* state) {
  Thread* thread = Processor::information().getCurrentThread();
  auto* lease = thread ? static_cast<HandlerLease*>(thread->getSyscallDispatchContext()) : nullptr;
  PostSyscallAction* action = lease ? lease->m_Action : nullptr;
  if (!action || action->kind != NoPostSyscallAction) {
    return false;
  }
  if ((kind == ReturnFromEvent || kind == PopEventState) && !thread->getStateLevel()) {
    return false;
  }

  if (state) {
    new (&action->state) ProcessorState(*state);
  }
  action->value = value;
  action->kind = kind;
  return true;
}

bool SyscallManager::requestThreadExit() {
  return requestPostSyscallAction(TerminateCurrentThread, 0);
}

bool SyscallManager::requestProcessExit(int status) {
  return requestPostSyscallAction(ExitCurrentProcess, static_cast<intptr_t>(status));
}

bool SyscallManager::requestEventReturn() {
  return requestPostSyscallAction(ReturnFromEvent, 0);
}

bool SyscallManager::requestEventStatePop() {
  return requestPostSyscallAction(PopEventState, 0);
}

bool SyscallManager::requestStateRestore(const ProcessorState& state) {
  return requestPostSyscallAction(RestoreProcessorState, 0, &state);
}

bool SyscallManager::requestUserJump(uintptr_t instructionPointer, uintptr_t stackPointer) {
  ProcessorState state;
  state.setInstructionPointer(instructionPointer);
  state.setStackPointer(stackPointer);
  return requestPostSyscallAction(JumpToUserspace, 0, &state);
}

bool SyscallManager::requestReboot(Machine::ShutdownType type) {
  return requestPostSyscallAction(RebootSystem, static_cast<intptr_t>(type));
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void SyscallManager::setHandlerPinHook(HandlerPinHook hook) {
  __atomic_store_n(&m_HandlerPinHook, hook, __ATOMIC_RELEASE);
}

void SyscallManager::setPostSyscallHook(PostSyscallHook hook) {
  __atomic_store_n(&m_PostSyscallHook, hook, __ATOMIC_RELEASE);
}

bool SyscallManager::postSyscallHookHandled(const PostSyscallAction& action) {
  PostSyscallHook hook = __atomic_load_n(&m_PostSyscallHook, __ATOMIC_ACQUIRE);
  const ProcessorState* state =
      action.kind == RestoreProcessorState || action.kind == JumpToUserspace ? &action.state
                                                                             : nullptr;
  return hook && hook(action.kind, action.value, state);
}
#endif
