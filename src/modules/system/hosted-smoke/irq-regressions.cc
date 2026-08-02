/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/IrqHandlerRegistry.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/time/Time.h"
#include "system/kernel/machine/hosted/IrqManager.h"
#include "system/kernel/machine/mach_pc/PicIrqState.h"

#include <signal.h>

namespace
{
bool check(
    bool condition, const char *detail,
    const char *test = "irq-handler-lifetime")
{
    if (condition)
    {
        return true;
    }

    ERROR("HOSTED-WAIT-TEST: FAIL " << test << ": " << detail);
    return false;
}

class ThreadedRegistryHandler : public IrqHandler
{
  public:
    ThreadedRegistryHandler() : calls(0), wrongContext(0)
    {
    }

    IrqDisposition irq(irq_id_t) override
    {
        calls += 1;
        Thread *current = Processor::information().getCurrentThread();
        if (!current || !Processor::getInterrupts() ||
            current->getHostedSignalDepth())
        {
            wrongContext += 1;
        }
        return IrqDisposition::Handled;
    }

    Atomic<size_t> calls;
    Atomic<size_t> wrongContext;
};

class HardRegistryHandler : public HardIrqHandler
{
  public:
    bool irq(irq_id_t, InterruptState &) override
    {
        return true;
    }
};

class DeliveryContextProbe : public HardIrqHandler
{
  public:
    explicit DeliveryContextProbe(IrqHandlerRegistry &registry)
        : m_Registry(registry), calls(0), hardAdmitted(0), hardHandled(0),
          signalThreadedAdmitted(0), signalThreadedHandled(0)
    {
    }

    bool irq(irq_id_t, InterruptState &state) override
    {
        if (!calls.compareAndSwap(0, 1))
        {
            return true;
        }

        bool handled = true;
        if (m_Registry.dispatchHard(5, state, handled))
        {
            hardAdmitted += 1;
        }
        if (handled)
        {
            hardHandled += 1;
        }

        handled = true;
        if (m_Registry.dispatchThreaded(5, handled))
        {
            signalThreadedAdmitted += 1;
        }
        if (handled)
        {
            signalThreadedHandled += 1;
        }
        return true;
    }

    IrqHandlerRegistry &m_Registry;
    Atomic<size_t> calls;
    Atomic<size_t> hardAdmitted;
    Atomic<size_t> hardHandled;
    Atomic<size_t> signalThreadedAdmitted;
    Atomic<size_t> signalThreadedHandled;
};

bool deliveryModeSeparation()
{
    constexpr const char *Test = "irq-delivery-mode-separation";
    IrqHandlerRegistry *registry = new IrqHandlerRegistry;
    ThreadedRegistryHandler threaded;
    HardRegistryHandler hard;

    const bool threadedRegistered =
        registry->registerThreadedHandler(5, &threaded);
    const bool hardMixRejected = !registry->registerHardHandler(5, &hard);

    DeliveryContextProbe *probe = new DeliveryContextProbe(*registry);
    IrqManager *manager = Machine::instance().getIrqManager();
    const irq_id_t probeId = manager->registerHardIsaIrqHandler(
        1, probe, IrqPolicy::syntheticHard());
    const bool signalQueued = probeId && raise(SIGUSR2) == 0;
    const bool probeRemoved =
        probeId && manager->unregisterHandler(probeId, probe);

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    bool atomicHandled = true;
    const bool atomicAdmitted = registry->dispatchThreaded(5, atomicHandled);
    Processor::setInterrupts(interruptsWereEnabled);

    bool handled = false;
    const bool admitted = registry->dispatchThreaded(5, handled);
    const bool threadedRemoved =
        registry->unregisterHandler(5, &threaded) ==
        IrqHandlerRegistry::UnregisterResult::Completed;
    const bool hardAfterThreaded = registry->registerHardHandler(5, &hard);
    const bool hardAfterThreadedRemoved =
        registry->unregisterHandler(5, &hard) ==
        IrqHandlerRegistry::UnregisterResult::Completed;

    const bool hardRegistered = registry->registerHardHandler(6, &hard);
    const bool threadedMixRejected =
        !registry->registerThreadedHandler(6, &threaded);
    bool wrongThreadedHandled = true;
    const bool wrongThreadedAdmitted =
        registry->dispatchThreaded(6, wrongThreadedHandled);
    const bool hardRemoved = registry->unregisterHandler(6, &hard) ==
                             IrqHandlerRegistry::UnregisterResult::Completed;
    const bool threadedAfterHard =
        registry->registerThreadedHandler(6, &threaded);
    const bool threadedAfterHardRemoved =
        registry->unregisterHandler(6, &threaded) ==
        IrqHandlerRegistry::UnregisterResult::Completed;

    bool passed = true;
    passed &= check(
        threadedRegistered && hardMixRejected && admitted && handled,
        "a threaded line accepted hard delivery or did not dispatch", Test);
    passed &= check(
        signalQueued && probe->calls == 1 && !probe->hardAdmitted &&
            !probe->hardHandled && !probe->signalThreadedAdmitted &&
            !probe->signalThreadedHandled && probeRemoved,
        "a threaded line dispatched in hard or hosted-signal context", Test);
    passed &= check(
        !atomicAdmitted && !atomicHandled,
        "a threaded line dispatched with interrupts disabled", Test);
    passed &= check(
        threaded.calls == 1 && threaded.wrongContext == 0,
        "the threaded callback observed hard or atomic context", Test);
    passed &= check(
        threadedRemoved && hardAfterThreaded && hardAfterThreadedRemoved &&
            hardRegistered && threadedMixRejected && !wrongThreadedAdmitted &&
            !wrongThreadedHandled && hardRemoved && threadedAfterHard &&
            threadedAfterHardRemoved,
        "a hard line accepted threaded delivery or could not retire", Test);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "irq-delivery-mode-separation");
    }

    if (!probeId || probeRemoved)
    {
        delete probe;
    }
    if (threadedRemoved && hardAfterThreadedRemoved && hardRemoved &&
        threadedAfterHardRemoved && (!probeId || probeRemoved))
    {
        delete registry;
    }
    return passed;
}

bool picLineStateLifecycle()
{
    constexpr const char *Test = "pic-line-state-mask-lifecycle";
    PicIrqState state;
    state.setAllEnabled(false);

    bool passed = true;
    passed &= check(
        state.mask() == 0xFFFB && state.masterMask() == 0xFB &&
            state.slaveMask() == 0xFF && state.enabled(2) && !state.enabled(12),
        "the dual-PIC mask did not represent all sixteen lines", Test);

    const IrqPolicy originalPolicy = IrqPolicy::edgeHard();
    const IrqPolicy replacementPolicy = IrqPolicy::levelHard();
    passed &= check(
        state.canRegister(12, originalPolicy),
        "an unconfigured slave line rejected its trigger mode", Test);
    state.setEnabled(2, false);
    passed &= check(
        !state.enabled(2), "an idle cascade line could not be masked", Test);
    state.handlerRegistered(12, originalPolicy);
    passed &= check(
        state.handlerCount(12) == 1 && state.enabled(2) && state.enabled(12) &&
            state.slaveMask() == 0xEF && state.edgeTriggered(12),
        "registering IRQ12 did not unmask its slave bit and cascade", Test);
    passed &= check(
        !state.canRegister(12, replacementPolicy),
        "a live shared PIC line accepted an incompatible policy", Test);

    state.setEnabled(2, false);
    passed &= check(
        state.enabled(2),
        "the cascade was masked while a slave line remained live", Test);

    const size_t earlyAckDispatch = state.beginDispatch(12);
    passed &= check(
        state.acknowledge(12),
        "an active handler could not acknowledge its dispatch", Test);
    state.completeDispatch(12, earlyAckDispatch, true);
    passed &= check(
        state.enabled(12) && !state.acknowledgementPending(12),
        "a raced acknowledgement was overwritten by the dispatch tail", Test);

    const size_t deferredAckDispatch = state.beginDispatch(12);
    state.completeDispatch(12, deferredAckDispatch, true);
    passed &= check(
        !state.enabled(12) && state.acknowledgementPending(12),
        "a dispatch requiring acknowledgement did not mask its line", Test);

    state.handlerRegistered(12, originalPolicy);
    passed &= check(
        state.handlerCount(12) == 2 && !state.enabled(12),
        "a shared registration reopened a pending-ack line", Test);
    passed &= check(
        state.acknowledge(12) && state.enabled(12) &&
            !state.acknowledgementPending(12),
        "a deferred acknowledgement did not reopen its line", Test);
    state.handlerUnregistered(12);

    // Model a new registration completing before the old unregister performs
    // its final line accounting. The live replacement must remain unmasked.
    state.handlerRegistered(12, originalPolicy);
    state.handlerUnregistered(12);
    passed &= check(
        state.handlerCount(12) == 1 && state.enabled(12),
        "an older unregister masked a concurrently registered handler", Test);

    state.handlerUnregistered(12);
    passed &= check(
        state.handlerCount(12) == 0 && !state.enabled(12),
        "the final handler did not mask its slave line", Test);
    passed &= check(
        !state.acknowledge(12) && !state.enabled(12),
        "a stale acknowledgement reopened a handlerless line", Test);
    passed &= check(
        state.canRegister(12, replacementPolicy),
        "final unregister retained stale trigger or completion policy", Test);

    // A later owner may legitimately reuse the physical line with a different
    // policy once the previous callback lifetime has drained completely.
    state.handlerRegistered(12, replacementPolicy);
    passed &= check(
        state.handlerCount(12) == 1 && state.enabled(12) &&
            !state.edgeTriggered(12) &&
            state.controllerAck(12) == IrqControllerAck::AfterHardStage &&
            state.lineRelease(12) == IrqLineRelease::AfterHardStage,
        "registration after final removal retained the previous policy",
        Test);
    state.handlerUnregistered(12);

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS pic-line-state-mask-lifecycle");
    }
    return passed;
}

bool irqPolicyOrthogonality()
{
    constexpr const char *Test = "irq-policy-orthogonality";
    const IrqPolicy edgeAckAfter(
        IrqTrigger::Edge, IrqControllerAck::AfterHardStage,
        IrqLineRelease::AfterHardStage);
    const IrqPolicy invalidEdgeOneShot(
        IrqTrigger::Edge, IrqControllerAck::AfterHardStage,
        IrqLineRelease::AfterThreadedCompletion);
    const IrqPolicy invalidSyntheticAck(
        IrqTrigger::Synthetic, IrqControllerAck::AfterHardStage,
        IrqLineRelease::AfterHardStage);
    const IrqPolicy invalidHardLevelEarlyAck(
        IrqTrigger::Level, IrqControllerAck::BeforeHardStage,
        IrqLineRelease::AfterHardStage);
    const IrqPolicy levelThreadedEarlyAck(
        IrqTrigger::Level, IrqControllerAck::BeforeHardStage,
        IrqLineRelease::AfterThreadedCompletion);

    bool passed = check(
        edgeAckAfter.validForHard() && edgeAckAfter.validForThreaded(),
        "edge trigger could not select acknowledgement order independently",
        Test);
    passed &= check(
        IrqPolicy::levelHard().validForHard() &&
            !IrqPolicy::levelHard().validForThreaded() &&
            !IrqPolicy::levelThreaded().validForHard() &&
            IrqPolicy::levelThreaded().validForThreaded(),
        "level completion policy was accepted by the wrong delivery mode",
        Test);
    passed &= check(
        !invalidEdgeOneShot.validForHard() &&
            !invalidEdgeOneShot.validForThreaded() &&
            !invalidSyntheticAck.validForHard() &&
            !invalidSyntheticAck.validForThreaded() &&
            !invalidHardLevelEarlyAck.validForHard() &&
            !invalidHardLevelEarlyAck.validForThreaded() &&
            !levelThreadedEarlyAck.validForHard() &&
            levelThreadedEarlyAck.validForThreaded(),
        "an invalid electrical or controller policy was accepted", Test);
    passed &= check(
        IrqPolicy::syntheticHard().validForHard() &&
            IrqPolicy::syntheticThreaded().validForThreaded() &&
            IrqPolicy::pciIntxHard() == IrqPolicy::levelHard() &&
            IrqPolicy::pciIntxThreaded() == IrqPolicy::levelThreaded(),
        "a named policy factory did not preserve its contract", Test);

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS irq-policy-orthogonality");
    }
    return passed;
}

struct IrqReadyPublicationContext
{
    IrqReadyPublicationContext()
        : gate(0), waiter(nullptr), armed(0), entered(0), completed(0),
          handlerCalls(0), genericPublications(0)
    {
    }

    Semaphore gate;
    Thread *waiter;
    Atomic<size_t> armed;
    Atomic<size_t> entered;
    Atomic<size_t> completed;
    Atomic<size_t> handlerCalls;
    Atomic<size_t> genericPublications;
};

IrqReadyPublicationContext *g_IrqReadyPublicationContext = nullptr;

int waitForIrqReadyPublication(void *parameter)
{
    IrqReadyPublicationContext *context =
        reinterpret_cast<IrqReadyPublicationContext *>(parameter);
    context->entered += 1;
    if (context->gate.acquireForCompletion())
    {
        context->completed += 1;
    }
    return 0;
}

class IrqReadyPublicationHandler : public HardIrqHandler
{
  public:
    explicit IrqReadyPublicationHandler(IrqReadyPublicationContext &context)
        : m_Context(context)
    {
    }

    bool irq(irq_id_t, InterruptState &) override
    {
        if (!m_Context.armed || !m_Context.handlerCalls.compareAndSwap(0, 1))
        {
            return true;
        }

        m_Context.gate.release();
        return true;
    }

  private:
    IrqReadyPublicationContext &m_Context;
};

void observeGenericThreadStatus(Thread *thread)
{
    IrqReadyPublicationContext *context = g_IrqReadyPublicationContext;
    if (context && thread == context->waiter)
    {
        context->genericPublications += 1;
    }
}

bool irqReadyPublication()
{
    constexpr const char *Test = "irq-wait-ready-publication";
    IrqManager *manager = Machine::instance().getIrqManager();
    IrqReadyPublicationContext context;
    IrqReadyPublicationHandler handler(context);
    context.waiter = new Thread(
        Scheduler::instance().getKernelProcess(), waitForIrqReadyPublication,
        &context, nullptr, false, true);
    context.waiter->setName("hosted IRQ-ready publication waiter");

    constexpr size_t Attempts = 10000;
    bool waiterBlocked = false;
    for (size_t attempt = 0; attempt < Attempts; ++attempt)
    {
        Thread::WaitDebugInfo wait = {};
        uintptr_t debugAddress = 0;
        if (context.entered == 1 && context.waiter->getWaitDebugInfo(wait) &&
            wait.queued &&
            context.waiter->getDebugState(debugAddress) == Thread::SemWait)
        {
            waiterBlocked = true;
            break;
        }
        Scheduler::instance().yield();
    }

    const irq_id_t id =
        waiterBlocked ? manager->registerHardIsaIrqHandler(
                            1, &handler, IrqPolicy::syntheticHard()) :
                        0;
    g_IrqReadyPublicationContext = &context;
    Scheduler::setGenericThreadStatusHook(observeGenericThreadStatus);
    context.armed = 1;
    const bool signalQueued = id && raise(SIGUSR2) == 0;
    context.armed = 0;
    Scheduler::setGenericThreadStatusHook(nullptr);
    g_IrqReadyPublicationContext = nullptr;

    if (!context.handlerCalls)
    {
        context.gate.release();
    }
    const bool waiterJoined = context.waiter->join();
    const bool cleaned = id && manager->unregisterHandler(id, &handler);

    bool passed = true;
    passed &= check(
        waiterBlocked, "the waiter did not publish its semaphore wait", Test);
    passed &= check(id != 0, "the IRQ handler could not be registered", Test);
    passed &= check(
        signalQueued && context.handlerCalls == 1,
        "the hosted IRQ did not dispatch exactly once", Test);
    passed &= check(
        context.genericPublications == 0,
        "the IRQ wake entered the global scheduler registry", Test);
    passed &= check(
        waiterJoined && context.completed == 1,
        "the IRQ wake did not make the waiter runnable", Test);
    passed &= check(cleaned, "the IRQ handler could not be removed", Test);
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS irq-wait-ready-publication");
    }
    return passed;
}

struct RegistryDispatchContext;
RegistryDispatchContext *g_RegistryDispatchContext = nullptr;
void dispatchWhileWriterLocked();
void dispatchWhileDeferredScopeLocked();

class RegistryDispatchHandler : public HardIrqHandler
{
  public:
    explicit RegistryDispatchHandler(RegistryDispatchContext &context)
        : m_Context(context)
    {
    }

    bool irq(irq_id_t, InterruptState &) override;

  private:
    RegistryDispatchContext &m_Context;
};

struct RegistryDispatchContext
{
    explicit RegistryDispatchContext(IrqManager *manager)
        : manager(manager), handler(*this), id(0), calls(0), hookCalls(0),
          admitted(0), handled(0), unregisterSucceeded(0), mutationRequested(0),
          deferredScopeRequested(0), deferredScopeHookCalls(0),
          deferredScopeAdmitted(0), deferredScopeHandled(0), state(nullptr)
    {
    }

    IrqManager *manager;
    RegistryDispatchHandler handler;
    irq_id_t id;
    Atomic<size_t> calls;
    Atomic<size_t> hookCalls;
    Atomic<size_t> admitted;
    Atomic<size_t> handled;
    Atomic<size_t> unregisterSucceeded;
    Atomic<size_t> mutationRequested;
    Atomic<size_t> deferredScopeRequested;
    Atomic<size_t> deferredScopeHookCalls;
    Atomic<size_t> deferredScopeAdmitted;
    Atomic<size_t> deferredScopeHandled;
    InterruptState *state;
};

bool RegistryDispatchHandler::irq(irq_id_t, InterruptState &state)
{
    m_Context.calls += 1;
    if (m_Context.mutationRequested.compareAndSwap(1, 2))
    {
        m_Context.state = &state;
        HostedIrqManager::withRegistryMutationLockForTest(
            dispatchWhileWriterLocked);
        m_Context.state = nullptr;
    }
    if (m_Context.deferredScopeRequested.compareAndSwap(1, 2))
    {
        m_Context.state = &state;
        Processor::information()
            .getCurrentThread()
            ->withDeferredScopeLockForTest(dispatchWhileDeferredScopeLocked);
        m_Context.state = nullptr;
    }
    return true;
}

void dispatchWhileWriterLocked()
{
    RegistryDispatchContext *context = g_RegistryDispatchContext;
    if (!context)
    {
        return;
    }

    context->hookCalls += 1;
    if (!context->state)
    {
        return;
    }

    bool handled = false;
    if (HostedIrqManager::dispatchHandlerForTest(
            1, &context->handler, *context->state, handled))
    {
        context->admitted += 1;
    }
    if (handled)
    {
        context->handled += 1;
    }
}

void dispatchWhileDeferredScopeLocked()
{
    RegistryDispatchContext *context = g_RegistryDispatchContext;
    if (!context)
    {
        return;
    }

    context->deferredScopeHookCalls += 1;
    if (!context->state)
    {
        return;
    }

    bool handled = false;
    if (HostedIrqManager::dispatchHandlerForTest(
            1, &context->handler, *context->state, handled))
    {
        context->deferredScopeAdmitted += 1;
    }
    if (handled)
    {
        context->deferredScopeHandled += 1;
    }
}

bool writerLockIndependentDispatch()
{
    constexpr const char *Test = "irq-dispatch-writer-lock-independent";
    IrqManager *manager = Machine::instance().getIrqManager();
    RegistryDispatchContext context(manager);
    context.id = manager->registerHardIsaIrqHandler(
        1, &context.handler, IrqPolicy::syntheticHard());

    g_RegistryDispatchContext = &context;
    context.mutationRequested = 1;
    const bool signalQueued = raise(SIGUSR2) == 0;
    g_RegistryDispatchContext = nullptr;

    const bool cleaned =
        context.id && manager->unregisterHandler(context.id, &context.handler);
    bool passed = true;
    passed &= check(
        context.id != 0, "the test handler could not be registered", Test);
    passed &= check(
        signalQueued && context.hookCalls == 1 && context.admitted == 1 &&
            context.handled == 1 && context.calls >= 2,
        "dispatch or callback completion waited for the writer lock", Test);
    passed &= check(cleaned, "the test handler could not be removed", Test);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "irq-dispatch-writer-lock-independent");
    }
    return passed;
}

struct WriterLockedRemovalContext;
WriterLockedRemovalContext *g_WriterLockedRemovalContext = nullptr;
void dispatchWriterLockedSelfRemoval();

class WriterLockedSelfRemovingHandler : public HardIrqHandler
{
  public:
    explicit WriterLockedSelfRemovingHandler(
        WriterLockedRemovalContext &context)
        : m_Context(context)
    {
    }

    bool irq(irq_id_t, InterruptState &state) override;

  private:
    WriterLockedRemovalContext &m_Context;
};

struct WriterLockedRemovalContext
{
    explicit WriterLockedRemovalContext(IrqManager *manager)
        : manager(manager), handler(*this), id(0), state(nullptr), armed(0),
          phase(0), outerCalls(0), nestedCalls(0), removalRejected(0),
          nestedAdmitted(0), nestedHandled(0)
    {
    }

    IrqManager *manager;
    WriterLockedSelfRemovingHandler handler;
    irq_id_t id;
    InterruptState *state;
    Atomic<size_t> armed;
    Atomic<size_t> phase;
    Atomic<size_t> outerCalls;
    Atomic<size_t> nestedCalls;
    Atomic<size_t> removalRejected;
    Atomic<size_t> nestedAdmitted;
    Atomic<size_t> nestedHandled;
};

bool WriterLockedSelfRemovingHandler::irq(irq_id_t, InterruptState &state)
{
    if (!m_Context.armed)
    {
        return true;
    }

    if (m_Context.phase.compareAndSwap(0, 1))
    {
        m_Context.outerCalls += 1;
        m_Context.state = &state;
        HostedIrqManager::withRegistryMutationLockForTest(
            dispatchWriterLockedSelfRemoval);
        m_Context.state = nullptr;
    }
    else if (m_Context.phase.compareAndSwap(1, 2))
    {
        m_Context.nestedCalls += 1;
        if (!m_Context.manager->unregisterHandler(m_Context.id, this))
        {
            m_Context.removalRejected += 1;
        }
    }
    return true;
}

void dispatchWriterLockedSelfRemoval()
{
    WriterLockedRemovalContext *context = g_WriterLockedRemovalContext;
    if (!context || !context->state)
    {
        return;
    }

    bool handled = false;
    if (HostedIrqManager::dispatchHandlerForTest(
            1, &context->handler, *context->state, handled))
    {
        context->nestedAdmitted += 1;
    }
    if (handled)
    {
        context->nestedHandled += 1;
    }
}

bool writerLockSelfUnregister()
{
    constexpr const char *Test = "irq-writer-lock-self-unregister";
    IrqManager *manager = Machine::instance().getIrqManager();
    WriterLockedRemovalContext context(manager);

    context.id = manager->registerHardIsaIrqHandler(
        1, &context.handler, IrqPolicy::syntheticHard());
    const bool identifierSeeded =
        context.id && manager->unregisterHandler(context.id, &context.handler);
    const irq_id_t activeId =
        manager->registerHardIsaIrqHandler(
            1, &context.handler, IrqPolicy::syntheticHard());

    g_WriterLockedRemovalContext = &context;
    context.armed = 1;
    const bool signalQueued =
        activeId && activeId == context.id && raise(SIGUSR2) == 0;
    context.armed = 0;
    g_WriterLockedRemovalContext = nullptr;

    const bool retired =
        !HostedIrqManager::containsHandlerForTest(1, &context.handler);
    const irq_id_t reusedId =
        manager->registerHardIsaIrqHandler(
            1, &context.handler, IrqPolicy::syntheticHard());
    const bool reused = reusedId != 0;
    bool cleaned = false;
    if (reused)
    {
        cleaned = manager->unregisterHandler(reusedId, &context.handler);
    }
    else
    {
        const irq_id_t cleanupId = activeId ? activeId : context.id;
        cleaned = cleanupId &&
                  manager->unregisterHandler(cleanupId, &context.handler);
    }

    bool passed = true;
    passed &= check(
        identifierSeeded && activeId == context.id,
        "the test handler could not be registered consistently", Test);
    passed &= check(
        signalQueued && context.phase == 2 && context.outerCalls == 1 &&
            context.nestedCalls == 1 && context.removalRejected == 1 &&
            context.nestedAdmitted == 1 && context.nestedHandled == 1,
        "self-removal entered or stalled on the held writer lock", Test);
    passed &= check(
        retired && reused && cleaned,
        "deferred self-removal did not retire and release the slot", Test);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "irq-writer-lock-self-unregister");
    }
    return passed;
}

bool deferredScopeLockIndependentDispatch()
{
    constexpr const char *Test = "irq-dispatch-deferred-scope-lock-independent";
    IrqManager *manager = Machine::instance().getIrqManager();
    RegistryDispatchContext context(manager);
    context.id = manager->registerHardIsaIrqHandler(
        1, &context.handler, IrqPolicy::syntheticHard());

    g_RegistryDispatchContext = &context;
    context.deferredScopeRequested = 1;
    const bool signalQueued = raise(SIGUSR2) == 0;
    g_RegistryDispatchContext = nullptr;

    const bool cleaned =
        context.id && manager->unregisterHandler(context.id, &context.handler);
    bool passed = true;
    passed &= check(
        context.id != 0, "the test handler could not be registered", Test);
    passed &= check(
        signalQueued && context.deferredScopeHookCalls == 1 &&
            context.deferredScopeAdmitted == 1 &&
            context.deferredScopeHandled == 1 && context.calls >= 2,
        "dispatch entered the interrupted Thread's deferred-scope lock", Test);
    passed &= check(cleaned, "the test handler could not be removed", Test);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "irq-dispatch-deferred-scope-lock-independent");
    }
    return passed;
}

void unregisterBeforePin(IrqHandlerBase *handler)
{
    RegistryDispatchContext *context = g_RegistryDispatchContext;
    if (!context || handler != &context->handler ||
        !context->hookCalls.compareAndSwap(0, 1))
    {
        return;
    }

    if (context->manager->unregisterHandler(context->id, &context->handler))
    {
        context->unregisterSucceeded += 1;
    }
}

bool prePinUnregisterRevalidation()
{
    constexpr const char *Test = "irq-pre-pin-unregister-revalidation";
    IrqManager *manager = Machine::instance().getIrqManager();
    RegistryDispatchContext context(manager);
    context.id = manager->registerHardIsaIrqHandler(
        1, &context.handler, IrqPolicy::syntheticHard());

    g_RegistryDispatchContext = &context;
    const size_t callsBeforeDispatch = context.calls;
    HostedIrqManager::setHandlerPrePinHook(unregisterBeforePin);
    const bool signalQueued = raise(SIGUSR2) == 0;
    HostedIrqManager::setHandlerPrePinHook(nullptr);
    g_RegistryDispatchContext = nullptr;

    const irq_id_t reusedId =
        manager->registerHardIsaIrqHandler(
            1, &context.handler, IrqPolicy::syntheticHard());
    const bool reused = reusedId != 0;
    const bool cleaned =
        reused && manager->unregisterHandler(reusedId, &context.handler);

    bool passed = true;
    passed &= check(
        context.id != 0, "the test handler could not be registered", Test);
    passed &= check(
        context.hookCalls == 1 && context.unregisterSucceeded == 1,
        "unregister did not retire the pre-pin publication", Test);
    passed &= check(
        signalQueued && context.calls == callsBeforeDispatch,
        "a stale pre-pin snapshot entered the retired callback", Test);
    passed &= check(
        reused && cleaned,
        "the revalidated slot could not be reused and removed", Test);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "irq-pre-pin-unregister-revalidation");
    }
    return passed;
}

struct StalePublicationContext
{
    explicit StalePublicationContext(IrqManager *manager)
        : manager(manager), originalId(0), replacementId(0), prePinCalls(0),
          originalRemoved(0), replacementRegistered(0), committedCalls(0),
          replacementRemoved(0)
    {
    }

    IrqManager *manager;
    HardRegistryHandler original;
    HardRegistryHandler replacement;
    irq_id_t originalId;
    irq_id_t replacementId;
    Atomic<size_t> prePinCalls;
    Atomic<size_t> originalRemoved;
    Atomic<size_t> replacementRegistered;
    Atomic<size_t> committedCalls;
    Atomic<size_t> replacementRemoved;
};

StalePublicationContext *g_StalePublicationContext = nullptr;

void replaceHandlerBeforeHazardClaim(IrqHandlerBase *handler)
{
    StalePublicationContext *context = g_StalePublicationContext;
    if (!context || handler != &context->original ||
        !context->prePinCalls.compareAndSwap(0, 1))
    {
        return;
    }

    if (context->manager->unregisterHandler(
            context->originalId, &context->original))
    {
        context->originalRemoved += 1;
    }
    context->replacementId =
        context->manager->registerHardIsaIrqHandler(
            1, &context->replacement, IrqPolicy::syntheticHard());
    if (context->replacementId)
    {
        context->replacementRegistered += 1;
    }
}

void removeReplacementAfterStaleCommit(
    IrqHandlerBase *handler, IrqHandlerRegistry::HandlerHazardStage hazardStage)
{
    StalePublicationContext *context = g_StalePublicationContext;
    if (!context || handler != &context->replacement ||
        hazardStage != IrqHandlerRegistry::HandlerHazardStage::Committed ||
        !context->committedCalls.compareAndSwap(0, 1))
    {
        return;
    }

    if (context->manager->unregisterHandler(
            context->replacementId, &context->replacement))
    {
        context->replacementRemoved += 1;
    }
}

bool staleGenerationRevalidation()
{
    constexpr const char *Test = "irq-stale-generation-reuse";
    IrqManager *manager = Machine::instance().getIrqManager();
    // Test hooks can already be loaded by another hosted signal when they are
    // disabled. Keep their backing storage alive for the module lifetime.
    static StalePublicationContext context(manager);
    context.manager = manager;
    context.originalId = 0;
    context.replacementId = 0;
    context.prePinCalls = 0;
    context.originalRemoved = 0;
    context.replacementRegistered = 0;
    context.committedCalls = 0;
    context.replacementRemoved = 0;
    context.originalId =
        manager->registerHardIsaIrqHandler(
            1, &context.original, IrqPolicy::syntheticHard());

    g_StalePublicationContext = &context;
    HostedIrqManager::setHandlerPrePinHook(replaceHandlerBeforeHazardClaim);
    HostedIrqManager::setHandlerHazardHook(removeReplacementAfterStaleCommit);
    const bool signalQueued = context.originalId && raise(SIGUSR2) == 0;
    HostedIrqManager::setHandlerHazardHook(nullptr);
    HostedIrqManager::setHandlerPrePinHook(nullptr);
    g_StalePublicationContext = nullptr;

    const bool replacementStillPublished =
        HostedIrqManager::containsHandlerForTest(1, &context.replacement);
    bool failureCleanup = true;
    if (replacementStillPublished)
    {
        failureCleanup = context.replacementId &&
                         manager->unregisterHandler(
                             context.replacementId, &context.replacement);
        if (!failureCleanup)
        {
            const irq_id_t revivedId =
                manager->registerHardIsaIrqHandler(
                    1, &context.replacement, IrqPolicy::syntheticHard());
            failureCleanup = revivedId && manager->unregisterHandler(
                                              revivedId, &context.replacement);
        }
    }
    if (HostedIrqManager::containsHandlerForTest(1, &context.original))
    {
        failureCleanup &=
            manager->unregisterHandler(context.originalId, &context.original);
    }

    const size_t activeOriginal =
        HostedIrqManager::activeDispatchCountForTest(&context.original);
    const size_t activeReplacement =
        HostedIrqManager::activeDispatchCountForTest(&context.replacement);
    const size_t claimed = HostedIrqManager::claimedDispatchCountForTest();

    bool passed = true;
    passed &= check(
        signalQueued && context.prePinCalls == 1 &&
            context.originalRemoved == 1 &&
            context.replacementRegistered == 1 && context.committedCalls == 1 &&
            context.replacementRemoved == 1,
        "a stale dispatch blocked removal of the replacement generation", Test);
    passed &= check(
        !replacementStillPublished && !activeOriginal && !activeReplacement &&
            !claimed && failureCleanup,
        "stale generation cleanup left a publication or callback hazard", Test);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "irq-stale-generation-reuse");
    }
    return passed;
}

struct AbandonedDispatchContext;
AbandonedDispatchContext *g_AbandonedDispatchContext = nullptr;

enum class AbandonedDispatchStage
{
    BeforeClaim,
    Claimed,
    Callback,
};

class AbandoningIrqHandler : public HardIrqHandler
{
  public:
    explicit AbandoningIrqHandler(AbandonedDispatchContext &context)
        : m_Context(context)
    {
    }

    bool irq(irq_id_t, InterruptState &) override;

  private:
    AbandonedDispatchContext &m_Context;
};

struct AbandonedDispatchContext
{
    explicit AbandonedDispatchContext(AbandonedDispatchStage stage)
        : handler(*this), worker(nullptr), stage(stage), hazardCalls(0),
          entered(0), returned(0)
    {
    }

    AbandoningIrqHandler handler;
    Thread *worker;
    AbandonedDispatchStage stage;
    Atomic<size_t> hazardCalls;
    Atomic<size_t> entered;
    Atomic<size_t> returned;
};

bool AbandoningIrqHandler::irq(irq_id_t, InterruptState &)
{
    if (m_Context.stage != AbandonedDispatchStage::Callback ||
        Processor::information().getCurrentThread() != m_Context.worker)
    {
        return true;
    }

    m_Context.entered += 1;
    HostedIrqManager::abandonCurrentThreadForTest();
    return true;
}

int abandonIrqDispatch(void *parameter)
{
    AbandonedDispatchContext *context =
        reinterpret_cast<AbandonedDispatchContext *>(parameter);
    raise(SIGUSR2);
    context->returned += 1;
    return 1;
}

void abandonIrqHazard(
    IrqHandlerBase *handler, IrqHandlerRegistry::HandlerHazardStage hazardStage)
{
    AbandonedDispatchContext *context = g_AbandonedDispatchContext;
    if (!context || handler != &context->handler ||
        Processor::information().getCurrentThread() != context->worker)
    {
        return;
    }

    const bool targetStage =
        (context->stage == AbandonedDispatchStage::BeforeClaim &&
         hazardStage == IrqHandlerRegistry::HandlerHazardStage::BeforeClaim) ||
        (context->stage == AbandonedDispatchStage::Claimed &&
         hazardStage == IrqHandlerRegistry::HandlerHazardStage::Claimed);
    if (!targetStage)
    {
        return;
    }

    context->hazardCalls += 1;
    HostedIrqManager::abandonCurrentThreadForTest();
}

bool abandonedDispatchStage(
    AbandonedDispatchStage stage, size_t expectedHazardCalls,
    size_t expectedCallbackCalls)
{
    IrqManager *manager = Machine::instance().getIrqManager();
    // A hosted signal can retain a loaded test hook while it is being
    // disabled, so the context must outlive this individual test call.
    static AbandonedDispatchContext beforeClaim(
        AbandonedDispatchStage::BeforeClaim);
    static AbandonedDispatchContext claimedContext(
        AbandonedDispatchStage::Claimed);
    static AbandonedDispatchContext callbackContext(
        AbandonedDispatchStage::Callback);
    AbandonedDispatchContext *context =
        stage == AbandonedDispatchStage::BeforeClaim ? &beforeClaim :
        stage == AbandonedDispatchStage::Claimed     ? &claimedContext :
                                                       &callbackContext;
    context->stage = stage;
    context->hazardCalls = 0;
    context->entered = 0;
    context->returned = 0;
    const irq_id_t id =
        manager->registerHardIsaIrqHandler(
            1, &context->handler, IrqPolicy::syntheticHard());

    context->worker = new Thread(
        Scheduler::instance().getKernelProcess(), abandonIrqDispatch, context,
        nullptr, false, true, true);
    context->worker->setName("hosted abandoned IRQ publication");

    g_AbandonedDispatchContext = context;
    if (stage != AbandonedDispatchStage::Callback)
    {
        HostedIrqManager::setHandlerHazardHook(abandonIrqHazard);
    }
    const bool started = id && context->worker->start();
    const bool joined = started && context->worker->joinForCompletion();
    HostedIrqManager::setHandlerHazardHook(nullptr);
    g_AbandonedDispatchContext = nullptr;

    const size_t active =
        HostedIrqManager::activeDispatchCountForTest(&context->handler);
    const size_t claimed = HostedIrqManager::claimedDispatchCountForTest();
    const bool cleaned =
        id && manager->unregisterHandler(id, &context->handler);

    const bool passed = id && started && joined && !context->returned &&
                        context->hazardCalls == expectedHazardCalls &&
                        context->entered == expectedCallbackCalls && !active &&
                        !claimed && cleaned;
    return passed;
}

bool abandonedDispatchCleanup()
{
    constexpr const char *Test = "irq-abandoned-dispatch-cleanup";
    const bool beforeClaim =
        abandonedDispatchStage(AbandonedDispatchStage::BeforeClaim, 1, 0);
    const bool claimed =
        abandonedDispatchStage(AbandonedDispatchStage::Claimed, 1, 0);
    const bool committed =
        abandonedDispatchStage(AbandonedDispatchStage::Callback, 0, 1);

    bool passed = true;
    passed &= check(
        beforeClaim && claimed && committed,
        "stack abandonment leaked an unclaimed, partial, or committed hazard",
        Test);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "irq-abandoned-dispatch-cleanup");
    }
    return passed;
}

struct HandlerLifetimeContext;
HandlerLifetimeContext *g_HandlerLifetimeContext = nullptr;

class LifetimeHandler : public HardIrqHandler
{
  public:
    explicit LifetimeHandler(HandlerLifetimeContext &context)
        : m_Context(context)
    {
    }

    bool irq(irq_id_t, InterruptState &) override;

  private:
    HandlerLifetimeContext &m_Context;
};

struct HandlerLifetimeContext
{
    explicit HandlerLifetimeContext(IrqManager *manager)
        : manager(manager), handler(*this), id(0), dispatcher(nullptr),
          remover(nullptr), dispatchEntered(0), releaseDispatch(0), phase(0),
          hookCalls(0), hookObservedDrain(0), releaseHookCalls(0),
          releaseObservedAtomicDrain(0), dispatchAdmitted(0),
          dispatchHandled(0), handlerCalls(0), callbacksAfterReturn(0),
          unregisterReturned(0), unregisterSucceeded(0), failures(0)
    {
    }

    IrqManager *manager;
    LifetimeHandler handler;
    irq_id_t id;
    Thread *dispatcher;
    Thread *remover;
    Semaphore dispatchEntered;
    Semaphore releaseDispatch;
    Atomic<size_t> phase;
    Atomic<size_t> hookCalls;
    Atomic<size_t> hookObservedDrain;
    Atomic<size_t> releaseHookCalls;
    Atomic<size_t> releaseObservedAtomicDrain;
    Atomic<size_t> dispatchAdmitted;
    Atomic<size_t> dispatchHandled;
    Atomic<size_t> handlerCalls;
    Atomic<size_t> callbacksAfterReturn;
    Atomic<size_t> unregisterReturned;
    Atomic<size_t> unregisterSucceeded;
    Atomic<size_t> failures;
};

bool LifetimeHandler::irq(irq_id_t, InterruptState &)
{
    m_Context.handlerCalls += 1;
    if (m_Context.unregisterReturned)
    {
        m_Context.callbacksAfterReturn += 1;
    }
    return true;
}

class SelfRemovingHandler : public HardIrqHandler
{
  public:
    SelfRemovingHandler(IrqManager *manager, irq_id_t id)
        : m_Manager(manager), m_Id(id), calls(0), rejectionSeen(0)
    {
    }

    bool irq(irq_id_t, InterruptState &) override
    {
        calls += 1;
        if (!m_Manager->unregisterHandler(m_Id, this))
        {
            rejectionSeen += 1;
        }
        return true;
    }

    IrqManager *m_Manager;
    irq_id_t m_Id;
    Atomic<size_t> calls;
    Atomic<size_t> rejectionSeen;
};

void handlerPinHook(IrqHandlerBase *handler)
{
    HandlerLifetimeContext *context = g_HandlerLifetimeContext;
    if (!context || handler != &context->handler ||
        !context->phase.compareAndSwap(0, 1))
    {
        return;
    }

    context->hookCalls += 1;
    context->dispatchEntered.release();
    if (!context->releaseDispatch.acquireForCompletion())
    {
        context->failures += 1;
    }
    context->phase = 3;
}

void handlerReleaseHook(
    IrqHandlerBase *handler, IrqHandlerRegistry::HandlerHazardStage hazardStage)
{
    HandlerLifetimeContext *context = g_HandlerLifetimeContext;
    if (!context || handler != &context->handler ||
        hazardStage != IrqHandlerRegistry::HandlerHazardStage::Released ||
        !context->releaseHookCalls.compareAndSwap(0, 1))
    {
        return;
    }

    Thread *current = Processor::information().getCurrentThread();
    Thread::WaitDebugInfo wait = {};
    uintptr_t debugAddress = 0;
    if (current && current == context->dispatcher &&
        !current->getHostedSignalDepth() && Processor::getInterrupts() &&
        context->phase == static_cast<size_t>(3) &&
        !context->unregisterReturned &&
        !context->remover->getWaitDebugInfo(wait) &&
        context->remover->getDebugState(debugAddress) ==
            Thread::CallbackDrain &&
        debugAddress == reinterpret_cast<uintptr_t>(&context->handler))
    {
        context->releaseObservedAtomicDrain += 1;
    }
    else
    {
        context->failures += 1;
    }
}

int dispatchPinnedHandler(void *parameter)
{
    HandlerLifetimeContext *context =
        reinterpret_cast<HandlerLifetimeContext *>(parameter);
    bool handled = false;
    const bool admitted = HostedIrqManager::dispatchHandlerForTest(
        2, &context->handler, handled);
    context->dispatchAdmitted = admitted ? 1 : 0;
    context->dispatchHandled = handled ? 1 : 0;
    return admitted && handled ? 0 : 1;
}

int unregisterPinnedHandler(void *parameter)
{
    HandlerLifetimeContext *context =
        reinterpret_cast<HandlerLifetimeContext *>(parameter);
    const Time::Timestamp deadline =
        Time::getTicks() + (500 * Time::Multiplier::Millisecond);
    while (context->phase != static_cast<size_t>(1) &&
           Time::getTicks() < deadline)
    {
        Scheduler::instance().yield();
    }

    if (context->phase != static_cast<size_t>(1))
    {
        context->failures += 1;
        return 1;
    }

    context->phase = 2;
    if (context->manager->unregisterHandler(context->id, &context->handler))
    {
        context->unregisterSucceeded += 1;
    }
    context->unregisterReturned += 1;
    context->phase = 4;
    return 0;
}

bool handlerLifetimeBarrier()
{
    IrqManager *manager = Machine::instance().getIrqManager();
    HandlerLifetimeContext context(manager);

    // Learn the manager-owned identifier before enabling the pin hook. The
    // identifier is assigned after hard registration returns, while an
    // IRQ is free to arrive as soon as the slot becomes visible.
    context.id = manager->registerHardIsaIrqHandler(
        2, &context.handler, IrqPolicy::syntheticHard());
    const bool identifierSeeded =
        context.id && manager->unregisterHandler(context.id, &context.handler);

    context.remover = new Thread(
        Scheduler::instance().getKernelProcess(), unregisterPinnedHandler,
        &context, nullptr, false, true, true);
    context.remover->setName("hosted IRQ-handler remover");
    context.dispatcher = new Thread(
        Scheduler::instance().getKernelProcess(), dispatchPinnedHandler,
        &context, nullptr, false, true, true);
    context.dispatcher->setName("hosted IRQ-handler dispatcher");

    g_HandlerLifetimeContext = &context;
    HostedIrqManager::setHandlerPinHook(handlerPinHook);
    HostedIrqManager::setHandlerHazardHook(handlerReleaseHook);
    const irq_id_t activeId =
        manager->registerHardIsaIrqHandler(
            2, &context.handler, IrqPolicy::syntheticHard());
    const bool registered =
        identifierSeeded && activeId && activeId == context.id;
    const bool dispatcherStarted = registered && context.dispatcher->start();
    const bool dispatchEntered =
        dispatcherStarted && context.dispatchEntered.acquireForCompletion();
    const bool removerStarted = dispatchEntered && context.remover->start();

    bool drainObserved = false;
    const Time::Timestamp drainDeadline =
        Time::getTicks() + (500 * Time::Multiplier::Millisecond);
    while (removerStarted && Time::getTicks() < drainDeadline)
    {
        Thread::WaitDebugInfo wait = {};
        uintptr_t debugAddress = 0;
        if (context.phase == static_cast<size_t>(2) &&
            !context.unregisterReturned &&
            !context.remover->getWaitDebugInfo(wait) &&
            context.remover->getDebugState(debugAddress) ==
                Thread::CallbackDrain &&
            debugAddress == reinterpret_cast<uintptr_t>(&context.handler))
        {
            drainObserved = true;
            context.hookObservedDrain += 1;
            break;
        }
        Scheduler::instance().yield();
    }
    if (!drainObserved)
    {
        context.failures += 1;
    }
    context.releaseDispatch.release();
    const bool dispatcherJoined =
        dispatcherStarted && context.dispatcher->joinForCompletion();
    const bool removerJoined =
        removerStarted && context.remover->joinForCompletion();
    HostedIrqManager::setHandlerHazardHook(nullptr);
    HostedIrqManager::setHandlerPinHook(nullptr);
    g_HandlerLifetimeContext = nullptr;

    bool primaryCleanup = true;
    if (activeId && !context.unregisterSucceeded)
    {
        primaryCleanup = manager->unregisterHandler(activeId, &context.handler);
    }

    const size_t callsAtUnregisterReturn = context.handlerCalls;
    const Time::Timestamp postReturnDeadline =
        Time::getTicks() + (10 * Time::Multiplier::Millisecond);
    while (Time::getTicks() < postReturnDeadline)
    {
        Scheduler::instance().yield();
    }

    SelfRemovingHandler selfRemoving(manager, context.id);
    const irq_id_t selfId =
        manager->registerHardIsaIrqHandler(
            2, &selfRemoving, IrqPolicy::syntheticHard());
    bool selfHandled = false;
    const bool selfAdmitted = selfId &&
                              HostedIrqManager::dispatchHandlerForTest(
                                  2, &selfRemoving, selfHandled);
    const size_t selfCallsAfterRetirement = selfRemoving.calls;

    // A callback cannot wait for its own pin. The rejected synchronous
    // contract still closes admission and retires the slot on return.
    const irq_id_t selfReregisteredId =
        manager->registerHardIsaIrqHandler(
            2, &selfRemoving, IrqPolicy::syntheticHard());
    const bool selfReregistered = selfReregisteredId != 0;
    bool selfCleanup = false;
    if (selfReregistered)
    {
        selfCleanup =
            manager->unregisterHandler(selfReregisteredId, &selfRemoving);
    }
    else if (selfId)
    {
        // Keep a failed assertion from leaving a stack-owned handler in the
        // live registry.
        selfCleanup = manager->unregisterHandler(selfId, &selfRemoving);
    }

    bool passed = true;
    passed &= check(
        registered, "the test handler could not be registered consistently");
    passed &= check(
        dispatcherJoined && removerJoined && context.failures == 0,
        "the concurrent unregister worker did not complete cleanly");
    passed &= check(
        context.hookCalls == 1 && context.hookObservedDrain == 1,
        "unregister returned instead of waiting for the pinned callback");
    passed &= check(
        context.releaseHookCalls == 1 &&
            context.releaseObservedAtomicDrain == 1,
        "hard callback release entered a wait-queue or scheduler wake path");
    passed &= check(
        context.dispatchAdmitted == 1 && context.dispatchHandled == 1,
        "the controlled registry dispatch did not reach the handler");
    passed &= check(
        context.unregisterSucceeded == 1 && context.unregisterReturned == 1,
        "the pinned handler did not unregister successfully");
    passed &= check(
        primaryCleanup,
        "failed primary assertions left the test handler registered");
    passed &= check(
        context.handlerCalls >= 1 &&
            context.handlerCalls == callsAtUnregisterReturn &&
            context.callbacksAfterReturn == 0,
        "a callback began after unregisterHandler returned");
    passed &= check(
        selfId != 0 && selfAdmitted && selfHandled &&
            selfRemoving.rejectionSeen == 1 &&
            selfCallsAfterRetirement == 1 &&
            selfRemoving.calls == selfCallsAfterRetirement,
        "self-unregister was not deferred and retired after callback return");
    passed &= check(
        selfReregistered && selfCleanup,
        "deferred self-removal did not release its registry slot");

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS irq-handler-atomic-drain");
        NOTICE("HOSTED-WAIT-TEST: PASS irq-handler-lifetime");
    }
    return passed;
}
}  // namespace

bool runHostedIrqRegressions()
{
    bool passed = deliveryModeSeparation();
    passed &= irqPolicyOrthogonality();
    passed &= picLineStateLifecycle();
    passed &= irqReadyPublication();
    passed &= writerLockIndependentDispatch();
    passed &= writerLockSelfUnregister();
    passed &= deferredScopeLockIndependentDispatch();
    passed &= prePinUnregisterRevalidation();
    passed &= staleGenerationRevalidation();
    passed &= abandonedDispatchCleanup();
    passed &= handlerLifetimeBarrier();
    return passed;
}
