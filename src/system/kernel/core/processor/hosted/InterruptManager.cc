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
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/utilities/StaticString.h"
#if DEBUGGER
#include "pedigree/kernel/debugger/Debugger.h"
#endif
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/processor/Processor.h"

#if THREADS
#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/process/Process.h"
#endif

namespace __pedigree_hosted
{
#include <errno.h>
#include <signal.h>
#include <stdio.h>
};
using namespace __pedigree_hosted;

namespace __pedigree_interrupt_manager_cc
{
#include <string.h>
}

HostedInterruptManager HostedInterruptManager::m_Instance;

struct sigaction HostedInterruptManager::m_OriginalActions[MAX_SIGNAL];

InterruptManager &InterruptManager::instance()
{
    return HostedInterruptManager::instance();
}

bool HostedInterruptManager::registerInterruptHandler(
    size_t nInterruptNumber, InterruptHandler *pHandler)
{
    // Lock the class until the end of the function
    LockGuard<Spinlock> lock(m_Lock);

    // Sanity checks
    if (UNLIKELY(nInterruptNumber >= MAX_SIGNAL))
        return false;
    if (UNLIKELY(pHandler != 0 && m_pHandler[nInterruptNumber] != 0))
        return false;
    if (UNLIKELY(pHandler == 0 && m_pHandler[nInterruptNumber] == 0))
        return false;

    // Change the pHandler
    m_pHandler[nInterruptNumber] = pHandler;

    return true;
}

#if DEBUGGER

bool HostedInterruptManager::registerInterruptHandlerDebugger(
    size_t nInterruptNumber, InterruptHandler *pHandler)
{
    // Lock the class until the end of the function
    LockGuard<Spinlock> lock(m_Lock);

    // Sanity checks
    if (UNLIKELY(nInterruptNumber >= MAX_SIGNAL))
        return false;
    if (UNLIKELY(pHandler != 0 && m_pDbgHandler[nInterruptNumber] != 0))
        return false;
    if (UNLIKELY(pHandler == 0 && m_pDbgHandler[nInterruptNumber] == 0))
        return false;

    // Change the pHandler
    m_pDbgHandler[nInterruptNumber] = pHandler;

    return true;
}
size_t HostedInterruptManager::getBreakpointInterruptNumber()
{
    return SIGTRAP;
}
size_t HostedInterruptManager::getDebugInterruptNumber()
{
    return SIGTRAP;
}

#endif

void HostedInterruptManager::interrupt(InterruptState &interruptState)
{
    size_t nIntNumber = interruptState.getInterruptNumber();

#if DEBUGGER
    {
        InterruptHandler *pHandler;

        // Get the debugger handler
        {
            LockGuard<Spinlock> lockGuard(m_Instance.m_Lock);
            pHandler = m_Instance.m_pDbgHandler[nIntNumber];
        }

        // Call the kernel debugger's handler, if any
        if (pHandler != 0)
        {
            pHandler->interrupt(nIntNumber, interruptState);
        }
    }
#endif

    InterruptHandler *pHandler;

    // Get the interrupt handler
    {
        LockGuard<Spinlock> lockGuard(m_Instance.m_Lock);
        pHandler = m_Instance.m_pHandler[nIntNumber];
    }

    // Call the normal interrupt handler, if any
    if (LIKELY(pHandler != 0))
    {
        pHandler->interrupt(nIntNumber, interruptState);
        return;
    }

    if (UNLIKELY(nIntNumber == SIGINT || nIntNumber == SIGTERM))
    {
        // Shut down (uncleanly for now).
        /// \todo Provide a better entry point for system shutdown.
        Processor::reset();
        panic("shutdown failed");
    }

#if HAS_ADDRESS_SANITIZER
    // If we're running with sanitizers, just raise the signal to them.
    siginfo_t *info = reinterpret_cast<siginfo_t *>(interruptState.getRegister(1));
    uintptr_t ucontext_loc = interruptState.getRegister(2);
    ucontext_t *ctx = reinterpret_cast<ucontext_t *>(ucontext_loc);

    // Escalate to the original signal handler - this is a real error, and in
    // asan we get asan-based analysis in the asan segv handler.
    struct sigaction oact = static_cast<HostedInterruptManager &>(InterruptManager::instance()).getOriginalSigaction(info->si_signo);
    if (oact.sa_flags | SA_SIGINFO)
    {
        oact.sa_sigaction(info->si_signo, info, ctx);
    }
    else
    {
        oact.sa_handler(info->si_signo);
    }

    return;
#endif

// Were we running in the kernel, or user space?
// User space processes have a subsystem, kernel ones do not.
#if THREADS
    Thread *pThread = Processor::information().getCurrentThread();
    Process *pProcess = pThread->getParent();
    Subsystem *pSubsystem = pProcess->getSubsystem();
    if (pSubsystem)
    {
        if (UNLIKELY(nIntNumber == SIGILL))
        {
            pSubsystem->threadException(pThread, Subsystem::InvalidOpcode);
            return;
        }
        else if (UNLIKELY(nIntNumber == SIGFPE))
        {
            pSubsystem->threadException(pThread, Subsystem::FpuError);
            return;
        }
    }
#endif

    // unhandled interrupt, check for an exception
    if (LIKELY(nIntNumber != SIGTRAP))
    {
        // TODO:: Check for debugger initialisation.
        // TODO: register dump, maybe a breakpoint so the deubbger can take
        // over?
        // TODO: Rework this
        // for now just print out the exception name and number
        static LargeStaticString e;
        e.clear();
        e.append("Signal #0x");
        e.append(nIntNumber, 16);
#if DEBUGGER
        Debugger::instance().start(interruptState, e);
#else
        panic(e);
#endif
    }
}

//
// Functions only usable in the kernel initialisation phase
//

static void handler(int which, siginfo_t *info, void *ptr)
{
    HostedInterruptManager::instance().signalShim(which, info, ptr);
}

void HostedInterruptManager::signalShim(int which, void *siginfo, void *meta)
{
    if (!Processor::getInterrupts())
    {
        if (which == SIGUSR1 || which == SIGUSR2)
        {
            FATAL_NOLOCK("interrupts disabled but interrupts are firing");
        }
    }

    siginfo_t *info = reinterpret_cast<siginfo_t *>(siginfo);

    InterruptState state;
    state.which = which;
    state.extra = reinterpret_cast<uint64_t>(info);
    state.state = reinterpret_cast<uint64_t>(info->si_value.sival_ptr);
    state.meta = reinterpret_cast<uint64_t>(meta);
    interrupt(state);

    // Update return signal mask.
    ucontext_t *ctx = reinterpret_cast<ucontext_t *>(meta);
    sigprocmask(0, 0, &ctx->uc_sigmask);
}

struct sigaction HostedInterruptManager::getOriginalSigaction(int which) const
{
    return m_OriginalActions[which];
}

void HostedInterruptManager::initialiseProcessor()
{
    // Set up our handler for every signal we want to trap.
    for (int i = 1; i < MAX_SIGNAL; ++i)
    {
        struct sigaction act, oact;
        ByteSet(&act, 0, sizeof(act));
        act.sa_sigaction = handler;
        sigemptyset(&act.sa_mask);
        act.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;

        sigaction(i, &act, &oact);

        m_OriginalActions[i] = oact;
    }
}

HostedInterruptManager::HostedInterruptManager() : m_Lock()
{
    // Initialise the pointers to the pHandler
    for (size_t i = 0; i < MAX_SIGNAL; i++)
    {
        m_pHandler[i] = 0;
#if DEBUGGER
        m_pDbgHandler[i] = 0;
#endif
    }
}

HostedInterruptManager::~HostedInterruptManager()
{
}
