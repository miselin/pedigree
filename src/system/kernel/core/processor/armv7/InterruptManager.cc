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

#include "InterruptManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Serial.h"
#include "pedigree/kernel/machine/types.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/utility.h"
#include "pedigree/kernel/debugger/Debugger.h"
#include "pedigree/kernel/Log.h"

#define SYSCALL_INTERRUPT_NUMBER 8
#define BREAKPOINT_INTERRUPT_NUMBER 9

const char *g_ExceptionNames[32] = {
    "Interrupt",
    "TLB modification exception",
    "TLB exception (load or instruction fetch)",
    "TLB exception (store)",
    "Address error exception (load or instruction fetch)",
    "Address error exception (store)",
    "Bus error exception (instruction fetch)",
    "Bus error exception (data: load or store)",
    "Syscall exception",
    "Breakpoint exception",
    "Reserved instruction exception",
    "Coprocessor unusable exception",
    "Arithmetic overflow exception",
    "Trap exception",
    "LDCz/SDCz to uncached address",
    "Virtual coherency exception",
    "Machine check exception",
    "Floating point exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Watchpoint exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
};

ARMV7InterruptManager ARMV7InterruptManager::m_Instance;

MemoryRegion ARMV7InterruptManager::m_MPUINTCRegion("mpu-intc");

SyscallManager &SyscallManager::instance()
{
    return ARMV7InterruptManager::instance();
}
InterruptManager &InterruptManager::instance()
{
    return ARMV7InterruptManager::instance();
}

bool ARMV7InterruptManager::registerInterruptHandler(
    size_t interruptNumber, InterruptHandler *handler)
{
    /// \todo This is very machine-specific...
    volatile uint32_t *mpuIntcRegisters = reinterpret_cast<volatile uint32_t *>(
        ARMV7InterruptManager::m_MPUINTCRegion.virtualAddress());
    if (!mpuIntcRegisters)
        return false;

    /// \todo Needs locking
    if (UNLIKELY(interruptNumber >= 96))
        return false;
    if (UNLIKELY(handler != 0 && m_Handler[interruptNumber] != 0))
        return false;
    if (UNLIKELY(handler == 0 && m_Handler[interruptNumber] == 0))
        return false;

    // Unmask this interrupt
    size_t n = (interruptNumber % 96) / 32;
    mpuIntcRegisters[INTCPS_MIR_CLEAR + (n * 8)] = 1 << (interruptNumber % 32);

    m_Handler[interruptNumber] = handler;
    return true;
}

#if DEBUGGER

bool ARMV7InterruptManager::registerInterruptHandlerDebugger(
    size_t interruptNumber, InterruptHandler *handler)
{
    volatile uint32_t *mpuIntcRegisters = reinterpret_cast<volatile uint32_t *>(
        ARMV7InterruptManager::m_MPUINTCRegion.virtualAddress());
    if (!mpuIntcRegisters)
        return false;

    /// \todo Needs locking
    if (UNLIKELY(interruptNumber >= 96))
        return false;
    if (UNLIKELY(handler != 0 && m_DbgHandler[interruptNumber] != 0))
        return false;
    if (UNLIKELY(handler == 0 && m_DbgHandler[interruptNumber] == 0))
        return false;

    // Unmask this interrupt
    size_t n = (interruptNumber % 96) / 32;
    mpuIntcRegisters[INTCPS_MIR_CLEAR + (n * 8)] = 1 << (interruptNumber % 32);

    m_DbgHandler[interruptNumber] = handler;
    return true;
}
size_t ARMV7InterruptManager::getBreakpointInterruptNumber()
{
    return 3;
}
size_t ARMV7InterruptManager::getDebugInterruptNumber()
{
    return 1;
}

#endif

bool ARMV7InterruptManager::registerSyscallHandler(
    Service_t Service, SyscallHandler *handler)
{
    // TODO: Needs locking

    if (UNLIKELY(Service >= serviceEnd))
        return false;
    if (UNLIKELY(handler != 0 && m_SyscallHandler[Service] != 0))
        return false;
    if (UNLIKELY(handler == 0 && m_SyscallHandler[Service] == 0))
        return false;

    m_SyscallHandler[Service] = handler;
    return true;
}

uintptr_t ARMV7InterruptManager::syscall(
    Service_t service, uintptr_t function, uintptr_t p1, uintptr_t p2,
    uintptr_t p3, uintptr_t p4, uintptr_t p5)
{
    /// \todo Software interrupt
    return 0;
}

// Handles data aborts, but with a stack frame.
void kdata_abort(InterruptState &state) NORETURN;
void kdata_abort(InterruptState &state)
{
#if DEBUGGER
    // Grab the aborted address.
    uintptr_t dfar = 0;
    uintptr_t dfsr = 0;
    asm volatile("MRC p15,0,%0,c6,c0,0" : "=r"(dfar));
    asm volatile("MRC p15,0,%0,c5,c0,0" : "=r"(dfsr));

    // Status.
    bool bWrite = !!(dfsr & (1 << 11));
    uint32_t status = (dfsr & (1 << 10) ? 1 << 4 : 0) | (dfsr & 0xF);

    static LargeStaticString sError;
    sError.clear();
    sError.append("Data Abort: ");
    if (bWrite)
        sError.append("W ");
    else
        sError.append("R ");
    sError.append("0x");
    sError.append(dfar, 16, 8, '0');
    sError.append(" @ 0x");
    sError.append(state.getInstructionPointer(), 16, 8, '0');
    sError.append("\n");

    switch (status)
    {
        case 0b00001:
            sError.append("Alignment fault");
            break;
        case 0b00101:
        case 0b00111:
            sError.append("Translation fault");
            break;
        case 0b00011:
        case 0b00110:
            sError.append("Access flag fault");
            break;
        case 0b01001:
        case 0b01011:
        {
            uint32_t domain = (dfsr >> 4) & 0xF;
            sError.append("Domain fault for domain 0x");
            sError.append(domain, 16, 8, '0');
            break;
        }
        case 0b01101:
        case 0b01111:
            sError.append("Permission fault");
            break;
        case 0b00010:
            sError.append("Debug event");
            break;
        case 0b01000:
            sError.append("Sync external abort");
            break;
        case 0b10110:
            sError.append("Async external abort");
            break;
        default:
            sError.append("Unknown fault");
    }

    Machine::instance().getSerial(0)->write(sError);
    ERROR_NOLOCK(static_cast<const char *>(sError));
    Debugger::instance().start(state, sError);
#else
    panic("data abort");
#endif

    while (1)
        ;
}

void kprefetch_abort(InterruptState &state) NORETURN;
void kprefetch_abort(InterruptState &state)
{
#if DEBUGGER
    static LargeStaticString sError;
    sError.clear();
    sError.append("Prefetch Abort at 0x");
    sError.append(state.getInstructionPointer(), 16, 8, '0');
    Debugger::instance().start(state, sError);
#endif

    while (1)
        ;
}

void kswi_handler(InterruptState &state)
{
    // Grab SWI number.
    const uint32_t *at =
        reinterpret_cast<const uint32_t *>(state.getInstructionPointer() - 4);
    uint32_t swi = (*at) & 0xFFFFFFUL;

    NOTICE("swi #" << Hex << swi);
    if (swi == 0xdeee)
    {
        // Dump state.
        for (size_t i = 0; i < state.getRegisterCount(); ++i)
        {
            NOTICE(
                state.getRegisterName(i) << "=" << Hex << state.getRegister(i));
        }
    }
#if DEBUGGER
    else if (swi == 0xdeb16)
    {
        static LargeStaticString sError;
        sError.clear();
        sError.append("Debugger Trap at 0x");
        sError.append(state.getInstructionPointer(), 16, 8, '0');
        Debugger::instance().start(state, sError);
        return;
    }
#endif
}

/// \note We don't use the interrupt attribute for everything as we do a lot of
///       the register saving ourselves before jumping into the kernel.
extern "C" void arm_swint_handler(InterruptState &state);
extern "C" void arm_instundef_handler() __attribute__((naked));
extern "C" void arm_fiq_handler() __attribute__((interrupt("FIQ"))) NORETURN;
extern "C" void arm_irq_handler(InterruptState &state);
extern "C" void arm_reset_handler() __attribute__((naked));
extern "C" void arm_prefetch_abort_handler(InterruptState &state)
    __attribute__((naked));
extern "C" void arm_data_abort_handler(InterruptState &state)
    __attribute__((naked));
extern "C" void arm_addrexcept_handler() __attribute__((naked));

extern "C" void arm_swint_handler(InterruptState &state)
{
    kswi_handler(state);
}

extern "C" void arm_instundef_handler()
{
    NOTICE_NOLOCK("undefined instruction");
    while (1)
        ;
}

extern "C" void arm_fiq_handler()
{
    NOTICE_NOLOCK("FIQ");
    while (1)
        ;
}

extern "C" void arm_irq_handler(InterruptState &state)
{
    ARMV7InterruptManager::interrupt(state);
}

extern "C" void arm_reset_handler()
{
    NOTICE_NOLOCK("reset");
    while (1)
        ;
}

extern "C" void arm_prefetch_abort_handler(InterruptState &state)
{
    kprefetch_abort(state);
    while (1)
        ;
}

extern "C" void arm_data_abort_handler(InterruptState &state)
{
    kdata_abort(state);
}

extern "C" void arm_addrexcept_handler()
{
    NOTICE_NOLOCK("address exception");
    while (1)
        ;
}

extern uint32_t __arm_vector_table;
extern uint32_t __end_arm_vector_table;
void ARMV7InterruptManager::initialiseProcessor()
{
    // Map in the MPU interrupt controller
    if (!PhysicalMemoryManager::instance().allocateRegion(
            ARMV7InterruptManager::m_MPUINTCRegion, 1,
            PhysicalMemoryManager::continuous,
            VirtualAddressSpace::Write | VirtualAddressSpace::KernelMode,
            0x48200000))
        return;

    // Use our custom IVT
    __asm__ __volatile__ ("mcr p15, #0, %0, c12, c0, #0" :: "r" (&__arm_vector_table));

    // Initialise the MPU INTC
    volatile uint32_t *mpuIntcRegisters = reinterpret_cast<volatile uint32_t *>(
        ARMV7InterruptManager::m_MPUINTCRegion.virtualAddress());

    // Perform a reset of the MPU INTC
    mpuIntcRegisters[INTCPS_SYSCONFIG] = 2;
    while ((mpuIntcRegisters[INTCPS_SYSSTATUS] & 1) == 0)
        ;

    // Write the MMIO address and hardware revisio to the console
    uint32_t revision = mpuIntcRegisters[0];
    NOTICE(
        "MPU interrupt controller at "
        << Hex
        << reinterpret_cast<uintptr_t>(
               ARMV7InterruptManager::m_MPUINTCRegion.virtualAddress())
        << "  - revision " << Dec << ((revision >> 4) & 0xF) << "."
        << (revision & 0xF) << Hex);

    // Set up the functional clock auto-idle and the synchronizer clock
    // auto-gating
    mpuIntcRegisters[INTCPS_IDLE] = 0;

    // Program the base priority and enable FIQs where necessary for all IRQs
    for (size_t m = 0; m < 96; m++)
    {
        // Priority: 0 (highest), route to IRQ (not FIQ)
        mpuIntcRegisters[INTCPS_ILR + m] = 0;
    }

    // Mask all interrupts (when an interrupt line is registered, it will be
    // unmasked)
    for (size_t n = 0; n < 3; n++)
    {
        mpuIntcRegisters[INTCPS_MIR_SET + (n * 8)] = 0xFFFFFFFF;
        mpuIntcRegisters[INTCPS_ISR_CLEAR + (n * 8)] = 0xFFFFFFFF;
    }

    // Disable the priority threshold
    mpuIntcRegisters[INTCPS_THRESHOLD] = 0xFF;

    // Reset IRQ and FIQ output in case any are pending
    mpuIntcRegisters[INTCPS_CONTROL] = 3;
}

void ARMV7InterruptManager::interrupt(InterruptState &interruptState)
{
    volatile uint32_t *mpuIntcRegisters = reinterpret_cast<volatile uint32_t *>(
        ARMV7InterruptManager::m_MPUINTCRegion.virtualAddress());
    if (!mpuIntcRegisters)
        return;

    // Grab the interrupt number
    size_t intNumber = mpuIntcRegisters[INTCPS_SIR_IRQ] & 0x7F;

#if DEBUGGER
    // Call the kernel debugger's handler, if any
    if (m_Instance.m_DbgHandler[intNumber] != 0)
        m_Instance.m_DbgHandler[intNumber]->interrupt(
            intNumber, interruptState);
#endif

    // Call the interrupt handler if one exists
    if (m_Instance.m_Handler[intNumber] != 0)
        m_Instance.m_Handler[intNumber]->interrupt(intNumber, interruptState);

    // Ack the interrupt
    mpuIntcRegisters[INTCPS_CONTROL] =
        1;  // Reset IRQ output and enable new IRQs
}

ARMV7InterruptManager::ARMV7InterruptManager()
{
    // Initialise the pointers to the interrupt handler
    for (size_t i = 0; i < 256; i++)
    {
        m_Handler[i] = 0;
#if DEBUGGER
        m_DbgHandler[i] = 0;
#endif
    }

    // Initialise the pointers to the syscall handler
    for (size_t i = 0; i < serviceEnd; i++)
        m_SyscallHandler[i] = 0;
}
ARMV7InterruptManager::~ARMV7InterruptManager()
{
}
