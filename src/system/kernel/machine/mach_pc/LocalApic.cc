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

#if APIC

#include "LocalApic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/SchedulerTimerDispatchCleanup.h"
#include "pedigree/kernel/machine/SchedulerTimerHandler.h"
#include "pedigree/kernel/processor/InterruptManager.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"

#include "LocalApicIcrTransaction.h"
#include "LocalApicLint0Policy.h"

#define LAPIC_REG_ID 0x0020
#define LAPIC_REG_VERSION 0x0030
#define LAPIC_REG_TASK_PRIORITY 0x0080
#define LAPIC_REG_PROCESSOR_PRIORITY 0x00A0
#define LAPIC_REG_EOI 0x00B0
#define LAPIC_REG_LOGICAL_DESTINATION 0x00D0
#define LAPIC_REG_DESTINATION_FORMAT 0x00E0
#define LAPIC_REG_SPURIOUS_INT 0x00F0
// NOTE ISR
// NOTE TMR
// NOTE IRR
#define LAPIC_REG_ERR_STATUS 0x0280
#define LAPIC_REG_INT_CMD_LOW 0x0300
#define LAPIC_REG_INT_CMD_HIGH 0x0310
#define LAPIC_REG_LVT_TIMER 0x0320
#define LAPIC_REG_LVT_THERMAL 0x0330
#define LAPIC_REG_LVT_PERFORMANCE 0x0340
#define LAPIC_REG_LVT_LINT0 0x0350
#define LAPIC_REG_LVT_LINT1 0x0360
#define LAPIC_REG_LVT_ERROR 0x0370
#define LAPIC_REG_INITIAL_COUNT 0x0380
#define LAPIC_REG_CURRENT_COUNT 0x0390
#define LAPIC_REG_DIVIDE_CONFIG 0x03E0

#define LAPIC_TIMER_PERIODIC 0x00020000
#define LAPIC_MASKED 0x00010000

/** Assume 1GHz bus speed, 128 divisor, gives 7812500 for one second delay.
    For 10ms delay, divide that by 100... */
#define INITIAL_COUNT_VALUE (78125 * 40)

/** 100 hz, as per the PIT which would do scheduling on non-MP builds. */
#define INITIAL_HZ 100

static constexpr size_t IcrDeliveryPollLimit = 100000;
static constexpr size_t ProcessorControlPollLimit = 10000000;
static constexpr size_t TlbShootdownPollLimit = 10000000;

bool LocalApic::initialise(uint64_t physicalAddress) {
  // Detect local APIC presence
  uint32_t eax, ebx, ecx, edx;
  Processor::cpuid(1, 0, eax, ebx, ecx, edx);
  if (((edx >> 9) & 0x01) != 0x01) {
    ERROR("Local APIC: No local APIC present");
    return false;
  }

  // Some checks
  if (check(physicalAddress) == false)
    return false;

  // Allocate the local APIC memory-mapped I/O space
  PhysicalMemoryManager& physicalMemoryManager = PhysicalMemoryManager::instance();
  if (physicalMemoryManager.allocateRegion(
          m_IoSpace, 1,
          PhysicalMemoryManager::continuous | PhysicalMemoryManager::nonRamMemory |
              PhysicalMemoryManager::force,
          VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write |
              VirtualAddressSpace::CacheDisable,
          physicalAddress) == false) {
    ERROR("Local APIC: Could not allocate the memory region");
    return false;
  }

  // Register the timer vector.
  if (!InterruptManager::instance().registerInterruptHandler(TIMER_VECTOR, this))
    return false;

  // Register the reversible/terminal processor-control IPI vector.
  if (!InterruptManager::instance().registerInterruptHandler(IPI_PROCESSOR_CONTROL_VECTOR, this))
    return false;

  // Register the synchronous shared-mapping TLB shootdown vector.
  if (!InterruptManager::instance().registerInterruptHandler(IPI_TLB_SHOOTDOWN_VECTOR, this))
    return false;

  // This vector is a directed scheduler prompt for work published by a
  // remote hard producer. It shares the timer's owner-qualified scheduler
  // callback but is never routed through the legacy PIC path.
  if (!InterruptManager::instance().registerInterruptHandler(IPI_RESCHEDULE_VECTOR, this))
    return false;

  return initialiseProcessor();
}

bool LocalApic::initialiseProcessor() {
  // Some checks
  if (check(m_IoSpace.physicalAddress()) == false)
    return false;

  // Enable the Local APIC and set the spurious interrupt vector
  uint32_t tmp = m_IoSpace.read32(LAPIC_REG_SPURIOUS_INT);
  m_IoSpace.write32((tmp & 0xFFFFFE00) | 0x100 | SPURIOUS_VECTOR, LAPIC_REG_SPURIOUS_INT);

  const uint64_t apicBase =
      Processor::readMachineSpecificRegister(LocalApicLint0Policy::ApicBaseMsr);
  tmp = m_IoSpace.read32(LAPIC_REG_LVT_LINT0);
  const uint32_t lint0 = LocalApicLint0Policy::configuredValue(tmp, apicBase);
  m_IoSpace.write32(lint0, LAPIC_REG_LVT_LINT0);
  const uint32_t lint0Readback = m_IoSpace.read32(LAPIC_REG_LVT_LINT0);
  if (!LocalApicLint0Policy::matchesRole(lint0Readback, apicBase)) {
    m_IoSpace.write32(lint0Readback | LocalApicLint0Policy::Masked, LAPIC_REG_LVT_LINT0);
    const uint32_t maskedReadback = m_IoSpace.read32(LAPIC_REG_LVT_LINT0);
    if (!(maskedReadback & LocalApicLint0Policy::Masked)) {
      FATAL("Local APIC: failed to mask an invalid LINT0 route");
    }
    FATAL("Local APIC: LINT0 virtual-wire routing did not latch");
  }

  // Set the task priority to 0
  tmp = m_IoSpace.read32(LAPIC_REG_TASK_PRIORITY);
  m_IoSpace.write32(tmp & 0xFFFFFF00, LAPIC_REG_TASK_PRIORITY);

  // No error-vector handler exists yet, so keep this source masked rather
  // than routing an interrupt that cannot be acknowledged safely.
  tmp = m_IoSpace.read32(LAPIC_REG_LVT_ERROR);
  m_IoSpace.write32((tmp & 0xFFFEEF00) | LAPIC_MASKED | ERROR_VECTOR, LAPIC_REG_LVT_ERROR);

  if (!m_BusFrequency) {
    // Divide by 16
    m_IoSpace.write32(0x3, LAPIC_REG_DIVIDE_CONFIG);

    // Set the maximum count so we can calculate the frequency without this
    // rolling over.
    m_IoSpace.write32(0xFFFFFFFF, LAPIC_REG_INITIAL_COUNT);

    // This should be approximately 10000 useconds (10 ms).
    for (size_t i = 0; i < 10000; ++i) {
      uint8_t a = 0;
      __asm__ __volatile__("outb %0, %1" ::"a"(a), "Nd"(0x80));
    }
    uint32_t out = m_IoSpace.read32(LAPIC_REG_CURRENT_COUNT);

    uint32_t ticks = 0xFFFFFFFFU - out;

    // We want the bus frequency to be in Hz (ticks/second).
    m_BusFrequency = ticks * 100U;
  }

  // Set the LVT timer register.
  m_IoSpace.write32(LAPIC_TIMER_PERIODIC | TIMER_VECTOR, LAPIC_REG_LVT_TIMER);

  // Initialise the intial-count register
  m_IoSpace.write32(m_BusFrequency / INITIAL_HZ, LAPIC_REG_INITIAL_COUNT);

  // Initialise the divisor register. (Divide by 16)
  m_IoSpace.write32(0x3, LAPIC_REG_DIVIDE_CONFIG);

  return true;
}

bool LocalApic::interProcessorInterrupt(uint8_t destinationApicId, uint8_t vector,
                                        size_t deliveryMode, bool bAssert, bool bLevelTriggered) {
  return submitIcr(destinationApicId << 24, vector | (deliveryMode << 8) |
                                                (bAssert ? (1 << 14) : 0) |
                                                (bLevelTriggered ? (1 << 15) : 0));
}

bool LocalApic::interProcessorInterruptAllExcludingThis(uint8_t vector, size_t deliveryMode) {
  return submitIcr(0, vector | (deliveryMode << 8) | (1 << 14) | (0x3 << 18));
}

TlbInvalidationResult LocalApic::invalidateAllProcessors(void* address) {
  const ExecutionContext context = Processor::executionContext();
  if (!LocalApicTlbShootdown::supportsContext(context)) {
    return TlbInvalidationResult::InvalidContext;
  }

  const size_t processorCount = Processor::getCount();
  const size_t processor = Processor::index();
  if (!processorCount || processorCount > LocalApicTlbShootdown::MaxProcessors ||
      processor >= processorCount) {
    return TlbInvalidationResult::UnsupportedTopology;
  }
  const bool terminalSelfOnly =
      processorControlState() == ProcessorControlState::Terminal &&
      LocalApicTlbShootdown::onlyCurrentProcessorServiceable(
          processorCount, m_TerminalProcessorCount.value());
  if (processorCount == 1 || terminalSelfOnly) {
    // Terminal processors have acknowledged their permanent CLI+HLT loop and
    // can never execute with a stale translation again. Processor::getCount()
    // intentionally remains the immutable discovered topology at shutdown.
    Processor::invalidate(address);
    return TlbInvalidationResult::Success;
  }

  if (!acquireTlbShootdownBarrier()) {
    return TlbInvalidationResult::SerialisationTimedOut;
  }

  // A terminal transition holds the owner until every peer has committed to
  // its permanent CLI+HLT loop. A caller which lost that race must recheck
  // after acquiring rather than publishing a request to processors which can
  // no longer acknowledge it.
  if (processorControlState() == ProcessorControlState::Terminal &&
      LocalApicTlbShootdown::onlyCurrentProcessorServiceable(
          processorCount, m_TerminalProcessorCount.value())) {
    Processor::invalidate(address);
    return m_TlbShootdown.release() ? TlbInvalidationResult::Success
                                    : TlbInvalidationResult::DrainTimedOut;
  }

  for (size_t poll = 0; !m_TlbShootdown.drained() && poll < TlbShootdownPollLimit; ++poll) {
    Processor::pause();
  }
  if (!m_TlbShootdown.drained()) {
    return TlbInvalidationResult::DrainTimedOut;
  }

  Processor::invalidate(address);
  if (!m_TlbShootdown.publish(reinterpret_cast<uintptr_t>(address), processor, processorCount)) {
    m_TlbShootdown.release();
    return TlbInvalidationResult::UnsupportedTopology;
  }

  if (!interProcessorInterruptAllExcludingThis(IPI_TLB_SHOOTDOWN_VECTOR, deliveryModeFixed)) {
    m_TlbShootdown.close();
    for (size_t poll = 0; !m_TlbShootdown.drained() && poll < TlbShootdownPollLimit; ++poll) {
      Processor::pause();
    }
    if (!m_TlbShootdown.drained()) {
      return TlbInvalidationResult::DrainTimedOut;
    }
    m_TlbShootdown.release();
    return TlbInvalidationResult::SubmissionFailed;
  }

  bool complete = false;
  for (size_t poll = 0; poll < TlbShootdownPollLimit; ++poll) {
    if (m_TlbShootdown.complete()) {
      complete = true;
      break;
    }
    Processor::pause();
  }

  m_TlbShootdown.close();
  for (size_t poll = 0; !m_TlbShootdown.drained() && poll < TlbShootdownPollLimit; ++poll) {
    Processor::pause();
  }
  if (!m_TlbShootdown.drained()) {
    return TlbInvalidationResult::DrainTimedOut;
  }
  if (!m_TlbShootdown.release()) {
    return TlbInvalidationResult::DrainTimedOut;
  }
  return complete ? TlbInvalidationResult::Success
                  : TlbInvalidationResult::AcknowledgementTimedOut;
}

TlbInvalidationResult LocalApic::beginTlbInvalidation(bool& global) {
  global = false;
  const ExecutionContext context = Processor::executionContext();
  if (!LocalApicTlbShootdown::supportsContext(context)) {
    return TlbInvalidationResult::InvalidContext;
  }

  const size_t processorCount = Processor::getCount();
  const size_t processor = Processor::index();
  if (!processorCount || processorCount > LocalApicTlbShootdown::MaxProcessors ||
      processor >= processorCount) {
    return TlbInvalidationResult::UnsupportedTopology;
  }

  for (size_t poll = 0; poll < TlbShootdownPollLimit; ++poll) {
    if (processorControlState() == ProcessorControlState::Terminal &&
        LocalApicTlbShootdown::onlyCurrentProcessorServiceable(
            processorCount, m_TerminalProcessorCount.value())) {
      return TlbInvalidationResult::Success;
    }
    if (m_TlbMutations.tryEnter()) {
      global = true;
      return TlbInvalidationResult::Success;
    }
    Processor::pause();
  }
  return TlbInvalidationResult::SerialisationTimedOut;
}

void LocalApic::endTlbInvalidation() {
  if (!m_TlbMutations.leave()) {
    FATAL("Local APIC TLB mutation admission underflow");
  }
}

void LocalApic::servicePendingTlbShootdown() {
  const size_t processor = Processor::index();
  LocalApicTlbShootdown::Service service;
  if (!m_TlbShootdown.beginService(processor, service)) {
    return;
  }

  Processor::invalidate(reinterpret_cast<void*>(service.address));
  m_TlbShootdown.finishService(service);
}

bool LocalApic::acquireTlbShootdownBarrier() {
  for (size_t poll = 0; poll < TlbShootdownPollLimit; ++poll) {
    if (m_TlbShootdown.tryAcquire()) {
      return true;
    }

    // The current processor can be part of the transaction which owns the
    // barrier, including while it holds an IRQ-disabling lock.
    servicePendingTlbShootdown();
    Processor::pause();
  }
  return false;
}

bool LocalApic::waitForTlbMutationDrain() {
  for (size_t poll = 0; poll < TlbShootdownPollLimit; ++poll) {
    if (m_TlbMutations.drained()) {
      return true;
    }
    servicePendingTlbShootdown();
    Processor::pause();
  }
  return m_TlbMutations.drained();
}

LocalApic::ProcessorControlState LocalApic::processorControlState() const {
  return static_cast<ProcessorControlState>(m_ProcessorControlState.value());
}

bool LocalApic::acquireProcessorControlOwner(
    bool acceptRetained, ProcessorControlOwnership& ownership) {
  ownership = ProcessorControlOwnership::Fresh;
  const size_t processor = Processor::index();
  if (processor >= LocalApicProcessorControlOwner::MaxProcessors) {
    return false;
  }

  if (acceptRetained && m_ProcessorControlOwner.claimQuiesced(processor)) {
    ownership = ProcessorControlOwnership::Quiesced;
    return true;
  }
  if (acceptRetained && m_ProcessorControlOwner.claimTerminal(processor)) {
    ownership = ProcessorControlOwnership::Terminal;
    return true;
  }

  for (size_t poll = 0; poll < ProcessorControlPollLimit; ++poll) {
    if (m_ProcessorControlOwner.tryAcquire(processor)) {
      return true;
    }
    if (acceptRetained && m_ProcessorControlOwner.claimQuiesced(processor)) {
      ownership = ProcessorControlOwnership::Quiesced;
      return true;
    }
    if (acceptRetained && m_ProcessorControlOwner.claimTerminal(processor)) {
      ownership = ProcessorControlOwnership::Terminal;
      return true;
    }
    Processor::pause();
  }
  return false;
}

bool LocalApic::releaseProcessorControlOwner() {
  return m_ProcessorControlOwner.release(Processor::index());
}

LocalApic::ProcessorControlResult LocalApic::completeTerminalControl(
    size_t expectedProcessors) {
  if (!waitForTerminalProcessorCount(expectedProcessors)) {
    // Make a timed-out terminal operation explicitly claimable only after
    // this invocation has stopped touching its barriers.
    if (!m_ProcessorControlOwner.markTerminal(Processor::index())) {
      return ProcessorControlResult::DrainTimedOut;
    }
    return ProcessorControlResult::AcknowledgementTimedOut;
  }

  const size_t processor = Processor::index();
  if (!m_ProcessorControlOwner.activeBy(processor)) {
    return ProcessorControlResult::InvalidState;
  }
  if (m_TlbShootdown.owned() && !m_TlbShootdown.release()) {
    return ProcessorControlResult::DrainTimedOut;
  }
  return releaseProcessorControlOwner() ? ProcessorControlResult::Success
                                        : ProcessorControlResult::DrainTimedOut;
}

bool LocalApic::waitForProcessorCount(size_t expectedProcessors) {
  for (size_t poll = 0; poll < ProcessorControlPollLimit; ++poll) {
    if (m_ControlledProcessorCount >= expectedProcessors)
      return true;
    Processor::pause();
  }
  return m_ControlledProcessorCount >= expectedProcessors;
}

bool LocalApic::waitForTerminalProcessorCount(size_t expectedProcessors) {
  for (size_t poll = 0; poll < ProcessorControlPollLimit; ++poll) {
    if (m_TerminalProcessorCount >= expectedProcessors)
      return true;
    Processor::pause();
  }
  return m_TerminalProcessorCount >= expectedProcessors;
}

bool LocalApic::waitForProcessorDrain() {
  for (size_t poll = 0; poll < ProcessorControlPollLimit; ++poll) {
    if (!m_ControlledProcessorCount)
      return true;
    Processor::pause();
  }
  return !m_ControlledProcessorCount;
}

LocalApic::ProcessorControlResult LocalApic::unwindQuiesce(ProcessorControlResult failure) {
  if (!m_ProcessorControlState.compareAndSwap(
          static_cast<size_t>(ProcessorControlState::Paused),
          static_cast<size_t>(ProcessorControlState::Unavailable))) {
    return ProcessorControlResult::InvalidState;
  }

  // The IPI transaction may have timed out after submission. Keep this
  // generation unavailable so a late interrupt cannot acknowledge a future
  // quiesce attempt.
  return waitForProcessorDrain() ? failure : ProcessorControlResult::DrainTimedOut;
}

LocalApic::ProcessorControlResult LocalApic::quiesceAllOtherProcessors(size_t expectedProcessors) {
  if (!expectedProcessors)
    return ProcessorControlResult::Success;

  ProcessorControlOwnership ownership = ProcessorControlOwnership::Fresh;
  if (!acquireProcessorControlOwner(false, ownership)) {
    return ProcessorControlResult::InvalidState;
  }
  if (processorControlState() != ProcessorControlState::Idle) {
    // Terminal is already safe and needs no retained owner. Any other state
    // is inconsistent without an existing owner, so preserve ownership and
    // fail closed rather than guessing who owns its gate closure.
    if (processorControlState() == ProcessorControlState::Terminal) {
      releaseProcessorControlOwner();
    } else if (!m_TlbMutations.closed()) {
      m_TlbMutations.close();
    }
    return ProcessorControlResult::InvalidState;
  }
  if (!waitForProcessorDrain()) {
    if (!m_ProcessorControlState.compareAndSwap(
            static_cast<size_t>(ProcessorControlState::Idle),
            static_cast<size_t>(ProcessorControlState::Unavailable))) {
      if (!m_TlbMutations.closed()) {
        m_TlbMutations.close();
      }
      return ProcessorControlResult::InvalidState;
    }
    // A stale control generation did not drain. Retain ownership and close
    // mapping admission so neither a new quiesce nor halt can reuse it.
    m_TlbMutations.close();
    return ProcessorControlResult::DrainTimedOut;
  }

  // Admission begins before the VAS lock and first PTE write. Closing it here
  // therefore drains both published shootdowns and the mutation-before-
  // publication window before any processor can be paused.
  if (!m_TlbMutations.close()) {
    return ProcessorControlResult::InvalidState;
  }
  if (!waitForTlbMutationDrain()) {
    // No processor-control state has changed, so a debugger which interrupted
    // an admitted mapper can safely cancel this close and resume that mapper.
    if (!m_TlbMutations.cancelClose()) {
      if (!m_TlbMutations.closed()) {
        m_TlbMutations.close();
      }
      return ProcessorControlResult::InvalidState;
    }
    return releaseProcessorControlOwner() ? ProcessorControlResult::DrainTimedOut
                                          : ProcessorControlResult::InvalidState;
  }

  // Do not interrupt a remote shootdown owner inside its synchronous wait.
  // Once this barrier is held, changing Idle to Paused closes the interval in
  // which a published generation can be stranded by the control IPI.
  if (!acquireTlbShootdownBarrier()) {
    if (!m_TlbMutations.reopen()) {
      return ProcessorControlResult::DrainTimedOut;
    }
    return releaseProcessorControlOwner() ? ProcessorControlResult::DrainTimedOut
                                          : ProcessorControlResult::InvalidState;
  }
  if (processorControlState() != ProcessorControlState::Idle) {
    // The control owner excludes legitimate state changes here. Keep the
    // mutation gate closed and ownership retained on any invariant failure.
    return m_TlbShootdown.release() ? ProcessorControlResult::InvalidState
                                    : ProcessorControlResult::DrainTimedOut;
  }
  if (!m_ProcessorControlState.compareAndSwap(static_cast<size_t>(ProcessorControlState::Idle),
                                              static_cast<size_t>(ProcessorControlState::Paused))) {
    return m_TlbShootdown.release() ? ProcessorControlResult::InvalidState
                                    : ProcessorControlResult::DrainTimedOut;
  }

  if (!interProcessorInterruptAllExcludingThis(IPI_PROCESSOR_CONTROL_VECTOR, deliveryModeFixed)) {
    const ProcessorControlResult result =
        unwindQuiesce(ProcessorControlResult::SubmissionFailed);
    return m_TlbShootdown.release() ? result : ProcessorControlResult::DrainTimedOut;
  }
  if (!waitForProcessorCount(expectedProcessors)) {
    const ProcessorControlResult result =
        unwindQuiesce(ProcessorControlResult::AcknowledgementTimedOut);
    return m_TlbShootdown.release() ? result : ProcessorControlResult::DrainTimedOut;
  }
  if (processorControlState() != ProcessorControlState::Paused) {
    return m_TlbShootdown.release() ? ProcessorControlResult::InvalidState
                                    : ProcessorControlResult::DrainTimedOut;
  }
  if (!m_TlbShootdown.release()) {
    return ProcessorControlResult::DrainTimedOut;
  }
  return m_ProcessorControlOwner.markQuiesced(Processor::index())
             ? ProcessorControlResult::Success
             : ProcessorControlResult::DrainTimedOut;
}

LocalApic::ProcessorControlResult LocalApic::resumeAllOtherProcessors() {
  const size_t processor = Processor::index();
  if (!m_ProcessorControlOwner.claimQuiesced(processor)) {
    return ProcessorControlResult::InvalidState;
  }
  if (!m_ProcessorControlState.compareAndSwap(
          static_cast<size_t>(ProcessorControlState::Paused),
          static_cast<size_t>(ProcessorControlState::Unavailable))) {
    return ProcessorControlResult::InvalidState;
  }
  if (!waitForProcessorDrain())
    return ProcessorControlResult::DrainTimedOut;
  if (!m_ProcessorControlState.compareAndSwap(
          static_cast<size_t>(ProcessorControlState::Unavailable),
          static_cast<size_t>(ProcessorControlState::Idle))) {
    return ProcessorControlResult::InvalidState;
  }
  if (!m_TlbMutations.reopen()) {
    if (!m_TlbMutations.closed()) {
      return releaseProcessorControlOwner() ? ProcessorControlResult::InvalidState
                                            : ProcessorControlResult::DrainTimedOut;
    }
    return ProcessorControlResult::DrainTimedOut;
  }
  return releaseProcessorControlOwner() ? ProcessorControlResult::Success
                                        : ProcessorControlResult::DrainTimedOut;
}

LocalApic::ProcessorControlResult LocalApic::haltAllOtherProcessors(size_t expectedProcessors) {
  if (!expectedProcessors)
    return ProcessorControlResult::Success;

  ProcessorControlOwnership ownership = ProcessorControlOwnership::Fresh;
  if (!acquireProcessorControlOwner(true, ownership)) {
    return ProcessorControlResult::InvalidState;
  }
  const bool retained = ownership == ProcessorControlOwnership::Quiesced;
  if (processorControlState() == ProcessorControlState::Terminal) {
    if (ownership == ProcessorControlOwnership::Quiesced) {
      return ProcessorControlResult::InvalidState;
    }
    return completeTerminalControl(expectedProcessors);
  }
  if (ownership == ProcessorControlOwnership::Terminal) {
    if (!m_TlbMutations.closed()) {
      m_TlbMutations.close();
    }
    return ProcessorControlResult::InvalidState;
  }

  bool barrierHeld = false;
  bool sendPauseIpi = false;
  for (size_t poll = 0; poll < ProcessorControlPollLimit; ++poll) {
    const ProcessorControlState state = processorControlState();
    if (state == ProcessorControlState::Terminal) {
      return completeTerminalControl(expectedProcessors);
    }
    if (state == ProcessorControlState::Idle && m_ControlledProcessorCount) {
      Processor::pause();
      continue;
    }

    if (!m_TlbMutations.closed() && !m_TlbMutations.close()) {
      Processor::pause();
      continue;
    }
    if (!waitForTlbMutationDrain()) {
      // Processor retirement is terminal. Keep admission closed on failure so
      // no mapping can start against a partially controlled processor set.
      return ProcessorControlResult::DrainTimedOut;
    }

    // Acquire before selecting Paused. A remote processor which already owns
    // a shootdown remains fully runnable until it has closed and released that
    // generation. Holding the owner from Paused through Terminal then excludes
    // every later publisher until no serviceable peer remains.
    if (!acquireTlbShootdownBarrier()) {
      return ProcessorControlResult::DrainTimedOut;
    }
    barrierHeld = true;

    const ProcessorControlState selectedState = processorControlState();
    if (selectedState == ProcessorControlState::Terminal) {
      return completeTerminalControl(expectedProcessors);
    }
    if (selectedState == ProcessorControlState::Paused) {
      if (!retained) {
        return m_TlbShootdown.release() ? ProcessorControlResult::InvalidState
                                        : ProcessorControlResult::DrainTimedOut;
      }
      sendPauseIpi = m_ControlledProcessorCount < expectedProcessors;
      break;
    }
    if (selectedState == ProcessorControlState::Idle ||
        selectedState == ProcessorControlState::Unavailable) {
      if (m_ProcessorControlState.compareAndSwap(
              static_cast<size_t>(selectedState),
              static_cast<size_t>(ProcessorControlState::Paused))) {
        sendPauseIpi = true;
        break;
      }
    }

    m_TlbShootdown.release();
    barrierHeld = false;
  }
  if (!barrierHeld) {
    // The only path which can exhaust the selection loop before closing the
    // gate is a stale controlled processor in Idle. Keep new mutations out
    // after that bounded wait while retaining the operation owner.
    if (!m_TlbMutations.closed()) {
      m_TlbMutations.close();
    }
    return ProcessorControlResult::DrainTimedOut;
  }

  const bool submitted =
      !sendPauseIpi ||
      interProcessorInterruptAllExcludingThis(IPI_PROCESSOR_CONTROL_VECTOR, deliveryModeFixed);
  if (!waitForProcessorCount(expectedProcessors)) {
    const ProcessorControlResult failure =
        submitted ? ProcessorControlResult::AcknowledgementTimedOut
                  : ProcessorControlResult::SubmissionFailed;
    const ProcessorControlResult result = unwindQuiesce(failure);
    return m_TlbShootdown.release() ? result : ProcessorControlResult::DrainTimedOut;
  }

  if (!m_ProcessorControlState.compareAndSwap(
          static_cast<size_t>(ProcessorControlState::Paused),
          static_cast<size_t>(ProcessorControlState::Terminal))) {
    return m_TlbShootdown.release() ? ProcessorControlResult::InvalidState
                                    : ProcessorControlResult::DrainTimedOut;
  }

  return completeTerminalControl(expectedProcessors);
}

bool LocalApic::waitForIcrIdle() {
  for (size_t poll = 0; poll < IcrDeliveryPollLimit; ++poll) {
    if ((m_IoSpace.read32(LAPIC_REG_INT_CMD_LOW) & 0x1000) == 0)
      return true;
    Processor::pause();
  }
  return (m_IoSpace.read32(LAPIC_REG_INT_CMD_LOW) & 0x1000) == 0;
}

bool LocalApic::submitIcr(uint32_t high, uint32_t low) {
  const LocalApicIcrTransaction transaction(Processor::getInterrupts());
  Processor::setInterrupts(false);

  bool submitted = waitForIcrIdle();
  if (submitted) {
    m_IoSpace.write32(high, LAPIC_REG_INT_CMD_HIGH);
    m_IoSpace.write32(low, LAPIC_REG_INT_CMD_LOW);
    submitted = waitForIcrIdle();
  }

  Processor::setInterrupts(transaction.restoreInterrupts());
  return submitted;
}

uint8_t LocalApic::getId() {
  return ((m_IoSpace.read32(LAPIC_REG_ID) >> 24) & 0xFF);
}

bool LocalApic::check(uint64_t physicalAddress) {
  // Check whether the Local APIC is enabled or not
  if ((Processor::readMachineSpecificRegister(0x1B) & 0x800) == 0) {
    ERROR("Local APIC: Disabled");
    return false;
  }

  // Check Local APIC base address
  if ((Processor::readMachineSpecificRegister(0x1B) & 0xFFFFFF000ULL) != physicalAddress) {
    ERROR("Local APIC: Wrong physical address");
    return false;
  }

  return true;
}

void LocalApic::interrupt(size_t nInterruptNumber, InterruptState& state) {
  if (nInterruptNumber == IPI_TLB_SHOOTDOWN_VECTOR) {
    ack();
    servicePendingTlbShootdown();
    return;
  }

  if (nInterruptNumber == TIMER_VECTOR || nInterruptNumber == IPI_RESCHEDULE_VECTOR) {
    // Ack early. timer() may schedule away and an IPI must never retain
    // its hard frame while the remote producer waits for progress.
    ack();

    // Load only after acknowledging the delivered vector. The slot is
    // qualified by the receiving physical processor's scheduler owner,
    // so an IPI can neither borrow another CPU's timer callback nor carry
    // an interrupted register frame into a bottom-half callback.
    SchedulerTimerHandlerSlot::DispatchGuard dispatch;
    if (LIKELY(m_Handlers.beginDispatch(getId(), dispatch))) {
      SchedulerTimerDispatchCleanup dispatchCleanup(dispatch);
      // TODO: Delta is wrong.
      ExecutionContextGuard schedulerContext(ExecutionContext::SchedulerIrq);
      dispatch.handler()->timer(0, state);
    }
    return;
  }

  // This IPI temporarily pauses processors for the debugger or permanently
  // halts them for panic and shutdown paths.
  if (nInterruptNumber == IPI_PROCESSOR_CONTROL_VECTOR) {
    ack();
    Processor::setInterrupts(false);
    m_ControlledProcessorCount += 1;
    while (true) {
      const ProcessorControlState controlState = processorControlState();
      if (controlState == ProcessorControlState::Idle ||
          controlState == ProcessorControlState::Unavailable) {
        m_ControlledProcessorCount -= 1;
        return;
      }
      if (controlState == ProcessorControlState::Terminal) {
        m_TerminalProcessorCount += 1;
        while (true)
          Processor::halt();
      }
      Processor::pause();
    }
  }
}

void LocalApic::ack() {
  // Send EOI.
  m_IoSpace.write32(0x00000000, LAPIC_REG_EOI);
}

#endif
