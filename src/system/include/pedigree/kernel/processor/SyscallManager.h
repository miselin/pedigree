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

#ifndef KERNEL_PROCESSOR_SYSCALLMANAGER_H
#define KERNEL_PROCESSOR_SYSCALLMANAGER_H
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/process/DeferredScope.h"
#include "pedigree/kernel/processor/SyscallHandler.h"
#include "pedigree/kernel/processor/Syscalls.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/processor/types.h"

#include <config.h>

class Thread;

/** @addtogroup kernelprocessor
 * @{ */

/** The syscall manager allows syscall handler registrations and handles
 * syscalls */
class SyscallManager {
 protected:
  struct PostSyscallAction;

 private:
  struct HandlerSlot;

 public:
  using FastEntry = uintptr_t (*)(SyscallHandler*, SyscallState&);

  enum PostSyscallActionKind {
    NoPostSyscallAction,
    TerminateCurrentThread,
    ExitCurrentProcess,
    ReturnFromEvent,
    PopEventState,
    RestoreProcessorState,
    JumpToUserspace,
    RebootSystem
  };

  class EXPORTED_PUBLIC Registration {
   public:
    Registration();
    Registration(Registration&& other);
    ~Registration();

    Registration& operator=(Registration&& other);

    /** Stops new handler leases while retaining ownership for a later reset. */
    bool closeAdmission();

    bool reset();
    explicit operator bool() const {
      return m_pManager != nullptr;
    }

   private:
    friend class SyscallManager;

    Registration(const Registration&) = delete;
    Registration& operator=(const Registration&) = delete;

    SyscallManager* m_pManager;
    Service_t m_Service;
    HandlerSlot* m_pSlot;
  };

  /** Get the syscall manager instance
   *\return instance of the syscall manager */
  EXPORTED_PUBLIC static SyscallManager& instance();
  /** Register a syscall handler
   *\param[in] Service the service number you want to register
   *\param[in] pHandler the interrupt handler
   *\param[in] entry optional nonvirtual implementation of the same syscall contract
   *\return true, if successfully registered, false otherwise */
  virtual bool registerSyscallHandler(Service_t Service, SyscallHandler* pHandler,
                                      Registration& registration, FastEntry entry = nullptr) = 0;

  /** Queue a terminal operation until the active handler lease is retired. */
  EXPORTED_PUBLIC bool requestThreadExit();
  EXPORTED_PUBLIC bool requestProcessExit(int status);
  EXPORTED_PUBLIC bool requestEventReturn();
  EXPORTED_PUBLIC bool requestEventStatePop();
  EXPORTED_PUBLIC bool requestStateRestore(const ProcessorState& state);
  EXPORTED_PUBLIC bool requestUserJump(uintptr_t instructionPointer, uintptr_t stackPointer);
  EXPORTED_PUBLIC bool requestReboot(Machine::ShutdownType type = Machine::ShutdownType::Restart);

  /** Calls a syscall. */
  virtual uintptr_t syscall(Service_t service, uintptr_t function, uintptr_t p1 = 0,
                            uintptr_t p2 = 0, uintptr_t p3 = 0, uintptr_t p4 = 0,
                            uintptr_t p5 = 0) = 0;

#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
  /** Dispatch a synthetic kernel-side call without entering through SYSCALL. */
  EXPORTED_PUBLIC bool dispatchHandlerForTest(Service_t service, uintptr_t& result);
#endif

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  using HandlerPinHook = void (*)(Service_t, SyscallHandler*);
  using PostSyscallHook = bool (*)(PostSyscallActionKind, intptr_t, const ProcessorState*);

  EXPORTED_PUBLIC void setHandlerPinHook(HandlerPinHook hook);
  EXPORTED_PUBLIC void setPostSyscallHook(PostSyscallHook hook);
#endif

 protected:
  struct PostSyscallAction {
    PostSyscallAction() : kind(NoPostSyscallAction), value(0) {}

    ~PostSyscallAction() {
      if (kind == RestoreProcessorState || kind == JumpToUserspace) {
        state.~ProcessorState();
      }
    }

    PostSyscallAction(const PostSyscallAction&) = delete;
    PostSyscallAction& operator=(const PostSyscallAction&) = delete;

    PostSyscallActionKind kind;
    intptr_t value;
    // Ordinary syscalls need only the discriminator. State-bearing actions
    // copy-construct their owned payload before the handler's frame disappears.
    union {
      ProcessorState state;
    };
  };

  /**
   * Scope-bound admission to a registered handler.
   *
   * The lease must outlive every handler access. Unregistration closes
   * admission and waits for all existing leases before returning.
   */
  class HandlerLease {
   public:
    ALWAYS_INLINE HandlerLease()
        : m_pManager(nullptr), m_Cleanup(DeferredScopeRecord::Uninitialised{}) {}
    ALWAYS_INLINE ~HandlerLease() {
      if (m_pManager) {
        m_pManager->releaseHandler(*this, true);
      }
    }

   private:
    friend class SyscallManager;

    HandlerLease(const HandlerLease&) = delete;
    HandlerLease& operator=(const HandlerLease&) = delete;

    SyscallManager* m_pManager;
    Service_t m_Service;
    SyscallHandler* m_pHandler;
    FastEntry m_Entry;
    Thread* m_pThread;
    HandlerLease* m_Previous;
    PostSyscallAction* m_Action;
    DeferredScopeRecord m_Cleanup;
  };

  bool registerHandler(Service_t service, SyscallHandler* pHandler, Registration& registration,
                       FastEntry entry = nullptr);
  ALWAYS_INLINE uintptr_t dispatchHandler(const HandlerLease& lease, SyscallState& state) const {
    return lease.m_Entry(lease.m_pHandler, state);
  }
  bool closeHandler(Registration& registration);
  bool unregisterHandler(Registration& registration);
  bool acquireHandler(Service_t service, HandlerLease& lease, PostSyscallAction& action);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  bool postSyscallHookHandled(const PostSyscallAction& action);
#endif

  /** The constructor */
  SyscallManager();
  /** The destructor */
  virtual ~SyscallManager();

 private:
  static uintptr_t dispatchVirtual(SyscallHandler* handler, SyscallState& state);
  static bool inCallback();
  static void abandonedHandlerCleanup(void* context);
  bool requestPostSyscallAction(PostSyscallActionKind kind, intptr_t value,
                                const ProcessorState* state = nullptr);
  void releaseHandler(HandlerLease& lease, bool normalReturn);

  /** The copy-constructor
   *\note Not implemented (singleton) */
  SyscallManager(const SyscallManager&);
  /** The copy-constructor
   *\note Not implemented (singleton) */
  SyscallManager& operator=(const SyscallManager&);

  Spinlock m_HandlerLock;
  HandlerSlot* m_HandlerSlots[serviceEnd];
  HandlerSlot* m_Published[serviceEnd];
  static HandlerSlot m_ClosingSlot;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  HandlerPinHook m_HandlerPinHook;
  PostSyscallHook m_PostSyscallHook;
#endif
};

/** @} */

#endif
