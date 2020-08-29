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
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/Log.h"

namespace __pedigree_hosted
{
};
using namespace __pedigree_hosted;

#include <setjmp.h>
#include <signal.h>
#include <ucontext.h>

bool ProcessorBase::m_bInterrupts;

#if HAS_SANITIZERS
// We are basically using fibers as we do scheduling ourselves. So we need to
// make sure we are annotating these correctly.
extern "C"
{
// TODO: to use these we need to know the full stack size, but that's not
// available to us in the switch functions. Need to change Scheduler?
void __sanitizer_start_switch_fiber(void** fake_stack_save,
                                    const void* bottom, size_t size);

void __sanitizer_finish_switch_fiber(void* fake_stack_save,
                                     const void** bottom_old,
                                     size_t* size_old);
}
#endif

typedef void (*jump_func_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

void ProcessorBase::initialisationDone()
{
    HostedPhysicalMemoryManager::instance().initialisationDone();
}

void ProcessorBase::initialise1(const BootstrapStruct_t &Info)
{
    HostedInterruptManager::initialiseProcessor();
    PageFaultHandler::instance().initialise();
    HostedPhysicalMemoryManager::instance().initialise(Info);
    HostedSyscallManager::initialiseProcessor();
    setInterrupts(false);
    m_Initialised = 1;
}

void ProcessorBase::initialise2(const BootstrapStruct_t &Info)
{
    initialiseMultitasking();
    m_Initialised = 2;
}

void ProcessorBase::deinitialise()
{
}

void ProcessorBase::identify(HugeStaticString &str)
{
    str.clear();
    str.append("Hosted Processor");
}

uintptr_t ProcessorBase::getInstructionPointer()
{
    return reinterpret_cast<uintptr_t>(__builtin_return_address(0));
}

uintptr_t ProcessorBase::getStackPointer()
{
    return 0;
}

uintptr_t ProcessorBase::getBasePointer()
{
    return reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
}

bool ProcessorBase::saveState(SchedulerState &state)
{
    FATAL("ProcessorBase::saveState is NOT safe on HOSTED builds.");
    return false;
}

void ProcessorBase::restoreState(SchedulerState &state, volatile uintptr_t *pLock)
{
    if (pLock)
        *pLock = 1;

    ucontext_t *ctx = reinterpret_cast<ucontext_t *>(state.state);
    __sanitizer_start_switch_fiber(nullptr, ctx->uc_stack.ss_sp, ctx->uc_stack.ss_size);
    setcontext(reinterpret_cast<ucontext_t *>(state.state));
    FATAL("Hosted: setcontext failed in Processor::restoreState");
    // Does not return.
}

void ProcessorBase::jumpUser(
    volatile uintptr_t *pLock, uintptr_t address, uintptr_t stack, uintptr_t p1,
    uintptr_t p2, uintptr_t p3, uintptr_t p4)
{
    // Same thing as jumping to kernel space.
    jumpKernel(pLock, address, stack, p1, p2, p3, p4);
}

#if SYSTEM_REQUIRES_ATOMIC_CONTEXT_SWITCH
void ProcessorBase::switchState(
    bool bInterrupts, SchedulerState &a, SchedulerState &b,
    volatile uintptr_t *pLock)
{
    void *fake_stack_save = nullptr;

    ucontext_t *a_ctx = reinterpret_cast<ucontext_t *>(a.state);
    ucontext_t *b_ctx = reinterpret_cast<ucontext_t *>(b.state);

    if (pLock)
        *pLock = 1;
#if HAS_SANITIZERS
    //NOTICE("sp [switchState] A: " << a_ctx->uc_stack.ss_sp << " B: " << b_ctx->uc_stack.ss_sp);
    //NOTICE("  -> " << a_ctx << " / " << b_ctx);
    //assert(adjust_pointer(b_ctx->uc_stack.ss_sp, b_ctx->uc_stack.ss_size) != nullptr);
    __sanitizer_start_switch_fiber(&fake_stack_save, b_ctx->uc_stack.ss_sp, b_ctx->uc_stack.ss_size);
#endif
    swapcontext(a_ctx, b_ctx);
#if HAS_SANITIZERS
    __sanitizer_finish_switch_fiber(fake_stack_save, nullptr, nullptr);
#endif
    if (bInterrupts)
        Processor::setInterrupts(true);
}

void ProcessorBase::switchState(
    bool bInterrupts, SchedulerState &a, SyscallState &b,
    volatile uintptr_t *pLock)
{
    FATAL("switchState with a SyscallState is not implemented for the HOSTED cpu");
}

static void threadWrapper(uintptr_t func, volatile uintptr_t *pLock, uintptr_t p1, uintptr_t p2, uintptr_t p3, uintptr_t p4)
{
#if HAS_SANITIZERS
    __sanitizer_finish_switch_fiber(nullptr, nullptr, nullptr);
#endif

    if (pLock)
    {
        // unlock other thread now that we are on the new stack
        *pLock = 1;
    }
    auto entry = reinterpret_cast<void (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t)>(func);
    entry(p1, p2, p3, p4);
    Thread::threadExited();
}

void ProcessorBase::jumpKernel(
    volatile uintptr_t *pLock, uintptr_t address, uintptr_t stack,
    uintptr_t p1, uintptr_t p2, uintptr_t p3,
    uintptr_t p4)
{
    FATAL("Hosted: jumpKernel() is not supported - an atomic context switch is needed");
}

void ProcessorBase::saveAndJumpKernel(
    bool bInterrupts, SchedulerState &s, volatile uintptr_t *pLock,
    uintptr_t address, uintptr_t stack, uintptr_t p1, uintptr_t p2,
    uintptr_t p3, uintptr_t p4)
{
    assert(stack);

    uintptr_t stackBottom = stack - KERNEL_STACK_SIZE;

    ucontext_t new_context;
    getcontext(&new_context);
    new_context.uc_stack.ss_sp = adjust_pointer(reinterpret_cast<void *>(stack), -KERNEL_STACK_SIZE);
    new_context.uc_stack.ss_size = KERNEL_STACK_SIZE;
    new_context.uc_link = NULL;
    makecontext(&new_context, reinterpret_cast<void (*)()>(threadWrapper), 6, address, reinterpret_cast<uintptr_t>(pLock), p1, p2, p3, p4);

#if HAS_SANITIZERS
    void *fake_stack_save = nullptr;
    //NOTICE("make sp [saveAndJumpKernel]: " << new_context.uc_stack.ss_sp);
    //NOTICE(" old -> " << reinterpret_cast<ucontext_t *>(s.state) << " / sp=" << reinterpret_cast<ucontext_t *>(s.state)->uc_stack.ss_sp);
    __sanitizer_start_switch_fiber(&fake_stack_save, new_context.uc_stack.ss_sp, new_context.uc_stack.ss_size);
#endif
    swapcontext(reinterpret_cast<ucontext_t *>(s.state), &new_context);
#if HAS_SANITIZERS
    __sanitizer_finish_switch_fiber(fake_stack_save, nullptr, nullptr);
#endif
    if (bInterrupts)
        Processor::setInterrupts(true);
}

void ProcessorBase::saveAndJumpUser(
    bool bInterrupts, SchedulerState &s, volatile uintptr_t *pLock,
    uintptr_t address, uintptr_t stack, uintptr_t p1, uintptr_t p2,
    uintptr_t p3, uintptr_t p4)
{
    Processor::saveAndJumpKernel(
        bInterrupts, s, pLock, address, stack, p1, p2, p3, p4);
}
#endif  // SYSTEM_REQUIRES_ATOMIC_CONTEXT_SWITCH

void ProcessorBase::switchAddressSpace(VirtualAddressSpace &AddressSpace)
{
    ProcessorInformation &info = Processor::information();
    if (&info.getVirtualAddressSpace() != &AddressSpace)
    {
        info.setVirtualAddressSpace(AddressSpace);
        HostedVirtualAddressSpace::switchAddressSpace(
            info.getVirtualAddressSpace(), AddressSpace);
    }
}

void ProcessorBase::setTlsBase(uintptr_t newBase)
{
}

size_t ProcessorBase::getDebugBreakpointCount()
{
    return 0;
}

uintptr_t ProcessorBase::getDebugBreakpoint(
    size_t nBpNumber, DebugFlags::FaultType &nFaultType, size_t &nLength,
    bool &bEnabled)
{
    // no-op on hosted
    return 0;
}

void ProcessorBase::enableDebugBreakpoint(
    size_t nBpNumber, uintptr_t nLinearAddress,
    DebugFlags::FaultType nFaultType, size_t nLength)
{
    // no-op on hosted
}

void ProcessorBase::disableDebugBreakpoint(size_t nBpNumber)
{
    // no-op on hosted
}

void ProcessorBase::setInterrupts(bool bEnable)
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
        ERROR("ProcessorBase::setInterrupts failed to set new mask");
    }

    // We can only mark interrupts disabled after masking signals as during the
    // mask operation signals may still come in. Setting the flag here means
    // those signals are handled correctly.
    if (!bEnable)
    {
        m_bInterrupts = false;
    }
}

bool ProcessorBase::getInterrupts()
{
    return m_bInterrupts;
}

void ProcessorBase::setSingleStep(bool bEnable, InterruptState &state)
{
    // no-op on hosted
}

void ProcessorBase::invalidate(void *pAddress)
{
    // no-op on hosted
}

ProcessorId ProcessorBase::id()
{
    return 0;
}

ProcessorInformation &ProcessorBase::information()
{
    return m_SafeBspProcessorInformation;
}

size_t ProcessorBase::getCount()
{
    return 1;
}

#include <sched.h>
#include <stdlib.h>
#include <unistd.h>

void ProcessorBase::_breakpoint()
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

void ProcessorBase::_reset()
{
    // Just exit.
    exit(0);
}

void ProcessorBase::_haltUntilInterrupt()
{
    sigset_t set;
    sigemptyset(&set);
    sigsuspend(&set);
}

void ProcessorBase::breakpoint()
{
    Processor::_breakpoint();
}

void ProcessorBase::halt()
{
    // Abnormal exit.
    __builtin_trap();
}

void ProcessorBase::pause()
{
    asm volatile("pause");
}

void ProcessorBase::reset()
{
    Processor::_reset();
}

void ProcessorBase::haltUntilInterrupt()
{
    Processor::_haltUntilInterrupt();
}

void ProcessorBase::invalidateICache(uintptr_t nAddr)
{
    __asm__ __volatile__ ("clflush (%0)" :: "a" (nAddr));
}

void ProcessorBase::invalidateDCache(uintptr_t nAddr)
{
    __asm__ __volatile__ ("clflush (%0)" :: "a" (nAddr));
}

void ProcessorBase::flushDCache(uintptr_t nAddr)
{
    __asm__ __volatile__ ("clflush (%0)" :: "a" (nAddr));
}

void ProcessorBase::flushDCacheAndInvalidateICache(uintptr_t startAddr, uintptr_t endAddr)
{
    for (size_t i = 0; i < endAddr; ++i)
    {
        __asm__ __volatile__ ("clflush (%0)" :: "a" (startAddr + i));
    }
}
