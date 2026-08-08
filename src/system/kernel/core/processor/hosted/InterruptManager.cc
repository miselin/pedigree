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
#include "pedigree/kernel/process/AtomicStateCleanup.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/InterruptTimeAccounting.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#endif

namespace __pedigree_hosted
{
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
};
using namespace __pedigree_hosted;

namespace __pedigree_interrupt_manager_cc
{
#include <string.h>
}

namespace
{
bool isHostedIrqSignal(int which)
{
    bool irqSignal = which == SIGUSR1 || which == SIGUSR2;
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    irqSignal |= which == SIGURG;
#endif
    return irqSignal;
}

void setHostedIrqSignals(sigset_t &set, bool blocked)
{
    if (blocked)
    {
        sigaddset(&set, SIGUSR1);
        sigaddset(&set, SIGUSR2);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        sigaddset(&set, SIGURG);
#endif
    }
    else
    {
        sigdelset(&set, SIGUSR1);
        sigdelset(&set, SIGUSR2);
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        sigdelset(&set, SIGURG);
#endif
    }
}
}  // namespace

HostedInterruptManager HostedInterruptManager::m_Instance;

struct sigaction HostedInterruptManager::m_OriginalActions[MAX_SIGNAL];
bool HostedInterruptManager::m_ActionInstalled[MAX_SIGNAL] = {};
bool HostedInterruptManager::m_bQuiesced = false;

#if THREADS
namespace
{
struct HostedSignalFrameCleanup
{
    explicit HostedSignalFrameCleanup(Thread *signalThread)
        : thread(signalThread), stateLevel(0), previousInterruptState(false),
          restoreInterruptState(false), cleanup()
    {
    }

    Thread *thread;
    size_t stateLevel;
    bool previousInterruptState;
    bool restoreInterruptState;
    AtomicStateCleanupRecord cleanup;
};

void restoreHostedSignalInterruptState(HostedSignalFrameCleanup &frame)
{
    if (!frame.restoreInterruptState)
    {
        return;
    }

    const bool previousInterruptState = frame.previousInterruptState;
    frame.restoreInterruptState = false;
    Processor::setInterrupts(previousInterruptState);
}

void abandonHostedSignalFrame(void *context)
{
    HostedSignalFrameCleanup *frame =
        reinterpret_cast<HostedSignalFrameCleanup *>(context);
    // Keep the host IRQ signals physically masked until the interrupted
    // logical IF state has been restored. The resumed context owns the final
    // signal mask after this abandoned stack is retired.
    restoreHostedSignalInterruptState(*frame);
    if (frame->thread)
    {
        frame->thread->leaveHostedSignalHandler(frame->stateLevel);
    }
    Processor::leaveHostedSignalFrame();
}

struct HostedUserReturnContext
{
    InterruptState *state;
};

void serviceHostedUserReturnWork(uintptr_t rawContext)
{
    HostedUserReturnContext *context =
        reinterpret_cast<HostedUserReturnContext *>(rawContext);
    Processor::information().getScheduler().serviceUserReturnWork(
        *context->state);
}

void runHostedUserReturnTail(Thread *thread, InterruptState &state)
{
    Processor::setInterrupts(false);
    if (!thread->pushState())
    {
        FATAL_NOLOCK(
            "Hosted user-return work exhausted Thread state stacks");
    }

    HostedUserReturnContext context = {&state};
    Processor::setInterrupts(true);
    callOnStack(
        reinterpret_cast<uintptr_t>(thread->getKernelStack()),
        reinterpret_cast<uintptr_t>(&serviceHostedUserReturnWork),
        reinterpret_cast<uintptr_t>(&context));

    const bool returnInterrupts = Processor::getInterrupts();
    Processor::setInterrupts(false);
    thread->popState(false);
    Processor::setInterrupts(returnInterrupts);
}
}  // namespace
#endif

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
    InterruptHandler *current =
        __atomic_load_n(&m_pHandler[nInterruptNumber], __ATOMIC_ACQUIRE);
    if (UNLIKELY(pHandler != 0 && current != 0))
        return false;
    if (UNLIKELY(pHandler == 0 && current == 0))
        return false;

    // Dispatch can re-enter on this CPU while the mutation lock is held.
    // Publish the complete old or new pointer without involving that lock.
    return __atomic_compare_exchange_n(
        &m_pHandler[nInterruptNumber], &current, pHandler, false,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
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
    InterruptHandler *current =
        __atomic_load_n(&m_pDbgHandler[nInterruptNumber], __ATOMIC_ACQUIRE);
    if (UNLIKELY(pHandler != 0 && current != 0))
        return false;
    if (UNLIKELY(pHandler == 0 && current == 0))
        return false;

    return __atomic_compare_exchange_n(
        &m_pDbgHandler[nInterruptNumber], &current, pHandler, false,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
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
        InterruptHandler *pHandler = __atomic_load_n(
            &m_Instance.m_pDbgHandler[nIntNumber], __ATOMIC_ACQUIRE);

        // Call the kernel debugger's handler, if any
        if (pHandler != 0)
        {
            ExecutionContextGuard debuggerContext(ExecutionContext::DebuggerTrap);
            pHandler->interrupt(nIntNumber, interruptState);
        }
    }
#endif

    InterruptHandler *pHandler = __atomic_load_n(
        &m_Instance.m_pHandler[nIntNumber], __ATOMIC_ACQUIRE);

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

#if THREADS
    Thread *pThread = Processor::information().getCurrentThread();
    Process *pProcess = pThread ? pThread->getParent() : nullptr;
    Subsystem *pSubsystem = pProcess ? pProcess->getSubsystem() : nullptr;
    if (pSubsystem && !interruptState.kernelMode())
    {
        if (UNLIKELY(nIntNumber == SIGILL))
        {
            siginfo_t *info = reinterpret_cast<siginfo_t *>(
                interruptState.getRegister(1));
            pThread->deferSubsystemException(
                static_cast<size_t>(Subsystem::InvalidOpcode),
                interruptState.getInstructionPointer(),
                info ? static_cast<uintptr_t>(info->si_code) : 0);
            return;
        }
        if (UNLIKELY(nIntNumber == SIGFPE))
        {
            siginfo_t *info = reinterpret_cast<siginfo_t *>(
                interruptState.getRegister(1));
            pThread->deferSubsystemException(
                static_cast<size_t>(Subsystem::FpuError),
                interruptState.getInstructionPointer(),
                info ? static_cast<uintptr_t>(info->si_code) : 0);
            return;
        }
    }
#endif

#if HAS_ADDRESS_SANITIZER
    // If we're running with sanitizers, just raise the signal to them.
    siginfo_t *info = reinterpret_cast<siginfo_t *>(interruptState.getRegister(1));
    uintptr_t ucontext_loc = interruptState.getRegister(2);
    ucontext_t *ctx = reinterpret_cast<ucontext_t *>(ucontext_loc);

    // Escalate to the original signal handler - this is a real error, and in
    // asan we get asan-based analysis in the asan segv handler.
    struct sigaction oact = static_cast<HostedInterruptManager &>(InterruptManager::instance()).getOriginalSigaction(info->si_signo);
    if (oact.sa_handler == SIG_IGN)
    {
        return;
    }
    else if (oact.sa_handler == SIG_DFL)
    {
        sigaction(info->si_signo, &oact, nullptr);
        raise(info->si_signo);
    }
    else if (oact.sa_flags & SA_SIGINFO)
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

extern "C" void hostedSignalTrampoline(
    int which, siginfo_t *info, void *ptr);

extern "C" void hostedSignalHandler(
    int which, siginfo_t *info, void *ptr, bool fromUserspace)
{
    HostedInterruptManager::instance().signalShim(
        which, info, ptr, fromUserspace);
}

void HostedInterruptManager::signalShim(
    int which, void *siginfo, void *meta, bool fromUserspace)
{
    const bool hostedIrq = isHostedIrqSignal(which);
    if (hostedIrq && !Processor::onHostedExecutionThread())
    {
        FATAL_NOLOCK("Hosted IRQ delivered on a non-processor host thread");
        return;
    }

    Processor::enterHostedSignalFrame();

#if THREADS
    Thread *pSignalThread = Processor::information().getCurrentThread();
    HostedSignalFrameCleanup frameCleanup(pSignalThread);
    if (pSignalThread)
    {
        frameCleanup.stateLevel = pSignalThread->enterHostedSignalHandler();
        if (fromUserspace && frameCleanup.stateLevel)
        {
            FATAL_NOLOCK(
                "Hosted userspace resumed with nested kernel state");
        }
        pSignalThread->armAtomicStateCleanup(
            frameCleanup.cleanup, abandonHostedSignalFrame, &frameCleanup);
    }
#endif

    InterruptState state;
    ucontext_t *interruptedContext = reinterpret_cast<ucontext_t *>(meta);
    {
        ExecutionContextGuard signalContext(
            which == SIGTRAP ? ExecutionContext::DebuggerTrap :
                               ExecutionContext::HostedSyntheticIrq);

        const bool previousInterruptState = Processor::getInterrupts();
        if (!previousInterruptState)
        {
            if (hostedIrq)
            {
                FATAL_NOLOCK("interrupts disabled but interrupts are firing");
            }
        }

        if (hostedIrq)
        {
#if THREADS
            // Hosted signals already mask IRQ delivery physically. Publish the
            // matching logical IF boundary as well, and make its restoration
            // survive a scheduler callback which abandons this signal stack.
            frameCleanup.previousInterruptState = previousInterruptState;
            frameCleanup.restoreInterruptState = true;
#endif
            Processor::setInterrupts(false);
        }

        siginfo_t *info = reinterpret_cast<siginfo_t *>(siginfo);

        state.which = which;
        state.fromUserspace = fromUserspace ? 1 : 0;
        state.extra = reinterpret_cast<uint64_t>(info);
        state.state = reinterpret_cast<uint64_t>(info->si_value.sival_ptr);
        state.meta = reinterpret_cast<uint64_t>(meta);
        state.setInstructionPointer(
            interruptedContext->uc_mcontext.gregs[REG_RIP]);
        state.setStackPointer(interruptedContext->uc_mcontext.gregs[REG_RSP]);
        state.setBasePointer(interruptedContext->uc_mcontext.gregs[REG_RBP]);
#if THREADS
        {
            // Match native interrupt accounting. User-origin frames remain in
            // Kernel accounting mode until the return tail has completed.
            InterruptTimeAccounting accounting(fromUserspace);
            interrupt(state);
        }
#else
        interrupt(state);
#endif

        // Raw IRQ dispatch is complete. Restore logical IF while hosted-signal
        // depth still keeps IRQ signals physically masked, then let the ordinary
        // return-to-user tail run at its IRQ-enabled thread boundary.
#if THREADS
        restoreHostedSignalInterruptState(frameCleanup);
#else
        if (hostedIrq)
        {
            Processor::setInterrupts(previousInterruptState);
        }
#endif
    }

#if THREADS
    if (
        fromUserspace && pSignalThread &&
        Processor::information().getCurrentThread() == pSignalThread)
    {
        runHostedUserReturnTail(pSignalThread, state);
    }
#endif

    // sigreturn restores this mask atomically with the interrupted context.
    ucontext_t *ctx = interruptedContext;
    sigprocmask(0, nullptr, &ctx->uc_sigmask);
    setHostedIrqSignals(ctx->uc_sigmask, !Processor::getInterrupts());

#if THREADS
    Processor::maskInterruptsForSignalReturn();
    if (pSignalThread)
    {
        pSignalThread->disarmAtomicStateCleanup(frameCleanup.cleanup);
        pSignalThread->leaveHostedSignalHandler(frameCleanup.stateLevel);
    }
    Processor::leaveHostedSignalFrame();
    if (fromUserspace && pSignalThread)
    {
        InterruptTimeAccounting::finishUserReturn(pSignalThread);
    }
#else
    Processor::leaveHostedSignalFrame();
#endif
}

struct sigaction HostedInterruptManager::getOriginalSigaction(int which) const
{
    return m_OriginalActions[which];
}

void HostedInterruptManager::initialiseProcessor()
{
    m_bQuiesced = false;
    ByteSet(m_ActionInstalled, 0, sizeof(m_ActionInstalled));

    // Set up our handler for every signal we want to trap.
    for (int i = 1; i < MAX_SIGNAL; ++i)
    {
        struct sigaction act, oact;
        ByteSet(&act, 0, sizeof(act));
        act.sa_sigaction = hostedSignalTrampoline;
        sigemptyset(&act.sa_mask);
        // A synchronous exception may publish deferred return work, but an
        // IRQ must not suspend that raw publication half-complete.
        setHostedIrqSignals(act.sa_mask, true);
        act.sa_flags = SA_SIGINFO | SA_ONSTACK;
        if (!isHostedIrqSignal(i))
        {
            // Keep synchronous signals re-entrant. IRQ signals remain masked
            // until raw fault publication reaches the return tail.
            act.sa_flags |= SA_NODEFER;
        }

        if (sigaction(i, &act, &oact) == 0)
        {
            m_OriginalActions[i] = oact;
            m_ActionInstalled[i] = true;
        }
    }
}

void HostedInterruptManager::quiesceProcessor()
{
    if (m_bQuiesced)
    {
        return;
    }

    sigset_t irqSignals;
    sigemptyset(&irqSignals);
    setHostedIrqSignals(irqSignals, true);
    sigprocmask(SIG_BLOCK, &irqSignals, nullptr);

    // The timers have already been deleted, so no new IRQ signals can be
    // queued. Drain signals that were pending while interrupts were masked.
    struct timespec noWait = {0, 0};
    while (sigtimedwait(&irqSignals, nullptr, &noWait) >= 0)
    {
    }

    for (size_t i = 1; i < MAX_SIGNAL; ++i)
    {
        if (m_ActionInstalled[i])
        {
            sigaction(i, &m_OriginalActions[i], nullptr);
            m_ActionInstalled[i] = false;
        }
    }
    m_bQuiesced = true;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void HostedInterruptManager::withMutationLockForTest(MutationLockHook hook)
{
    LockGuard<Spinlock> lock(m_Instance.m_Lock);
    if (hook)
    {
        hook();
    }
}
#endif

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
