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

#include "Pic.h"

#include "LocalApicLint0Policy.h"
#if APIC
#include "LocalApic.h"
#include "Pc.h"
#endif
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/SchedulerIrqHandler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/InterruptManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/utility.h"

#define BASE_INTERRUPT_VECTOR 0x20

// Number of IRQs in a single millisecond before an IRQ source is blocked.
// A value of 10, for example, would mean if an IRQ matches the threshold
// and sustained its output for a second, 10,000 IRQs would be triggered.
#define DEFAULT_IRQ_MITIGATE_THRESHOLD 10

Pic Pic::m_Instance;

class Pic::StateGuard {
 public:
  explicit StateGuard(Pic& pic) : m_Pic(pic), m_Owned(false), m_ThreadOwned(false) {
    const bool canWait = Processor::executionContext() == ExecutionContext::WaitableThread;
    if (!canWait) {
      // The main hard path releases its entry guard before callbacks,
      // so callback-time line replacement can claim a clean gate. An
      // atomic caller which races a thread owner must fail, not wait.
      m_Owned = m_Pic.m_ControllerStateGate.tryClaim();
      if (m_Owned) {
        m_Pic.drainPendingControllerActionsLocked();
      }
      return;
    }

    m_ThreadOwned = m_Pic.m_ControllerThreadMutex.acquire();
    if (!m_ThreadOwned) {
      return;
    }

    while (!m_Pic.m_ControllerStateGate.tryClaim()) {
      Processor::pause();
    }

    m_Owned = true;
    m_Pic.drainPendingControllerActionsLocked();
  }

  ~StateGuard() {
    if (m_Owned) {
      m_Pic.releaseControllerState();
    }
    if (m_ThreadOwned) {
      m_Pic.m_ControllerThreadMutex.release();
    }
  }

  bool owned() const {
    return m_Owned;
  }

 private:
  Pic& m_Pic;
  bool m_Owned;
  bool m_ThreadOwned;
};

class Pic::HardStateGuard {
 public:
  HardStateGuard(Pic& pic, uint8_t irq, size_t lifetime)
      : m_Pic(pic), m_Owned(pic.m_ControllerStateGate.tryAcquireClean()) {
    if (m_Owned && lifetime != PicControllerStateGate::TransitionLifetime &&
        lifetime == m_Pic.m_ControllerStateGate.currentLifetime(irq)) {
      return;
    }

    const bool claimed = m_Pic.m_ControllerStateGate.queueEntry(irq, lifetime);
    if (m_Owned) {
      m_Pic.releaseControllerStateFromInterrupt();
      m_Owned = false;
      return;
    }
    if (claimed) {
      m_Pic.releaseControllerStateFromInterrupt();
    }
  }

  ~HardStateGuard() {
    release();
  }

  bool owned() const {
    return m_Owned;
  }

  void release() {
    if (m_Owned) {
      m_Pic.releaseControllerStateFromInterrupt();
      m_Owned = false;
    }
  }

 private:
  Pic& m_Pic;
  bool m_Owned;
};

void Pic::tick() {}

bool Pic::control(uint8_t irq, ControlCode code, size_t argument) {
  if (UNLIKELY(irq >= 16))
    return false;

  StateGuard guard(*this);
  if (!guard.owned())
    return false;

  switch (code) {
    case MitigationThreshold:
      if (LIKELY(argument)) {
        if (UNLIKELY(m_IrqState.handlerCount(irq) > 1))
          m_MitigationThreshold[irq] += argument;
        else
          m_MitigationThreshold[irq] = argument;
      } else
        m_MitigationThreshold[irq] = DEFAULT_IRQ_MITIGATE_THRESHOLD;
      return true;
  }

  return false;
}

irq_id_t Pic::registerIsaIrqHandler(uint8_t irq, IrqHandler* handler, const IrqPolicy& policy) {
  if (UNLIKELY(irq >= PicIrqState::LineCount || !handler || !m_ThreadedDispatcher.isInitialised() ||
               !policy.validForThreaded() || policy.trigger() == IrqTrigger::Synthetic))
    return 0;

  StateGuard guard(*this);
  if (!guard.owned() || m_ShuttingDown || m_UnregisterReservations[irq] ||
      !m_ThreadedDispatcher.isInitialised())
    return 0;
  if (!m_IrqState.canRegister(irq, policy, IrqDelivery::Threaded)) {
    ERROR("PIC: IRQ " << Dec << irq << " was registered with incompatible trigger modes");
    return 0;
  }
  beginLineTransitionLocked(irq);
  const bool firstHandler = !m_IrqState.handlerCount(irq);
  if (!m_Handlers.registerThreadedHandler(irq, handler, policy)) {
    finishLineTransitionLocked(irq);
    return 0;
  }

  if (firstHandler) {
    advanceThreadedCookieLocked(irq);
    m_FailClosedReasons[irq] = 0;
    m_ThreadedHardVetoRecovery[irq] = false;
    m_ThreadedHardVetoRecoveryGenerations[irq] = 0;
  }
  m_IrqState.handlerRegistered(irq, policy, IrqDelivery::Threaded);
  finishLineTransitionLocked(irq);
  publishDiagnosticLineLocked(irq);

  return irq + BASE_INTERRUPT_VECTOR;
}

irq_id_t Pic::registerHardIsaIrqHandler(uint8_t irq, HardIrqHandler* handler,
                                        const IrqPolicy& policy) {
  if (UNLIKELY(irq >= PicIrqState::LineCount || !handler || !policy.validForHard() ||
               policy.trigger() == IrqTrigger::Synthetic))
    return 0;

  StateGuard guard(*this);
  if (!guard.owned() || m_ShuttingDown || m_UnregisterReservations[irq])
    return 0;
  if (!m_IrqState.canRegister(irq, policy, IrqDelivery::Hard)) {
    ERROR("PIC: IRQ " << Dec << irq << " was registered with incompatible trigger modes");
    return 0;
  }
  beginLineTransitionLocked(irq);
  const bool firstHandler = !m_IrqState.handlerCount(irq);
  if (!m_Handlers.registerHardHandler(irq, handler, policy)) {
    finishLineTransitionLocked(irq);
    return 0;
  }

  if (firstHandler) {
    advanceThreadedCookieLocked(irq);
    m_FailClosedReasons[irq] = 0;
    m_ThreadedHardVetoRecovery[irq] = false;
    m_ThreadedHardVetoRecoveryGenerations[irq] = 0;
  }
  m_IrqState.handlerRegistered(irq, policy, IrqDelivery::Hard);
  finishLineTransitionLocked(irq);
  publishDiagnosticLineLocked(irq);

  return irq + BASE_INTERRUPT_VECTOR;
}
irq_id_t Pic::registerPciIrqHandler(IrqHandler* handler, Device* pDevice, const IrqPolicy& policy) {
  if (UNLIKELY(!pDevice))
    return 0;
  irq_id_t irq = pDevice->getInterruptNumber();
  if (UNLIKELY(irq >= PicIrqState::LineCount || !handler || !m_ThreadedDispatcher.isInitialised() ||
               !policy.validForThreaded() || policy.trigger() != IrqTrigger::Level))
    return 0;

  StateGuard guard(*this);
  if (!guard.owned() || m_ShuttingDown || m_UnregisterReservations[irq] ||
      !m_ThreadedDispatcher.isInitialised())
    return 0;
  if (!m_IrqState.canRegister(irq, policy, IrqDelivery::Threaded)) {
    ERROR("PIC: PCI IRQ " << Dec << irq << " conflicts with an edge-triggered handler");
    return 0;
  }
  beginLineTransitionLocked(irq);
  const bool firstHandler = !m_IrqState.handlerCount(irq);
  if (!m_Handlers.registerThreadedHandler(irq, handler, policy)) {
    finishLineTransitionLocked(irq);
    return 0;
  }

  if (firstHandler) {
    advanceThreadedCookieLocked(irq);
    m_FailClosedReasons[irq] = 0;
    m_ThreadedHardVetoRecovery[irq] = false;
    m_ThreadedHardVetoRecoveryGenerations[irq] = 0;
  }
  m_IrqState.handlerRegistered(irq, policy, IrqDelivery::Threaded);
  finishLineTransitionLocked(irq);
  publishDiagnosticLineLocked(irq);

  return irq + BASE_INTERRUPT_VECTOR;
}

irq_id_t Pic::registerHardPciIrqHandler(HardIrqHandler* handler, Device* pDevice,
                                        const IrqPolicy& policy) {
  if (UNLIKELY(!pDevice))
    return 0;
  irq_id_t irq = pDevice->getInterruptNumber();
  if (UNLIKELY(irq >= PicIrqState::LineCount || !handler || !policy.validForHard() ||
               policy.trigger() != IrqTrigger::Level))
    return 0;

  StateGuard guard(*this);
  if (!guard.owned() || m_ShuttingDown || m_UnregisterReservations[irq])
    return 0;
  if (!m_IrqState.canRegister(irq, policy, IrqDelivery::Hard)) {
    ERROR("PIC: PCI IRQ " << Dec << irq << " conflicts with an edge-triggered handler");
    return 0;
  }
  beginLineTransitionLocked(irq);
  const bool firstHandler = !m_IrqState.handlerCount(irq);
  if (!m_Handlers.registerHardHandler(irq, handler, policy)) {
    finishLineTransitionLocked(irq);
    return 0;
  }

  if (firstHandler) {
    advanceThreadedCookieLocked(irq);
    m_FailClosedReasons[irq] = 0;
    m_ThreadedHardVetoRecovery[irq] = false;
    m_ThreadedHardVetoRecoveryGenerations[irq] = 0;
  }
  m_IrqState.handlerRegistered(irq, policy, IrqDelivery::Hard);
  finishLineTransitionLocked(irq);
  publishDiagnosticLineLocked(irq);

  return irq + BASE_INTERRUPT_VECTOR;
}

irq_id_t Pic::registerSchedulerIrqHandler(uint8_t irq, SchedulerIrqHandler* handler,
                                          const IrqPolicy& policy) {
  if (irq >= PicIrqState::LineCount || !handler)
    return 0;

  StateGuard guard(*this);
  if (!guard.owned() || m_ShuttingDown || m_UnregisterReservations[irq] ||
      !m_IrqState.canRegisterScheduler(irq, policy))
    return 0;

  beginLineTransitionLocked(irq);
  m_SchedulerIrqHandler = handler;
  m_IrqState.schedulerRegistered(irq, policy);
  finishLineTransitionLocked(irq);
  publishDiagnosticLineLocked(irq);
  return irq + BASE_INTERRUPT_VECTOR;
}

bool Pic::unregisterSchedulerIrqHandler(irq_id_t Id, SchedulerIrqHandler* handler) {
  if (Id != BASE_INTERRUPT_VECTOR || !handler)
    return false;

  StateGuard guard(*this);
  if (!guard.owned() || m_SchedulerIrqHandler != handler || !m_IrqState.schedulerRegistered(0))
    return false;

  beginLineTransitionLocked(0);
  m_SchedulerIrqHandler = nullptr;
  m_IrqState.schedulerUnregistered(0);
  finishLineTransitionLocked(0);
  publishDiagnosticLineLocked(0);
  return true;
}

void Pic::finishHandlerUnregisterLocked(uint8_t irq, IrqHandlerRegistry::UnregisterResult result,
                                        IrqHandlerRegistry::LineMode removedDelivery) {
  assert(m_UnregisterReservations[irq]);
  --m_UnregisterReservations[irq];
  if (result == IrqHandlerRegistry::UnregisterResult::Completed ||
      result == IrqHandlerRegistry::UnregisterResult::Deferred) {
    assert(removedDelivery == IrqHandlerRegistry::LineMode::Threaded ||
           removedDelivery == IrqHandlerRegistry::LineMode::HardOnly);
    const IrqDelivery previousDelivery = m_IrqState.delivery(irq);
    const IrqDelivery delivery = removedDelivery == IrqHandlerRegistry::LineMode::Threaded
                                     ? IrqDelivery::Threaded
                                     : IrqDelivery::Hard;
    m_IrqState.handlerUnregistered(irq, delivery);
    const IrqDelivery currentDelivery = m_IrqState.delivery(irq);
    if (delivery == IrqDelivery::Hard && !m_IrqState.hardHandlerCount(irq)) {
      ++m_HardStageGenerations[irq];
    }
    const bool hardStageEnded =
        previousDelivery == IrqDelivery::Mixed && currentDelivery == IrqDelivery::Threaded;
    const bool hardQuarantineEnded =
        (m_FailClosedReasons[irq] & HardHandoffQuarantine) && !m_Handlers.hardLineQuarantined(irq);
    if (hardQuarantineEnded) {
      m_FailClosedReasons[irq] &= ~HardHandoffQuarantine;
    }
    if (hardStageEnded || hardQuarantineEnded) {
      if (currentDelivery == IrqDelivery::Threaded || currentDelivery == IrqDelivery::Mixed) {
        m_ThreadedHardAdmitted[irq] = true;
        m_ThreadedHardDisposition[irq] = HardIrqDisposition::Handled;
      }
      if (!m_FailClosedReasons[irq] && m_IrqState.acknowledgementPending(irq)) {
        m_IrqState.acknowledge(irq);
      }
      if (!m_FailClosedReasons[irq] && m_ThreadedHardVetoRecovery[irq] &&
          m_IrqState.completeThreadedDispatch(irq, m_ThreadedHardVetoRecoveryGenerations[irq],
                                              true)) {
        m_ThreadedHardVetoRecovery[irq] = false;
        m_ThreadedHardVetoRecoveryGenerations[irq] = 0;
      }
    }
    if (currentDelivery == IrqDelivery::None ||
        (previousDelivery == IrqDelivery::Mixed && currentDelivery == IrqDelivery::Hard)) {
      if (currentDelivery == IrqDelivery::Hard) {
        m_FailClosedReasons[irq] &= ~ThreadedPublicationQuarantine;
        if (!m_FailClosedReasons[irq] && m_IrqState.acknowledgementPending(irq)) {
          m_IrqState.acknowledge(irq);
        }
      }
      const size_t boundary = advanceThreadedCookieLocked(irq);
      m_Handlers.invalidateThreadedLine(irq, boundary);
      m_ThreadedDispatchGenerations[irq] = 0;
      m_ThreadedHadHardStage[irq] = false;
      m_ThreadedHardAdmitted[irq] = false;
      m_ThreadedHardDisposition[irq] = HardIrqDisposition::NotHandled;
      m_ThreadedHardVetoRecovery[irq] = false;
      m_ThreadedHardVetoRecoveryGenerations[irq] = 0;
    }
    if (currentDelivery == IrqDelivery::None) {
      m_FailClosedReasons[irq] = 0;
    }
  }
  finishLineTransitionLocked(irq);
  publishDiagnosticLineLocked(irq);
}

bool Pic::unregisterHandler(irq_id_t Id, IrqHandlerBase* handler) {
  if (Id < BASE_INTERRUPT_VECTOR || Id >= BASE_INTERRUPT_VECTOR + PicIrqState::LineCount ||
      !handler)
    return false;

  const uint8_t irq = Id - BASE_INTERRUPT_VECTOR;
  IrqHandlerRegistry::LineMode removedDelivery = IrqHandlerRegistry::LineMode::Empty;
  IrqHandlerRegistry::UnregisterResult result = IrqHandlerRegistry::UnregisterResult::Rejected;
  const bool atomicContext = Processor::executionContext() != ExecutionContext::WaitableThread;

  if (atomicContext) {
    StateGuard guard(*this);
    if (!guard.owned() || !m_IrqState.handlerCount(irq) || m_UnregisterReservations[irq]) {
      return false;
    }
    ++m_UnregisterReservations[irq];
    beginLineTransitionLocked(irq);
    result = m_Handlers.unregisterHandler(irq, handler, removedDelivery);
    finishHandlerUnregisterLocked(irq, result, removedDelivery);
  } else {
    {
      StateGuard guard(*this);
      if (!guard.owned() || !m_IrqState.handlerCount(irq) || m_UnregisterReservations[irq]) {
        return false;
      }
      ++m_UnregisterReservations[irq];
      beginLineTransitionLocked(irq);
    }

    result = m_Handlers.unregisterHandler(irq, handler, removedDelivery);
    {
      StateGuard guard(*this);
      if (!guard.owned()) {
        FATAL("PIC unregister lost its reserved controller boundary.");
        return false;
      }
      finishHandlerUnregisterLocked(irq, result, removedDelivery);
    }
  }

  if (result == IrqHandlerRegistry::UnregisterResult::Rejected) {
    __atomic_add_fetch(&m_RemovalRejections[irq], static_cast<size_t>(1), __ATOMIC_RELAXED);
  }
  return result == IrqHandlerRegistry::UnregisterResult::Completed;
}

bool Pic::initialise() {
  m_DeliveryProcessor = Processor::index();

  // Allocate the I/O ports
  if (m_SlavePort.allocate(0xA0, 4) == false)
    return false;
  if (m_MasterPort.allocate(0x20, 4) == false)
    return false;

  // Initialise the slave and master PIC
  m_MasterPort.write8(0x11, 0);
  m_SlavePort.write8(0x11, 0);
  m_MasterPort.write8(BASE_INTERRUPT_VECTOR, 1);
  m_SlavePort.write8(BASE_INTERRUPT_VECTOR + 0x08, 1);
  m_MasterPort.write8(0x04, 1);
  m_SlavePort.write8(0x02, 1);
  m_MasterPort.write8(0x01, 1);
  m_SlavePort.write8(0x01, 1);
  // Keep every source masked until vectors are installed and the canonical
  // cascade policy is applied below.
  m_MasterPort.write8(0xFF, 1);
  m_SlavePort.write8(0xFF, 1);

  // Register the interrupts
  InterruptManager& IntManager = InterruptManager::instance();
  for (size_t i = 0; i < 16; i++)
    if (IntManager.registerInterruptHandler(i + BASE_INTERRUPT_VECTOR, this) == false)
      return false;

  for (size_t i = 0; i < 16; i++) {
    __atomic_store_n(&m_IrqCount[i], static_cast<size_t>(0), __ATOMIC_RELAXED);
    __atomic_store_n(&m_SpuriousIrqCount[i], static_cast<size_t>(0), __ATOMIC_RELAXED);
    __atomic_store_n(&m_UnhandledIrqCount[i], static_cast<size_t>(0), __ATOMIC_RELAXED);
    __atomic_store_n(&m_ControllerContentions[i], static_cast<size_t>(0), __ATOMIC_RELAXED);
    m_MitigatedIrqs[i] = false;
    m_MitigationThreshold[i] = DEFAULT_IRQ_MITIGATE_THRESHOLD;
  }

  // Disable all IRQ's (exept IRQ2)
  enableAll(false);

  return true;
}

bool Pic::initialiseThreaded() {
  const uint64_t apicBase =
      Processor::readMachineSpecificRegister(LocalApicLint0Policy::ApicBaseMsr);
  if (!LocalApicLint0Policy::isBootstrapProcessor(apicBase)) {
    ERROR("PIC: threaded IRQ workers must be initialised on the BSP");
    return false;
  }

  // Processor topology is stable by initialise3(). The dispatcher pins its
  // workers to this scheduler, so the controller continuation and ExtINT
  // receiver now share one durable processor identity.
  m_DeliveryProcessor = Processor::index();
#if APIC
  // LAPIC destination IDs are physical routing identifiers, not scheduler
  // topology indexes. Capture the BSP's actual ID alongside the worker so
  // remote controller owners can force prompt BSP service.
  m_DeliveryApicId = Pc::instance().getLocalApic().getId();
  if (!m_ThreadedDispatcher.setRemoteWakeCallback(promptThreadedWorker, this)) {
    return false;
  }
#endif
  return m_ThreadedDispatcher.initialise();
}

bool Pic::shutdownThreaded() {
  if (Processor::index() != m_DeliveryProcessor) {
    ERROR("PIC: only the BSP may join PIC threaded IRQ workers");
    return false;
  }
  if (!m_ThreadedDispatcher.canShutdown()) {
    return false;
  }

  TerminationDeferral shutdownTermination;
  {
    StateGuard guard(*this);
    if (!guard.owned())
      return false;
    m_ShuttingDown = true;
    m_IrqState.setAllEnabled(false);
    m_IrqState.setEnabled(2, false);
    for (size_t irq = 0; irq < PicIrqState::LineCount; ++irq) {
      const uint8_t line = static_cast<uint8_t>(irq);
      beginLineTransitionLocked(line);
      const size_t boundary = advanceThreadedCookieLocked(line);
      m_Handlers.invalidateThreadedLine(line, boundary);
      m_ThreadedDispatchGenerations[irq] = 0;
      ++m_HardStageGenerations[irq];
      m_ThreadedHadHardStage[irq] = false;
      m_ThreadedHardAdmitted[irq] = false;
      m_ThreadedHardDisposition[irq] = HardIrqDisposition::NotHandled;
      m_ThreadedHardVetoRecovery[irq] = false;
      m_ThreadedHardVetoRecoveryGenerations[irq] = 0;
      m_FailClosedReasons[irq] = 0;
    }
    m_DeferredLineTransitions = 0;
    applyMaskLocked();
    publishAllDiagnosticLinesLocked();
  }
  return m_ThreadedDispatcher.shutdown();
}

Pic::Pic()
    : m_SlavePort("PIC #2"),
      m_MasterPort("PIC #1"),
      m_Handlers(),
      m_SchedulerIrqHandler(nullptr),
      m_IrqState(),
      m_ControllerStateGate(),
      m_HardTailQueue(),
      m_ControllerThreadMutex(),
      m_ThreadedDispatcher(MakeConstantString("PIC IRQ bottom half"), ControllerWorkLine + 1,
                           dispatchThreadedLine, this),
      m_ThreadedCookies(),
      m_ThreadedDispatchGenerations(),
      m_HardStageGenerations(),
      m_ThreadedHadHardStage(),
      m_ThreadedHardAdmitted(),
      m_ThreadedHardDisposition(),
      m_ThreadedHardVetoRecovery(),
      m_ThreadedHardVetoRecoveryGenerations(),
      m_FailClosedReasons(),
      m_ThreadedPublicationFailures(),
      m_RemovalRejections(),
      m_ControllerContentions(),
      m_ControllerPromptAttempts(0),
      m_ControllerPromptFailures(0),
      m_ControllerPromptDestination(0),
      m_ControllerPromptState(static_cast<size_t>(IrqControllerPromptState::NotRequired)),
      m_ControllerTemporaryMask(0),
      m_AppliedControllerMask(0xFFFF),
      m_DeferredLineTransitions(0),
      m_DeliveryProcessor(0),
      m_DeliveryApicId(0),
      m_Diagnostics(),
      m_UnregisterReservations(),
      m_ShuttingDown(false),
      m_IrqCount(),
      m_SpuriousIrqCount(),
      m_UnhandledIrqCount(),
      m_MitigatedIrqs(),
      m_MitigationThreshold() {
  publishAllDiagnosticLinesLocked();
}

void Pic::publishDiagnosticLineLocked(uint8_t irq) {
  if (irq >= PicIrqState::LineCount) {
    return;
  }

  size_t targetBank = 0;
  IrqLineDiagnosticSnapshot* target = m_Diagnostics.beginPublication(irq, targetBank);
  if (!target) {
    return;
  }

  IrqLineDiagnosticSnapshot line = {};
  line.line = irq;
  line.handlerCount = m_IrqState.handlerCount(irq);
  line.configured = line.handlerCount != 0;
  line.delivery = m_IrqState.delivery(irq);
  line.effectiveMasked =
      !m_IrqState.enabled(irq) || (m_ControllerTemporaryMask & static_cast<uint16_t>(1U << irq));
  line.requestedEnabled = m_IrqState.requestedEnabled(irq);
  line.acknowledgementPending = m_IrqState.acknowledgementPending(irq);
  line.threadedPending = m_IrqState.threadedPending(irq);
  line.dispatchGeneration = m_IrqState.dispatchGeneration(irq);
  line.acknowledgedGeneration = m_IrqState.acknowledgedGeneration(irq);
  line.publicationCookie = m_ThreadedCookies[irq];
  line.interruptCount = __atomic_load_n(&m_IrqCount[irq], __ATOMIC_RELAXED);
  line.spuriousCount = __atomic_load_n(&m_SpuriousIrqCount[irq], __ATOMIC_RELAXED);
  line.unhandledCount = __atomic_load_n(&m_UnhandledIrqCount[irq], __ATOMIC_RELAXED);
  line.publicationFailures = __atomic_load_n(&m_ThreadedPublicationFailures[irq], __ATOMIC_RELAXED);
  line.removalRejections = __atomic_load_n(&m_RemovalRejections[irq], __ATOMIC_RELAXED);
  line.controllerContentions = __atomic_load_n(&m_ControllerContentions[irq], __ATOMIC_RELAXED);
  line.controllerPromptAttempts = __atomic_load_n(&m_ControllerPromptAttempts, __ATOMIC_RELAXED);
  line.controllerPromptFailures = __atomic_load_n(&m_ControllerPromptFailures, __ATOMIC_RELAXED);
  line.controllerPromptDestination =
      __atomic_load_n(&m_ControllerPromptDestination, __ATOMIC_RELAXED);
  line.controllerPromptState = static_cast<IrqControllerPromptState>(
      __atomic_load_n(&m_ControllerPromptState, __ATOMIC_RELAXED));

  if (!line.configured) {
    line.maskReasons |= IrqMaskNoHandler;
  } else {
    line.trigger = m_IrqState.trigger(irq);
    line.controllerAck = m_IrqState.controllerAck(irq);
    line.lineRelease = m_IrqState.lineRelease(irq);
  }
  if (!line.requestedEnabled) {
    line.maskReasons |= IrqMaskAdministrativelyDisabled;
  }
  if (line.acknowledgementPending) {
    line.maskReasons |= IrqMaskAwaitingAcknowledgement;
  }
  if (line.threadedPending) {
    line.maskReasons |= IrqMaskAwaitingThreadedCompletion;
  }
  if (m_MitigatedIrqs[irq]) {
    line.maskReasons |= IrqMaskMitigated;
  }
  if (m_ShuttingDown) {
    line.maskReasons |= IrqMaskShuttingDown;
  }
  if (m_FailClosedReasons[irq] & ControllerContentionQuarantine) {
    line.maskReasons |= IrqMaskControllerContention;
  }
  if (m_ControllerTemporaryMask & static_cast<uint16_t>(1U << irq)) {
    line.maskReasons |= IrqMaskControllerContention;
  }

  *target = line;
  m_Diagnostics.finishPublication(irq, targetBank);
}

void Pic::publishAllDiagnosticLinesLocked() {
  for (size_t irq = 0; irq < PicIrqState::LineCount; ++irq) {
    publishDiagnosticLineLocked(static_cast<uint8_t>(irq));
  }
}

size_t Pic::snapshotIrqLines(IrqLineDiagnosticSnapshot* out, size_t capacity) const {
  if (!out || !capacity) {
    return 0;
  }

  const size_t count = capacity < PicIrqState::LineCount ? capacity : PicIrqState::LineCount;
  for (size_t irq = 0; irq < count; ++irq) {
    if (!m_Diagnostics.snapshot(irq, out[irq])) {
      out[irq] = {};
      out[irq].line = static_cast<uint8_t>(irq);
    }
  }

  const bool dispatcherInitialised = m_ThreadedDispatcher.isInitialised();
  const size_t controllerPromptAttempts =
      __atomic_load_n(&m_ControllerPromptAttempts, __ATOMIC_RELAXED);
  const size_t controllerPromptFailures =
      __atomic_load_n(&m_ControllerPromptFailures, __ATOMIC_RELAXED);
  const size_t controllerPromptDestination =
      __atomic_load_n(&m_ControllerPromptDestination, __ATOMIC_RELAXED);
  const IrqControllerPromptState controllerPromptState = static_cast<IrqControllerPromptState>(
      __atomic_load_n(&m_ControllerPromptState, __ATOMIC_RELAXED));
  for (size_t irq = 0; irq < count; ++irq) {
    out[irq].pendingCookie = m_ThreadedDispatcher.pendingCookie(static_cast<uint8_t>(irq));
    out[irq].activeHardDispatchCount = m_Handlers.hardDispatchState(
        static_cast<uint8_t>(irq), out[irq].activeHardDispatchGeneration);
    out[irq].hardStageActive = out[irq].activeHardDispatchCount != 0;
    out[irq].activeThreadedDispatchCount = m_Handlers.threadedDispatchState(
        static_cast<uint8_t>(irq), out[irq].activeThreadedHandlerIdentity);
    out[irq].activeCookie = m_ThreadedDispatcher.activeCookie(static_cast<uint8_t>(irq));
    out[irq].completedCookie = m_ThreadedDispatcher.completedCookie(static_cast<uint8_t>(irq));
    out[irq].completedBatches = m_ThreadedDispatcher.completedBatches(static_cast<uint8_t>(irq));
    out[irq].interruptCount = __atomic_load_n(&m_IrqCount[irq], __ATOMIC_RELAXED);
    out[irq].spuriousCount = __atomic_load_n(&m_SpuriousIrqCount[irq], __ATOMIC_RELAXED);
    out[irq].unhandledCount = __atomic_load_n(&m_UnhandledIrqCount[irq], __ATOMIC_RELAXED);
    out[irq].publicationFailures =
        __atomic_load_n(&m_ThreadedPublicationFailures[irq], __ATOMIC_RELAXED);
    out[irq].removalRejections = __atomic_load_n(&m_RemovalRejections[irq], __ATOMIC_RELAXED);
    out[irq].controllerContentions =
        __atomic_load_n(&m_ControllerContentions[irq], __ATOMIC_RELAXED);
    out[irq].controllerPromptAttempts = controllerPromptAttempts;
    out[irq].controllerPromptFailures = controllerPromptFailures;
    out[irq].controllerPromptDestination = controllerPromptDestination;
    out[irq].controllerPromptState = controllerPromptState;
    out[irq].diagnosticPublicationFailures = m_Diagnostics.missedPublications(irq);
    out[irq].workerIdentity = m_ThreadedDispatcher.workerIdentity(static_cast<uint8_t>(irq));
    out[irq].dispatcherInitialised = dispatcherInitialised;
    out[irq].dispatcherActive = m_ThreadedDispatcher.callbackActive(static_cast<uint8_t>(irq));
    out[irq].dispatcherClosed = m_ThreadedDispatcher.publicationClosed(static_cast<uint8_t>(irq));
    m_ThreadedDispatcher.snapshotDiagnostics(static_cast<uint8_t>(irq), out[irq]);
  }
  return count;
}

size_t Pic::advanceThreadedCookieLocked(uint8_t irq) {
  assert(irq < PicIrqState::LineCount);
  size_t cookie = ++m_ThreadedCookies[irq];
  if (!cookie) {
    cookie = ++m_ThreadedCookies[irq];
  }
  return cookie;
}

void Pic::beginLineTransitionLocked(uint8_t irq) {
  assert(irq < PicIrqState::LineCount);
  m_DeferredLineTransitions = cancelDeferredPicLineTransition(m_DeferredLineTransitions, irq);
  m_IrqState.beginLineTransition(irq);
  applyMaskLocked();
  m_ControllerStateGate.beginLineTransition(irq);
}

void Pic::finishLineTransitionLocked(uint8_t irq) {
  assert(irq < PicIrqState::LineCount);
  const uint16_t lineMask = static_cast<uint16_t>(1U << irq);
  if (Processor::index() == m_DeliveryProcessor && !Processor::getInterrupts()) {
    // IF is disabled on the processor which receives ExtINT, so a fresh
    // lifetime can be published immediately before the physical unmask.
    m_DeferredLineTransitions &= static_cast<uint16_t>(~lineMask);
    m_ControllerStateGate.finishLineTransition(irq);
    m_IrqState.finishLineTransition(irq);
    applyMaskLocked();
    return;
  }

  m_IrqState.finishLineTransition(irq);
  // A mask does not retract a vector already accepted by the BSP. Keep the
  // transition lifetime unpublished even when the replacement line ends
  // disabled, until the BSP closes that delivery window with IF disabled.
  m_DeferredLineTransitions |= lineMask;
  applyMaskLocked();
}

bool Pic::spuriousLocked(size_t irq) {
  if (irq > 7) {
    // Get ISR for slave.
    uint8_t mask = 1 << (irq - 8);
    m_SlavePort.write8(0x0B, 0);
    uint8_t isr = m_SlavePort.read8(0);
    m_SlavePort.write8(0x0A, 0);
    return (isr & mask) == 0;
  } else {
    // Get ISR for master.
    uint8_t mask = 1 << irq;
    m_MasterPort.write8(0x0B, 0);
    uint8_t isr = m_MasterPort.read8(0);
    m_MasterPort.write8(0x0A, 0);
    return (isr & mask) == 0;
  }
}

bool Pic::drainOnePendingControllerBatchLocked() {
  PicControllerStateGate::PendingActions pending;
  if (!m_ControllerStateGate.takePending(pending)) {
    return false;
  }

  PicControllerStateGate::PendingActions currentActions;
  PicControllerStateGate::PendingActions staleActions;
  size_t realEntries[PicIrqState::LineCount] = {};
  size_t staleRealEntries[PicIrqState::LineCount] = {};
  size_t nonMutatingSpuriousEntries[PicIrqState::LineCount] = {};
  size_t spuriousCascadeEois = 0;
  size_t staleSpuriousCascadeEois = 0;
  bool maskChanged = false;

  for (size_t irq = 0; irq < PicIrqState::LineCount; ++irq) {
    m_HardTailQueue.consume(static_cast<uint8_t>(irq), [this, irq](const PicHardTailRecord& tail) {
      __atomic_add_fetch(&m_ControllerContentions[irq], static_cast<size_t>(1), __ATOMIC_RELAXED);
      finishHardDispatchLocked(tail);
    });
  }

  for (size_t irq = 0; irq < PicIrqState::LineCount; ++irq) {
    currentActions.entry[irq] = pending.entry[irq];
    currentActions.tail[irq] = pending.tail[irq];
    currentActions.tailEoi[irq] = pending.tailEoi[irq];
    staleActions.entry[irq] = pending.staleEntry[irq];
    staleActions.tail[irq] = pending.staleTail[irq];
    staleActions.tailEoi[irq] = pending.staleTailEoi[irq];

    for (size_t occurrence = 0; occurrence < pending.entry[irq]; ++occurrence) {
      if ((!m_IrqState.enabled(irq) || irq == 7 || irq == 15) && spuriousLocked(irq)) {
        __atomic_add_fetch(&m_SpuriousIrqCount[irq], static_cast<size_t>(1), __ATOMIC_RELAXED);
        ++nonMutatingSpuriousEntries[irq];
        if (irq > 7) {
          ++spuriousCascadeEois;
        }
      } else {
        ++realEntries[irq];
      }
    }

    for (size_t occurrence = 0; occurrence < pending.staleEntry[irq]; ++occurrence) {
      if ((!m_IrqState.enabled(irq) || irq == 7 || irq == 15) && spuriousLocked(irq)) {
        __atomic_add_fetch(&m_SpuriousIrqCount[irq], static_cast<size_t>(1), __ATOMIC_RELAXED);
        ++nonMutatingSpuriousEntries[irq];
        if (irq > 7) {
          ++staleSpuriousCascadeEois;
        }
      } else {
        ++staleRealEntries[irq];
      }
    }

    const size_t contentions =
        pending.entry[irq] + pending.tail[irq] + pending.staleEntry[irq] + pending.staleTail[irq];
    if (contentions) {
      __atomic_add_fetch(&m_ControllerContentions[irq], contentions, __ATOMIC_RELAXED);
    }

    const PicContentionLineResult result = resolvePicContentionLine(
        m_IrqState, irq, realEntries[irq], pending.tail[irq], pending.tailEoi[irq]);
    assert(result.threadedOccurrences <= realEntries[irq]);
    assert(result.threadedOccurrences <= currentActions.entry[irq]);
    for (size_t occurrence = 0; occurrence < result.threadedOccurrences; ++occurrence) {
      admitThreadedOccurrenceLocked(static_cast<uint8_t>(irq));
    }
    realEntries[irq] -= result.threadedOccurrences;
    currentActions.entry[irq] -= result.threadedOccurrences;
    if (result.terminalWork) {
      __atomic_add_fetch(&m_UnhandledIrqCount[irq], result.unhandledOccurrences, __ATOMIC_RELAXED);
    }

    if (result.schedulerDrop) {
      m_FailClosedReasons[irq] &= ~ControllerContentionQuarantine;
      // Keep the PIT physically one-shot until the BSP releases the
      // controller gate; otherwise its next edge can starve the worker
      // which completes this handoff.
      m_ControllerTemporaryMask |= static_cast<uint16_t>(1U << irq);
      maskChanged = true;
    } else if (result.quarantine) {
      m_FailClosedReasons[irq] |= ControllerContentionQuarantine;
    }

    if (result.invalidateThreaded) {
      const size_t boundary = advanceThreadedCookieLocked(static_cast<uint8_t>(irq));
      m_Handlers.invalidateThreadedGenerationFromInterrupt(static_cast<uint8_t>(irq), boundary);
      m_ThreadedDispatchGenerations[irq] = 0;
      m_ThreadedHadHardStage[irq] = false;
      m_ThreadedHardAdmitted[irq] = false;
      m_ThreadedHardDisposition[irq] = HardIrqDisposition::NotHandled;
      m_ThreadedHardVetoRecovery[irq] = false;
      m_ThreadedHardVetoRecoveryGenerations[irq] = 0;
    }
    maskChanged |= result.maskChanged;

    const size_t staleUnhandled = staleRealEntries[irq] + pending.staleTail[irq];
    if (staleUnhandled) {
      __atomic_add_fetch(&m_UnhandledIrqCount[irq], staleUnhandled, __ATOMIC_RELAXED);
    }
  }

  const uint16_t temporaryMask = temporaryPicMaskForDeferredWork(
      m_ControllerTemporaryMask, staleRealEntries, staleActions, nonMutatingSpuriousEntries);
  maskChanged |= temporaryMask != m_ControllerTemporaryMask;
  m_ControllerTemporaryMask = temporaryMask;

  if (maskChanged) {
    applyMaskLocked();
  }

  auto writeController = [this](PicControllerWriteTarget target, uint8_t value) {
    switch (target) {
      case PicControllerWriteTarget::MasterCommand:
        m_MasterPort.write8(value, 0);
        break;
      case PicControllerWriteTarget::MasterMask:
        m_MasterPort.write8(value, 1);
        break;
      case PicControllerWriteTarget::SlaveCommand:
        m_SlavePort.write8(value, 0);
        break;
      case PicControllerWriteTarget::SlaveMask:
        m_SlavePort.write8(value, 1);
        break;
    }
  };
  emitPicContentionWrites(m_IrqState, false, realEntries, currentActions, spuriousCascadeEois,
                          writeController);
  emitPicContentionWrites(m_IrqState, false, staleRealEntries, staleActions,
                          staleSpuriousCascadeEois, writeController);

  for (size_t irq = 0; irq < PicIrqState::LineCount; ++irq) {
    if (pending.entry[irq] || pending.tail[irq] || pending.staleEntry[irq] ||
        pending.staleTail[irq]) {
      publishDiagnosticLineLocked(static_cast<uint8_t>(irq));
    }
  }
  return true;
}

void Pic::drainPendingControllerActionsLocked() {
  while (drainOnePendingControllerBatchLocked()) {
  }
}

bool Pic::handControllerStateToWorker() {
  m_ControllerStateGate.requestContinuation();
  if (!m_ThreadedDispatcher.publishFromInterrupt(ControllerWorkLine, 1)) {
    return false;
  }
  return m_ControllerStateGate.relinquishOwnerForContinuation();
}

void Pic::releaseControllerState() {
  for (;;) {
    drainPendingControllerActionsLocked();
    if (m_DeferredLineTransitions) {
      finishDeferredLineTransitionsLocked();
    }

    const bool deliveryProcessor = Processor::index() == m_DeliveryProcessor;
    if (!deliveryProcessor) {
      // A stable lifetime does not need the BSP publication barrier.
      // Restore it while retaining Owner, so an interrupt racing the
      // unmask either observes Clean or makes releaseIfIdle fail.
      const uint16_t restorable =
          restorablePicTemporaryMask(m_ControllerTemporaryMask, m_DeferredLineTransitions);
      if (restorable) {
        clearControllerTemporaryMaskLocked(restorable);
      }
    }
    const uint16_t pendingUnmask =
        static_cast<uint16_t>(m_ControllerTemporaryMask & ~m_IrqState.mask());
    if (!deliveryProcessor && m_DeferredLineTransitions) {
      if (handControllerStateToWorker()) {
        return;
      }
      if (!m_ControllerStateGate.urgentPending()) {
        FATAL_NOLOCK(
            "PIC controller restore work could not reach its BSP "
            "continuation.");
      }
      continue;
    }

    if (m_ControllerTemporaryMask && !pendingUnmask) {
      clearControllerTemporaryMaskLocked();
    }

    if (deliveryProcessor && (m_ControllerTemporaryMask || m_DeferredLineTransitions)) {
      const bool interruptsWereEnabled = Processor::getInterrupts();
      if (interruptsWereEnabled) {
        Processor::setInterrupts(false);
      }

      // Close the final pending-vector window before publishing fresh
      // lifetimes, physically unmasking, and releasing ownership.
      drainPendingControllerActionsLocked();
      if (m_DeferredLineTransitions) {
        finishDeferredLineTransitionsLocked();
      }
      if (m_ControllerTemporaryMask) {
        clearControllerTemporaryMaskLocked();
      }
      const bool released = m_ControllerStateGate.releaseIfIdle();
      if (interruptsWereEnabled) {
        Processor::setInterrupts(true);
      }
      if (released) {
        return;
      }
      continue;
    }

    if (m_ControllerStateGate.releaseIfIdle()) {
      return;
    }
  }
}

void Pic::releaseControllerStateFromInterrupt() {
  if (m_ControllerStateGate.hasPending() && !m_ControllerStateGate.urgentPending() &&
      handControllerStateToWorker()) {
    return;
  }

  // IRQ0 is the only mandatory hard-context batch because its worker cannot
  // run if the unacknowledged PIT edge is also the scheduler's next wake.
  drainOnePendingControllerBatchLocked();
  for (;;) {
    if (m_ControllerStateGate.urgentPending()) {
      drainOnePendingControllerBatchLocked();
      continue;
    }
    if (m_ControllerStateGate.hasPending()) {
      if (handControllerStateToWorker()) {
        return;
      }
      // Startup and late shutdown can legitimately have no worker. The
      // verified BSP-only ExtINT route keeps this synchronous fallback
      // finite because IF is disabled on the sole hard producer.
      drainOnePendingControllerBatchLocked();
      continue;
    }
    if (m_DeferredLineTransitions) {
      // Hard entries only arrive on the BSP with IF disabled, which is
      // also the safe terminal boundary for a replacement lifetime.
      finishDeferredLineTransitionsLocked();
    }
    if (m_ControllerTemporaryMask) {
      clearControllerTemporaryMaskLocked();
    }
    if (m_ControllerStateGate.releaseIfIdle()) {
      return;
    }
  }
}

void Pic::admitThreadedOccurrenceLocked(uint8_t irq) {
  assert(irq < PicIrqState::LineCount);
  assert(m_IrqState.delivery(irq) == IrqDelivery::Threaded);

  const IrqControllerAck controllerAck = m_IrqState.controllerAck(irq);
  const IrqLineRelease lineRelease = m_IrqState.lineRelease(irq);
  const size_t dispatchGeneration = m_IrqState.beginDispatch(irq);
  IrqHandlerRegistry::AdmissionCutoff admissionCutoff = {};
  if (!m_Handlers.captureAdmissionCutoff(irq, admissionCutoff)) {
    const bool wasEnabled = m_IrqState.enabled(irq);
    m_FailClosedReasons[irq] |= ThreadedPublicationQuarantine;
    m_IrqState.completeDispatch(irq, dispatchGeneration, true);
    if (wasEnabled != m_IrqState.enabled(irq)) {
      applyMaskLocked();
    }
    if (controllerAck != IrqControllerAck::None) {
      eoiLocked(irq);
    }
    __atomic_add_fetch(&m_ThreadedPublicationFailures[irq], static_cast<size_t>(1),
                       __ATOMIC_RELAXED);
    publishDiagnosticLineLocked(irq);
    return;
  }

  // A one-shot threaded policy masks before EOI. Immediate-release
  // policies remain open while the worker runs.
  m_IrqState.beginThreadedDispatch(irq);
  if (lineRelease == IrqLineRelease::AfterThreadedCompletion) {
    applyMaskLocked();
  }
  if (controllerAck == IrqControllerAck::BeforeHardStage) {
    eoiLocked(irq);
  }

  const size_t threadedCookie = advanceThreadedCookieLocked(irq);
  m_ThreadedDispatchGenerations[irq] = dispatchGeneration;
  m_ThreadedHadHardStage[irq] = false;
  m_ThreadedHardAdmitted[irq] = false;
  m_ThreadedHardDisposition[irq] = HardIrqDisposition::NotHandled;
  m_ThreadedHardVetoRecovery[irq] = false;
  m_ThreadedHardVetoRecoveryGenerations[irq] = 0;

  bool published = m_Handlers.publishThreadedDispatch(irq, threadedCookie, admissionCutoff);
  if (published && !m_ThreadedDispatcher.publishFromInterrupt(irq, threadedCookie)) {
    m_Handlers.invalidateThreadedGenerationFromInterrupt(irq, threadedCookie);
    published = false;
  }
  if (!published) {
    m_FailClosedReasons[irq] |= ThreadedPublicationQuarantine;
    m_ThreadedHadHardStage[irq] = false;
    m_ThreadedHardAdmitted[irq] = false;
    m_ThreadedHardDisposition[irq] = HardIrqDisposition::NotHandled;
    const bool wasEnabled = m_IrqState.enabled(irq);
    m_IrqState.completeDispatch(irq, dispatchGeneration, true);
    if (wasEnabled != m_IrqState.enabled(irq)) {
      applyMaskLocked();
    }
    __atomic_add_fetch(&m_ThreadedPublicationFailures[irq], static_cast<size_t>(1),
                       __ATOMIC_RELAXED);
  }

  if (controllerAck == IrqControllerAck::AfterHardStage) {
    eoiLocked(irq);
  }
  publishDiagnosticLineLocked(irq);
}

void Pic::interrupt(size_t interruptNumber, InterruptState& state) {
  size_t irq = (interruptNumber - BASE_INTERRUPT_VECTOR);
  if (irq >= PicIrqState::LineCount) {
    return;
  }
  if (UNLIKELY(Processor::index() != m_DeliveryProcessor)) {
    FATAL_NOLOCK(
        "Legacy PIC interrupt reached a processor whose LINT0 must be "
        "masked.");
    return;
  }

  __atomic_add_fetch(&m_IrqCount[irq], static_cast<size_t>(1), __ATOMIC_RELAXED);
  const size_t controllerLifetime = m_ControllerStateGate.currentLifetime(irq);
  HardStateGuard entry(*this, static_cast<uint8_t>(irq), controllerLifetime);
  if (!entry.owned()) {
    return;
  }

  if (irq == 0) {
    SchedulerIrqHandler* schedulerHandler = m_SchedulerIrqHandler;
    if (schedulerHandler) {
      if (!m_IrqState.enabled(irq) && spuriousLocked(irq)) {
        __atomic_add_fetch(&m_SpuriousIrqCount[irq], static_cast<size_t>(1), __ATOMIC_RELAXED);
        publishDiagnosticLineLocked(static_cast<uint8_t>(irq));
        return;
      }

      const size_t generation = m_IrqState.beginDispatch(irq);

      // timer() can switch away permanently, so complete controller and
      // software acknowledgement before entering the scheduler.
      eoiLocked(static_cast<uint8_t>(irq));
      m_IrqState.acknowledge(irq);
      m_IrqState.completeDispatch(irq, generation, false);
      publishDiagnosticLineLocked(static_cast<uint8_t>(irq));
      entry.release();
      schedulerHandler->schedulerIrq(static_cast<irq_id_t>(irq), state);
      return;
    }
  }

  IrqControllerAck controllerAck = IrqControllerAck::None;
  IrqDelivery delivery = IrqDelivery::None;
  bool hasThreadedStage = false;
  size_t dispatchGeneration = 0;
  size_t hardStageGeneration = 0;
  size_t threadedCookie = 0;
  bool threadedPublished = false;
  IrqHandlerRegistry::AdmissionCutoff hardAdmissionCutoff = {};
  {
    // IRQ7 and IRQ15 are the architectural spurious-vector cases. A
    // disabled line can also have a vector already in flight, so retain
    // the broader check before touching its in-service state.
    if ((!m_IrqState.enabled(irq) || irq == 7 || irq == 15) && spuriousLocked(irq)) {
      if (irq > 7) {
        // A spurious slave vector never entered the slave ISR, but
        // the master still accepted the cascade interrupt.
        m_MasterPort.write8(0x62, 0);
      }
      __atomic_add_fetch(&m_SpuriousIrqCount[irq], static_cast<size_t>(1), __ATOMIC_RELAXED);
      publishDiagnosticLineLocked(static_cast<uint8_t>(irq));
      return;
    }

    delivery = m_IrqState.delivery(irq);
    if (delivery == IrqDelivery::Threaded) {
      admitThreadedOccurrenceLocked(static_cast<uint8_t>(irq));
      return;
    }

    controllerAck = m_IrqState.controllerAck(irq);
    const IrqLineRelease lineRelease = m_IrqState.lineRelease(irq);
    dispatchGeneration = m_IrqState.beginDispatch(irq);
    hardStageGeneration = m_HardStageGenerations[irq];
    hasThreadedStage = delivery == IrqDelivery::Mixed;

    IrqHandlerRegistry::AdmissionCutoff threadedAdmissionCutoff = {};
    bool cutoffCaptured = false;
    if (delivery == IrqDelivery::Mixed) {
      IrqHandlerRegistry::MixedAdmissionCutoffs cutoffs = {};
      cutoffCaptured = m_Handlers.captureMixedAdmissionCutoffs(static_cast<uint8_t>(irq), cutoffs);
      hardAdmissionCutoff = cutoffs.hard;
      threadedAdmissionCutoff = cutoffs.threaded;
    } else {
      // Preserve the ordinary unhandled-vector path for a disabled line
      // which was already accepted by the controller.
      cutoffCaptured =
          m_Handlers.captureAdmissionCutoff(static_cast<uint8_t>(irq), hardAdmissionCutoff);
    }

    if (!cutoffCaptured) {
      const bool wasEnabled = m_IrqState.enabled(irq);
      m_FailClosedReasons[irq] |=
          hasThreadedStage ? ThreadedPublicationQuarantine : UnhandledQuarantine;
      m_IrqState.completeDispatch(irq, dispatchGeneration, true);
      if (wasEnabled != m_IrqState.enabled(irq)) {
        applyMaskLocked();
      }
      if (controllerAck != IrqControllerAck::None) {
        eoiLocked(irq);
      }
      __atomic_add_fetch(
          hasThreadedStage ? &m_ThreadedPublicationFailures[irq] : &m_UnhandledIrqCount[irq],
          static_cast<size_t>(1), __ATOMIC_RELAXED);
      publishDiagnosticLineLocked(static_cast<uint8_t>(irq));
      return;
    }

    if (hasThreadedStage) {
      // A one-shot threaded policy masks before EOI. Immediate-release
      // policies remain open while the worker runs.
      m_IrqState.beginThreadedDispatch(irq);
      if (lineRelease == IrqLineRelease::AfterThreadedCompletion) {
        applyMaskLocked();
      }
      if (controllerAck == IrqControllerAck::BeforeHardStage) {
        eoiLocked(irq);
      }
      threadedCookie = advanceThreadedCookieLocked(irq);
      m_ThreadedDispatchGenerations[irq] = dispatchGeneration;
      m_ThreadedHadHardStage[irq] = true;
      m_ThreadedHardAdmitted[irq] = false;
      m_ThreadedHardDisposition[irq] = HardIrqDisposition::NotHandled;
      m_ThreadedHardVetoRecovery[irq] = false;
      m_ThreadedHardVetoRecoveryGenerations[irq] = 0;
      threadedPublished =
          m_Handlers.publishThreadedDispatch(irq, threadedCookie, threadedAdmissionCutoff);
      if (!threadedPublished) {
        m_FailClosedReasons[irq] |= ThreadedPublicationQuarantine;
        m_ThreadedHadHardStage[irq] = false;
        m_ThreadedHardAdmitted[irq] = false;
        m_ThreadedHardDisposition[irq] = HardIrqDisposition::NotHandled;
        __atomic_add_fetch(&m_ThreadedPublicationFailures[irq], static_cast<size_t>(1),
                           __ATOMIC_RELAXED);
      }

      // A mixed line's worker must not overlap the hard callbacks which
      // share its physical occurrence. Its doorbell is rung only after
      // the hard stage below has completed.
    } else if (controllerAck == IrqControllerAck::BeforeHardStage) {
      eoiLocked(irq);
    }
    publishDiagnosticLineLocked(static_cast<uint8_t>(irq));
  }

  entry.release();

  HardIrqDisposition hardDisposition = HardIrqDisposition::NotHandled;
  const bool admitted = m_Handlers.dispatchHard(irq, state, hardDisposition, nullptr,
                                                dispatchGeneration, hardAdmissionCutoff);

  PicHardTailRecord tail = {};
  tail.irq = static_cast<uint8_t>(irq);
  tail.controllerLifetime = controllerLifetime;
  tail.dispatchGeneration = dispatchGeneration;
  tail.hardStageGeneration = hardStageGeneration;
  tail.threadedCookie = threadedCookie;
  tail.controllerAck = controllerAck;
  tail.hardDisposition = hardDisposition;
  tail.hasThreadedStage = hasThreadedStage;
  tail.threadedPublished = threadedPublished;
  tail.admitted = admitted;
  finishHardDispatchFromInterrupt(tail);
}

void Pic::finishHardDispatchLocked(const PicHardTailRecord& record) {
  const uint8_t irq = record.irq;
  const size_t dispatchGeneration = record.dispatchGeneration;
  const size_t threadedCookie = record.threadedCookie;
  const IrqControllerAck controllerAck = record.controllerAck;
  const bool hasThreadedStage = record.hasThreadedStage;
  bool threadedPublished = record.threadedPublished;
  const bool admitted = record.admitted;

  PicHardTailCurrentState current = {};
  current.controllerLifetime = m_ControllerStateGate.currentLifetime(irq);
  current.dispatchGeneration = m_IrqState.dispatchGeneration(irq);
  current.hardStageGeneration = m_HardStageGenerations[irq];
  current.threadedCookie = m_ThreadedCookies[irq];
  current.threadedDispatchGeneration = m_ThreadedDispatchGenerations[irq];
  current.hardHandlerCount = m_IrqState.hardHandlerCount(irq);
  current.delivery = hasThreadedStage ? m_IrqState.delivery(irq) : IrqDelivery::None;
  current.hardLineQuarantined = m_Handlers.hardLineQuarantined(irq);
  const PicHardTailPlan plan = resolvePicHardTail(record, current);
  PicHardTailTerminalSequence terminal(!plan.controllerLifetimeCurrent, controllerAck);
  terminal.applyTemporaryMask([this, irq]() {
    m_ControllerTemporaryMask |= static_cast<uint16_t>(1U << irq);
    applyMaskLocked();
  });

  const IrqDelivery currentDelivery = current.delivery;
  const bool hardStageLifetimeCurrent = plan.hardStageLifetimeCurrent;
  const bool threadedLifetimeCurrent = plan.threadedLifetimeCurrent;
  const HardIrqDisposition effectiveHardDisposition = plan.effectiveHardDisposition;
  const bool hardHandoffFailed = effectiveHardDisposition == HardIrqDisposition::KeepMasked;
  if (hasThreadedStage && (hardHandoffFailed || (threadedLifetimeCurrent && !threadedPublished))) {
    const bool wasEnabled = m_IrqState.enabled(irq);
    if (hardHandoffFailed) {
      m_FailClosedReasons[irq] |= HardHandoffQuarantine;
    }
    m_IrqState.completeDispatch(irq, dispatchGeneration, true);
    if (wasEnabled != m_IrqState.enabled(irq)) {
      applyMaskLocked();
    }
    if (hardHandoffFailed) {
      __atomic_add_fetch(&m_ThreadedPublicationFailures[irq], static_cast<size_t>(1),
                         __ATOMIC_RELAXED);
    }
  }

  if (hasThreadedStage) {
    if (threadedLifetimeCurrent) {
      if (!threadedPublished) {
        m_ThreadedHadHardStage[irq] = false;
        m_ThreadedHardAdmitted[irq] = false;
        m_ThreadedHardDisposition[irq] = HardIrqDisposition::NotHandled;
        const bool wasEnabled = m_IrqState.enabled(irq);
        m_IrqState.completeDispatch(irq, dispatchGeneration, true);
        if (wasEnabled != m_IrqState.enabled(irq)) {
          applyMaskLocked();
        }
        terminal.acknowledge([this, irq]() { eoiLocked(irq); });
        publishDiagnosticLineLocked(static_cast<uint8_t>(irq));
        return;
      }
      const bool hardStageQuiesced =
          !hardStageLifetimeCurrent || currentDelivery == IrqDelivery::Threaded;
      m_ThreadedHardAdmitted[irq] = admitted || hardStageQuiesced;
      m_ThreadedHardDisposition[irq] = effectiveHardDisposition;
      const PicHardTailDoorbellResult doorbell = resolvePicHardTailDoorbell(
          true, m_ThreadedDispatcher.publishFromInterrupt(irq, threadedCookie));
      if (doorbell.invalidateStagedDispatch) {
        m_Handlers.invalidateThreadedGenerationFromInterrupt(irq, threadedCookie);
        if (doorbell.quarantine) {
          m_FailClosedReasons[irq] |= ThreadedPublicationQuarantine;
        }
        m_ThreadedHadHardStage[irq] = false;
        m_ThreadedHardAdmitted[irq] = false;
        m_ThreadedHardDisposition[irq] = HardIrqDisposition::NotHandled;
        __atomic_add_fetch(&m_ThreadedPublicationFailures[irq], static_cast<size_t>(1),
                           __ATOMIC_RELAXED);
      }
      threadedPublished = doorbell.published;
      if (doorbell.completeDispatch) {
        const bool wasEnabled = m_IrqState.enabled(irq);
        m_IrqState.completeDispatch(irq, dispatchGeneration, true);
        if (wasEnabled != m_IrqState.enabled(irq)) {
          applyMaskLocked();
        }
      }
    } else if (plan.threadedAction == PicHardTailThreadedAction::Quiesced) {
      // The old threaded action was synchronously quiesced. A
      // replacement threaded handler belongs to the next
      // occurrence, but this hard result still needs one terminal
      // decision for the occurrence already in flight.
      const bool threadedStageQuiesced = true;
      const bool aggregateAdmitted = admitted || threadedStageQuiesced;
      const bool aggregateAllowRearm =
          (effectiveHardDisposition == HardIrqDisposition::Handled || threadedStageQuiesced) &&
          effectiveHardDisposition != HardIrqDisposition::KeepMasked;
      const bool wasEnabled = m_IrqState.enabled(irq);
      m_IrqState.completeDispatch(irq, dispatchGeneration,
                                  aggregateAdmitted && !aggregateAllowRearm);
      if (!aggregateAdmitted || (!aggregateAllowRearm && !hardHandoffFailed)) {
        m_FailClosedReasons[irq] |= UnhandledQuarantine;
        __atomic_add_fetch(&m_UnhandledIrqCount[irq], static_cast<size_t>(1), __ATOMIC_RELAXED);
      }
      if (wasEnabled != m_IrqState.enabled(irq)) {
        applyMaskLocked();
      }
    }
    terminal.acknowledge([this, irq]() { eoiLocked(irq); });
    publishDiagnosticLineLocked(static_cast<uint8_t>(irq));
    return;
  }

  const bool wasEnabled = m_IrqState.enabled(irq);
  const bool needsAcknowledgement =
      effectiveHardDisposition == HardIrqDisposition::KeepMasked ||
      (admitted && effectiveHardDisposition == HardIrqDisposition::NotHandled);
  if (needsAcknowledgement) {
    m_FailClosedReasons[irq] |= effectiveHardDisposition == HardIrqDisposition::KeepMasked
                                    ? HardHandoffQuarantine
                                    : UnhandledQuarantine;
  }
  m_IrqState.completeDispatch(irq, dispatchGeneration, needsAcknowledgement);
  if (effectiveHardDisposition == HardIrqDisposition::KeepMasked) {
    __atomic_add_fetch(&m_ThreadedPublicationFailures[irq], static_cast<size_t>(1),
                       __ATOMIC_RELAXED);
  } else if (hardStageLifetimeCurrent &&
             (!admitted || effectiveHardDisposition == HardIrqDisposition::NotHandled)) {
    __atomic_add_fetch(&m_UnhandledIrqCount[irq], static_cast<size_t>(1), __ATOMIC_RELAXED);
  }
  if (wasEnabled != m_IrqState.enabled(irq)) {
    applyMaskLocked();
  }
  terminal.acknowledge([this, irq]() { eoiLocked(irq); });
  publishDiagnosticLineLocked(static_cast<uint8_t>(irq));
}

void Pic::finishHardDispatchFromInterrupt(const PicHardTailRecord& record) {
  if (m_ControllerStateGate.tryAcquireClean()) {
    finishHardDispatchLocked(record);
    releaseControllerStateFromInterrupt();
    return;
  }

  if (m_HardTailQueue.publish(record.irq, record)) {
    if (m_ControllerStateGate.queueTailRecord(record.irq)) {
      releaseControllerStateFromInterrupt();
    }
    return;
  }

  // The per-line slot cannot normally be occupied because gate ownership
  // prevents another callback on that line. Preserve the hardware terminal
  // obligations even if that invariant is violated instead of waiting.
  if (m_ControllerStateGate.queueTail(record.irq, record.controllerLifetime,
                                      record.controllerAck == IrqControllerAck::AfterHardStage)) {
    releaseControllerStateFromInterrupt();
  }
}

void Pic::dispatchThreadedLine(void* context, uint8_t irq, size_t cookie) {
  Pic* pic = reinterpret_cast<Pic*>(context);
  if (irq == ControllerWorkLine) {
    StateGuard guard(*pic);
    if (!guard.owned())
      FATAL("PIC controller continuation could not claim thread state.");
    return;
  }

  size_t dispatchGeneration = 0;
  {
    StateGuard guard(*pic);
    if (!guard.owned()) {
      FATAL("PIC threaded dispatch could not snapshot line state.");
      return;
    }
    if (irq >= PicIrqState::LineCount) {
      return;
    }
    const IrqDelivery delivery = pic->m_IrqState.delivery(irq);
    if (cookie != pic->m_ThreadedCookies[irq] ||
        (delivery != IrqDelivery::Threaded && delivery != IrqDelivery::Mixed)) {
      return;
    }
    dispatchGeneration = pic->m_ThreadedDispatchGenerations[irq];
  }

  IrqHandlerRegistry::ThreadedDispatchResult result = {};
  const bool admitted = pic->m_Handlers.dispatchThreaded(irq, cookie, result);

  {
    StateGuard guard(*pic);
    if (!guard.owned()) {
      FATAL("PIC threaded completion could not claim line state.");
      return;
    }
    const IrqDelivery delivery = pic->m_IrqState.delivery(irq);
    if (cookie != pic->m_ThreadedCookies[irq] ||
        dispatchGeneration != pic->m_ThreadedDispatchGenerations[irq] ||
        (delivery != IrqDelivery::Threaded && delivery != IrqDelivery::Mixed)) {
      return;
    }

    const bool hadHardStage = pic->m_ThreadedHadHardStage[irq];
    const bool aggregateAdmitted = pic->m_ThreadedHardAdmitted[irq] || admitted;
    const HardIrqDisposition hardDisposition = pic->m_ThreadedHardDisposition[irq];
    const bool aggregateAllowRearm =
        (hardDisposition == HardIrqDisposition::Handled || result.allowRearm) &&
        hardDisposition != HardIrqDisposition::KeepMasked;
    const bool hardVetoRecovery =
        hadHardStage && hardDisposition == HardIrqDisposition::KeepMasked && admitted &&
        result.allowRearm &&
        pic->m_IrqState.lineRelease(irq) == IrqLineRelease::AfterThreadedCompletion;
    pic->m_ThreadedHardVetoRecovery[irq] = hardVetoRecovery;
    pic->m_ThreadedHardVetoRecoveryGenerations[irq] = hardVetoRecovery ? dispatchGeneration : 0;
    const bool wasEnabled = pic->m_IrqState.enabled(irq);
    if (!aggregateAdmitted ||
        (!aggregateAllowRearm && hardDisposition != HardIrqDisposition::KeepMasked)) {
      pic->m_FailClosedReasons[irq] |= UnhandledQuarantine;
      __atomic_add_fetch(&pic->m_UnhandledIrqCount[irq], static_cast<size_t>(1), __ATOMIC_RELAXED);
    }
    if (hadHardStage && pic->m_IrqState.lineRelease(irq) == IrqLineRelease::AfterHardStage) {
      pic->m_IrqState.completeDispatch(irq, dispatchGeneration,
                                       aggregateAdmitted && !aggregateAllowRearm);
    }
    pic->m_IrqState.completeThreadedDispatch(irq, dispatchGeneration,
                                             aggregateAdmitted && aggregateAllowRearm);
    pic->m_ThreadedHadHardStage[irq] = false;
    pic->m_ThreadedHardAdmitted[irq] = false;
    pic->m_ThreadedHardDisposition[irq] = HardIrqDisposition::NotHandled;
    if (wasEnabled != pic->m_IrqState.enabled(irq)) {
      pic->applyMaskLocked();
    }
    pic->publishDiagnosticLineLocked(irq);
  }
}

#if APIC
bool Pic::promptThreadedWorker(void* context, uint8_t line, size_t workerProcessor) {
  Pic* pic = reinterpret_cast<Pic*>(context);
  if (!pic) {
    return false;
  }

  __atomic_add_fetch(&pic->m_ControllerPromptAttempts, static_cast<size_t>(1), __ATOMIC_RELAXED);
  __atomic_store_n(&pic->m_ControllerPromptDestination, static_cast<size_t>(pic->m_DeliveryApicId),
                   __ATOMIC_RELAXED);
  if (line != ControllerWorkLine || workerProcessor != pic->m_DeliveryProcessor) {
    __atomic_add_fetch(&pic->m_ControllerPromptFailures, static_cast<size_t>(1), __ATOMIC_RELAXED);
    __atomic_store_n(&pic->m_ControllerPromptState,
                     static_cast<size_t>(IrqControllerPromptState::Failed), __ATOMIC_RELEASE);
    return false;
  }

  // The dispatcher has already staged the BSP scheduler doorbell and the
  // remote producer's cookie. This bounded ICR transaction only prompts the
  // owning BSP; it cannot retract the accepted controller occurrence.
  const bool submitted = Pc::instance().getLocalApic().interProcessorInterrupt(
      pic->m_DeliveryApicId, IPI_RESCHEDULE_VECTOR, LocalApic::deliveryModeFixed, true, false);
  if (!submitted) {
    __atomic_add_fetch(&pic->m_ControllerPromptFailures, static_cast<size_t>(1), __ATOMIC_RELAXED);
  }
  __atomic_store_n(&pic->m_ControllerPromptState,
                   static_cast<size_t>(submitted ? IrqControllerPromptState::Submitted
                                                 : IrqControllerPromptState::Failed),
                   __ATOMIC_RELEASE);
  return submitted;
}
#endif

void Pic::eoiLocked(uint8_t irq) {
  if (irq > 7) {
    m_SlavePort.write8(0x60 + (irq - 8), 0);

    // ACK the cascade IRQ (IRQ2).
    m_MasterPort.write8(0x62, 0);
  } else {
    m_MasterPort.write8(0x60 + irq, 0);
  }
}

void Pic::applyMaskLocked() {
  const uint16_t canonical = m_IrqState.mask();
  const bool canUnmaskBeforeRelease =
      Processor::index() == m_DeliveryProcessor && !Processor::getInterrupts();
  m_ControllerTemporaryMask =
      preserveUnsafePicUnmasks(m_AppliedControllerMask, canonical, m_ControllerTemporaryMask,
                               m_DeferredLineTransitions, canUnmaskBeforeRelease);
  const uint16_t mask = effectiveMaskLocked();
  emitPicMaskWrites(mask, [this](PicControllerWriteTarget target, uint8_t value) {
    if (target == PicControllerWriteTarget::MasterMask) {
      m_MasterPort.write8(value, 1);
    } else {
      m_SlavePort.write8(value, 1);
    }
  });
  m_AppliedControllerMask = mask;
}

uint16_t Pic::effectiveMaskLocked() const {
  return effectivePicMask(m_IrqState.mask(), m_ControllerTemporaryMask);
}

void Pic::finishDeferredLineTransitionsLocked() {
  const uint16_t pending = m_DeferredLineTransitions;
  m_DeferredLineTransitions = 0;
  const bool canPublish = Processor::index() == m_DeliveryProcessor && !Processor::getInterrupts();
  for (size_t irq = 0; irq < PicIrqState::LineCount; ++irq) {
    if ((pending & static_cast<uint16_t>(1U << irq)) && !m_IrqState.lineTransitionPending(irq)) {
      if (canPublish) {
        m_ControllerStateGate.finishLineTransition(irq);
      } else {
        m_DeferredLineTransitions |= static_cast<uint16_t>(1U << irq);
      }
    }
  }
}

void Pic::clearControllerTemporaryMaskLocked(uint16_t restoreMask) {
  const uint16_t restored = static_cast<uint16_t>(m_ControllerTemporaryMask & restoreMask);
  if (!restored) {
    return;
  }

  if (m_DeferredLineTransitions) {
    finishDeferredLineTransitionsLocked();
  }
  m_ControllerTemporaryMask &= static_cast<uint16_t>(~restored);
  applyMaskLocked();
  for (size_t irq = 0; irq < PicIrqState::LineCount; ++irq) {
    if (restored & static_cast<uint16_t>(1U << irq)) {
      publishDiagnosticLineLocked(static_cast<uint8_t>(irq));
    }
  }
}

void Pic::setEnabledLocked(uint8_t irq, bool enable) {
  m_IrqState.setEnabled(irq, enable);
  applyMaskLocked();
}

void Pic::enable(uint8_t irq, bool enable) {
  if (irq >= PicIrqState::LineCount) {
    return;
  }

  StateGuard guard(*this);
  if (!guard.owned())
    return;
  if (m_ShuttingDown && enable) {
    return;
  }
  setEnabledLocked(irq, enable);
  publishDiagnosticLineLocked(irq);
}
void Pic::enableAll(bool enable) {
  StateGuard guard(*this);
  if (!guard.owned()) {
    FATAL("PIC could not claim controller state for a global mask.");
    return;
  }
  m_IrqState.setAllEnabled(enable);
  applyMaskLocked();
  publishAllDiagnosticLinesLocked();
}
