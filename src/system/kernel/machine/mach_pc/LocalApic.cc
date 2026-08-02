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
#include "pedigree/kernel/machine/SchedulerTimerHandler.h"
#include "pedigree/kernel/processor/InterruptManager.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"

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

bool LocalApic::initialise(uint64_t physicalAddress)
{
    // Detect local APIC presence
    uint32_t eax, ebx, ecx, edx;
    Processor::cpuid(1, 0, eax, ebx, ecx, edx);
    if (((edx >> 9) & 0x01) != 0x01)
    {
        ERROR("Local APIC: No local APIC present");
        return false;
    }

    // Some checks
    if (check(physicalAddress) == false)
        return false;

    // Allocate the local APIC memory-mapped I/O space
    PhysicalMemoryManager &physicalMemoryManager =
        PhysicalMemoryManager::instance();
    if (physicalMemoryManager.allocateRegion(
            m_IoSpace, 1,
            PhysicalMemoryManager::continuous |
                PhysicalMemoryManager::nonRamMemory |
                PhysicalMemoryManager::force,
            VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write |
                VirtualAddressSpace::CacheDisable,
            physicalAddress) == false)
    {
        ERROR("Local APIC: Could not allocate the memory region");
        return false;
    }

    // Register the timer vector.
    if (!InterruptManager::instance().registerInterruptHandler(
            TIMER_VECTOR, this))
        return false;

    // Register the reversible/terminal processor-control IPI vector.
    if (!InterruptManager::instance().registerInterruptHandler(
            IPI_PROCESSOR_CONTROL_VECTOR, this))
        return false;

    return initialiseProcessor();
}

bool LocalApic::initialiseProcessor()
{
    // Some checks
    if (check(m_IoSpace.physicalAddress()) == false)
        return false;

    // Enable the Local APIC and set the spurious interrupt vector
    uint32_t tmp = m_IoSpace.read32(LAPIC_REG_SPURIOUS_INT);
    m_IoSpace.write32(
        (tmp & 0xFFFFFE00) | 0x100 | SPURIOUS_VECTOR, LAPIC_REG_SPURIOUS_INT);

    // Set the task priority to 0
    tmp = m_IoSpace.read32(LAPIC_REG_TASK_PRIORITY);
    m_IoSpace.write32(tmp & 0xFFFFFF00, LAPIC_REG_TASK_PRIORITY);

    // No error-vector handler exists yet, so keep this source masked rather
    // than routing an interrupt that cannot be acknowledged safely.
    tmp = m_IoSpace.read32(LAPIC_REG_LVT_ERROR);
    m_IoSpace.write32(
        (tmp & 0xFFFEEF00) | LAPIC_MASKED | ERROR_VECTOR,
        LAPIC_REG_LVT_ERROR);

    if (!m_BusFrequency)
    {
        // Divide by 16
        m_IoSpace.write32(0x3, LAPIC_REG_DIVIDE_CONFIG);

        // Set the maximum count so we can calculate the frequency without this
        // rolling over.
        m_IoSpace.write32(0xFFFFFFFF, LAPIC_REG_INITIAL_COUNT);

        // This should be approximately 10000 useconds (10 ms).
        for (size_t i = 0; i < 10000; ++i)
        {
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

    // TODO

    return true;
}

bool LocalApic::interProcessorInterrupt(
    uint8_t destinationApicId, uint8_t vector, size_t deliveryMode,
    bool bAssert, bool bLevelTriggered)
{
    if (!acquireIcrSubmission())
        return false;
    if (!waitForIcrIdle())
    {
        m_IcrSubmissionActive = false;
        return false;
    }

    m_IoSpace.write32(destinationApicId << 24, LAPIC_REG_INT_CMD_HIGH);
    m_IoSpace.write32(
        vector | (deliveryMode << 8) | (bAssert ? (1 << 14) : 0) |
            (bLevelTriggered ? (1 << 15) : 0),
        LAPIC_REG_INT_CMD_LOW);
    const bool delivered = waitForIcrIdle();
    m_IcrSubmissionActive = false;
    return delivered;
}

bool LocalApic::interProcessorInterruptAllExcludingThis(
    uint8_t vector, size_t deliveryMode)
{
    if (!acquireIcrSubmission())
        return false;
    if (!waitForIcrIdle())
    {
        m_IcrSubmissionActive = false;
        return false;
    }

    m_IoSpace.write32(
        vector | (deliveryMode << 8) | (1 << 14) | (0x3 << 18),
        LAPIC_REG_INT_CMD_LOW);
    const bool delivered = waitForIcrIdle();
    m_IcrSubmissionActive = false;
    return delivered;
}

LocalApic::ProcessorControlState LocalApic::processorControlState() const
{
    return static_cast<ProcessorControlState>(m_ProcessorControlState.value());
}

bool LocalApic::waitForProcessorCount(size_t expectedProcessors)
{
    for (size_t poll = 0; poll < ProcessorControlPollLimit; ++poll)
    {
        if (m_ControlledProcessorCount >= expectedProcessors)
            return true;
        Processor::pause();
    }
    return m_ControlledProcessorCount >= expectedProcessors;
}

bool LocalApic::waitForTerminalProcessorCount(size_t expectedProcessors)
{
    for (size_t poll = 0; poll < ProcessorControlPollLimit; ++poll)
    {
        if (m_TerminalProcessorCount >= expectedProcessors)
            return true;
        Processor::pause();
    }
    return m_TerminalProcessorCount >= expectedProcessors;
}

bool LocalApic::waitForProcessorDrain()
{
    for (size_t poll = 0; poll < ProcessorControlPollLimit; ++poll)
    {
        if (!m_ControlledProcessorCount)
            return true;
        Processor::pause();
    }
    return !m_ControlledProcessorCount;
}

LocalApic::ProcessorControlResult
LocalApic::unwindQuiesce(ProcessorControlResult failure)
{
    if (!m_ProcessorControlState.compareAndSwap(
            static_cast<size_t>(ProcessorControlState::Paused),
            static_cast<size_t>(ProcessorControlState::Unavailable)))
    {
        return ProcessorControlResult::InvalidState;
    }

    // The IPI transaction may have timed out after submission. Keep this
    // generation unavailable so a late interrupt cannot acknowledge a future
    // quiesce attempt.
    return waitForProcessorDrain() ? failure
                                   : ProcessorControlResult::DrainTimedOut;
}

LocalApic::ProcessorControlResult
LocalApic::quiesceAllOtherProcessors(size_t expectedProcessors)
{
    if (!expectedProcessors)
        return ProcessorControlResult::Success;
    if (processorControlState() != ProcessorControlState::Idle)
        return ProcessorControlResult::InvalidState;
    if (!waitForProcessorDrain())
    {
        if (!m_ProcessorControlState.compareAndSwap(
                static_cast<size_t>(ProcessorControlState::Idle),
                static_cast<size_t>(ProcessorControlState::Unavailable)))
        {
            return ProcessorControlResult::InvalidState;
        }
        return ProcessorControlResult::DrainTimedOut;
    }
    if (!m_ProcessorControlState.compareAndSwap(
            static_cast<size_t>(ProcessorControlState::Idle),
            static_cast<size_t>(ProcessorControlState::Paused)))
    {
        return ProcessorControlResult::InvalidState;
    }

    if (!interProcessorInterruptAllExcludingThis(
            IPI_PROCESSOR_CONTROL_VECTOR, deliveryModeFixed))
    {
        return unwindQuiesce(ProcessorControlResult::SubmissionFailed);
    }
    if (!waitForProcessorCount(expectedProcessors))
        return unwindQuiesce(ProcessorControlResult::AcknowledgementTimedOut);
    if (processorControlState() != ProcessorControlState::Paused)
        return ProcessorControlResult::InvalidState;
    return ProcessorControlResult::Success;
}

LocalApic::ProcessorControlResult LocalApic::resumeAllOtherProcessors()
{
    if (!m_ProcessorControlState.compareAndSwap(
            static_cast<size_t>(ProcessorControlState::Paused),
            static_cast<size_t>(ProcessorControlState::Unavailable)))
    {
        return ProcessorControlResult::InvalidState;
    }
    if (!waitForProcessorDrain())
        return ProcessorControlResult::DrainTimedOut;
    if (!m_ProcessorControlState.compareAndSwap(
            static_cast<size_t>(ProcessorControlState::Unavailable),
            static_cast<size_t>(ProcessorControlState::Idle)))
    {
        return ProcessorControlResult::InvalidState;
    }
    return ProcessorControlResult::Success;
}

LocalApic::ProcessorControlResult
LocalApic::haltAllOtherProcessors(size_t expectedProcessors)
{
    if (!expectedProcessors)
        return ProcessorControlResult::Success;

    bool sendIpi = false;
    bool stateSelected = false;
    for (size_t poll = 0; poll < ProcessorControlPollLimit; ++poll)
    {
        const ProcessorControlState state = processorControlState();
        if (state == ProcessorControlState::Terminal)
        {
            sendIpi = m_TerminalProcessorCount < expectedProcessors;
            stateSelected = true;
            break;
        }
        if (state == ProcessorControlState::Idle)
        {
            if (m_ControlledProcessorCount)
            {
                Processor::pause();
                continue;
            }
            if (!m_ProcessorControlState.compareAndSwap(
                    static_cast<size_t>(ProcessorControlState::Idle),
                    static_cast<size_t>(ProcessorControlState::Terminal)))
            {
                continue;
            }
            sendIpi = true;
            stateSelected = true;
            break;
        }
        if (state == ProcessorControlState::Unavailable)
        {
            if (!m_ProcessorControlState.compareAndSwap(
                    static_cast<size_t>(ProcessorControlState::Unavailable),
                    static_cast<size_t>(ProcessorControlState::Terminal)))
            {
                continue;
            }
            sendIpi = true;
            stateSelected = true;
            break;
        }
        if (m_ProcessorControlState.compareAndSwap(
                static_cast<size_t>(ProcessorControlState::Paused),
                static_cast<size_t>(ProcessorControlState::Terminal)))
        {
            sendIpi = m_ControlledProcessorCount < expectedProcessors;
            stateSelected = true;
            break;
        }
        Processor::pause();
    }
    if (!stateSelected)
        return ProcessorControlResult::DrainTimedOut;

    if (sendIpi)
    {
        if (!interProcessorInterruptAllExcludingThis(
                IPI_PROCESSOR_CONTROL_VECTOR, deliveryModeFixed))
        {
            return waitForTerminalProcessorCount(expectedProcessors)
                       ? ProcessorControlResult::Success
                       : ProcessorControlResult::SubmissionFailed;
        }
    }

    return waitForTerminalProcessorCount(expectedProcessors)
               ? ProcessorControlResult::Success
               : ProcessorControlResult::AcknowledgementTimedOut;
}

bool LocalApic::waitForIcrIdle()
{
    for (size_t poll = 0; poll < IcrDeliveryPollLimit; ++poll)
    {
        if ((m_IoSpace.read32(LAPIC_REG_INT_CMD_LOW) & 0x1000) == 0)
            return true;
        Processor::pause();
    }
    return (m_IoSpace.read32(LAPIC_REG_INT_CMD_LOW) & 0x1000) == 0;
}

bool LocalApic::acquireIcrSubmission()
{
    for (size_t poll = 0; poll < IcrDeliveryPollLimit; ++poll)
    {
        if (m_IcrSubmissionActive.compareAndSwap(false, true))
            return true;
        Processor::pause();
    }
    return m_IcrSubmissionActive.compareAndSwap(false, true);
}

uint8_t LocalApic::getId()
{
    return ((m_IoSpace.read32(LAPIC_REG_ID) >> 24) & 0xFF);
}

bool LocalApic::check(uint64_t physicalAddress)
{
    // Check whether the Local APIC is enabled or not
    if ((Processor::readMachineSpecificRegister(0x1B) & 0x800) == 0)
    {
        ERROR("Local APIC: Disabled");
        return false;
    }

    // Check Local APIC base address
    if ((Processor::readMachineSpecificRegister(0x1B) & 0xFFFFFF000ULL) !=
        physicalAddress)
    {
        ERROR("Local APIC: Wrong physical address");
        return false;
    }

    return true;
}

void LocalApic::interrupt(size_t nInterruptNumber, InterruptState &state)
{
    if (nInterruptNumber == TIMER_VECTOR)
    {
        // Ack early, timer() may not return for quite some time (if it
        // schedules).
        ack();

        SchedulerTimerHandler *handler = m_Handlers.lookup(Processor::id());
        // TODO: Delta is wrong.
        if (LIKELY(handler != 0))
        {
            // NOTICE("Timer " << Processor::id());
            handler->timer(0, state);
        }
    }

    // This IPI temporarily pauses processors for the debugger or permanently
    // halts them for panic and shutdown paths.
    if (nInterruptNumber == IPI_PROCESSOR_CONTROL_VECTOR)
    {
        ack();
        Processor::setInterrupts(false);
        m_ControlledProcessorCount += 1;
        while (true)
        {
            const ProcessorControlState controlState = processorControlState();
            if (controlState == ProcessorControlState::Idle ||
                controlState == ProcessorControlState::Unavailable)
            {
                m_ControlledProcessorCount -= 1;
                return;
            }
            if (controlState == ProcessorControlState::Terminal)
            {
                m_TerminalProcessorCount += 1;
                while (true)
                    Processor::halt();
            }
            Processor::pause();
        }
    }
}

void LocalApic::ack()
{
    // Send EOI.
    m_IoSpace.write32(0x00000000, LAPIC_REG_EOI);
}

#endif
