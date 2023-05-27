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
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/debugger/Debugger.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/openfirmware/Device.h"
#include "pedigree/kernel/machine/openfirmware/OpenFirmware.h"
#include "pedigree/kernel/machine/types.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/utilities/utility.h"

extern "C" int isr_reset;
extern "C" int isr_machine_check;
extern "C" int isr_dsi;
extern "C" int isr_isi;
extern "C" int isr_interrupt;
extern "C" int isr_alignment;
extern "C" int isr_program;
extern "C" int isr_fpu;
extern "C" int isr_decrementer;
extern "C" int isr_sc;
extern "C" int isr_trace;
extern "C" int isr_perf_mon;
extern "C" int isr_instr_breakpoint;
extern "C" int isr_system_management;
extern "C" int isr_thermal_management;

const char *g_pExceptions[] = {"System reset",
                               "Machine check",
                               "DSI",
                               "ISI",
                               "External interrupt",
                               "Alignment",
                               "Program",
                               "Floating-point unavailable",
                               "Decrementer",
                               "System call",
                               "Trace",
                               "Performance monitor",
                               "Instruction address breakpoint",
                               "System management interrupt",
                               "Thermal management interrupt"};

PPC32InterruptManager PPC32InterruptManager::m_Instance;

SyscallManager &SyscallManager::instance()
{
    return PPC32InterruptManager::instance();
}
InterruptManager &InterruptManager::instance()
{
    return PPC32InterruptManager::instance();
}

bool PPC32InterruptManager::registerInterruptHandler(
    size_t interruptNumber, InterruptHandler *pHandler)
{
    LockGuard<Spinlock> lockGuard(m_Lock);

    if (UNLIKELY(
            interruptNumber >= 256 ||
            interruptNumber == SYSCALL_INTERRUPT_NUMBER))
        return false;
    if (UNLIKELY(pHandler != 0 && m_pHandler[interruptNumber] != 0))
        return false;
    if (UNLIKELY(pHandler == 0 && m_pHandler[interruptNumber] == 0))
        return false;

    m_pHandler[interruptNumber] = pHandler;
    return true;
}

#if DEBUGGER
bool PPC32InterruptManager::registerInterruptHandlerDebugger(
    size_t interruptNumber, InterruptHandler *pHandler)
{
    LockGuard<Spinlock> lockGuard(m_Lock);

    if (UNLIKELY(
            interruptNumber >= 256 ||
            interruptNumber == SYSCALL_INTERRUPT_NUMBER))
        return false;
    if (UNLIKELY(pHandler != 0 && m_pDbgHandler[interruptNumber] != 0))
        return false;
    if (UNLIKELY(pHandler == 0 && m_pDbgHandler[interruptNumber] == 0))
        return false;

    m_pDbgHandler[interruptNumber] = pHandler;
    return true;
}
size_t PPC32InterruptManager::getBreakpointInterruptNumber()
{
    return TRAP_INTERRUPT_NUMBER;
}
size_t PPC32InterruptManager::getDebugInterruptNumber()
{
    return TRACE_INTERRUPT_NUMBER;
}
#endif

bool PPC32InterruptManager::registerSyscallHandler(
    Service_t Service, SyscallHandler *pHandler)
{
    LockGuard<Spinlock> lockGuard(m_Lock);

    if (UNLIKELY(Service >= serviceEnd))
        return false;
    if (UNLIKELY(pHandler != 0 && m_pSyscallHandler[Service] != 0))
        return false;
    if (UNLIKELY(pHandler == 0 && m_pSyscallHandler[Service] == 0))
        return false;

    m_pSyscallHandler[Service] = pHandler;
    return true;
}

uintptr_t PPC32InterruptManager::syscall(
    Service_t service, uintptr_t function, uintptr_t p1, uintptr_t p2,
    uintptr_t p3, uintptr_t p4, uintptr_t p5)
{
    register uint32_t r3 __asm__("r3") =
        ((service & 0xFFFF) << 16) | (function & 0xFFFF);
    register uint32_t r4 __asm__("r6") = p1;
    register uint32_t r5 __asm__("r7") = p2;
    register uint32_t r6 __asm__("r8") = p3;
    register uint32_t r7 __asm__("r9") = p4;
    register uint32_t r8 __asm__("r10") = p5;

    asm volatile("sc"
                 : "=r"(r3)
                 : "r"(r3), "r"(r4), "r"(r5), "r"(r6), "r"(r7), "r"(r8));
    return r3;
}

void PPC32InterruptManager::initialiseProcessor()
{
    // We know that we get called before the virtual address space is
    // initialised, so we'll have to do the identity mapping ourselves. How
    // crude!
    OFDevice chosen(OpenFirmware::instance().findDevice("/chosen"));
    OFDevice mmu(chosen.getProperty("mmu"));

    // Identity map the lower area of memory.
    mmu.executeMethod(
        "map", 4, reinterpret_cast<OFParam>(-1),
        reinterpret_cast<OFParam>(0x3000), reinterpret_cast<OFParam>(0x0),
        reinterpret_cast<OFParam>(0x0));
    // Copy the interrupt handlers into lower memory.
    MemoryCopy(reinterpret_cast<void *>(0x0100), &isr_reset, 0x100);
    MemoryCopy(reinterpret_cast<void *>(0x0200), &isr_machine_check, 0x100);
    MemoryCopy(reinterpret_cast<void *>(0x0300), &isr_dsi, 0x100);
    MemoryCopy(reinterpret_cast<void *>(0x0400), &isr_isi, 0x100);
    MemoryCopy(reinterpret_cast<void *>(0x0500), &isr_interrupt, 0x100);
    MemoryCopy(reinterpret_cast<void *>(0x0600), &isr_alignment, 0x100);
    MemoryCopy(reinterpret_cast<void *>(0x0700), &isr_program, 0x100);
    MemoryCopy(reinterpret_cast<void *>(0x0800), &isr_fpu, 0x100);
    MemoryCopy(reinterpret_cast<void *>(0x0900), &isr_decrementer, 0x100);
    MemoryCopy(reinterpret_cast<void *>(0x0C00), &isr_sc, 0x100);
    MemoryCopy(reinterpret_cast<void *>(0x0D00), &isr_trace, 0x100);
    MemoryCopy(reinterpret_cast<void *>(0x0F00), &isr_perf_mon, 0x100);
    MemoryCopy(reinterpret_cast<void *>(0x1300), &isr_instr_breakpoint, 0x100);
    MemoryCopy(reinterpret_cast<void *>(0x1400), &isr_system_management, 0x100);
    MemoryCopy(
        reinterpret_cast<void *>(0x1700), &isr_thermal_management, 0x100);

    for (uintptr_t i = 0x0; i < 0x1800; i += 4)
        Processor::flushDCache(i);

    asm volatile("sync");

    for (uintptr_t i = 0; i < 0x1800; i += 4)
        Processor::invalidateICache(i);

    asm volatile("sync");
    asm volatile("isync");
}

void PPC32InterruptManager::interrupt(InterruptState &interruptState)
{
    // TODO: Needs locking
    size_t intNumber = interruptState.getInterruptNumber();

#if DEBUGGER
    // Call the kernel debugger's handler, if any
    if (m_Instance.m_pDbgHandler[intNumber] != 0)
    {
        m_Instance.m_pDbgHandler[intNumber]->interrupt(
            intNumber, interruptState);
    }
#endif

    // Call the syscall handler, if it is the syscall interrupt
    if (intNumber == SYSCALL_INTERRUPT_NUMBER)
    {
        size_t serviceNumber = interruptState.getSyscallService();
        if (LIKELY(
                serviceNumber < serviceEnd &&
                m_Instance.m_pSyscallHandler[serviceNumber] != 0))
            interruptState.m_R3 =
                m_Instance.m_pSyscallHandler[serviceNumber]->syscall(
                    interruptState);
    }
    else if (m_Instance.m_pHandler[intNumber] != 0)
        m_Instance.m_pHandler[intNumber]->interrupt(intNumber, interruptState);
    else if (intNumber != 6 && intNumber != 10)
    {
        // TODO:: Check for debugger initialisation.
        static LargeStaticString e;
        e.clear();
        e.append("Exception #");
        e.append(interruptState.m_IntNumber, 10);
        e.append(": \"");
        e.append(g_pExceptions[intNumber]);
        e.append("\"");
#if DEBUGGER
        Debugger::instance().start(interruptState, e);
#else
        FATAL(
            "SRR0: " << Hex << interruptState.m_Srr0
                     << ", SRR1: " << interruptState.m_Srr1);
        FATAL(
            "DAR: " << interruptState.m_Dar
                    << ", DSISR: " << interruptState.m_Dsisr);
        panic(e);
#endif
    }

    // Some interrupts (like Program) require the PC to be advanced before
    // returning.
    if (intNumber == TRAP_INTERRUPT_NUMBER)
    {
        interruptState.m_Srr0 += 4;
    }
}

PPC32InterruptManager::PPC32InterruptManager() : m_Lock()
{
}
PPC32InterruptManager::~PPC32InterruptManager()
{
}
