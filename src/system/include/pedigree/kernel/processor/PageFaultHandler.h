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

#ifndef KERNEL_CORE_PROCESSOR_PAGEFAULTHANDLER_H_
#define KERNEL_CORE_PROCESSOR_PAGEFAULTHANDLER_H_
#include <config.h>

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/AtomicStateCleanup.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/processor/InterruptHandler.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"

/** @addtogroup kernelprocessor
 * @{ */

class Thread;

class EXPORTED_PUBLIC MemoryTrapHandler {
 public:
  virtual ~MemoryTrapHandler();

  /** Trap event handler.
      \param address The address of the trap.
      \param bIsWrite True if the trap was caused by a write, false if by a
     read. \return True if the trap was handled successfully (and the handler
     can return), or false if another handler needs to be tried. */
  virtual bool trap(InterruptState& state, uintptr_t address, bool bIsWrite) = 0;
};

/** The x86 Page Fault Exception handler. */
class PageFaultHandler : private InterruptHandler {
 public:
  /** Get the PageFaultHandler instance
   *  \return the PageFaultHandler instance.  */
  inline static PageFaultHandler& instance() {
    return m_Instance;
  }

  /** Register the PageFaultHandler with the InterruptManager.
   * \return true if sucessful, false otherwise.  */
  bool initialise();

  /**
   * Registers a trap handler.
   *
   * Duplicate handlers and registrations beyond the fixed registry capacity
   * are rejected.
   */
  EXPORTED_PUBLIC bool registerHandler(MemoryTrapHandler* pHandler);

  /**
   * Stops new callbacks and waits for callbacks which already pinned the
   * handler to return.
   *
   * A handler cannot synchronously drain its own active callback. Such a
   * request returns false and retires the handler when that callback returns.
   * A synchronous caller must not retain a resource which the active handler
   * needs to finish.
   */
  EXPORTED_PUBLIC bool unregisterHandler(MemoryTrapHandler* pHandler);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  using HandlerPinHook = void (*)(MemoryTrapHandler*);
  using HandlerPrePinHook = void (*)(MemoryTrapHandler*);
  using AtomicDrainHook = void (*)(MemoryTrapHandler*);
  using MutationLockHook = void (*)();

  /** Installs a deterministic observer after a handler has been pinned. */
  static EXPORTED_PUBLIC void setHandlerPinHook(HandlerPinHook hook);

  /** Installs a deterministic observer before callback admission commits. */
  static EXPORTED_PUBLIC void setHandlerPrePinHook(HandlerPrePinHook hook);

  /** Observes atomic admission close before its active-hazard scan. */
  static EXPORTED_PUBLIC void setAtomicDrainHook(AtomicDrainHook hook);

  /** Runs a hook while the registry's mutation lock is held. */
  EXPORTED_PUBLIC void withMutationLockForTest(MutationLockHook hook);

  /** Dispatches only the given handler through the production pin path. */
  EXPORTED_PUBLIC bool dispatchHandlerForTest(MemoryTrapHandler* pHandler);

  /** Counts committed callback hazards for a handler. */
  EXPORTED_PUBLIC size_t activeDispatchCountForTest(MemoryTrapHandler* pHandler);
#endif

  //
  // InterruptHandler interface.
  //
  virtual void interrupt(size_t interruptNumber, InterruptState& state);

 private:
  /** The default constructor.  */
  PageFaultHandler() INITIALISATION_ONLY;

  /**The copy constructor.
   * Note not implemented.  */
  PageFaultHandler(const PageFaultHandler&);

  bool dispatchHandlers(InterruptState& state, uintptr_t address, bool bIsWrite,
                        MemoryTrapHandler* pOnlyHandler = nullptr);

  static const size_t MaxMemoryTrapHandlers = 16;
  static const size_t MaxActiveDispatches = 64;

  enum class SlotMode : size_t {
    Empty = 0,
    Enabled,
    Draining,
    Deferred,
    Retiring,
  };

  static const size_t ModeBits = 3;
  static const size_t ModeMask = (1 << ModeBits) - 1;
  static const size_t GenerationShift = ModeBits;

  struct HandlerSlot;

  struct ActiveDispatch {
    ActiveDispatch() : token(nullptr), generation(0), owner(nullptr), slot(nullptr) {}

    void* token;
    size_t generation;
    void* owner;
    HandlerSlot* slot;
  };

  struct HandlerSlot {
    HandlerSlot() : handler(nullptr), publication(0) {}

    MemoryTrapHandler* handler;
    size_t publication;
  };

  struct DispatchCleanup {
    explicit DispatchCleanup(PageFaultHandler* registry) : registry(registry), cleanup() {}

    PageFaultHandler* registry;
    AtomicStateCleanupRecord cleanup;
  };

  static size_t makePublication(size_t generation, SlotMode mode);
  static size_t generationOf(size_t publication);
  static SlotMode modeOf(size_t publication);

  bool retireSlot(HandlerSlot& slot, size_t expectedPublication,
                  MemoryTrapHandler* expectedHandler);
  bool publishDispatch(HandlerSlot& slot, void* owner, void* token);
  void unpublishDispatch(void* token);
  static void abandonedHandlerCleanup(void* context);
  bool hasActiveDispatch(HandlerSlot& slot) const;
  bool findCurrentDispatch(void* owner, HandlerSlot* target, bool& callbackContext) const;
  static void* currentDispatchOwner();

  /** Fixed storage keeps the fault path allocation-free. */
  HandlerSlot m_Handlers[MaxMemoryTrapHandlers];
  ActiveDispatch m_ActiveDispatches[MaxActiveDispatches];
  WaitQueue m_DispatchWaiters;
  Spinlock m_HandlerLock;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  static HandlerPinHook m_HandlerPinHook;
  static HandlerPrePinHook m_HandlerPrePinHook;
  static AtomicDrainHook m_AtomicDrainHook;
#endif

  /** The PageFaultHandler instance */
  EXPORTED_PUBLIC static PageFaultHandler m_Instance;
};

/** @} */

#endif /* KERNEL_CORE_PROCESSOR_X86_PAGEFAULTHANDLER_H_ */
