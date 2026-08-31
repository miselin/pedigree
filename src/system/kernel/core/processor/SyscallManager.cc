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
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/SyscallHandler.h"
#include "pedigree/kernel/processor/SyscallManager.h"
#include "pedigree/kernel/utilities/assert.h"

SyscallManager::HandlerSlot::HandlerSlot()
    : handler(nullptr),
      generation(0),
      inFlight(0),
      enabled(false),
      draining(false),
      dispatches(nullptr),
      drainWaiters() {}

SyscallManager::PostSyscallAction::PostSyscallAction()
    : kind(NoPostSyscallAction), value(0), state() {}

SyscallManager::Registration::Registration()
    : m_pManager(nullptr), m_Service(serviceEnd), m_pHandler(nullptr), m_Generation(0) {}

SyscallManager::Registration::Registration(Registration&& other)
    : m_pManager(other.m_pManager),
      m_Service(other.m_Service),
      m_pHandler(other.m_pHandler),
      m_Generation(other.m_Generation) {
  other.m_pManager = nullptr;
  other.m_Service = serviceEnd;
  other.m_pHandler = nullptr;
  other.m_Generation = 0;
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
    m_pHandler = other.m_pHandler;
    m_Generation = other.m_Generation;
    other.m_pManager = nullptr;
    other.m_Service = serviceEnd;
    other.m_pHandler = nullptr;
    other.m_Generation = 0;
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
  m_pHandler = nullptr;
  m_Generation = 0;
  return true;
}

SyscallManager::HandlerLease::HandlerLease()
    : m_pManager(nullptr),
      m_pSlot(nullptr),
      m_pHandler(nullptr),
      m_Generation(0),
      m_pThread(nullptr),
      m_Cleanup(),
      m_Dispatch{nullptr, nullptr, 0, 0, nullptr, nullptr} {}

SyscallManager::HandlerLease::~HandlerLease() {
  if (m_pManager) {
    m_pManager->releaseHandler(*this, true);
  }
}

SyscallManager::SyscallManager()
    : m_HandlerLock(),
      m_HandlerSlots(),
      m_NextDispatchSequence(0)
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      ,
      m_HandlerPinHook(nullptr),
      m_PostSyscallHook(nullptr)
#endif
{
}

SyscallManager::~SyscallManager() = default;

void SyscallManager::clearSlot(HandlerSlot& slot) {
  assert(!slot.inFlight);
  assert(!slot.dispatches);
  slot.handler = nullptr;
  slot.enabled = false;
  slot.draining = false;
}

void* SyscallManager::currentDispatchOwner() {
  ProcessorInformation& information = Processor::information();
  Thread* thread = information.getCurrentThread();
  return thread ? static_cast<void*>(thread) : static_cast<void*>(&information);
}

bool SyscallManager::callbackContextLocked(void* owner) const {
  for (size_t i = 0; i < serviceEnd; ++i) {
    for (HandlerDispatch* dispatch = m_HandlerSlots[i].dispatches; dispatch;
         dispatch = dispatch->next) {
      if (dispatch->owner == owner) {
        return true;
      }
    }
  }
  return false;
}

bool SyscallManager::registerHandler(Service_t service, SyscallHandler* pHandler,
                                     Registration& registration) {
  if (UNLIKELY(service >= serviceEnd) || !pHandler || registration) {
    return false;
  }

  m_HandlerLock.acquire();
  HandlerSlot& slot = m_HandlerSlots[service];

  if (slot.handler) {
    m_HandlerLock.release();
    return false;
  }

  ++slot.generation;
  if (!slot.generation) {
    ++slot.generation;
  }
  slot.handler = pHandler;
  slot.enabled = true;
  slot.draining = false;
  registration.m_pManager = this;
  registration.m_Service = service;
  registration.m_pHandler = pHandler;
  registration.m_Generation = slot.generation;
  m_HandlerLock.release();
  return true;
}

bool SyscallManager::closeHandler(Registration& registration) {
  if (registration.m_pManager != this || UNLIKELY(registration.m_Service >= serviceEnd) ||
      !registration.m_pHandler || !registration.m_Generation) {
    return false;
  }

  void* owner = currentDispatchOwner();
  m_HandlerLock.acquire();
  HandlerSlot& slot = m_HandlerSlots[registration.m_Service];
  if (slot.handler != registration.m_pHandler || slot.generation != registration.m_Generation ||
      callbackContextLocked(owner)) {
    m_HandlerLock.release();
    return false;
  }

  slot.enabled = false;
  slot.draining = true;
  m_HandlerLock.release();
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
  result = handler.handler()->syscall(state);
  return action.kind == NoPostSyscallAction;
}
#endif

bool SyscallManager::unregisterHandler(Registration& registration) {
  if (registration.m_pManager != this || UNLIKELY(registration.m_Service >= serviceEnd) ||
      !registration.m_pHandler || !registration.m_Generation) {
    return false;
  }

  Thread* current = Processor::information().getCurrentThread();
  // Spinlock acquisition changes raw IF state, so logical schedulability must
  // be captured before taking m_HandlerLock.
  const bool canYield =
      current && Processor::executionContext() == ExecutionContext::WaitableThread;
  void* owner = currentDispatchOwner();
  TerminationDeferral terminationDeferral;
  m_HandlerLock.acquire();
  HandlerSlot& slot = m_HandlerSlots[registration.m_Service];
  if (slot.handler != registration.m_pHandler || slot.generation != registration.m_Generation) {
    m_HandlerLock.release();
    return false;
  }

  const size_t targetGeneration = registration.m_Generation;
  if (!slot.inFlight) {
    clearSlot(slot);
    m_HandlerLock.release();
    return true;
  }

  // A callback draining any handler can form a reciprocal wait with that
  // handler. Keep token ownership live for an ordinary external drain.
  if (callbackContextLocked(owner)) {
    m_HandlerLock.release();
    return false;
  }

  if (!canYield) {
    // Waiting without a schedulable thread cannot make progress. Keep the
    // registration live so false never authorises caller teardown.
    m_HandlerLock.release();
    return false;
  }

  slot.enabled = false;
  slot.draining = true;
  m_HandlerLock.release();

  while (true) {
    auto waitGuard = slot.drainWaiters.acquire();
    m_HandlerLock.acquire();

    if (slot.generation != targetGeneration) {
      m_HandlerLock.release();
      return false;
    }

    if (!slot.inFlight) {
      if (slot.handler) {
        assert(slot.draining);
        clearSlot(slot);
      }
      m_HandlerLock.release();
      return true;
    }
    m_HandlerLock.release();

    const WaitQueue::WakeReason reason =
        waitGuard.waitForCompletion(WaitQueue::Channel(&slot), Thread::CallbackDrain,
                                    reinterpret_cast<uintptr_t>(slot.handler));
    (void)reason;
  }
}

bool SyscallManager::acquireHandler(Service_t service, HandlerLease& lease,
                                    PostSyscallAction& action) {
  if (UNLIKELY(service >= serviceEnd) || lease.m_pManager) {
    return false;
  }

  Thread* thread = Processor::information().getCurrentThread();
  if (thread) {
    thread->armStateCleanup(lease.m_Cleanup, abandonedHandlerCleanup, &lease);
  }

  m_HandlerLock.acquire();
  HandlerSlot& slot = m_HandlerSlots[service];
  if (!slot.handler || !slot.enabled) {
    m_HandlerLock.release();
    if (thread) {
      thread->disarmStateCleanup(lease.m_Cleanup);
    }
    return false;
  }

  ++m_NextDispatchSequence;
  if (!m_NextDispatchSequence) {
    ++m_NextDispatchSequence;
  }
  lease.m_Dispatch.owner = currentDispatchOwner();
  lease.m_Dispatch.thread = thread;
  lease.m_Dispatch.stateLevel = thread ? thread->getStateLevel() : 0;
  lease.m_Dispatch.sequence = m_NextDispatchSequence;
  lease.m_Dispatch.action = &action;
  lease.m_Dispatch.next = slot.dispatches;

  lease.m_pManager = this;
  lease.m_pSlot = &slot;
  lease.m_pHandler = slot.handler;
  lease.m_Generation = slot.generation;
  lease.m_pThread = thread;
  slot.dispatches = &lease.m_Dispatch;
  ++slot.inFlight;
  m_HandlerLock.release();

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  HandlerPinHook hook = __atomic_load_n(&m_HandlerPinHook, __ATOMIC_ACQUIRE);
  if (hook) {
    hook(service, lease.m_pHandler);
  }
#endif

  return true;
}

void SyscallManager::releaseHandler(HandlerLease& lease, bool normalReturn) {
  HandlerSlot* slot = lease.m_pSlot;
  HandlerDispatch* dispatch = &lease.m_Dispatch;
  bool wakeDrainers = false;

  m_HandlerLock.acquire();
  if (!slot) {
    m_HandlerLock.release();
    return;
  }
  assert(slot);
  assert(slot->generation == lease.m_Generation);
  if (normalReturn && lease.m_pThread) {
    // Keep admission pinned until teardown can no longer detach the
    // stack record. The manager lock disables the nonlocal interrupt
    // window between these two ownership transitions.
    lease.m_pThread->disarmStateCleanup(lease.m_Cleanup);
  }
  HandlerDispatch** link = &slot->dispatches;
  while (*link && *link != dispatch) {
    link = &(*link)->next;
  }
  assert(*link == dispatch);
  *link = dispatch->next;
  assert(slot->inFlight);
  --slot->inFlight;
  wakeDrainers = !slot->inFlight && slot->draining;

  lease.m_pManager = nullptr;
  lease.m_pSlot = nullptr;
  lease.m_pHandler = nullptr;
  lease.m_Generation = 0;
  lease.m_pThread = nullptr;
  lease.m_Dispatch = {nullptr, nullptr, 0, 0, nullptr, nullptr};
  m_HandlerLock.release();

  if (wakeDrainers) {
    slot->drainWaiters.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(slot));
  }
}

void SyscallManager::abandonedHandlerCleanup(void* context) {
  HandlerLease* lease = reinterpret_cast<HandlerLease*>(context);
  if (lease && lease->m_pManager) {
    lease->m_pManager->releaseHandler(*lease, false);
  }
}

bool SyscallManager::requestPostSyscallAction(PostSyscallActionKind kind, intptr_t value,
                                              const ProcessorState* state) {
  void* owner = currentDispatchOwner();
  HandlerDispatch* target = nullptr;

  m_HandlerLock.acquire();
  for (size_t i = 0; i < serviceEnd; ++i) {
    for (HandlerDispatch* dispatch = m_HandlerSlots[i].dispatches; dispatch;
         dispatch = dispatch->next) {
      if (dispatch->owner == owner && (!target || dispatch->sequence > target->sequence)) {
        target = dispatch;
      }
    }
  }

  if (!target || !target->action || target->action->kind != NoPostSyscallAction) {
    m_HandlerLock.release();
    return false;
  }
  if ((kind == ReturnFromEvent || kind == PopEventState) && !target->stateLevel) {
    m_HandlerLock.release();
    return false;
  }

  target->action->kind = kind;
  target->action->value = value;
  if (state) {
    target->action->state = *state;
  }
  m_HandlerLock.release();
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

bool SyscallManager::requestReboot() {
  return requestPostSyscallAction(RebootSystem, 0);
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
  return hook && hook(action.kind, action.value);
}
#endif
