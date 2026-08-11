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

#ifndef KERNEL_MACHINE_X86_COMMON_LOCAL_APIC_H
#define KERNEL_MACHINE_X86_COMMON_LOCAL_APIC_H

#if APIC

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/SchedulerTimer.h"
#include "pedigree/kernel/processor/InterruptHandler.h"
#include "pedigree/kernel/processor/MemoryMappedIo.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"

#include "LocalApicTimerHandlerSlots.h"
#include "LocalApicTlbShootdown.h"

class SchedulerTimerHandler;

#define IPI_TLB_SHOOTDOWN_VECTOR 0xF9
#define IPI_RESCHEDULE_VECTOR 0xFA
#define IPI_PROCESSOR_CONTROL_VECTOR 0xFB
#define ERROR_VECTOR 0xFC
#define SPURIOUS_VECTOR 0xFD
#define TIMER_VECTOR 0xFE

/** @addtogroup kernelmachinex86common
 * @{ */

/** The x86/x64 local APIC
 *\todo Initialise the Local APIC Timer */
class LocalApic : public SchedulerTimer, private InterruptHandler {
 private:
  enum class ProcessorControlState : size_t { Idle, Paused, Unavailable, Terminal };
  enum class ProcessorControlOwnership { Fresh, Quiesced, Failed, Terminal };

 public:
  /** The default constructor */
  inline LocalApic()
      : m_IoSpace("Local APIC"),
        m_Handlers(),
        m_BusFrequency(0),
        m_ProcessorControlOwner(),
        m_TlbMutations(),
        m_TlbTerminalFailure(),
        m_TlbShootdown(),
        m_ProcessorControlState(static_cast<size_t>(ProcessorControlState::Idle)),
        m_ControlledProcessorCount(0),
        m_TerminalProcessorCount(0),
        m_TerminalProcessorMask(0) {}
  /** The destructor */
  inline virtual ~LocalApic() {}

  /** Initialise the local APIC class. This includes allocating the I/O space.
   *\param[in] physicalAddress the physical address of the Local APIC (taken
   *from the SMP/ACPI tables) \return true, if the Local APIC class and this
   *processor's Local APIC have been initialised successfully, false otherwise
   */
  bool initialise(uint64_t physicalAddress) INITIALISATION_ONLY;
  /** Initialise the local APIC on the current processor
   *\return true, if this processor's Local APIC has been initialised
   *successfully. LINT0 is configured for the virtual-wire PIC role; LINT1 is
   *left to future firmware NMI-routing support. */
  bool initialiseProcessor() INITIALISATION_ONLY;

  /** Local APIC delivery modes */
  enum {
    deliveryModeFixed = 0,
    deliveryModeLowestPriority = 1,
    deliveryModeSmi = 2,
    deliveryModeNmi = 4,
    deliveryModeInit = 5,
    deliveryModeStartup = 6,
    deliveryModeExtInt = 7
  };

  /** Issue an IPI (= Interprocessor Interrupt)
   *\param[in] destinationApicId Identifier of the Local APIC of the
   *destination processor \param[in] vector the IPI vector \param[in]
   *deliveryMode the delivery mode \param[in] bAssert Assert? \param[in]
   *bLevelTriggered Level-triggered? \return true if the IPI was submitted
   *and left the delivery-pending state, false otherwise */
  MUST_USE_RESULT bool interProcessorInterrupt(uint8_t destinationApicId, uint8_t vector,
                                               size_t deliveryMode, bool bAssert,
                                               bool bLevelTriggered);

  /** Issue an IPI (= Interprocessor Interrupt) to all logical processors
   * except this one. (i.e. to all other cores). \param[in] vector The IPI
   * vector \param[in] deliveryMode The delivery mode \return true if the IPI
   * was submitted and left the delivery-pending state, false otherwise */
  MUST_USE_RESULT bool interProcessorInterruptAllExcludingThis(uint8_t vector, size_t deliveryMode);

  /**
   * Synchronously invalidate one address on every online processor.
   * See Processor::invalidateAll for the caller and failure contract.
   */
  MUST_USE_RESULT TlbInvalidationResult invalidateAllProcessors(void* address);

  /** Admit a page-table mutation before its first PTE write. */
  MUST_USE_RESULT TlbInvalidationResult beginTlbInvalidation(bool& global);

  /** Retire a page-table mutation admitted by beginTlbInvalidation. */
  void endTlbInvalidation();

  /** Elect the terminal failure coordinator before permanently closing admission. */
  MUST_USE_RESULT bool closeTlbInvalidationAdmissionForTerminalFailure(
      TlbInvalidationResult result);

  bool tlbInvalidationFailureActive() const;
  bool tlbInvalidationTerminal() const;

  /** Service a published shootdown without relying on maskable interrupts. */
  void servicePendingTlbShootdown();

  /** Join irreversible processor control without relying on maskable IPIs. */
  void servicePendingTerminalProcessorControl();

  enum class ProcessorControlResult {
    Success,
    InvalidState,
    SubmissionFailed,
    AcknowledgementTimedOut,
    DrainTimedOut
  };

  /** Temporarily pause every other processor until resume is requested. */
  MUST_USE_RESULT ProcessorControlResult quiesceAllOtherProcessors(size_t expectedProcessors);

  /** Resume processors paused by quiesceAllOtherProcessors. */
  MUST_USE_RESULT ProcessorControlResult resumeAllOtherProcessors();

  /** Halt every other processor and wait for each one to acknowledge that it
   * has entered the terminal halt path. */
  MUST_USE_RESULT ProcessorControlResult haltAllOtherProcessors(size_t expectedProcessors);

  /** Get the Local APIC Id for this processor
   *\return the Local APIC Id of this processor */
  uint8_t getId();

  //
  // SchedulerTimer interface
  //
  virtual bool registerHandler(SchedulerTimerHandler* handler) {
    // Logical Processor::id() is assigned after early BSP timer setup and
    // can change during topology construction. The LAPIC's raw physical
    // identifier is already stable on both sides of that transition.
    return m_Handlers.registerHandler(getId(), handler);
  }

  virtual bool removeHandler(SchedulerTimerHandler* handler) {
    return canRemoveHandlerInCurrentContext() && m_Handlers.removeHandler(getId(), handler);
  }

  void ack();

 private:
  /** The copy-constructor
   *\note NOT implemented */
  LocalApic(const LocalApic&);
  /** The assignment operator
   *\note NOT implemented */
  LocalApic& operator=(const LocalApic&);

  /** Check whether the local APIC is enabled and at the desired address
   *\param[in] physicalAddress the desired physical address
   *\return true, if the local APIC is enabled and at physicalAddress, false
   *otherwise */
  bool check(uint64_t physicalAddress) INITIALISATION_ONLY;

  /** Wait for the local interrupt-command register to become idle. */
  bool waitForIcrIdle();
  bool submitIcr(uint32_t high, uint32_t low);

  ProcessorControlState processorControlState() const;
  bool acquireProcessorControlOwner(bool acceptRetained, ProcessorControlOwnership& ownership);
  bool releaseProcessorControlOwner();
  ProcessorControlResult completeTerminalControl(size_t expectedProcessors);
  ProcessorControlResult retainTerminalControl(ProcessorControlResult result);
  bool acquireTlbShootdownBarrier();
  bool releaseTlbShootdownBarrier();
  bool adoptTerminalTlbShootdownBarrier();
  bool waitForTlbMutationDrain();
  bool waitForProcessorCount(size_t expectedProcessors);
  bool waitForTerminalProcessorCount(size_t expectedProcessors);
  bool waitForProcessorDrain();
  ProcessorControlResult retainFailedControl(ProcessorControlResult result);
  ProcessorControlResult releaseProcessorControlOrRetainFailure(ProcessorControlResult released,
                                                                ProcessorControlResult retained);
  ProcessorControlResult retainQuiescedControl(ProcessorControlResult result);
  ProcessorControlResult finishFailedQuiesce(ProcessorControlResult result);
  ProcessorControlResult unwindQuiesce(ProcessorControlResult failure);
  bool markTerminalProcessor(size_t processor);
  void enterTerminalProcessorControl() NORETURN;

  //
  // InterruptHandler interface
  //
  virtual void interrupt(size_t nInterruptNumber, InterruptState& state);

  /** The local APIC memory-mapped I/O space */
  MemoryMappedIo m_IoSpace;

  /** Atomically published timer handlers, tracked per processor. */
  LocalApicTimerHandlerSlots m_Handlers;

  /** System bus frequency, for setting up the initial timer counter. */
  size_t m_BusFrequency;

  /** Owner of the processor-control state and its mutation-gate closure. */
  LocalApicProcessorControlOwner m_ProcessorControlOwner;

  /** One allocation-free, synchronously acknowledged TLB transaction. */
  LocalApicTlbMutationGate m_TlbMutations;
  LocalApicTlbTerminalFailure m_TlbTerminalFailure;
  LocalApicTlbShootdown m_TlbShootdown;

  /** Shared state observed by CPUs inside the processor-control IPI. */
  Atomic<size_t> m_ProcessorControlState;

  /** CPUs currently paused or committed to the terminal halt path. */
  Atomic<size_t> m_ControlledProcessorCount;

  /** CPUs which have reached the permanent interrupt-disabled halt loop. */
  Atomic<size_t> m_TerminalProcessorCount;
  Atomic<uint64_t> m_TerminalProcessorMask;
};

/** @} */

#endif

#endif
