/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_MACHINE_THREADEDIRQDISPATCHER_H
#define PEDIGREE_KERNEL_MACHINE_THREADEDIRQDISPATCHER_H

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/String.h"

class Thread;
class PerProcessorScheduler;
class Pic;
struct IrqLineDiagnosticSnapshot;

/**
 * Owns one coalescing worker for each configured IRQ or controller work lane.
 *
 * The controller performs the trigger-specific hard-stage work before
 * publishing a monotonically increasing, nonzero cookie. The worker invokes
 * the callback in ordinary thread context. Interrupted machine state is never
 * retained or exposed to the callback. Publication records atomics, rings the
 * worker scheduler's doorbell, and may issue a bounded directed prompt; it
 * never wakes a sleeping thread from hard context.
 */
class EXPORTED_PUBLIC ThreadedIrqDispatcher {
 public:
  static constexpr size_t MaxLines = 17;
  using DispatchCallback = void (*)(void*, uint8_t, size_t);
  /**
   * Prompts the worker processor after a remote producer has made a batch
   * visible. The callback must be bounded, hard-IRQ safe, and must not
   * attempt to roll back the already-published cookie on failure.
   */
  using RemoteWakeCallback = bool (*)(void*, uint8_t, size_t);

  ThreadedIrqDispatcher(const String& name, size_t lineCount, DispatchCallback callback,
                        void* callbackContext);
  ~ThreadedIrqDispatcher();

  /** Starts every stable per-line worker after scheduler initialisation. */
  bool initialise();

  /** Rejects new publications and joins every worker. */
  bool shutdown();

  /** Whether the current context can synchronously join worker threads. */
  bool canShutdown() const;

  bool isInitialised() const;

  /** True when called by one of this dispatcher's callback workers. */
  bool isCurrentWorker() const;

  /**
   * Marks a line and rings its owning scheduler in one bounded operation.
   * Concurrent or nested producers are coalesced by cookie generation. A
   * constant cookie is safe for multi-producer work-bit users. Callers must
   * enter through a hard/atomic source which serialises same-CPU execution;
   * a nested publication can only occur after the outer cookie is stored.
   */
  bool publishFromInterrupt(uint8_t line, size_t cookie);

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  using PublicationObservedHook = void (*)(ThreadedIrqDispatcher*, uint8_t, size_t, size_t);
  using PendingScanAdmittedHook = void (*)(ThreadedIrqDispatcher*, uint8_t);

  /** Runs after the test publisher has atomically stored its cookie. */
  void setPublicationObservedHookForTest(PublicationObservedHook hook);

  /** Overrides the per-CPU slot count before workers are started. */
  bool setPendingSlotCountForTest(size_t slotCount);

  /** Publishes through a selected synthetic per-CPU slot. */
  bool publishFromSlotForTest(uint8_t line, size_t slot, size_t cookie);

  /** Rejects exactly the next otherwise-valid hard publication. */
  void rejectNextPublicationForTest();

  /** Runs after a diagnostic scan has joined shutdown admission. */
  void setPendingScanAdmittedHookForTest(PendingScanAdmittedHook hook);

  /** Counts attempts rejected by an explicit local-only policy. */
  size_t remotePublicationRejectionsForTest() const {
    return __atomic_load_n(&m_RemotePublicationRejectionsForTest, __ATOMIC_ACQUIRE);
  }

  /** Hosted-only fake prompt injection for the dispatcher race models. */
  MUST_USE_RESULT bool setRemoteWakeCallbackForTest(RemoteWakeCallback callback,
                                                    void* callbackContext) {
    // Hosted smoke modules are dynamically loaded, so keep this setup
    // hook inline rather than adding a production-exported test symbol.
    m_ConfigurationLock.acquire();
    if (__atomic_load_n(&m_ConfigurationClosed, __ATOMIC_ACQUIRE)) {
      m_ConfigurationLock.release();
      return false;
    }

    m_RemoteWakeCallback = callback;
    m_RemoteWakeCallbackContext = callbackContext;
    m_ConfigurationLock.release();
    return true;
  }
#endif

  /** Whether an occurrence arrived after the worker claimed its batch. */
  bool hasPending(uint8_t line) const;

  /** Lock-free diagnostic state; invalid lines return zero/false. */
  size_t pendingCookie(uint8_t line) const;
  size_t activeCookie(uint8_t line) const;
  size_t completedBatches(uint8_t line) const;
  size_t completedCookie(uint8_t line) const;
  uintptr_t workerIdentity(uint8_t line) const;
  bool callbackActive(uint8_t line) const;
  bool publicationClosed(uint8_t line) const;

  /**
   * Copies lock-free timing and detached worker state for the debugger.
   * Live fields are sampled independently; stopped-world use is authoritative.
   */
  void snapshotDiagnostics(uint8_t line, IrqLineDiagnosticSnapshot& snapshot) const;

 private:
  friend class Pic;

  /**
   * PIC is the sole production client allowed to request a directed worker
   * prompt. Generic dispatchers deliberately remain local-only so an opaque
   * callback cannot smuggle a blocking operation into hard IRQ context.
   */
  MUST_USE_RESULT bool setRemoteWakeCallback(RemoteWakeCallback callback, void* callbackContext);

  class Line {
   public:
    Line();
    ~Line();

    void configure(ThreadedIrqDispatcher* owner, uint8_t line, DispatchCallback callback,
                   void* callbackContext);
    bool start();
    void beginStop();
    bool join();
    bool publishFromInterrupt(size_t cookie);
    bool hasPending() const;
    bool isWorker(const Thread* thread) const;
    size_t pendingCookie() const;
    size_t activeCookie() const;
    size_t completedBatches() const;
    size_t completedCookie() const;
    uintptr_t workerIdentity() const;
    bool callbackActive() const;
    bool publicationClosed() const;
    void snapshotDiagnostics(IrqLineDiagnosticSnapshot& snapshot) const;

   private:
    static constexpr size_t PublicationClosed = static_cast<size_t>(1)
                                                << ((sizeof(size_t) * 8) - 1);
    static constexpr size_t PublicationCountMask = ~PublicationClosed;

    static int workerEntry(void* context);
    static bool workerReady(void* context);
    int run();
    bool hasPendingForWorker() const;
    size_t pendingCookieForWorker() const;
    size_t takePendingCookie();
    static bool generationReached(size_t current, size_t target);

    ThreadedIrqDispatcher* m_Owner;
    DispatchCallback m_Callback;
    void* m_CallbackContext;
    Thread* m_Thread;
    PerProcessorScheduler* m_Scheduler;
    size_t m_WorkerProcessor;
    uint8_t m_Line;
    size_t* m_PendingCookies;
    size_t m_PendingCookieCount;
    size_t m_ActiveCookie;
    size_t m_CallbackActive;
    mutable size_t m_PublicationState;
    size_t m_Started;
    size_t m_CompletedBatches;
    size_t m_CompletedCookie;
    size_t m_PendingSinceTimestamp;
    size_t m_ActiveCallbackStartedTimestamp;
    size_t m_LastWakeLatency;
    size_t m_MaximumWakeLatency;
    size_t m_LastCallbackRuntime;
    size_t m_MaximumCallbackRuntime;

    Line(const Line&) = delete;
    Line& operator=(const Line&) = delete;
  };

  Line m_Lines[MaxLines];
  /** Inline so global interrupt-controller constructors cannot enter the
   * heap. */
  NormalStaticString m_Name;
  size_t m_LineCount;
  DispatchCallback m_Callback;
  void* m_CallbackContext;
  /** Serialises normal-context callback configuration with initialise(). */
  Spinlock m_ConfigurationLock;
  /** Immutable after initialise(): null intentionally means local-only. */
  RemoteWakeCallback m_RemoteWakeCallback;
  void* m_RemoteWakeCallbackContext;
  /** Closes callback configuration before the first worker is created. */
  size_t m_ConfigurationClosed;
  size_t m_Initialised;
  /** Exactly one normal-context caller may join and release workers. */
  size_t m_ShutdownClaimed;
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  PublicationObservedHook m_PublicationObservedHook;
  PendingScanAdmittedHook m_PendingScanAdmittedHook;
  size_t m_PendingSlotCountForTest;
  size_t m_PublicationSlotForTest;
  size_t m_RejectNextPublicationForTest;
  size_t m_RemotePublicationRejectionsForTest;
#endif

  ThreadedIrqDispatcher(const ThreadedIrqDispatcher&) = delete;
  ThreadedIrqDispatcher& operator=(const ThreadedIrqDispatcher&) = delete;
};

#endif
