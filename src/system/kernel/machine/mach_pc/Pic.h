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

#ifndef KERNEL_MACHINE_X86_COMMON_PIC_H
#define KERNEL_MACHINE_X86_COMMON_PIC_H
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/IrqDiagnosticSnapshotStore.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/IrqHandlerRegistry.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/ThreadedIrqDispatcher.h"
#include "pedigree/kernel/machine/types.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/processor/InterruptHandler.h"
#include "pedigree/kernel/processor/IoPort.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/new"

#include <config.h>

#include "PicIrqState.h"

class Device;
class HardIrqHandler;
class IrqHandler;
class IrqHandlerBase;
class SchedulerIrqHandler;

/** @addtogroup kernelmachinex86common
 * @{ */

/** The x86/x64 programmable interrupt controller as IrqManager */
class Pic : public IrqManager, private InterruptHandler {
 public:
  /** Get the Pic class instance
   *\return the Pic class instance */
  inline static Pic& instance() {
    return m_Instance;
  }

  //
  // IrqManager interface
  //
  virtual irq_id_t registerIsaIrqHandler(uint8_t irq, IrqHandler* handler, const IrqPolicy& policy);
  virtual irq_id_t registerPciIrqHandler(IrqHandler* handler, Device* pDevice,
                                         const IrqPolicy& policy);
  virtual irq_id_t registerHardIsaIrqHandler(uint8_t irq, HardIrqHandler* handler,
                                             const IrqPolicy& policy);
  virtual irq_id_t registerHardPciIrqHandler(HardIrqHandler* handler, Device* pDevice,
                                             const IrqPolicy& policy);
  virtual irq_id_t registerSchedulerIrqHandler(uint8_t irq, SchedulerIrqHandler* handler,
                                               const IrqPolicy& policy);
  virtual bool unregisterSchedulerIrqHandler(irq_id_t Id, SchedulerIrqHandler* handler);
  virtual bool unregisterHandler(irq_id_t Id, IrqHandlerBase* handler);
  virtual size_t snapshotIrqLines(IrqLineDiagnosticSnapshot* out, size_t capacity) const;

  /** Initialises the PIC hardware and registers the interrupts with the
   *  InterruptManager.
   *\return true, if successfull, false otherwise */
  bool initialise() INITIALISATION_ONLY;

  /** Starts manager-owned bottom-half workers after scheduler startup. */
  bool initialiseThreaded();

  /** Masks all lines, cancels queued batches, then joins every worker. */
  bool shutdownThreaded();

  /** Called every millisecond, typically handles IRQ mitigation. */
  virtual void tick();

  /** Controls specific elements of a given IRQ */
  virtual bool control(uint8_t irq, ControlCode code, size_t argument);

 private:
  class StateGuard;
  class HardStateGuard;

  /** The default constructor */
  Pic() INITIALISATION_ONLY;
  /** The destructor */
  inline virtual ~Pic() {}
  /** The copy-constructor
   *\note NOT implemented */
  Pic(const Pic&);
  /** The assignment operator
   *\note NOT implemented */
  Pic& operator=(const Pic&);

  //
  // InterruptHandler interface
  //
  virtual void interrupt(size_t interruptNumber, InterruptState& state);

  static void dispatchThreadedLine(void* context, uint8_t irq, size_t cookie);
#if APIC
  static bool promptThreadedWorker(void* context, uint8_t line, size_t workerProcessor);
#endif
  static constexpr uint8_t ControllerWorkLine = PicIrqState::LineCount;
  bool drainOnePendingControllerBatchLocked();
  void drainPendingControllerActionsLocked();
  void releaseControllerState();
  void releaseControllerStateFromInterrupt();
  bool handControllerStateToWorker();
  void finishDeferredLineTransitionsLocked();
  void clearControllerTemporaryMaskLocked(uint16_t restoreMask = 0xFFFF);
  uint16_t effectiveMaskLocked() const;
  void beginLineTransitionLocked(uint8_t irq);
  void finishLineTransitionLocked(uint8_t irq);
  void finishHandlerUnregisterLocked(uint8_t irq, IrqHandlerRegistry::UnregisterResult result,
                                     IrqHandlerRegistry::LineMode removedDelivery);
  void admitThreadedOccurrenceLocked(uint8_t irq);
  void finishHardDispatchLocked(const PicHardTailRecord& record);
  void finishHardDispatchFromInterrupt(const PicHardTailRecord& record);
  size_t advanceThreadedCookieLocked(uint8_t irq);
  void publishDiagnosticLineLocked(uint8_t irq);
  void publishAllDiagnosticLinesLocked();

  enum FailClosedReason : uint8_t {
    HardHandoffQuarantine = 1U << 0,
    ThreadedPublicationQuarantine = 1U << 1,
    UnhandledQuarantine = 1U << 2,
    ControllerContentionQuarantine = 1U << 3,
  };

  void eoiLocked(uint8_t irq);
  void applyMaskLocked();
  void setEnabledLocked(uint8_t irq, bool enable);
  void enable(uint8_t irq, bool enable);
  void enableAll(bool enable);

  /** Handle a potentially-spurious IRQ while the PIC lock is held. */
  bool spuriousLocked(size_t irq);

  /** The slave PIC I/O Port range */
  IoPort m_SlavePort;
  /** The master PIC I/O Port range */
  IoPort m_MasterPort;

  /** IRQ handlers and their callback lifetime state. */
  IrqHandlerRegistry m_Handlers;
  /** Dedicated IRQ0 callback which may abandon its interrupt frame. */
  SchedulerIrqHandler* m_SchedulerIrqHandler;
  /** Trigger mode, registration ownership and the complete 16-bit mask. */
  PicIrqState m_IrqState;
  /** Non-blocking hard-entry ownership and terminal-action handoff. */
  PicControllerStateGate m_ControllerStateGate;
  /** Complete hard results which could not immediately reclaim ownership. */
  PicHardTailQueue m_HardTailQueue;
  /** Sleeps competing threads so a preempted gate owner can run again. */
  Mutex m_ControllerThreadMutex;
  /** Stable per-line workers plus one controller-continuation worker. */
  ThreadedIrqDispatcher m_ThreadedDispatcher;
  /** Invalidates queued work when a physical line changes ownership. */
  size_t m_ThreadedCookies[PicIrqState::LineCount];
  /** PIC dispatch generation associated with each queued cookie. */
  size_t m_ThreadedDispatchGenerations[PicIrqState::LineCount];
  /** Invalidates an old hard result after its final callback lifetime ends. */
  size_t m_HardStageGenerations[PicIrqState::LineCount];
  /** Hard-stage outcome folded into the matching mixed worker batch. */
  bool m_ThreadedHadHardStage[PicIrqState::LineCount];
  bool m_ThreadedHardAdmitted[PicIrqState::LineCount];
  HardIrqDisposition m_ThreadedHardDisposition[PicIrqState::LineCount];
  bool m_ThreadedHardVetoRecovery[PicIrqState::LineCount];
  size_t m_ThreadedHardVetoRecoveryGenerations[PicIrqState::LineCount];
  /** Reasons which must remain masked until their owning lifetime ends. */
  uint8_t m_FailClosedReasons[PicIrqState::LineCount];
  /** Atomic diagnostics for work rejected after dispatcher closure. */
  size_t m_ThreadedPublicationFailures[PicIrqState::LineCount];
  size_t m_RemovalRejections[PicIrqState::LineCount];
  /** Hard entries/tails which could not immediately own controller state. */
  size_t m_ControllerContentions[PicIrqState::LineCount];
  /** Lock-free controller-worker prompt state for stopped-world diagnosis. */
  size_t m_ControllerPromptAttempts;
  size_t m_ControllerPromptFailures;
  size_t m_ControllerPromptDestination;
  size_t m_ControllerPromptState;
  /** Physical-only masks retained until the BSP can release atomically. */
  uint16_t m_ControllerTemporaryMask;
  /** Last mask written to hardware, including physical-only overrides. */
  uint16_t m_AppliedControllerMask;
  /** Lifetimes published by the BSP immediately before physical unmask. */
  uint16_t m_DeferredLineTransitions;
  /** Stable BSP index rebound after processor topology initialisation. */
  size_t m_DeliveryProcessor;
  /** Physical LAPIC destination captured with the BSP worker scheduler. */
  uint8_t m_DeliveryApicId;
  /** Per-line immutable diagnostic publications. */
  IrqDiagnosticSnapshotStore<PicIrqState::LineCount> m_Diagnostics;
  /** Unregister operations which have not completed line accounting. */
  size_t m_UnregisterReservations[PicIrqState::LineCount];
  /** Closes registration and re-enable paths before worker shutdown. */
  bool m_ShuttingDown;
  /** IRQ counts for given handlers */
  size_t m_IrqCount[16];
  /** Architecturally spurious or disabled in-flight occurrences. */
  size_t m_SpuriousIrqCount[16];
  /** Occurrences for which no callback accepted ownership. */
  size_t m_UnhandledIrqCount[16];
  /** Mitigated IRQs */
  bool m_MitigatedIrqs[16];
  /** Mitigation thresholds */
  size_t m_MitigationThreshold[16];
  /** The Pic instance */
  static Pic m_Instance;
};

/** @} */

#endif
