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

#include "SyscallManager.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/TimeTracker.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/SyscallHandler.h"
#include "pedigree/kernel/processor/state.h"

X64SyscallManager X64SyscallManager::m_Instance;

#define TIME_SYSCALLS 0

extern void system_reboot();

SyscallManager &SyscallManager::instance()
{
    return X64SyscallManager::instance();
}

bool X64SyscallManager::registerSyscallHandler(
    Service_t Service, SyscallHandler *pHandler,
    Registration &registration)
{
    return registerHandler(Service, pHandler, registration);
}

void X64SyscallManager::syscall(SyscallState &syscallState)
{
    TimeTracker tracker(0, true);
#if TIME_SYSCALLS
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();
    Time::Stopwatch syscallTimer(true);
    size_t syscallNumber = syscallState.getSyscallNumber();
#endif

    // Enable IRQs - stack switching and such are done now and it's now safe to
    // start processing interrupts elsewhere.
    Processor::setInterrupts(true);

    size_t serviceNumber = syscallState.getSyscallService();

    if (UNLIKELY(serviceNumber >= serviceEnd))
    {
        // TODO: We should return an error here
        return;
    }

    bool handled = false;
    PostSyscallAction action;
    {
        // The lease must retire before the deferral allows a pending terminal
        // request to consume this thread's stack.
        TerminationDeferral callbackDeferral;
        HandlerLease handler;
        if (m_Instance.acquireHandler(
                static_cast<Service_t>(serviceNumber), handler,
                action))
        {
            handled = true;
            uint64_t result = handler.handler()->syscall(syscallState);
            uint64_t errno =
                Processor::information().getCurrentThread()->getErrno();
            /// \todo this is an extraordinary hack, this should be done in a
            /// way more abstract way than this!!
            if (serviceNumber == linuxCompat)
            {
                if (errno != 0)
                {
                    syscallState.setSyscallReturnValue(-errno);
                }
                else
                {
                    syscallState.setSyscallReturnValue(result);
                }
            }
            else
            {
                syscallState.setSyscallReturnValue(result);
                syscallState.setSyscallErrno(errno);
            }
            // Reset error number now that we've extracted it.
            Processor::information().getCurrentThread()->setErrno(0);
        }
    }

    if (handled)
    {
        switch (action.kind)
        {
            case TerminateCurrentThread:
                tracker.finish();
                Processor::information()
                    .getScheduler()
                    .killCurrentThread();
            case ExitCurrentProcess:
                tracker.finish();
                Processor::information()
                    .getCurrentThread()
                    ->getParent()
                    ->getSubsystem()
                    ->exit(static_cast<int>(action.value));
                return;
            case ReturnFromEvent:
                tracker.finish();
                Processor::information()
                    .getScheduler()
                    .eventHandlerReturned();
            case PopEventState:
                Processor::information()
                    .getCurrentThread()
                    ->abandonCurrentState(false);
                break;
            case RestoreProcessorState:
            {
                // Linux rt_sigreturn replaces the current user register
                // image; it does not own a Pedigree event state to pop.
                const uintptr_t userStack = action.state.rsp;
                const uint64_t userFlags = action.state.rflags;
                uintptr_t interruptStack[24] = {};
                action.state.setStackPointer(
                    reinterpret_cast<uintptr_t>(
                        interruptStack + 24));
                InterruptState *returnState =
                    InterruptState::construct(action.state, true);
                returnState->setStackPointer(userStack);
                returnState->setFlags(userFlags);
                tracker.finish();
                Processor::setInterrupts(false);
                Processor::information()
                    .getCurrentThread()
                    ->transitionTime(
                        CpuTimeMode::Kernel, CpuTimeMode::User);
                Processor::contextSwitch(returnState);
            }
            case JumpToUserspace:
            {
                tracker.finish();
                Processor::setInterrupts(false);
                Thread *current =
                    Processor::information().getCurrentThread();
                current->abandonAllStates();
                current->transitionTime(
                    CpuTimeMode::Kernel, CpuTimeMode::User);
                Processor::jumpUser(
                    nullptr, action.state.getInstructionPointer(),
                    action.state.getStackPointer());
            }
            case RebootSystem:
                tracker.finish();
                Processor::setInterrupts(false);
                Processor::information()
                    .getCurrentThread()
                    ->abandonAllStates();
                Processor::setInterrupts(true);
                system_reboot();
                return;
            case NoPostSyscallAction:
                break;
        }

        Thread *pThread =
            Processor::information().getCurrentThread();
        const Thread::UnwindType unwindState =
            pThread->getUnwindState();
        if (unwindState == Thread::TerminateThread)
        {
            tracker.finish();
            Processor::information().getScheduler().killCurrentThread();
        }
        if (unwindState == Thread::Exit)
        {
            tracker.finish();
            NOTICE("Unwind state exit, in interrupt handler");
            pThread->getParent()
                ->getSubsystem()
                ->exit(pThread->takeDeferredProcessExitCode());
        }
    }

    // Make sure we come back out with interrupts enabled at all times.
    if ((syscallState.m_RFlagsR11 & 0x200) != 0x200)
    {
        syscallState.m_RFlagsR11 |= 0x200;
    }

#if TIME_SYSCALLS
    syscallTimer.stop();
    Time::Timestamp value = syscallTimer.value();
    NOTICE(
        "SYSCALL pid=" << Dec << pProcess->getId()
                       << " service=" << serviceNumber
                       << " num=" << syscallNumber << " ns=" << value << Hex);
#endif
}

uintptr_t X64SyscallManager::syscall(
    Service_t service, uintptr_t function, uintptr_t p1, uintptr_t p2,
    uintptr_t p3, uintptr_t p4, uintptr_t p5)
{
    uint64_t rax = (static_cast<uint64_t>(service) << 16) | function;
    uint64_t ret;
    asm volatile("mov %6, %%r8; \
                  syscall"
                 : "=a"(ret)
                 : "0"(rax), "b"(p1), "d"(p2), "S"(p3), "D"(p4), "m"(p5)
                 : "rcx", "r11");
    return ret;
}

//
// Functions only usable in the kernel initialisation phase
//

extern "C" void syscall_handler();
void X64SyscallManager::initialiseProcessor()
{
    // Enable SCE (= System Call Extensions)
    // Set IA32_EFER/EFER.SCE
    Processor::writeMachineSpecificRegister(
        0xC0000080, Processor::readMachineSpecificRegister(0xC0000080) |
                        0x0000000000000001);

    // Setup SYSCALL/SYSRET
    // Set the IA32_STAR/STAR (CS/SS segment selectors)
    Processor::writeMachineSpecificRegister(0xC0000081, 0x001B000800000000LL);
    // Set the IA32_LSTAR/LSTAR (RIP)
    Processor::writeMachineSpecificRegister(
        0xC0000082, reinterpret_cast<uint64_t>(syscall_handler));
    // Set the IA32_FMASK/SF_MASK (RFLAGS mask, RFLAGS.IF,TF cleared after
    // syscall)
    Processor::writeMachineSpecificRegister(0xC0000084, 0x0000000000000300LL);
}

X64SyscallManager::X64SyscallManager()
{
}
X64SyscallManager::~X64SyscallManager()
{
}
