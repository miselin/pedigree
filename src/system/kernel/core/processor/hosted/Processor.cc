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

#include "pedigree/kernel/processor/Processor.h"
#include "InterruptManager.h"
#include "PhysicalMemoryManager.h"
#include "SyscallManager.h"
#include "VirtualAddressSpace.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/initialiseMultitasking.h"
#include "pedigree/kernel/processor/PageFaultHandler.h"

namespace __pedigree_hosted
{
};
using namespace __pedigree_hosted;

#include <setjmp.h>
#include <signal.h>

bool Processor::m_bInterrupts;

typedef void (*jump_func_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

void Processor::initialisationDone()
{
    HostedPhysicalMemoryManager::instance().initialisationDone();
}

void Processor::initialise1(const BootstrapStruct_t &Info)
{
    HostedInterruptManager::initialiseProcessor();
    PageFaultHandler::instance().initialise();
    HostedPhysicalMemoryManager::instance().initialise(Info);
    HostedSyscallManager::initialiseProcessor();
    setInterrupts(false);
    m_Initialised = 1;
}

void Processor::initialise2(const BootstrapStruct_t &Info)
{
    initialiseMultitasking();
    m_Initialised = 2;
}

void Processor::deinitialise()
{
}

void Processor::identify(HugeStaticString &str)
{
    str.clear();
    str.append("Hosted Processor");
}

uintptr_t Processor::getInstructionPointer()
{
    return reinterpret_cast<uintptr_t>(__builtin_return_address(0));
}

uintptr_t Processor::getStackPointer()
{
    return 0;
}

uintptr_t Processor::getBasePointer()
{
    return reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
}

bool Processor::saveState(SchedulerState &state)
{
    ERROR("Processor::saveState is NOT safe on HOSTED builds.");

    sigjmp_buf _state;
    if (sigsetjmp(_state, 0) == 1)
        return true;

    MemoryCopy(state.state, _state, sizeof(_state));
    return false;
}

void Processor::restoreState(SchedulerState &state, volatile uintptr_t *pLock)
{
    NOTICE("hallo");
    sigjmp_buf _state;
    if (pLock)
        *pLock = 1;
    MemoryCopy(_state, state.state, sizeof(_state));
    siglongjmp(_state, 1);
    // Does not return.
}

void Processor::jumpUser(
    volatile uintptr_t *pLock, uintptr_t address, uintptr_t stack, uintptr_t p1,
    uintptr_t p2, uintptr_t p3, uintptr_t p4)
{
    // Same thing as jumping to kernel space.
    jumpKernel(pLock, address, stack, p1, p2, p3, p4);
}

#if SYSTEM_REQUIRES_ATOMIC_CONTEXT_SWITCH
void Processor::switchState(
    bool bInterrupts, SchedulerState &a, SchedulerState &b,
    volatile uintptr_t *pLock)
{
    sigjmp_buf _state;
    if (sigsetjmp(_state, 0) == 1)
    {
        if (bInterrupts)
            Processor::setInterrupts(true);
        return;
    }

    MemoryCopy(a.state, _state, sizeof(_state));
    restoreState(b, pLock);
}

void Processor::switchState(
    bool bInterrupts, SchedulerState &a, SyscallState &b,
    volatile uintptr_t *pLock)
{
    sigjmp_buf _state;
    if (sigsetjmp(_state, 0) == 1)
    {
        if (bInterrupts)
            Processor::setInterrupts(true);
        return;
    }

    MemoryCopy(a.state, _state, sizeof(_state));
    Processor::restoreState(b, pLock);
}

void Processor::saveAndJumpKernel(
    bool bInterrupts, SchedulerState &s, volatile uintptr_t *pLock,
    uintptr_t address, uintptr_t stack, uintptr_t p1, uintptr_t p2,
    uintptr_t p3, uintptr_t p4)
{
    sigjmp_buf _state;
    if (sigsetjmp(_state, 0) == 1)
    {
        if (bInterrupts)
            Processor::setInterrupts(true);
        return;
    }

    MemoryCopy(s.state, _state, sizeof(_state));
    Processor::jumpKernel(pLock, address, stack, p1, p2, p3, p4);
}

void Processor::saveAndJumpUser(
    bool bInterrupts, SchedulerState &s, volatile uintptr_t *pLock,
    uintptr_t address, uintptr_t stack, uintptr_t p1, uintptr_t p2,
    uintptr_t p3, uintptr_t p4)
{
    Processor::saveAndJumpKernel(
        bInterrupts, s, pLock, address, stack, p1, p2, p3, p4);
}
#endif  // SYSTEM_REQUIRES_ATOMIC_CONTEXT_SWITCH

void Processor::switchAddressSpace(VirtualAddressSpace &AddressSpace)
{
    ProcessorInformation &info = Processor::information();
    if (&info.getVirtualAddressSpace() != &AddressSpace)
    {
        info.setVirtualAddressSpace(AddressSpace);
        HostedVirtualAddressSpace::switchAddressSpace(
            info.getVirtualAddressSpace(), AddressSpace);
    }
}

void Processor::setTlsBase(uintptr_t newBase)
{
}

size_t Processor::getDebugBreakpointCount()
{
    return 0;
}

uintptr_t Processor::getDebugBreakpoint(
    size_t nBpNumber, DebugFlags::FaultType &nFaultType, size_t &nLength,
    bool &bEnabled)
{
    // no-op on hosted
    return 0;
}

void Processor::enableDebugBreakpoint(
    size_t nBpNumber, uintptr_t nLinearAddress,
    DebugFlags::FaultType nFaultType, size_t nLength)
{
    // no-op on hosted
}

void Processor::disableDebugBreakpoint(size_t nBpNumber)
{
    // no-op on hosted
}

void Processor::setInterrupts(bool bEnable)
{
    // Block signals to toggle "interrupts".
    sigset_t set;
    if (bEnable)
        sigemptyset(&set);
    else
    {
        sigemptyset(&set);

        // Only SIGUSR1 and SIGUSR2 are true "interrupts". The rest are all
        // more like exceptions, which we are okay with triggering even if
        // bEnable is false.
        sigaddset(&set, SIGUSR1);
        sigaddset(&set, SIGUSR2);
    }

    // We must mark interrupts enabled before we unmask signals, as any pending
    // signals may trigger immediately (and will cause problems if interrupts
    // are marked as disabled)
    if (bEnable)
    {
        m_bInterrupts = true;
    }

    int r = sigprocmask(SIG_SETMASK, &set, 0);
    if (r != 0)
    {
        ERROR("Processor::setInterrupts failed to set new mask");
    }

    // We can only mark interrupts disabled after masking signals as during the
    // mask operation signals may still come in. Setting the flag here means
    // those signals are handled correctly.
    if (!bEnable)
    {
        m_bInterrupts = false;
    }
}

bool Processor::getInterrupts()
{
    return m_bInterrupts;
}

void Processor::setSingleStep(bool bEnable, InterruptState &state)
{
    // no-op on hosted
}

void Processor::invalidate(void *pAddress)
{
    // no-op on hosted
}

namespace __processor_cc_hosted
{
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
}  // namespace __processor_cc_hosted

using namespace __processor_cc_hosted;

void Processor::_breakpoint()
{
    sigset_t set;
    sigset_t oset;
    sigemptyset(&set);
    sigemptyset(&oset);
    sigaddset(&set, SIGTRAP);
    sigprocmask(SIG_UNBLOCK, &set, &oset);
    raise(SIGTRAP);
    sigprocmask(SIG_SETMASK, &oset, 0);
}

void Processor::_reset()
{
    // Just exit.
    exit(0);
}

void Processor::_haltUntilInterrupt()
{
    bool bOld = m_bInterrupts;
    Processor::setInterrupts(true);
    sigset_t set;
    sigemptyset(&set);
    sigsuspend(&set);
    Processor::setInterrupts(bOld);
}

void Processor::breakpoint()
{
    Processor::_breakpoint();
}

void Processor::halt()
{
    // Abnormal exit.
    __builtin_trap();
}

void Processor::pause()
{
    asm volatile("pause");
}

void Processor::reset()
{
    Processor::_reset();
}

void Processor::haltUntilInterrupt()
{
    Processor::_haltUntilInterrupt();
}
