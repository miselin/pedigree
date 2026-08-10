/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_MACHINE_SPLITIRQHANDLER_H
#define PEDIGREE_KERNEL_MACHINE_SPLITIRQHANDLER_H

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/ThreadedIrqDispatcher.h"
#include "pedigree/kernel/machine/types.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"

class Device;
class IrqManager;
class IrqPolicy;
class String;

/**
 * Owns a coalescing worker for an IRQ handler.
 *
 * The hard callback is limited to claiming and quiescing the device source,
 * capturing any data which cannot wait, and returning Deferred with a work
 * bitmask. The worker receives only that bitmask: InterruptState never escapes
 * the hard callback. If exact event counts matter, the hard callback must put
 * them in a preallocated device-owned ring and the worker must drain it.
 *
 * A Deferred result means the device source is already safe for the machine
 * IRQ line to remain enabled. Level-triggered devices must mask or acknowledge
 * their own source in hardIrq(). threadedIrq() drains the source but must not
 * unmask it; the base calls rearmIrqSources() only while rearm is serialised
 * against shutdown.
 */
class EXPORTED_PUBLIC SplitIrqHandler : private HardIrqHandler {
 public:
  enum class HardStageDisposition : size_t {
    NotHandled,
    Handled,
    Deferred,
  };

 protected:
  explicit SplitIrqHandler(const String& name);
  virtual ~SplitIrqHandler();

  /** Starts the owned worker before any device IRQ source is enabled. */
  bool initialiseSplitIrq();

  /** Registers this handler after its owned worker is running. */
  irq_id_t registerIsaSplitIrq(IrqManager& manager, uint8_t irq, const IrqPolicy& policy);
  irq_id_t registerPciSplitIrq(IrqManager& manager, Device& device, const IrqPolicy& policy);

  /**
   * Quiesces the hardware, closes every hard callback, drains accepted work,
   * then joins the worker.
   *
   * The derived owner must call this while its fields and the scheduler are
   * alive. Calls from hard/atomic context or the owned worker are rejected
   * before any lifecycle state changes.
   */
  bool shutdownSplitIrq();

  /** Runs in hard IRQ context and must be bounded and nonblocking. */
  virtual HardStageDisposition hardIrq(irq_id_t number, InterruptState& state, size_t& work) = 0;

  /**
   * Runs in ordinary thread context and may use blocking APIs. The owned
   * worker normally calls it; teardown may call it synchronously after the
   * worker is joined to drain a rejected publication. It must not re-enable
   * an interrupt source.
   */
  virtual void threadedIrq(size_t work) = 0;

  /**
   * Prevents new device interrupts before registry admission is closed.
   * This runs in thread context and must be idempotent across failed retries.
   */
  virtual bool quiesceIrqSources() = 0;

  /**
   * Rearms drained device sources unless shutdown has begun.
   *
   * This runs in thread context with interrupts disabled and is serialised
   * against quiesceIrqSources(). It must be bounded and nonblocking.
   */
  virtual void rearmIrqSources(size_t work) = 0;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  using RegistrationPublishedHook = void (*)(SplitIrqHandler*);

  HardIrqHandler* hardHandlerForTest();
  void setRegistrationPublishedHookForTest(RegistrationPublishedHook hook);
  size_t publicationFailuresForTest() const;
  size_t pendingWorkForTest() const;
  size_t deferredIrqsForTest() const;
  size_t completedBatchesForTest() const;
  bool publishWorkForTest(size_t work);
  void rejectNextPublicationForTest();
#endif

 private:
  static constexpr size_t MaxRegistrations = 16;

  struct Registration {
    IrqManager* manager = nullptr;
    irq_id_t id = 0;
  };

  ::HardIrqDisposition irq(irq_id_t number, InterruptState& state) final;
  static void dispatchThreaded(void* context, uint8_t line, size_t cookie);
  void dispatchThreaded();

  bool publishWork(size_t work);

  Registration m_Registrations[MaxRegistrations];
  size_t m_RegistrationCount;
  Atomic<size_t> m_LifecycleBusy;
  Atomic<size_t> m_AcceptingRegistrations;
  ThreadedIrqDispatcher m_Dispatcher;
  Spinlock m_StateLock;
  bool m_Quiescing;
  Atomic<size_t> m_Stopping;
  Atomic<size_t> m_PublicationFailures;
  Atomic<size_t> m_DeferredIrqs;
  Atomic<size_t> m_CompletedBatches;
  size_t m_PendingWork;
  bool m_Started;
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  RegistrationPublishedHook m_RegistrationPublishedHook;
#endif

  SplitIrqHandler(const SplitIrqHandler&) = delete;
  SplitIrqHandler& operator=(const SplitIrqHandler&) = delete;
};

#endif
