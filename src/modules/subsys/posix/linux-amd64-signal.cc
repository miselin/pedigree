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

#include "linux-amd64-signal.h"

#if X64

#include "linux-amd64-signal-abi.h"
#include "system-syscalls.h"

#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/NMFaultHandler.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/SyscallManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/utilities/lib.h"

#include <signal.h>

namespace
{
using namespace LinuxAmd64SignalAbi;

constexpr uint64_t UnblockableSignals =
    (static_cast<uint64_t>(1) << (SIGKILL - 1)) |
    (static_cast<uint64_t>(1) << (SIGSTOP - 1));
constexpr uint64_t HandlerFlagsToClear = 0x100 | 0x400 | 0x10000;
constexpr uint64_t RestorableRflags = 0x50DD5;
constexpr uint64_t SafeUserRflags = 0x202;
constexpr uint64_t SupportedUcontextFlags = 0x6;
constexpr uintptr_t MaximumCanonicalUserAddress = 0x00007FFFFFFFFFFF;

uintptr_t alignDown(uintptr_t value, uintptr_t alignment)
{
    return value & ~(alignment - 1);
}

bool userBounds(uintptr_t address, size_t extent)
{
    if (!extent || !address ||
        extent - 1 > (~static_cast<uintptr_t>(0) - address))
    {
        return false;
    }

    uintptr_t end = address + extent - 1;
    VirtualAddressSpace &va =
        Processor::information().getVirtualAddressSpace();
    return address >= va.getUserStart() &&
           end <= MaximumCanonicalUserAddress &&
           end < va.getKernelStart() &&
           va.isAddressValid(reinterpret_cast<void *>(address)) &&
           va.isAddressValid(reinterpret_cast<void *>(end));
}

bool userRegion(uintptr_t address, size_t extent, size_t access)
{
    if (!userBounds(address, extent))
    {
        return false;
    }

    // Keep the inclusive-end check local to the signal copy boundary even if
    // the shared range validator changes.
    return PosixSubsystem::checkAddress(address, extent, access) &&
           PosixSubsystem::checkAddress(
               address + extent - 1, 1, access);
}

bool userExecutable(uintptr_t address)
{
    if (!userRegion(address, 1, PosixSubsystem::SafeRead))
    {
        return false;
    }

    VirtualAddressSpace &va =
        Processor::information().getVirtualAddressSpace();
    physical_uintptr_t physical = 0;
    size_t flags = 0;
    va.getMapping(reinterpret_cast<void *>(address), physical, flags);
    return !(flags & VirtualAddressSpace::KernelMode) &&
           (flags & VirtualAddressSpace::Execute);
}

bool onAlternateStack(
    uintptr_t stackPointer, const Thread::AlternateSignalStack &stack)
{
    if (!stack.enabled || !stack.base ||
        stack.size > (~static_cast<uintptr_t>(0) - stack.base))
    {
        return false;
    }

    return stackPointer >= stack.base &&
           stackPointer < stack.base + stack.size;
}

void setSiginfo32(Siginfo &info, size_t offset, int32_t value)
{
    MemoryCopy(info.bytes + offset, &value, sizeof(value));
}

void setSiginfo64(Siginfo &info, size_t offset, uint64_t value)
{
    MemoryCopy(info.bytes + offset, &value, sizeof(value));
}

void populateSigcontext(
    Sigcontext &context, const InterruptState &state, uint64_t oldMask,
    uintptr_t faultAddress, uintptr_t errorCode, uintptr_t fpstate)
{
    context.rax = state.getRegister(0);
    context.rbx = state.getRegister(1);
    context.rcx = state.getRegister(2);
    context.rdx = state.getRegister(3);
    context.rdi = state.getRegister(4);
    context.rsi = state.getRegister(5);
    context.rbp = state.getRegister(6);
    context.r8 = state.getRegister(7);
    context.r9 = state.getRegister(8);
    context.r10 = state.getRegister(9);
    context.r11 = state.getRegister(10);
    context.r12 = state.getRegister(11);
    context.r13 = state.getRegister(12);
    context.r14 = state.getRegister(13);
    context.r15 = state.getRegister(14);
    context.rsp = state.getStackPointer();
    context.rip = state.getInstructionPointer();
    context.rflags = state.getFlags();
    context.cs = state.getCodeSegment();
    context.ss = state.getStackSegment();
    context.errorCode = errorCode;
    context.trapNumber = state.getInterruptNumber();
    context.oldMask = oldMask;
    context.cr2 = faultAddress;
    context.fpstate = fpstate;
}

int signalCode(Subsystem::ExceptionType exception, uintptr_t errorCode)
{
    switch (exception)
    {
        case Subsystem::PageFault:
            return (errorCode & 1) ? SEGV_ACCERR : SEGV_MAPERR;
        case Subsystem::InvalidOpcode:
            return ILL_ILLOPC;
        case Subsystem::GeneralProtectionFault:
            return BUS_ADRERR;
        case Subsystem::DivideByZero:
            return FPE_INTDIV;
        case Subsystem::FpuError:
        case Subsystem::SpecialFpuError:
            return FPE_FLTINV;
        default:
            return SI_KERNEL;
    }
}

uintptr_t signalAddress(
    Subsystem::ExceptionType exception, const InterruptState &state,
    uintptr_t faultAddress)
{
    return exception == Subsystem::PageFault
               ? faultAddress
               : state.getInstructionPointer();
}

void badFrame()
{
    if (!SyscallManager::instance().requestProcessExit(128 + SIGSEGV))
    {
        FATAL("Invalid signal frame exit was not dispatched.");
    }
}
}  // namespace

LinuxAmd64Signal::DeliveryResult LinuxAmd64Signal::deliverSynchronous(
    Thread *thread, int signal,
    const PosixSubsystem::SignalDisposition &disposition,
    Subsystem::ExceptionType exception, InterruptState &state,
    uintptr_t faultAddress, uintptr_t errorCode)
{
    if (!thread ||
        thread != Processor::information().getCurrentThread() ||
        disposition.type != 0)
    {
        return NotApplicable;
    }

    if (
        signal <= 0 || signal > 64 || state.kernelMode() ||
        state.getCodeSegment() != 0x1B || state.getStackSegment() != 0x23 ||
        !(disposition.flags & SA_RESTORER) ||
        !userExecutable(disposition.handler) ||
        !userExecutable(disposition.restorer))
    {
        return Failed;
    }

    uintptr_t originalStack = state.getStackPointer();
    if (!userBounds(originalStack, 1))
    {
        return Failed;
    }

    Thread::AlternateSignalStack &alternate =
        thread->getAlternateSignalStack();
    bool wasOnAlternate = onAlternateStack(originalStack, alternate);
    bool enterAlternate =
        (disposition.flags & SA_ONSTACK) && alternate.enabled &&
        !wasOnAlternate;

    uintptr_t stackTop = 0;
    if (enterAlternate)
    {
        if (
            !alternate.base ||
            alternate.size >
                (~static_cast<uintptr_t>(0) - alternate.base))
        {
            return Failed;
        }
        stackTop = alternate.base + alternate.size;
    }
    else
    {
        if (originalStack < 128)
        {
            return Failed;
        }
        stackTop = originalStack - 128;
    }

    if (stackTop < sizeof(Fpstate))
    {
        return Failed;
    }
    uintptr_t fpstateAddress =
        alignDown(stackTop - sizeof(Fpstate), 64);
    if (fpstateAddress < sizeof(RtSigframe) + 8)
    {
        return Failed;
    }
    uintptr_t frameAddress =
        alignDown(fpstateAddress - sizeof(RtSigframe), 16) - 8;

    if (
        !userRegion(
            frameAddress, sizeof(RtSigframe), PosixSubsystem::SafeWrite) ||
        !userRegion(
            fpstateAddress, sizeof(Fpstate), PosixSubsystem::SafeWrite))
    {
        return Failed;
    }

    if (enterAlternate || wasOnAlternate)
    {
        uintptr_t alternateEnd = alternate.base + alternate.size;
        if (
            frameAddress < alternate.base ||
            fpstateAddress + sizeof(Fpstate) > alternateEnd)
        {
            return Failed;
        }
    }

    RtSigframe frame = {};
    Fpstate fpstate = {};
    if (!NMFaultHandler::saveCurrentThreadFpuState(&fpstate, true))
    {
        return Failed;
    }
    ByteSet(fpstate.reserved3, 0, sizeof(fpstate.reserved3));

    uint64_t oldMask = thread->getSignalMask();
    frame.restorer = disposition.restorer;
    frame.ucontext.flags = SupportedUcontextFlags;
    if (alternate.enabled)
    {
        frame.ucontext.stack.stackPointer = alternate.base;
        frame.ucontext.stack.size = alternate.size;
        frame.ucontext.stack.flags = wasOnAlternate ? SS_ONSTACK : 0;
    }
    else
    {
        frame.ucontext.stack.flags = SS_DISABLE;
    }
    populateSigcontext(
        frame.ucontext.mcontext, state, oldMask,
        exception == Subsystem::PageFault ? faultAddress : 0, errorCode,
        fpstateAddress);
    frame.ucontext.signalMask = oldMask;

    setSiginfo32(frame.info, 0, signal);
    setSiginfo32(frame.info, 4, 0);
    setSiginfo32(frame.info, 8, signalCode(exception, errorCode));
    setSiginfo64(
        frame.info, 16, signalAddress(exception, state, faultAddress));
    setSiginfo32(
        frame.info, 24, static_cast<int32_t>(state.getInterruptNumber()));

    MemoryCopy(
        reinterpret_cast<void *>(frameAddress), &frame, sizeof(frame));
    MemoryCopy(
        reinterpret_cast<void *>(fpstateAddress), &fpstate, sizeof(fpstate));

    uint64_t handlerMask = oldMask | disposition.signalMask;
    if (!(disposition.flags & SA_NODEFER))
    {
        handlerMask |= static_cast<uint64_t>(1) << (signal - 1);
    }
    thread->setSignalMask(handlerMask & ~UnblockableSignals);
    alternate.inUse = wasOnAlternate || enterAlternate;

    state.setRegister(0, 0);
    state.setRegister(4, static_cast<uintptr_t>(signal));
    state.setRegister(
        5, frameAddress + __builtin_offsetof(RtSigframe, info));
    state.setRegister(
        3, frameAddress + __builtin_offsetof(RtSigframe, ucontext));
    state.setInstructionPointer(disposition.handler);
    state.setStackPointer(frameAddress);
    state.setFlags(
        (state.getFlags() & ~HandlerFlagsToClear) | SafeUserRflags);
    return Delivered;
}

void LinuxAmd64Signal::sigreturn(SyscallState &state)
{
    uintptr_t syscallStack = state.getStackPointer();
    if (syscallStack < 8)
    {
        badFrame();
        return;
    }
    uintptr_t frameAddress = syscallStack - 8;
    if (!userRegion(
            frameAddress, sizeof(RtSigframe), PosixSubsystem::SafeRead))
    {
        badFrame();
        return;
    }

    RtSigframe frame = {};
    MemoryCopy(
        &frame, reinterpret_cast<const void *>(frameAddress), sizeof(frame));
    const Sigcontext &context = frame.ucontext.mcontext;

    if (
        (frame.ucontext.flags & ~SupportedUcontextFlags) ||
        context.cs != 0x1B || context.ss != 0x23 ||
        !userBounds(context.rip, 1) || !userBounds(context.rsp, 1) ||
        !context.fpstate || (context.fpstate & 0x3F) ||
        !userRegion(
            context.fpstate, sizeof(Fpstate), PosixSubsystem::SafeRead))
    {
        badFrame();
        return;
    }

    int alternateFlags = frame.ucontext.stack.flags;
    if (
        alternateFlags & ~(SS_ONSTACK | SS_DISABLE) ||
        ((alternateFlags & SS_ONSTACK) &&
         (alternateFlags & SS_DISABLE)))
    {
        badFrame();
        return;
    }

    Thread::AlternateSignalStack restoredAlternate;
    if (!(alternateFlags & SS_DISABLE))
    {
        restoredAlternate.base = frame.ucontext.stack.stackPointer;
        restoredAlternate.size = frame.ucontext.stack.size;
        restoredAlternate.enabled = true;
        restoredAlternate.inUse = alternateFlags & SS_ONSTACK;
        if (
            restoredAlternate.size < MINSIGSTKSZ ||
            !userBounds(
                restoredAlternate.base, restoredAlternate.size) ||
            (restoredAlternate.inUse &&
             !onAlternateStack(context.rsp, restoredAlternate)))
        {
            badFrame();
            return;
        }
    }

    Fpstate fpstate = {};
    MemoryCopy(
        &fpstate, reinterpret_cast<const void *>(context.fpstate),
        sizeof(fpstate));
    for (size_t i = 0; i < 12; ++i)
    {
        if (fpstate.reserved3[i])
        {
            badFrame();
            return;
        }
    }
    if (!NMFaultHandler::restoreCurrentThreadFpuState(&fpstate))
    {
        badFrame();
        return;
    }

    Thread *thread = Processor::information().getCurrentThread();
    thread->setSignalMask(
        frame.ucontext.signalMask & ~UnblockableSignals);
    thread->getAlternateSignalStack() = restoredAlternate;

    X64ProcessorState restored;
    restored.r8 = context.r8;
    restored.r9 = context.r9;
    restored.r10 = context.r10;
    restored.r11 = context.r11;
    restored.r12 = context.r12;
    restored.r13 = context.r13;
    restored.r14 = context.r14;
    restored.r15 = context.r15;
    restored.rdi = context.rdi;
    restored.rsi = context.rsi;
    restored.rbp = context.rbp;
    restored.rbx = context.rbx;
    restored.rdx = context.rdx;
    restored.rax = context.rax;
    restored.rcx = context.rcx;
    restored.rip = context.rip;
    restored.rflags =
        (context.rflags & RestorableRflags) | SafeUserRflags;
    restored.rsp = context.rsp;

    if (!SyscallManager::instance().requestStateRestore(restored))
    {
        FATAL("Signal state restoration was not dispatched.");
    }
}

#endif
