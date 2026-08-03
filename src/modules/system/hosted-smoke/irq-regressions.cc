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
#include "pedigree/kernel/process/AtomicStateCleanup.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/time/Time.h"
#include "system/kernel/machine/hosted/IrqManager.h"
#include "system/kernel/machine/mach_pc/PicIrqState.h"

#include <signal.h>

namespace
{
constexpr size_t OuterSyntheticDispatchGeneration = 0xC001;
constexpr size_t InnerSyntheticDispatchGeneration = 0xC002;
// IRQ 1 is the live scheduler timer and can retain a dispatch while another
// Thread runs. Use the signal-backed line reserved for hosted regressions.
constexpr uint8_t HardContextTestIrq = 2;
constexpr int HardContextTestSignal = SIGURG;

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

class CountingHardRegistryHandler : public HardIrqHandler
{
  public:
    CountingHardRegistryHandler() : calls(0)
    {
    }

    bool irq(irq_id_t, InterruptState &) override
    {
        ++calls;
        return true;
    }

    size_t calls;
};

class DispositionHandler : public IrqHandler
{
  public:
    explicit DispositionHandler(IrqDisposition disposition)
        : disposition(disposition), calls(0)
    {
    }

    IrqDisposition irq(irq_id_t) override
    {
        ++calls;
        return disposition;
    }

    IrqDisposition disposition;
    size_t calls;
};

class SelfCancellingThreadedHandler : public IrqHandler
{
  public:
    SelfCancellingThreadedHandler(IrqHandlerRegistry &registry, uint8_t line)
        : m_Registry(registry), m_Line(line), calls(0), deferred(false)
    {
    }

    IrqDisposition irq(irq_id_t) override
    {
        ++calls;
        deferred = m_Registry.unregisterHandler(m_Line, this) ==
                   IrqHandlerRegistry::UnregisterResult::Deferred;
        return IrqDisposition::NotHandled;
    }

    IrqHandlerRegistry &m_Registry;
    uint8_t m_Line;
    size_t calls;
    bool deferred;
};

struct PostCallbackCancellationContext
{
    IrqHandlerRegistry *registry;
    IrqHandlerBase *handler;
    uint8_t line;
    size_t releases;
    IrqHandlerRegistry::UnregisterResult result;
};

struct QuiescedHandoffContext
{
    IrqHandlerRegistry *registry;
    IrqHandlerBase *handler;
    uint8_t line;
    Atomic<size_t> calls;
    IrqHandlerRegistry::UnregisterResult removalResult;
};

QuiescedHandoffContext *g_QuiescedHandoff = nullptr;

void removeAfterQuiescedObservation(
    IrqHandlerBase *handler, IrqHandlerRegistry::HandlerHazardStage stage)
{
    QuiescedHandoffContext *context = g_QuiescedHandoff;
    if (!context || handler != context->handler ||
        stage != IrqHandlerRegistry::HandlerHazardStage::QuiescedObserved ||
        !context->calls.compareAndSwap(0, 1))
    {
        return;
    }

    context->removalResult =
        context->registry->unregisterHandler(context->line, context->handler);
}

struct AbandonedActionMutationContext
{
    IrqHandlerRegistry *registry;
    IrqHandlerBase *handler;
    Thread *worker;
    uint8_t line;
    size_t generation;
    IrqHandlerRegistry::HandlerHazardStage abandonStage;
    Atomic<size_t> hookCalls;
    Atomic<size_t> returned;
};

AbandonedActionMutationContext *g_AbandonedActionMutation = nullptr;

enum class AbandonedCutoffPath
{
    MixedHard,
    ThreadedPublication,
    ThreadedWorker,
};

struct AbandonedCutoffContext
{
    AbandonedCutoffContext(
        AbandonedCutoffPath dispatchPath, uint8_t irqLine, size_t generation)
        : threaded(IrqDisposition::Handled), worker(nullptr), path(dispatchPath),
          line(irqLine), dispatchGeneration(generation), mixedCutoffs(),
          admissionCutoff(), cutoffCaptured(0), hookCalls(0), returned(0)
    {
    }

    IrqHandlerRegistry registry;
    HardRegistryHandler hard;
    DispositionHandler threaded;
    Thread *worker;
    AbandonedCutoffPath path;
    uint8_t line;
    size_t dispatchGeneration;
    IrqHandlerRegistry::MixedAdmissionCutoffs mixedCutoffs;
    IrqHandlerRegistry::AdmissionCutoff admissionCutoff;
    Atomic<size_t> cutoffCaptured;
    Atomic<size_t> hookCalls;
    Atomic<size_t> returned;
};

AbandonedCutoffContext *g_AbandonedCutoff = nullptr;

void abandonCutoffDispatch(
    IrqHandlerBase *handler, IrqHandlerRegistry::HandlerHazardStage stage)
{
    AbandonedCutoffContext *context = g_AbandonedCutoff;
    IrqHandlerBase *expectedHandler =
        context && context->path == AbandonedCutoffPath::MixedHard ?
            static_cast<IrqHandlerBase *>(&context->hard) :
            context ? static_cast<IrqHandlerBase *>(&context->threaded) :
                      nullptr;
    if (!context || handler != expectedHandler ||
        stage != IrqHandlerRegistry::HandlerHazardStage::BeforeClaim ||
        Processor::information().getCurrentThread() != context->worker ||
        !context->hookCalls.compareAndSwap(0, 1))
    {
        return;
    }

    HostedIrqManager::abandonCurrentThreadForTest();
}

int abandonCutoffWorker(void *parameter)
{
    AbandonedCutoffContext *context =
        reinterpret_cast<AbandonedCutoffContext *>(parameter);
    if (context->path == AbandonedCutoffPath::MixedHard)
    {
        if (!context->registry.captureMixedAdmissionCutoffs(
                context->line, context->mixedCutoffs))
        {
            return 1;
        }
        context->cutoffCaptured = 1;
        alignas(InterruptState) uint8_t stateStorage[sizeof(InterruptState)] =
            {};
        InterruptState &state =
            *reinterpret_cast<InterruptState *>(stateStorage);
        bool handled = false;
        context->registry.dispatchHard(
            context->line, state, handled, &context->hard,
            context->dispatchGeneration, context->mixedCutoffs.hard);
    }
    else if (context->path == AbandonedCutoffPath::ThreadedPublication)
    {
        if (!context->registry.captureAdmissionCutoff(
                context->line, context->admissionCutoff))
        {
            return 1;
        }
        context->cutoffCaptured = 1;
        context->registry.publishThreadedDispatch(
            context->line, context->dispatchGeneration,
            context->admissionCutoff);
    }
    else
    {
        IrqHandlerRegistry::ThreadedDispatchResult result = {};
        context->registry.dispatchThreaded(
            context->line, context->dispatchGeneration, result,
            &context->threaded);
    }
    context->returned += 1;
    return 1;
}

struct ClaimedCancellationContext
{
    IrqHandlerRegistry *registry;
    IrqHandlerBase *handler;
    size_t cancelledGeneration;
    Atomic<size_t> hookCalls;
    Atomic<size_t> consumedCancelledQuiesce;
};

ClaimedCancellationContext *g_ClaimedCancellation = nullptr;

void consumeCancelledQuiesceAfterClaimClear(
    IrqHandlerBase *handler, IrqHandlerRegistry::HandlerHazardStage stage)
{
    ClaimedCancellationContext *context = g_ClaimedCancellation;
    if (!context || handler != context->handler ||
        stage !=
            IrqHandlerRegistry::HandlerHazardStage::CancellationClaimCleared ||
        !context->hookCalls.compareAndSwap(0, 1))
    {
        return;
    }

    if (context->registry->consumeThreadedQuiescedForTest(
            context->handler, context->cancelledGeneration))
    {
        context->consumedCancelledQuiesce += 1;
    }
}

void abandonActionMutation(
    IrqHandlerBase *handler, IrqHandlerRegistry::HandlerHazardStage stage)
{
    AbandonedActionMutationContext *context = g_AbandonedActionMutation;
    if (!context || handler != context->handler ||
        stage != context->abandonStage ||
        Processor::information().getCurrentThread() != context->worker ||
        !context->hookCalls.compareAndSwap(0, 1))
    {
        return;
    }

    HostedIrqManager::abandonCurrentThreadForTest();
}

int abandonActionMutationThread(void *parameter)
{
    AbandonedActionMutationContext *context =
        reinterpret_cast<AbandonedActionMutationContext *>(parameter);
    context->registry->cancelThreadedDispatch(
        context->line, context->generation);
    context->returned += 1;
    return 1;
}

PostCallbackCancellationContext *g_PostCallbackCancellation = nullptr;

void cancelThreadedHandlerAfterUnpublish(
    IrqHandlerBase *handler, IrqHandlerRegistry::HandlerHazardStage stage)
{
    PostCallbackCancellationContext *context = g_PostCallbackCancellation;
    if (!context || handler != context->handler ||
        stage != IrqHandlerRegistry::HandlerHazardStage::Released ||
        context->releases)
    {
        return;
    }

    ++context->releases;
    context->result =
        context->registry->unregisterHandler(context->line, context->handler);
}

struct RetirementBoundaryContext
{
    IrqHandlerRegistry *registry;
    IrqHandlerBase *handler;
    uint8_t line;
    size_t calls;
    bool admitted;
    bool handled;
};

RetirementBoundaryContext *g_RetirementBoundary = nullptr;

void dispatchAfterRetirementBoundary(
    IrqHandlerBase *handler, IrqHandlerRegistry::HandlerHazardStage stage)
{
    RetirementBoundaryContext *context = g_RetirementBoundary;
    if (!context || handler != context->handler || context->calls ||
        stage != IrqHandlerRegistry::HandlerHazardStage::
                     RetirementBoundaryPublished)
    {
        return;
    }

    ++context->calls;
    IrqHandlerRegistry::AdmissionCutoff cutoff = {};
    if (!context->registry->captureAdmissionCutoff(context->line, cutoff))
    {
        return;
    }
    alignas(InterruptState) uint8_t stateStorage[sizeof(InterruptState)] = {};
    InterruptState &state = *reinterpret_cast<InterruptState *>(stateStorage);
    context->admitted = context->registry->dispatchHard(
        context->line, state, context->handled, nullptr, 0x5203, cutoff);
}

struct ActionReuseContext
{
    IrqHandlerRegistry *registry;
    IrqHandlerBase *original;
    IrqHandler *replacement;
    IrqHandlerRegistry::AdmissionCutoff heldCutoff;
    uint8_t originalLine;
    uint8_t replacementLine;
    size_t generation;
    size_t calls;
    bool originalRemoved;
    bool cutoffReleased;
    bool replacementRegistered;
    bool replacementPublished;
};

ActionReuseContext *g_ActionReuse = nullptr;

void reuseSlotBeforeActionMutationPin(
    IrqHandlerBase *handler, IrqHandlerRegistry::HandlerHazardStage stage)
{
    ActionReuseContext *context = g_ActionReuse;
    if (!context || handler != context->original || context->calls ||
        stage !=
            IrqHandlerRegistry::HandlerHazardStage::BeforeActionMutationPin)
    {
        return;
    }

    ++context->calls;
    context->originalRemoved =
        context->registry->unregisterHandler(
            context->originalLine, context->original) ==
        IrqHandlerRegistry::UnregisterResult::Completed;
    context->registry->invalidateThreadedLine(
        context->originalLine, context->generation);
    context->registry->releaseAdmissionCutoff(context->heldCutoff);
    context->cutoffReleased = true;
    context->replacementRegistered = context->registry->registerThreadedHandler(
        context->replacementLine, context->replacement);
    context->replacementPublished =
        context->replacementRegistered &&
        context->registry->publishThreadedDispatch(
            context->replacementLine, context->generation);
}

struct ClaimFinalizationContext
{
    ClaimFinalizationContext(
        IrqHandlerRegistry &registry, IrqHandlerBase &handler, uint8_t line)
        : registry(registry), handler(handler), line(line), remover(nullptr),
          finalizationEntered(0), removerContended(0), finalizationCalls(0),
          contentionCalls(0), failures(0), removerHadThread(0),
          removerHadInterrupts(0), removerWasHardIrq(0),
          removerSignalDepth(0),
          removalResult(IrqHandlerRegistry::UnregisterResult::NotFound)
    {
    }

    IrqHandlerRegistry &registry;
    IrqHandlerBase &handler;
    uint8_t line;
    Thread *remover;
    Semaphore finalizationEntered;
    Semaphore removerContended;
    Atomic<size_t> finalizationCalls;
    Atomic<size_t> contentionCalls;
    Atomic<size_t> failures;
    Atomic<size_t> removerHadThread;
    Atomic<size_t> removerHadInterrupts;
    Atomic<size_t> removerWasHardIrq;
    Atomic<size_t> removerSignalDepth;
    IrqHandlerRegistry::UnregisterResult removalResult;
};

ClaimFinalizationContext *g_ClaimFinalization = nullptr;

void coordinateClaimFinalization(
    IrqHandlerBase *handler, IrqHandlerRegistry::HandlerHazardStage stage)
{
    ClaimFinalizationContext *context = g_ClaimFinalization;
    if (!context || handler != &context->handler)
    {
        return;
    }

    if (stage ==
            IrqHandlerRegistry::HandlerHazardStage::BeforeClaimFinalization &&
        context->finalizationCalls.compareAndSwap(0, 1))
    {
        context->finalizationEntered.release();
        if (!context->removerContended.acquireForCompletion(1, 2, 0))
        {
            context->failures += 1;
        }
    }
    else if (
        stage ==
            IrqHandlerRegistry::HandlerHazardStage::FinalizationContended &&
        context->contentionCalls.compareAndSwap(0, 1))
    {
        context->removerContended.release();
    }
}

int removeDuringClaimFinalization(void *parameter)
{
    ClaimFinalizationContext *context =
        reinterpret_cast<ClaimFinalizationContext *>(parameter);
    if (!context->finalizationEntered.acquireForCompletion(1, 2, 0))
    {
        context->failures += 1;
        return 1;
    }
    Thread *current = Processor::information().getCurrentThread();
    context->removerHadThread = current ? 1 : 0;
    context->removerHadInterrupts = Processor::getInterrupts() ? 1 : 0;
    context->removerWasHardIrq = Processor::inDeviceHardIrq() ? 1 : 0;
    context->removerSignalDepth =
        current ? current->getHostedSignalDepth() : 0;
    context->removalResult =
        context->registry.unregisterHandler(context->line, &context->handler);
    return 0;
}

class NestedDeviceHardIrqProbe : public HardIrqHandler
{
  public:
    NestedDeviceHardIrqProbe()
        : depth(0), activeCount(0), activeGeneration(0), marked(false), calls(0)
    {
    }

    bool irq(irq_id_t, InterruptState &) override
    {
        depth = Processor::deviceHardIrqDepthForTest();
        marked = Processor::inDeviceHardIrq();
        IrqLineDiagnosticSnapshot lines[3] = {};
        if (Machine::instance().getIrqManager()->snapshotIrqLines(lines, 3) ==
            3)
        {
            activeCount = lines[HardContextTestIrq].activeHardDispatchCount;
            activeGeneration =
                lines[HardContextTestIrq].activeHardDispatchGeneration;
        }
        ++calls;
        return true;
    }

    size_t depth;
    size_t activeCount;
    size_t activeGeneration;
    bool marked;
    size_t calls;
};

class OuterDeviceHardIrqProbe : public HardIrqHandler
{
  public:
    explicit OuterDeviceHardIrqProbe(HardIrqHandler *nested)
        : m_Nested(nested), entryDepth(0), restoredDepth(0), activeCount(0),
          activeGeneration(0), marked(false), nestedAdmitted(false),
          nestedHandled(false), calls(0)
    {
    }

    bool irq(irq_id_t, InterruptState &) override
    {
        entryDepth = Processor::deviceHardIrqDepthForTest();
        marked = Processor::inDeviceHardIrq();
        IrqLineDiagnosticSnapshot lines[3] = {};
        if (Machine::instance().getIrqManager()->snapshotIrqLines(lines, 3) ==
            3)
        {
            activeCount = lines[HardContextTestIrq].activeHardDispatchCount;
            activeGeneration =
                lines[HardContextTestIrq].activeHardDispatchGeneration;
        }
        nestedAdmitted = HostedIrqManager::dispatchHandlerForTest(
            HardContextTestIrq, m_Nested, nestedHandled,
            InnerSyntheticDispatchGeneration);
        restoredDepth = Processor::deviceHardIrqDepthForTest();
        ++calls;
        return true;
    }

    HardIrqHandler *m_Nested;
    size_t entryDepth;
    size_t restoredDepth;
    size_t activeCount;
    size_t activeGeneration;
    bool marked;
    bool nestedAdmitted;
    bool nestedHandled;
    size_t calls;
};

class ThreadedDeviceHardIrqProbe : public IrqHandler
{
  public:
    ThreadedDeviceHardIrqProbe() : depth(~static_cast<size_t>(0)), marked(true)
    {
    }

    IrqDisposition irq(irq_id_t) override
    {
        depth = Processor::deviceHardIrqDepthForTest();
        marked = Processor::inDeviceHardIrq();
        return IrqDisposition::Handled;
    }

    size_t depth;
    bool marked;
};

bool deviceHardIrqContextTracking()
{
    constexpr const char *Test = "device-hard-irq-context";
    NestedDeviceHardIrqProbe nested;
    OuterDeviceHardIrqProbe outer(&nested);
    IrqHandlerRegistry threadedRegistry;
    ThreadedDeviceHardIrqProbe threaded;

    bool passed = check(
        !Processor::inDeviceHardIrq() &&
            Processor::deviceHardIrqDepthForTest() == 0,
        "the test began with stale device hard-IRQ state", Test);
    IrqManager *manager = Machine::instance().getIrqManager();
    const irq_id_t outerId = manager->registerHardIsaIrqHandler(
        HardContextTestIrq, &outer, IrqPolicy::syntheticHard());
    const irq_id_t nestedId = manager->registerHardIsaIrqHandler(
        HardContextTestIrq, &nested, IrqPolicy::syntheticHard());
    const bool threadedRegistered =
        threadedRegistry.registerThreadedHandler(8, &threaded);

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    bool outerHandled = false;
    const bool outerAdmitted = HostedIrqManager::dispatchHandlerForTest(
        HardContextTestIrq, &outer, outerHandled,
        OuterSyntheticDispatchGeneration);
    Processor::setInterrupts(interruptsWereEnabled);
    const size_t postHardDepth = Processor::deviceHardIrqDepthForTest();
    const bool postHardMarked = Processor::inDeviceHardIrq();

    IrqHandlerRegistry::ThreadedDispatchResult threadedResult = {};
    threadedRegistry.publishThreadedDispatch(8, 1);
    const bool threadedAdmitted =
        threadedRegistry.dispatchThreaded(8, 1, threadedResult);
    const bool threadedHandled = threadedResult.handled;

    const bool outerRemoved =
        outerId && manager->unregisterHandler(outerId, &outer);
    const bool nestedRemoved =
        nestedId && manager->unregisterHandler(nestedId, &nested);
    const bool threadedRemoved =
        threadedRegistry.unregisterHandler(8, &threaded) ==
        IrqHandlerRegistry::UnregisterResult::Completed;

    passed &= check(
        outerId && nestedId && threadedRegistered,
        "the context probes could not all be registered", Test);
    passed &= check(
        outerAdmitted && outerHandled && outer.calls == 1 &&
            outer.entryDepth == 1 && outer.marked && outer.activeCount == 1 &&
            outer.activeGeneration == OuterSyntheticDispatchGeneration,
        "an outer hard callback did not observe depth one", Test);
    passed &= check(
        outer.nestedAdmitted && outer.nestedHandled && nested.calls == 1 &&
            nested.depth == 2 && nested.marked && outer.restoredDepth == 1,
        "nested hard dispatch did not restore its caller's depth", Test);
    passed &= check(
        nested.activeCount == 2 && nested.activeGeneration == 0,
        "same-line nested hard dispatch did not report an ambiguous generation",
        Test);
    passed &= check(
        postHardDepth == 0 && !postHardMarked,
        "hard dispatch leaked its device marker after return", Test);
    passed &= check(
        threadedAdmitted && threadedHandled && threadedResult.allowRearm &&
            threaded.depth == 0 && !threaded.marked,
        "threaded dispatch inherited device hard-IRQ state", Test);
    passed &= check(
        outerRemoved && nestedRemoved && threadedRemoved,
        "the context probes did not unregister cleanly", Test);

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS irq-threaded-result-abi");
        NOTICE("HOSTED-WAIT-TEST: PASS device-hard-irq-context");
    }
    return passed;
}

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
        if (m_Registry.dispatchHard(5, state, handled, nullptr, 1))
        {
            hardAdmitted += 1;
        }
        if (handled)
        {
            hardHandled += 1;
        }

        IrqHandlerRegistry::ThreadedDispatchResult threadedResult = {};
        if (m_Registry.dispatchThreaded(5, 1, threadedResult))
        {
            signalThreadedAdmitted += 1;
        }
        if (threadedResult.handled)
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
    const bool occurrencePublished =
        registry->publishThreadedDispatch(5, 1);

    DeliveryContextProbe *probe = new DeliveryContextProbe(*registry);
    IrqManager *manager = Machine::instance().getIrqManager();
    const irq_id_t probeId = manager->registerHardIsaIrqHandler(
        1, probe, IrqPolicy::syntheticHard());
    const bool signalQueued = probeId && raise(SIGUSR2) == 0;
    const bool probeRemoved =
        probeId && manager->unregisterHandler(probeId, probe);

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    IrqHandlerRegistry::ThreadedDispatchResult atomicResult = {true, true};
    const bool atomicAdmitted =
        registry->dispatchThreaded(5, 1, atomicResult);
    Processor::setInterrupts(interruptsWereEnabled);

    IrqHandlerRegistry::ThreadedDispatchResult threadedResult = {};
    const bool threadedAdmitted =
        registry->dispatchThreaded(5, 1, threadedResult);
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
    IrqHandlerRegistry::ThreadedDispatchResult wrongThreadedResult = {
        true, true};
    const bool wrongThreadedAdmitted =
        registry->dispatchThreaded(6, 1, wrongThreadedResult);
    const bool hardRemoved = registry->unregisterHandler(6, &hard) ==
                             IrqHandlerRegistry::UnregisterResult::Completed;
    const bool threadedAfterHard =
        registry->registerThreadedHandler(6, &threaded);
    const bool threadedAfterHardRemoved =
        registry->unregisterHandler(6, &threaded) ==
        IrqHandlerRegistry::UnregisterResult::Completed;

    bool passed = true;
    passed &= check(
        threadedRegistered && hardMixRejected && occurrencePublished &&
            threadedAdmitted &&
            threadedResult.handled && threadedResult.allowRearm,
        "a threaded line accepted hard delivery or did not dispatch", Test);
    passed &= check(
        signalQueued && probe->calls == 1 && !probe->hardAdmitted &&
            !probe->hardHandled && !probe->signalThreadedAdmitted &&
            !probe->signalThreadedHandled && probeRemoved,
        "a threaded line dispatched in hard or hosted-signal context", Test);
    passed &= check(
        !atomicAdmitted && !atomicResult.handled && !atomicResult.allowRearm,
        "a threaded line dispatched with interrupts disabled", Test);
    passed &= check(
        threaded.calls == 1 && threaded.wrongContext == 0,
        "the threaded callback observed hard or atomic context", Test);
    passed &= check(
        threadedRemoved && hardAfterThreaded && hardAfterThreadedRemoved &&
            hardRegistered && threadedMixRejected &&
            !wrongThreadedAdmitted && !wrongThreadedResult.handled &&
            !wrongThreadedResult.allowRearm && hardRemoved &&
            threadedAfterHard && threadedAfterHardRemoved,
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

bool mixedDeliveryOccurrenceBinding()
{
    constexpr const char *Test = "irq-mixed-delivery-occurrence";
    constexpr uint8_t Line = 23;
    constexpr size_t Generation = 0x5301;

    IrqHandlerRegistry registry;
    CountingHardRegistryHandler originalHard;
    CountingHardRegistryHandler lateHard;
    ThreadedRegistryHandler originalThreaded;
    ThreadedRegistryHandler lateThreaded;
    const bool hardRegistered = registry.registerHardHandler(
        Line, &originalHard, IrqPolicy::syntheticHard());
    const bool threadedRegistered = registry.registerThreadedHandler(
        Line, &originalThreaded, IrqPolicy::syntheticThreaded());

    IrqHandlerRegistry::LineConfiguration mixed = {};
    const bool mixedSnapshot = registry.snapshotLineConfiguration(Line, mixed);
    IrqHandlerRegistry::MixedAdmissionCutoffs cutoffs = {};
    const bool cutoffsCaptured =
        registry.captureMixedAdmissionCutoffs(Line, cutoffs);
    const bool identicalCutoff =
        cutoffs.hard.epoch == cutoffs.threaded.epoch &&
        cutoffs.hard.occurrenceEpoch == cutoffs.threaded.occurrenceEpoch &&
        cutoffs.hard.readerToken == cutoffs.threaded.readerToken;

    const bool lateHardRegistered = registry.registerHardHandler(
        Line, &lateHard, IrqPolicy::syntheticHard());
    const bool lateThreadedRegistered = registry.registerThreadedHandler(
        Line, &lateThreaded, IrqPolicy::syntheticThreaded());
    const bool threadedPublished = registry.publishThreadedDispatch(
        Line, Generation, cutoffs.threaded);
    alignas(InterruptState) uint8_t stateStorage[sizeof(InterruptState)] = {};
    InterruptState &state = *reinterpret_cast<InterruptState *>(stateStorage);
    bool hardHandled = false;
    const bool hardAdmitted = registry.dispatchHard(
        Line, state, hardHandled, nullptr, Generation, cutoffs.hard);
    IrqHandlerRegistry::ThreadedDispatchResult threadedResult = {};
    const bool threadedAdmitted =
        registry.dispatchThreaded(Line, Generation, threadedResult);

    IrqHandlerRegistry::LineMode removedDelivery =
        IrqHandlerRegistry::LineMode::Empty;
    const bool originalHardRemoved =
        registry.unregisterHandler(Line, &originalHard, removedDelivery) ==
            IrqHandlerRegistry::UnregisterResult::Completed &&
        removedDelivery == IrqHandlerRegistry::LineMode::HardOnly;
    const bool stillMixed =
        registry.lineMode(Line) == IrqHandlerRegistry::LineMode::Mixed;
    const bool lateHardRemoved =
        registry.unregisterHandler(Line, &lateHard, removedDelivery) ==
            IrqHandlerRegistry::UnregisterResult::Completed &&
        removedDelivery == IrqHandlerRegistry::LineMode::HardOnly;
    IrqHandlerRegistry::LineConfiguration threadedOnly = {};
    const bool threadedOnlySnapshot =
        registry.snapshotLineConfiguration(Line, threadedOnly);
    const bool originalThreadedRemoved =
        registry.unregisterHandler(
            Line, &originalThreaded, removedDelivery) ==
            IrqHandlerRegistry::UnregisterResult::Completed &&
        removedDelivery == IrqHandlerRegistry::LineMode::Threaded;
    const bool lateThreadedRemoved =
        registry.unregisterHandler(Line, &lateThreaded, removedDelivery) ==
            IrqHandlerRegistry::UnregisterResult::Completed &&
        removedDelivery == IrqHandlerRegistry::LineMode::Threaded;

    constexpr uint8_t IncompatibleLine = 24;
    ThreadedRegistryHandler edgeThreaded;
    CountingHardRegistryHandler edgeHard;
    const bool edgeThreadedRegistered = registry.registerThreadedHandler(
        IncompatibleLine, &edgeThreaded, IrqPolicy::edgeThreaded());
    const bool incompatibleEdgeRejected = !registry.registerHardHandler(
        IncompatibleLine, &edgeHard, IrqPolicy::edgeHard());
    const bool compatibleEdgeRegistered = registry.registerHardHandler(
        IncompatibleLine, &edgeHard, IrqPolicy::edgeThreaded());
    const bool edgeHardRemoved =
        registry.unregisterHandler(IncompatibleLine, &edgeHard) ==
        IrqHandlerRegistry::UnregisterResult::Completed;
    const bool edgeThreadedRemoved =
        registry.unregisterHandler(IncompatibleLine, &edgeThreaded) ==
        IrqHandlerRegistry::UnregisterResult::Completed;

    constexpr uint8_t LevelLine = 26;
    CountingHardRegistryHandler levelHard;
    ThreadedRegistryHandler levelThreaded;
    const bool levelHardRegistered = registry.registerHardHandler(
        LevelLine, &levelHard, IrqPolicy::levelHard());
    const bool levelThreadedRegistered = registry.registerThreadedHandler(
        LevelLine, &levelThreaded, IrqPolicy::levelThreaded());
    IrqHandlerRegistry::LineConfiguration mixedLevel = {};
    const bool mixedLevelSnapshot =
        registry.snapshotLineConfiguration(LevelLine, mixedLevel);
    const bool levelThreadedRemoved =
        registry.unregisterHandler(LevelLine, &levelThreaded) ==
        IrqHandlerRegistry::UnregisterResult::Completed;
    IrqHandlerRegistry::LineConfiguration hardLevel = {};
    const bool hardLevelSnapshot =
        registry.snapshotLineConfiguration(LevelLine, hardLevel);
    const bool levelHardRemoved =
        registry.unregisterHandler(LevelLine, &levelHard) ==
        IrqHandlerRegistry::UnregisterResult::Completed;

    constexpr uint8_t LeaseLine = 25;
    CountingHardRegistryHandler leased;
    const bool leasedRegistered = registry.registerHardHandler(
        LeaseLine, &leased, IrqPolicy::syntheticHard());
    IrqHandlerRegistry::MixedAdmissionCutoffs held = {};
    const bool heldCaptured =
        registry.captureMixedAdmissionCutoffs(LeaseLine, held);
    const bool leasedRemoved =
        registry.unregisterHandler(LeaseLine, &leased) ==
        IrqHandlerRegistry::UnregisterResult::Completed;
    const size_t tombstonesBeforeRelease =
        registry.tombstoneCountForTest(LeaseLine);
    registry.releaseAdmissionCutoff(held.hard);
    const size_t tombstonesAfterFirstRelease =
        registry.tombstoneCountForTest(LeaseLine);
    registry.releaseAdmissionCutoff(held.threaded);
    const size_t tombstonesAfterFinalRelease =
        registry.tombstoneCountForTest(LeaseLine);

    bool passed = true;
    passed &= check(
        hardRegistered && threadedRegistered && mixedSnapshot &&
            mixed.handlerCount == 2 &&
            mixed.mode == IrqHandlerRegistry::LineMode::Mixed &&
            mixed.policyConfigured && mixed.trigger == IrqTrigger::Synthetic &&
            mixed.controllerAck == IrqControllerAck::None &&
            mixed.lineRelease == IrqLineRelease::AfterHardStage,
        "compatible hard and threaded handlers did not form one line", Test);
    passed &= check(
        cutoffsCaptured && identicalCutoff && lateHardRegistered &&
            lateThreadedRegistered &&
            threadedPublished && hardAdmitted && hardHandled &&
            threadedAdmitted && threadedResult.handled &&
            threadedResult.allowRearm && originalHard.calls == 1 &&
            lateHard.calls == 0 && originalThreaded.calls == 1 &&
            lateThreaded.calls == 0,
        "the two mixed stages did not share one occurrence cutoff", Test);
    passed &= check(
        originalHardRemoved && stillMixed && lateHardRemoved &&
            threadedOnlySnapshot && threadedOnly.handlerCount == 2 &&
            threadedOnly.mode == IrqHandlerRegistry::LineMode::Threaded &&
            originalThreadedRemoved && lateThreadedRemoved &&
            registry.lineMode(Line) == IrqHandlerRegistry::LineMode::Empty,
        "mixed-line removal lost per-delivery state transitions", Test);
    passed &= check(
        edgeThreadedRegistered && incompatibleEdgeRejected &&
            compatibleEdgeRegistered &&
            registry.lineMode(IncompatibleLine) ==
                IrqHandlerRegistry::LineMode::Empty &&
            edgeHardRemoved && edgeThreadedRemoved,
        "mixed registration ignored controller acknowledgement ordering",
        Test);
    passed &= check(
        levelHardRegistered && levelThreadedRegistered && mixedLevelSnapshot &&
            mixedLevel.mode == IrqHandlerRegistry::LineMode::Mixed &&
            mixedLevel.trigger == IrqTrigger::Level &&
            mixedLevel.controllerAck == IrqControllerAck::AfterHardStage &&
            mixedLevel.lineRelease ==
                IrqLineRelease::AfterThreadedCompletion &&
            levelThreadedRemoved && hardLevelSnapshot &&
            hardLevel.mode == IrqHandlerRegistry::LineMode::HardOnly &&
            hardLevel.lineRelease == IrqLineRelease::AfterHardStage &&
            levelHardRemoved,
        "level-mixed policy did not promote and demote line release", Test);
    passed &= check(
        leasedRegistered && heldCaptured && leasedRemoved &&
            tombstonesBeforeRelease == 1 &&
            tombstonesAfterFirstRelease == 1 &&
            tombstonesAfterFinalRelease == 0,
        "one mixed cutoff lease reclaimed an occurrence still in use", Test);
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS irq-mixed-delivery-occurrence");
    }
    return passed;
}

bool threadedOccurrenceLifetimeBinding()
{
    constexpr const char *Test = "irq-threaded-occurrence-lifetime";
    constexpr uint8_t Line = 9;
    constexpr size_t FirstGeneration = 0x5101;
    constexpr size_t SecondGeneration = 0x5102;
    constexpr size_t CancelledGeneration = 0x5103;
    constexpr size_t ReplacementGeneration = 0x5104;

    IrqHandlerRegistry registry;
    DispositionHandler original(IrqDisposition::Handled);
    DispositionHandler late(IrqDisposition::Handled);
    DispositionHandler replacement(IrqDisposition::Handled);

    const bool originalRegistered =
        registry.registerThreadedHandler(Line, &original);
    const bool firstPublished =
        registry.publishThreadedDispatch(Line, FirstGeneration);
    const bool lateRegistered = registry.registerThreadedHandler(Line, &late);
    IrqHandlerRegistry::ThreadedDispatchResult firstResult = {};
    const bool firstAdmitted =
        registry.dispatchThreaded(Line, FirstGeneration, firstResult);
    const size_t firstOriginalCalls = original.calls;
    const size_t firstLateCalls = late.calls;

    const bool secondPublished =
        registry.publishThreadedDispatch(Line, SecondGeneration);
    IrqHandlerRegistry::ThreadedDispatchResult secondResult = {};
    const bool secondAdmitted =
        registry.dispatchThreaded(Line, SecondGeneration, secondResult);
    const size_t secondOriginalCalls = original.calls;
    const size_t secondLateCalls = late.calls;

    const bool cancelledPublished =
        registry.publishThreadedDispatch(Line, CancelledGeneration);
    late.disposition = IrqDisposition::NotHandled;
    const bool originalRemoved =
        registry.unregisterHandler(Line, &original) ==
        IrqHandlerRegistry::UnregisterResult::Completed;
    const bool replacementRegistered =
        registry.registerThreadedHandler(Line, &replacement);
    IrqHandlerRegistry::ThreadedDispatchResult cancelledResult = {};
    const bool cancelledAdmitted = registry.dispatchThreaded(
        Line, CancelledGeneration, cancelledResult);
    const size_t cancelledOriginalCalls = original.calls;
    const size_t cancelledLateCalls = late.calls;
    const size_t cancelledReplacementCalls = replacement.calls;

    const bool replacementPublished =
        registry.publishThreadedDispatch(Line, ReplacementGeneration);
    IrqHandlerRegistry::ThreadedDispatchResult replacementResult = {};
    const bool replacementAdmitted = registry.dispatchThreaded(
        Line, ReplacementGeneration, replacementResult);

    const bool lateRemoved =
        registry.unregisterHandler(Line, &late) ==
        IrqHandlerRegistry::UnregisterResult::Completed;
    const bool replacementRemoved =
        registry.unregisterHandler(Line, &replacement) ==
        IrqHandlerRegistry::UnregisterResult::Completed;

    bool passed = true;
    passed &= check(
        originalRegistered && firstPublished && lateRegistered &&
            firstAdmitted && firstResult.handled && firstResult.allowRearm &&
            firstOriginalCalls == 1 && firstLateCalls == 0,
        "a handler registered after publication received the old occurrence",
        Test);
    passed &= check(
        secondPublished && secondAdmitted && secondResult.handled &&
            secondResult.allowRearm && secondOriginalCalls == 2 &&
            secondLateCalls == 1,
        "the next occurrence did not admit both current handler lifetimes",
        Test);
    passed &= check(
        cancelledPublished && originalRemoved && replacementRegistered &&
            cancelledAdmitted && !cancelledResult.handled &&
            cancelledResult.allowRearm && cancelledOriginalCalls == 2 &&
            cancelledLateCalls == 2 && cancelledReplacementCalls == 0,
        "removal did not quiesce its queued occurrence or slot reuse inherited "
        "stale work",
        Test);
    passed &= check(
        replacementPublished && replacementAdmitted &&
            replacementResult.handled && replacementResult.allowRearm &&
            late.calls == 3 && replacement.calls == 1,
        "the replacement was not admitted by its first new occurrence", Test);
    passed &= check(
        lateRemoved && replacementRemoved,
        "the occurrence-lifetime handlers did not unregister", Test);

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS irq-threaded-late-registration");
        NOTICE("HOSTED-WAIT-TEST: PASS irq-threaded-cancelled-generation");
        NOTICE("HOSTED-WAIT-TEST: PASS irq-threaded-slot-reuse-generation");
    }
    return passed;
}

struct OccurrenceCaptureRetirementContext
{
    IrqHandlerRegistry *registry;
    CountingHardRegistryHandler *handler;
    uint8_t line;
    IrqHandlerRegistry::OccurrenceCaptureStage targetStage;
    IrqHandlerRegistry::OccurrenceCaptureStage observedStage;
    size_t hookCalls;
    size_t sampledEpoch;
    bool removed;
};

OccurrenceCaptureRetirementContext *g_OccurrenceCaptureRetirement = nullptr;

void retireAtOccurrenceCaptureStage(
    IrqHandlerRegistry *registry, uint8_t line,
    IrqHandlerRegistry::OccurrenceCaptureStage stage, size_t occurrenceEpoch)
{
    OccurrenceCaptureRetirementContext *context =
        g_OccurrenceCaptureRetirement;
    if (!context || context->registry != registry || context->line != line ||
        context->targetStage != stage)
    {
        return;
    }

    registry->setOccurrenceCaptureHookForTest(nullptr);
    ++context->hookCalls;
    context->observedStage = stage;
    context->sampledEpoch = occurrenceEpoch;
    context->removed =
        registry->unregisterHandler(line, context->handler) ==
        IrqHandlerRegistry::UnregisterResult::Completed;
}

bool waitFreeOccurrenceCapturePreservesBoundary()
{
    constexpr const char *Test = "irq-occurrence-wait-free-boundary";
    constexpr uint8_t Line = 14;
    using Stage = IrqHandlerRegistry::OccurrenceCaptureStage;
    const Stage stages[] = {
        Stage::BankZeroClaimed, Stage::BankOneClaimed, Stage::EpochSampled,
        Stage::UnusedBankReleased};

    bool passed = true;
    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); ++i)
    {
        IrqHandlerRegistry registry;
        CountingHardRegistryHandler handler;
        CountingHardRegistryHandler replacement;
        OccurrenceCaptureRetirementContext context = {
            &registry, &handler, Line, stages[i], stages[i], 0, 0, false};
        const bool registered = registry.registerHardHandler(Line, &handler);
        g_OccurrenceCaptureRetirement = &context;
        registry.setOccurrenceCaptureHookForTest(
            retireAtOccurrenceCaptureStage);
        IrqHandlerRegistry::AdmissionCutoff cutoff = {};
        const bool captured = registry.captureAdmissionCutoff(Line, cutoff);
        registry.setOccurrenceCaptureHookForTest(nullptr);
        g_OccurrenceCaptureRetirement = nullptr;

        const bool retained = registry.tombstoneCountForTest(Line) == 1;
        alignas(InterruptState) uint8_t stateStorage[sizeof(InterruptState)] =
            {};
        InterruptState &state =
            *reinterpret_cast<InterruptState *>(stateStorage);
        bool handled = false;
        const bool admitted = captured && registry.dispatchHard(
                                              Line, state, handled, nullptr,
                                              0x5200 + i, cutoff);
        const bool reclaimed = registry.tombstoneCountForTest(Line) == 0;
        const bool replacementRegistered =
            registry.registerHardHandler(Line, &replacement);
        const bool replacementRemoved =
            replacementRegistered &&
            registry.unregisterHandler(Line, &replacement) ==
                IrqHandlerRegistry::UnregisterResult::Completed;
        const bool retirementPrecededSample = i < 2;

        passed &= check(
            registered && captured && context.hookCalls == 1 &&
                context.observedStage == stages[i] && context.removed &&
                retained && admitted == !retirementPrecededSample &&
                handled == !retirementPrecededSample && handler.calls == 0 &&
                reclaimed && replacementRegistered && replacementRemoved,
            "a dual-bank retirement stage violated occurrence membership or "
            "slot lifetime",
            Test);
    }

    {
        constexpr uint8_t MixedLine = 13;
        IrqHandlerRegistry registry;
        CountingHardRegistryHandler handler;
        CountingHardRegistryHandler replacement;
        OccurrenceCaptureRetirementContext context = {
            &registry, &handler, MixedLine, Stage::EpochSampled,
            Stage::EpochSampled, 0, 0, false};
        const bool registered =
            registry.registerHardHandler(MixedLine, &handler);
        g_OccurrenceCaptureRetirement = &context;
        registry.setOccurrenceCaptureHookForTest(
            retireAtOccurrenceCaptureStage);
        IrqHandlerRegistry::MixedAdmissionCutoffs cutoffs = {};
        const bool captured =
            registry.captureMixedAdmissionCutoffs(MixedLine, cutoffs);
        registry.setOccurrenceCaptureHookForTest(nullptr);
        g_OccurrenceCaptureRetirement = nullptr;

        const bool retained = registry.tombstoneCountForTest(MixedLine) == 1;
        registry.releaseAdmissionCutoff(cutoffs.hard);
        const bool retainedAfterFirstRelease =
            registry.tombstoneCountForTest(MixedLine) == 1;
        registry.releaseAdmissionCutoff(cutoffs.threaded);
        const bool reclaimed =
            registry.tombstoneCountForTest(MixedLine) == 0;
        const bool replacementRegistered =
            registry.registerHardHandler(MixedLine, &replacement);
        const bool replacementRemoved =
            replacementRegistered &&
            registry.unregisterHandler(MixedLine, &replacement) ==
                IrqHandlerRegistry::UnregisterResult::Completed;

        passed &= check(
            registered && captured && context.hookCalls == 1 &&
                context.removed && retained && retainedAfterFirstRelease &&
                reclaimed && replacementRegistered && replacementRemoved &&
                handler.calls == 0,
            "mixed occurrence capture did not retain both selected-bank "
            "leases independently",
            Test);
    }

    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "irq-occurrence-wait-free-boundary");
    }
    return passed;
}

bool occurrenceGraceTombstones()
{
    constexpr const char *Test = "irq-occurrence-grace-tombstone";

    bool hardPassed = false;
    {
        constexpr uint8_t Line = 12;
        IrqHandlerRegistry registry;
        HardRegistryHandler handler;
        HardRegistryHandler replacement;
        const bool registered = registry.registerHardHandler(Line, &handler);
        IrqHandlerRegistry::AdmissionCutoff cutoff = {};
        const bool cutoffCaptured =
            registry.captureAdmissionCutoff(Line, cutoff);
        const bool removed =
            registry.unregisterHandler(Line, &handler) ==
            IrqHandlerRegistry::UnregisterResult::Completed;
        alignas(InterruptState) uint8_t stateStorage[sizeof(InterruptState)] = {};
        InterruptState &state =
            *reinterpret_cast<InterruptState *>(stateStorage);
        bool handled = false;
        const bool admitted = registry.dispatchHard(
            Line, state, handled, nullptr, 0x5201, cutoff);
        const bool replacementRegistered =
            registry.registerHardHandler(Line, &replacement);
        const bool replacementRemoved =
            registry.unregisterHandler(Line, &replacement) ==
            IrqHandlerRegistry::UnregisterResult::Completed;
        hardPassed = registered && cutoffCaptured && removed && admitted && handled &&
                     replacementRegistered && replacementRemoved;
    }

    bool threadedPassed = false;
    {
        constexpr uint8_t Line = 13;
        IrqHandlerRegistry registry;
        DispositionHandler handler(IrqDisposition::Handled);
        DispositionHandler replacement(IrqDisposition::Handled);
        const bool registered =
            registry.registerThreadedHandler(Line, &handler);
        IrqHandlerRegistry::AdmissionCutoff cutoff = {};
        const bool cutoffCaptured =
            registry.captureAdmissionCutoff(Line, cutoff);
        const bool removed =
            registry.unregisterHandler(Line, &handler) ==
            IrqHandlerRegistry::UnregisterResult::Completed;
        const bool published =
            registry.publishThreadedDispatch(Line, 0x5202, cutoff);
        IrqHandlerRegistry::ThreadedDispatchResult result = {};
        const bool admitted =
            registry.dispatchThreaded(Line, 0x5202, result);
        const bool replacementRegistered =
            registry.registerThreadedHandler(Line, &replacement);
        const bool replacementRemoved =
            registry.unregisterHandler(Line, &replacement) ==
            IrqHandlerRegistry::UnregisterResult::Completed;
        threadedPassed = registered && cutoffCaptured && removed && published && admitted &&
                          !result.handled && result.allowRearm &&
                          !handler.calls && replacementRegistered &&
                          replacementRemoved;
    }

    bool collidingGracePassed = false;
    {
        constexpr uint8_t HeldLine = 0;
        constexpr uint8_t TombstoneLine = 16;
        IrqHandlerRegistry registry;
        HardRegistryHandler handler;
        const bool registered =
            registry.registerHardHandler(TombstoneLine, &handler);
        IrqHandlerRegistry::AdmissionCutoff cutoff = {};
        const bool cutoffCaptured =
            registry.captureAdmissionCutoff(HeldLine, cutoff);
        const bool removed =
            registry.unregisterHandler(TombstoneLine, &handler) ==
            IrqHandlerRegistry::UnregisterResult::Completed;
        const bool retained =
            registry.tombstoneCountForTest(TombstoneLine) == 1;
        registry.releaseAdmissionCutoff(cutoff);
        const bool reclaimed =
            registry.tombstoneCountForTest(TombstoneLine) == 0;
        collidingGracePassed =
            registered && cutoffCaptured && removed && retained && reclaimed;
    }

    bool passed = check(
        hardPassed && threadedPassed,
        "a pre-close occurrence lost its retired handler membership", Test);
    passed &= check(
        collidingGracePassed,
        "a colliding grace reader stranded another line's tombstone", Test);
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS irq-occurrence-grace-tombstone");
    }
    return passed;
}

bool abandonedOccurrenceLeaseCleanup()
{
    constexpr const char *Test = "irq-occurrence-lease-abandon-cleanup";

    bool mixedHardPassed = false;
    {
        constexpr uint8_t Line = 27;
        AbandonedCutoffContext context(
            AbandonedCutoffPath::MixedHard, Line, 0x5210);
        const bool hardRegistered = context.registry.registerHardHandler(
            Line, &context.hard, IrqPolicy::syntheticHard());
        const bool threadedRegistered =
            context.registry.registerThreadedHandler(
                Line, &context.threaded, IrqPolicy::syntheticThreaded());
        context.worker = new Thread(
            Scheduler::instance().getKernelProcess(), abandonCutoffWorker,
            &context, nullptr, false, true, true);
        context.worker->setName("hosted abandoned mixed IRQ cutoff");

        g_AbandonedCutoff = &context;
        context.registry.setHandlerHazardHook(abandonCutoffDispatch);
        const bool started = context.worker->start();
        const bool joined = started && context.worker->joinForCompletion();
        context.registry.setHandlerHazardHook(nullptr);
        g_AbandonedCutoff = nullptr;

        const bool hardRemoved =
            context.registry.unregisterHandler(Line, &context.hard) ==
            IrqHandlerRegistry::UnregisterResult::Completed;
        const size_t tombstonesWithSibling =
            context.registry.tombstoneCountForTest(Line);
        if (context.cutoffCaptured)
        {
            context.registry.releaseAdmissionCutoff(
                context.mixedCutoffs.threaded);
        }
        const size_t tombstonesAfterSibling =
            context.registry.tombstoneCountForTest(Line);
        const bool threadedRemoved =
            context.registry.unregisterHandler(Line, &context.threaded) ==
            IrqHandlerRegistry::UnregisterResult::Completed;

        mixedHardPassed =
            hardRegistered && threadedRegistered && started && joined &&
            context.cutoffCaptured == 1 && context.hookCalls == 1 &&
            !context.returned && hardRemoved && tombstonesWithSibling == 1 &&
            tombstonesAfterSibling == 0 && threadedRemoved;
    }

    bool publicationPassed = false;
    {
        constexpr uint8_t Line = 28;
        AbandonedCutoffContext context(
            AbandonedCutoffPath::ThreadedPublication, Line, 0x5211);
        const bool registered = context.registry.registerThreadedHandler(
            Line, &context.threaded);
        context.worker = new Thread(
            Scheduler::instance().getKernelProcess(), abandonCutoffWorker,
            &context, nullptr, false, true, true);
        context.worker->setName("hosted abandoned IRQ publication cutoff");

        g_AbandonedCutoff = &context;
        context.registry.setHandlerHazardHook(abandonCutoffDispatch);
        const bool started = context.worker->start();
        const bool joined = started && context.worker->joinForCompletion();
        context.registry.setHandlerHazardHook(nullptr);
        g_AbandonedCutoff = nullptr;

        const bool removed =
            context.registry.unregisterHandler(Line, &context.threaded) ==
            IrqHandlerRegistry::UnregisterResult::Completed;
        publicationPassed =
            registered && started && joined && context.cutoffCaptured == 1 &&
            context.hookCalls == 1 && !context.returned && removed &&
            context.registry.tombstoneCountForTest(Line) == 0;
    }

    bool threadedWorkerPassed = false;
    {
        constexpr uint8_t Line = 29;
        constexpr uint8_t ProbeLine = Line + 16;
        constexpr size_t Generation = 0x5212;
        AbandonedCutoffContext context(
            AbandonedCutoffPath::ThreadedWorker, Line, Generation);
        const bool registered = context.registry.registerThreadedHandler(
            Line, &context.threaded);
        const bool published = context.registry.publishThreadedDispatch(
            Line, Generation);
        context.worker = new Thread(
            Scheduler::instance().getKernelProcess(), abandonCutoffWorker,
            &context, nullptr, false, true, true);
        context.worker->setName("hosted abandoned IRQ worker cutoff");

        g_AbandonedCutoff = &context;
        context.registry.setHandlerHazardHook(abandonCutoffDispatch);
        const bool started = context.worker->start();
        const bool joined = started && context.worker->joinForCompletion();
        context.registry.setHandlerHazardHook(nullptr);
        g_AbandonedCutoff = nullptr;

        HardRegistryHandler probe;
        const bool probeRegistered =
            context.registry.registerHardHandler(ProbeLine, &probe);
        const bool probeRemoved =
            context.registry.unregisterHandler(ProbeLine, &probe) ==
            IrqHandlerRegistry::UnregisterResult::Completed;
        const bool probeReclaimed =
            context.registry.tombstoneCountForTest(ProbeLine) == 0;
        context.registry.invalidateThreadedLine(Line, Generation);
        const bool removed =
            context.registry.unregisterHandler(Line, &context.threaded) ==
            IrqHandlerRegistry::UnregisterResult::Completed;
        threadedWorkerPassed =
            registered && published && started && joined &&
            context.hookCalls == 1 && !context.returned &&
            !context.threaded.calls && probeRegistered && probeRemoved &&
            probeReclaimed && removed;
    }

    bool passed = check(
        mixedHardPassed,
        "an abandoned mixed hard dispatch did not release exactly its own "
        "cutoff lease",
        Test);
    passed &= check(
        publicationPassed,
        "an abandoned threaded publication stranded its explicit cutoff "
        "lease",
        Test);
    passed &= check(
        threadedWorkerPassed,
        "an abandoned threaded worker stranded its internal cutoff lease",
        Test);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "irq-occurrence-lease-abandon-cleanup");
    }
    return passed;
}

bool retirementBoundaryExcludesNewOccurrences()
{
    constexpr const char *Test = "irq-retirement-boundary";
    constexpr uint8_t Line = 15;

    IrqHandlerRegistry registry;
    HardRegistryHandler handler;
    RetirementBoundaryContext context = {
        &registry, &handler, Line, 0, false, false};
    const bool registered = registry.registerHardHandler(Line, &handler);
    g_RetirementBoundary = &context;
    registry.setHandlerHazardHook(dispatchAfterRetirementBoundary);
    const bool removed =
        registry.unregisterHandler(Line, &handler) ==
        IrqHandlerRegistry::UnregisterResult::Completed;
    registry.setHandlerHazardHook(nullptr);
    g_RetirementBoundary = nullptr;

    const bool passed = check(
        registered && removed && context.calls == 1 && !context.admitted &&
            !context.handled,
        "an occurrence captured after retirement inherited old membership",
        Test);
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS irq-retirement-boundary");
    }
    return passed;
}

bool actionMutationPinsSlotLifetime()
{
    constexpr const char *Test = "irq-action-mutation-slot-lifetime";
    constexpr uint8_t OriginalLine = 16;
    constexpr uint8_t ReplacementLine = 17;
    constexpr size_t Generation = 0x5204;

    IrqHandlerRegistry registry;
    DispositionHandler original(IrqDisposition::Handled);
    DispositionHandler replacement(IrqDisposition::Handled);
    const bool originalRegistered =
        registry.registerThreadedHandler(OriginalLine, &original);
    const bool originalPublished =
        registry.publishThreadedDispatch(OriginalLine, Generation);
    IrqHandlerRegistry::AdmissionCutoff heldCutoff = {};
    const bool heldCutoffCaptured =
        registry.captureAdmissionCutoff(0, heldCutoff);
    ActionReuseContext context = {
        &registry,
        &original,
        &replacement,
        heldCutoff,
        OriginalLine,
        ReplacementLine,
        Generation,
        0,
        false,
        false,
        false,
        false};

    g_ActionReuse = &context;
    registry.setHandlerHazardHook(reuseSlotBeforeActionMutationPin);
    registry.cancelThreadedDispatch(OriginalLine, Generation);
    registry.setHandlerHazardHook(nullptr);
    g_ActionReuse = nullptr;
    if (!context.cutoffReleased)
    {
        registry.releaseAdmissionCutoff(context.heldCutoff);
    }

    IrqHandlerRegistry::ThreadedDispatchResult result = {};
    const bool replacementAdmitted = context.replacementPublished &&
                                     registry.dispatchThreaded(
                                         ReplacementLine, Generation, result);
    const bool replacementRemoved =
        context.replacementRegistered &&
        registry.unregisterHandler(ReplacementLine, &replacement) ==
            IrqHandlerRegistry::UnregisterResult::Completed;
    bool originalClean =
        !registry.containsHandlerForTest(OriginalLine, &original);
    if (!originalClean)
    {
        originalClean =
            registry.unregisterHandler(OriginalLine, &original) ==
            IrqHandlerRegistry::UnregisterResult::Completed;
        registry.invalidateThreadedLine(OriginalLine, Generation);
    }

    const bool passed = check(
        originalRegistered && originalPublished && heldCutoffCaptured &&
            context.calls == 1 &&
            context.originalRemoved && context.cutoffReleased &&
            context.replacementRegistered && context.replacementPublished &&
            replacementAdmitted && result.handled && result.allowRearm &&
            replacement.calls == 1 && replacementRemoved && originalClean,
        "a stale cancellation mutated a reused slot lifetime", Test);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS irq-action-mutation-slot-lifetime");
    }
    return passed;
}

bool quiescedPublicationClosesWorkerExit()
{
    constexpr const char *Test = "irq-quiesced-terminal-handoff";
    constexpr uint8_t Line = 19;
    constexpr size_t Generation = 0x5206;

    IrqHandlerRegistry registry;
    DispositionHandler handler(IrqDisposition::Handled);
    QuiescedHandoffContext context = {
        &registry, &handler, Line, 0,
        IrqHandlerRegistry::UnregisterResult::NotFound};
    const bool registered = registry.registerThreadedHandler(Line, &handler);
    const bool published =
        registry.publishThreadedDispatch(Line, Generation);

    g_QuiescedHandoff = &context;
    registry.setHandlerHazardHook(removeAfterQuiescedObservation);
    IrqHandlerRegistry::ThreadedDispatchResult result = {};
    const bool admitted =
        registered && published &&
        registry.dispatchThreaded(Line, Generation, result);
    registry.setHandlerHazardHook(nullptr);
    g_QuiescedHandoff = nullptr;

    const bool passed = check(
        registered && published && context.calls == 1 &&
            context.removalResult ==
                IrqHandlerRegistry::UnregisterResult::Completed &&
            admitted && !result.handled && result.allowRearm &&
            handler.calls == 0 &&
            !registry.containsHandlerForTest(Line, &handler) &&
            registry.tombstoneCountForTest(Line) == 0,
        "a terminal worker missed a concurrent quiesced publication", Test);
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS irq-quiesced-terminal-handoff");
    }
    return passed;
}

bool abandonedActionMutationReleasesWriter()
{
    constexpr const char *Test = "irq-action-mutation-abandon-cleanup";
    constexpr uint8_t Line = 20;
    constexpr size_t Generation = 0x5207;

    IrqHandlerRegistry registry;
    DispositionHandler handler(IrqDisposition::Handled);
    const bool registered = registry.registerThreadedHandler(Line, &handler);
    const bool published =
        registry.publishThreadedDispatch(Line, Generation);
    AbandonedActionMutationContext context = {
        &registry, &handler, nullptr, Line, Generation,
        IrqHandlerRegistry::HandlerHazardStage::BeforeActionMutationPin, 0, 0};
    context.worker = new Thread(
        Scheduler::instance().getKernelProcess(), abandonActionMutationThread,
        &context, nullptr, false, true, true);
    context.worker->setName("hosted abandoned IRQ action mutation");

    g_AbandonedActionMutation = &context;
    registry.setHandlerHazardHook(abandonActionMutation);
    const bool started = context.worker->start();
    const bool joined = started && context.worker->joinForCompletion();
    registry.setHandlerHazardHook(nullptr);
    g_AbandonedActionMutation = nullptr;

    IrqHandlerRegistry::ThreadedDispatchResult result = {};
    const bool admitted =
        joined && !registry.threadedActionMutationWriterCountForTest() &&
        registry.dispatchThreaded(Line, Generation, result);
    const bool removed =
        registry.unregisterHandler(Line, &handler) ==
        IrqHandlerRegistry::UnregisterResult::Completed;

    const bool passed = check(
        registered && published && started && joined &&
            context.hookCalls == 1 && !context.returned && admitted &&
            result.handled && result.allowRearm && handler.calls == 1 &&
            removed && !registry.threadedActionMutationWriterCountForTest(),
        "an abandoned action mutation stranded the worker handshake", Test);
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS irq-action-mutation-abandon-cleanup");
    }
    return passed;
}

bool abandonedCancellationCompletesRollback()
{
    constexpr const char *Test = "irq-cancellation-abandon-rollback";
    constexpr uint8_t Line = 21;
    constexpr size_t PreviousGeneration = 0x5208;
    constexpr size_t CancelledGeneration = 0x5209;

    IrqHandlerRegistry registry;
    DispositionHandler handler(IrqDisposition::Handled);
    const bool registered = registry.registerThreadedHandler(Line, &handler);
    const bool previousPublished =
        registry.publishThreadedDispatch(Line, PreviousGeneration);
    const bool cancelledPublished =
        registry.publishThreadedDispatch(Line, CancelledGeneration);
    AbandonedActionMutationContext context = {
        &registry, &handler, nullptr, Line, CancelledGeneration,
        IrqHandlerRegistry::HandlerHazardStage::CancellationMarkerPublished,
        0, 0};
    context.worker = new Thread(
        Scheduler::instance().getKernelProcess(), abandonActionMutationThread,
        &context, nullptr, false, true, true);
    context.worker->setName("hosted abandoned IRQ cancellation");

    g_AbandonedActionMutation = &context;
    registry.setHandlerHazardHook(abandonActionMutation);
    const bool started = context.worker->start();
    const bool joined = started && context.worker->joinForCompletion();
    registry.setHandlerHazardHook(nullptr);
    g_AbandonedActionMutation = nullptr;

    IrqHandlerRegistry::ThreadedDispatchResult previousResult = {};
    const bool previousAdmitted =
        joined && !registry.threadedActionMutationWriterCountForTest() &&
        registry.dispatchThreaded(
            Line, PreviousGeneration, previousResult);
    IrqHandlerRegistry::ThreadedDispatchResult cancelledResult = {};
    const bool cancelledAdmitted = registry.dispatchThreaded(
        Line, CancelledGeneration, cancelledResult);
    const bool removed =
        registry.unregisterHandler(Line, &handler) ==
        IrqHandlerRegistry::UnregisterResult::Completed;

    const bool passed = check(
        registered && previousPublished && cancelledPublished && started &&
            joined && context.hookCalls == 1 && !context.returned &&
            previousAdmitted && previousResult.handled &&
            previousResult.allowRearm && handler.calls == 1 &&
            !cancelledAdmitted && removed &&
            !registry.threadedActionMutationWriterCountForTest(),
        "an abandoned cancellation did not restore its prior action", Test);
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS irq-cancellation-abandon-rollback");
    }
    return passed;
}

bool claimedCancellationPreservesPriorQuiesce()
{
    constexpr const char *Test = "irq-cancellation-claimed-quiesce";
    constexpr uint8_t Line = 22;
    constexpr size_t PreviousGeneration = 0x520A;
    constexpr size_t CancelledGeneration = 0x520B;

    IrqHandlerRegistry registry;
    DispositionHandler handler(IrqDisposition::Handled);
    const bool registered = registry.registerThreadedHandler(Line, &handler);
    const bool previousPublished =
        registry.publishThreadedDispatch(Line, PreviousGeneration);
    const bool cancelledPublished =
        registry.publishThreadedDispatch(Line, CancelledGeneration);
    const bool seeded = registry.setThreadedActionLanesForTest(
        &handler, 0, PreviousGeneration, CancelledGeneration,
        CancelledGeneration, 0);
    ClaimedCancellationContext context = {
        &registry, &handler, CancelledGeneration, 0, 0};
    g_ClaimedCancellation = &context;
    registry.setHandlerHazardHook(consumeCancelledQuiesceAfterClaimClear);
    registry.cancelThreadedDispatch(Line, CancelledGeneration);
    registry.setHandlerHazardHook(nullptr);
    g_ClaimedCancellation = nullptr;

    IrqHandlerRegistry::ThreadedDispatchResult previousResult = {};
    const bool previousAdmitted = registry.dispatchThreaded(
        Line, PreviousGeneration, previousResult);
    IrqHandlerRegistry::ThreadedDispatchResult cancelledResult = {};
    const bool cancelledAdmitted = registry.dispatchThreaded(
        Line, CancelledGeneration, cancelledResult);
    const bool removed =
        registry.unregisterHandler(Line, &handler) ==
        IrqHandlerRegistry::UnregisterResult::Completed;

    const bool passed = check(
        registered && previousPublished && cancelledPublished && seeded &&
            context.hookCalls == 1 &&
            !context.consumedCancelledQuiesce && previousAdmitted &&
            !previousResult.handled && previousResult.allowRearm &&
            !cancelledAdmitted && handler.calls == 0 && removed,
        "claim cancellation exposed the rejected quiesce watermark", Test);
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS irq-cancellation-claimed-quiesce");
    }
    return passed;
}

bool claimFinalizationArbitratesRemoval()
{
    constexpr const char *Test = "irq-claim-finalization-removal";
    constexpr uint8_t Line = 18;
    constexpr size_t Generation = 0x5205;

    IrqHandlerRegistry registry;
    DispositionHandler handler(IrqDisposition::NotHandled);
    ClaimFinalizationContext context(registry, handler, Line);
    context.remover = new Thread(
        Scheduler::instance().getKernelProcess(),
        removeDuringClaimFinalization, &context, nullptr, false, true, true);
    context.remover->setName("hosted IRQ claim-finalization remover");

    const bool registered = registry.registerThreadedHandler(Line, &handler);
    const bool published =
        registry.publishThreadedDispatch(Line, Generation);
    g_ClaimFinalization = &context;
    registry.setHandlerHazardHook(coordinateClaimFinalization);
    const bool removerStarted = context.remover->start();
    IrqHandlerRegistry::ThreadedDispatchResult result = {};
    const bool admitted = registered && published && removerStarted &&
                          registry.dispatchThreaded(Line, Generation, result);
    const bool removerJoined =
        removerStarted && context.remover->joinForCompletion();
    registry.setHandlerHazardHook(nullptr);
    g_ClaimFinalization = nullptr;

    bool cleaned = !registry.containsHandlerForTest(Line, &handler);
    if (!cleaned)
    {
        cleaned = registry.unregisterHandler(Line, &handler) ==
                  IrqHandlerRegistry::UnregisterResult::Completed;
    }

    bool passed = check(
        registered && published && removerStarted && admitted &&
            !result.handled && !result.allowRearm && handler.calls == 1,
        "the controlled callback produced an unexpected result", Test);
    passed &= check(
        removerJoined && context.finalizationCalls == 1 &&
            context.contentionCalls == 1 && context.failures == 0,
        "the remover did not contend at the claim-finalization boundary", Test);
    passed &= check(
        context.removerHadThread && context.removerHadInterrupts &&
            !context.removerWasHardIrq && !context.removerSignalDepth,
        "the remover did not run in a wait-capable thread context", Test);
    passed &= check(
        context.removalResult ==
            IrqHandlerRegistry::UnregisterResult::Completed,
        "the post-finalization unregister returned the wrong result", Test);
    passed &= check(
        cleaned, "the post-finalization handler remained published", Test);
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS irq-claim-finalization-removal");
    }
    return passed;
}

bool threadedPostCallbackCancellation()
{
    constexpr const char *Test = "irq-threaded-post-callback-cancellation";
    constexpr uint8_t Line = 14;
    constexpr size_t Generation = 0x5301;

    IrqHandlerRegistry registry;
    DispositionHandler handler(IrqDisposition::NotHandled);
    PostCallbackCancellationContext context = {
        &registry, &handler, Line, 0,
        IrqHandlerRegistry::UnregisterResult::NotFound};
    const bool registered = registry.registerThreadedHandler(Line, &handler);
    const bool published =
        registry.publishThreadedDispatch(Line, Generation);
    g_PostCallbackCancellation = &context;
    registry.setHandlerHazardHook(cancelThreadedHandlerAfterUnpublish);
    IrqHandlerRegistry::ThreadedDispatchResult result = {};
    const bool admitted =
        registry.dispatchThreaded(Line, Generation, result);
    registry.setHandlerHazardHook(nullptr);
    g_PostCallbackCancellation = nullptr;

    const bool passed = check(
        registered && published && admitted && !result.handled &&
            result.allowRearm && handler.calls == 1 && context.releases == 1 &&
            context.result ==
                IrqHandlerRegistry::UnregisterResult::Completed &&
            !registry.handlerCount(Line),
        "teardown after hazard release lost the claimed action", Test);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "irq-threaded-post-callback-cancellation");
    }
    return passed;
}

bool threadedActionGenerationBoundaries()
{
    constexpr const char *Test = "irq-threaded-action-generations";
    constexpr uint8_t Line = 10;

    IrqHandlerRegistry registry;
    DispositionHandler handler(IrqDisposition::Handled);
    const bool registered = registry.registerThreadedHandler(Line, &handler);
    const bool firstPublished = registry.publishThreadedDispatch(Line, 0x6101);
    const bool newerPublished = registry.publishThreadedDispatch(Line, 0x6102);
    registry.cancelThreadedDispatch(Line, 0x6101);
    IrqHandlerRegistry::ThreadedDispatchResult newerResult = {};
    const bool newerAdmitted =
        registry.dispatchThreaded(Line, 0x6102, newerResult);

    const bool rejectedPublished =
        registry.publishThreadedDispatch(Line, 0x6103);
    registry.cancelThreadedDispatch(Line, 0x6103);
    IrqHandlerRegistry::ThreadedDispatchResult rejectedResult = {true, true};
    const bool rejectedAdmitted =
        registry.dispatchThreaded(Line, 0x6103, rejectedResult);
    const size_t callsAfterRollback = handler.calls;
    const bool removed =
        registry.unregisterHandler(Line, &handler) ==
        IrqHandlerRegistry::UnregisterResult::Completed;

    DispositionHandler preserved(IrqDisposition::Handled);
    const bool preservedRegistered =
        registry.registerThreadedHandler(Line, &preserved);
    const bool preservedPublished =
        registry.publishThreadedDispatch(Line, 0x6110);
    const bool failedNewerPublished =
        registry.publishThreadedDispatch(Line, 0x6111);
    registry.cancelThreadedDispatch(Line, 0x6111);
    IrqHandlerRegistry::ThreadedDispatchResult preservedResult = {};
    const bool preservedAdmitted =
        registry.dispatchThreaded(Line, 0x6110, preservedResult);
    const bool preservedRemoved =
        registry.unregisterHandler(Line, &preserved) ==
        IrqHandlerRegistry::UnregisterResult::Completed;

    DispositionHandler stale(IrqDisposition::Handled);
    const bool staleRegistered =
        registry.registerThreadedHandler(Line, &stale);
    const bool stalePublished = registry.publishThreadedDispatch(Line, 0x6201);
    registry.invalidateThreadedLine(Line, 0x6201);
    const bool staleRemoved =
        registry.unregisterHandler(Line, &stale) ==
        IrqHandlerRegistry::UnregisterResult::Completed;
    DispositionHandler replacement(IrqDisposition::NotHandled);
    const bool replacementRegistered =
        registry.registerThreadedHandler(Line, &replacement);
    const bool replacementPublished =
        registry.publishThreadedDispatch(Line, 0x6202);
    IrqHandlerRegistry::ThreadedDispatchResult replacementResult = {};
    const bool replacementAdmitted =
        registry.dispatchThreaded(Line, 0x6202, replacementResult);
    const bool replacementRemoved =
        registry.unregisterHandler(Line, &replacement) ==
        IrqHandlerRegistry::UnregisterResult::Completed;

    IrqHandlerRegistry wrappingRegistry;
    DispositionHandler wrapping(IrqDisposition::Handled);
    const bool wrappingRegistered =
        wrappingRegistry.registerThreadedHandler(Line, &wrapping);
    const size_t lastGeneration = ~static_cast<size_t>(0);
    const bool lastPublished =
        wrappingRegistry.publishThreadedDispatch(Line, lastGeneration);
    const bool wrappedPublished =
        wrappingRegistry.publishThreadedDispatch(Line, 1);
    IrqHandlerRegistry::ThreadedDispatchResult wrappedResult = {};
    const bool wrappedAdmitted =
        wrappingRegistry.dispatchThreaded(Line, 1, wrappedResult);
    const bool wrappingRemoved =
        wrappingRegistry.unregisterHandler(Line, &wrapping) ==
        IrqHandlerRegistry::UnregisterResult::Completed;

    constexpr uint8_t CancellingLine = 11;
    IrqHandlerRegistry cancellingRegistry;
    SelfCancellingThreadedHandler cancelling(
        cancellingRegistry, CancellingLine);
    const bool cancellingRegistered =
        cancellingRegistry.registerThreadedHandler(
            CancellingLine, &cancelling);
    const bool cancellingPublished =
        cancellingRegistry.publishThreadedDispatch(CancellingLine, 0x6301);
    IrqHandlerRegistry::ThreadedDispatchResult cancellingResult = {};
    const bool cancellingAdmitted = cancellingRegistry.dispatchThreaded(
        CancellingLine, 0x6301, cancellingResult);

    bool passed = true;
    passed &= check(
        registered && firstPublished && newerPublished && newerAdmitted &&
            newerResult.handled && newerResult.allowRearm && handler.calls == 1,
        "coalescing or an older rollback cleared the newer action", Test);
    passed &= check(
        rejectedPublished && !rejectedAdmitted && !rejectedResult.handled &&
            !rejectedResult.allowRearm && callsAfterRollback == 1 && removed,
        "an exactly rolled-back action reached its handler", Test);
    passed &= check(
        preservedRegistered && preservedPublished && failedNewerPublished &&
            preservedAdmitted && preservedResult.handled &&
            preservedResult.allowRearm && preserved.calls == 1 &&
            preservedRemoved,
        "rolling back a newer action discarded an accepted older action",
        Test);
    passed &= check(
        staleRegistered && stalePublished && staleRemoved &&
            replacementRegistered && replacementPublished &&
            replacementAdmitted && !replacementResult.handled &&
            !replacementResult.allowRearm && replacement.calls == 1 &&
            replacementRemoved,
        "an invalidated lifetime poisoned replacement rearm", Test);
    passed &= check(
        wrappingRegistered && lastPublished && wrappedPublished &&
            wrappedAdmitted && wrappedResult.handled &&
            wrappedResult.allowRearm && wrapping.calls == 1 && wrappingRemoved,
        "generation wrap did not retain the newest coalesced action", Test);
    passed &= check(
        cancellingRegistered && cancellingPublished && cancellingAdmitted &&
            !cancellingResult.handled && cancellingResult.allowRearm &&
            cancelling.calls == 1 && cancelling.deferred &&
            !cancellingRegistry.handlerCount(CancellingLine),
        "a claimed action lost teardown's quiesced rearm result", Test);

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS irq-threaded-exact-rollback");
        NOTICE("HOSTED-WAIT-TEST: PASS irq-threaded-rollback-restores-prior");
        NOTICE("HOSTED-WAIT-TEST: PASS irq-threaded-lifetime-floor");
        NOTICE("HOSTED-WAIT-TEST: PASS irq-threaded-generation-wrap");
        NOTICE("HOSTED-WAIT-TEST: PASS irq-threaded-claimed-cancellation");
    }
    return passed;
}

bool threadedQuiescedRearm()
{
    constexpr const char *Test = "irq-threaded-quiesced-rearm";
    constexpr uint8_t Line = 7;
    IrqHandlerRegistry registry;
    DispositionHandler quiesced(IrqDisposition::Quiesced);
    DispositionHandler notHandled(IrqDisposition::NotHandled);

    const bool quiescedRegistered =
        registry.registerThreadedHandler(Line, &quiesced);
    const bool notHandledRegistered =
        registry.registerThreadedHandler(Line, &notHandled);

    PicIrqState state;
    state.setAllEnabled(false);
    state.handlerRegistered(Line, IrqPolicy::levelThreaded());
    state.handlerRegistered(Line, IrqPolicy::levelThreaded());

    const size_t quiescedGeneration = state.beginDispatch(Line);
    state.beginThreadedDispatch(Line);
    registry.publishThreadedDispatch(Line, quiescedGeneration);
    IrqHandlerRegistry::ThreadedDispatchResult quiescedResult = {};
    const bool quiescedAdmitted =
        registry.dispatchThreaded(Line, quiescedGeneration, quiescedResult);
    const bool quiescedCompleted = state.completeThreadedDispatch(
        Line, quiescedGeneration,
        quiescedAdmitted && quiescedResult.allowRearm);
    const bool quiescedRearmed =
        state.enabled(Line) && !state.threadedPending(Line);

    quiesced.disposition = IrqDisposition::NotHandled;
    const size_t unhandledGeneration = state.beginDispatch(Line);
    state.beginThreadedDispatch(Line);
    registry.publishThreadedDispatch(Line, unhandledGeneration);
    IrqHandlerRegistry::ThreadedDispatchResult unhandledResult = {};
    const bool unhandledAdmitted =
        registry.dispatchThreaded(Line, unhandledGeneration, unhandledResult);
    const bool unhandledCompleted = state.completeThreadedDispatch(
        Line, unhandledGeneration,
        unhandledAdmitted && unhandledResult.allowRearm);
    const bool unhandledQuarantined =
        !state.enabled(Line) && state.threadedPending(Line);

    const bool quiescedRemoved =
        registry.unregisterHandler(Line, &quiesced) ==
        IrqHandlerRegistry::UnregisterResult::Completed;
    const bool notHandledRemoved =
        registry.unregisterHandler(Line, &notHandled) ==
        IrqHandlerRegistry::UnregisterResult::Completed;
    state.handlerUnregistered(Line);
    state.handlerUnregistered(Line);

    bool passed = true;
    passed &= check(
        quiescedRegistered && notHandledRegistered,
        "the shared threaded handlers did not register", Test);
    passed &= check(
        quiescedAdmitted && !quiescedResult.handled &&
            quiescedResult.allowRearm && quiescedCompleted && quiescedRearmed,
        "a quiesced shared callback did not permit line rearm", Test);
    passed &= check(
        state.handlerCount(Line) == 0 && !state.threadedPending(Line),
        "the shared line did not retire after cleanup", Test);
    passed &= check(
        unhandledAdmitted && !unhandledResult.handled &&
            !unhandledResult.allowRearm && unhandledCompleted &&
            unhandledQuarantined,
        "all-not-handled callbacks reported a rearmable batch", Test);
    passed &= check(
        quiescedRemoved && notHandledRemoved && quiesced.calls == 2 &&
            notHandled.calls == 2,
        "the disposition handlers did not dispatch and retire exactly", Test);

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS irq-threaded-quiesced-rearm");
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

    constexpr size_t MixedLine = 10;
    const bool mixedHardAccepted = state.canRegister(
        MixedLine, IrqPolicy::levelHard(), IrqDelivery::Hard);
    state.handlerRegistered(
        MixedLine, IrqPolicy::levelHard(), IrqDelivery::Hard);
    const bool mixedThreadedAccepted = state.canRegister(
        MixedLine, IrqPolicy::levelThreaded(), IrqDelivery::Threaded);
    state.handlerRegistered(
        MixedLine, IrqPolicy::levelThreaded(), IrqDelivery::Threaded);
    const size_t mixedGeneration = state.beginDispatch(MixedLine);
    state.beginThreadedDispatch(MixedLine);
    passed &= check(
        mixedHardAccepted && mixedThreadedAccepted &&
            state.delivery(MixedLine) == IrqDelivery::Mixed &&
            state.hardHandlerCount(MixedLine) == 1 &&
            state.threadedHandlerCount(MixedLine) == 1 &&
            state.lineRelease(MixedLine) ==
                IrqLineRelease::AfterThreadedCompletion &&
            !state.enabled(MixedLine) && state.threadedPending(MixedLine),
        "a mixed level line did not defer release to its worker", Test);
    state.handlerUnregistered(MixedLine, IrqDelivery::Threaded);
    passed &= check(
            state.delivery(MixedLine) == IrqDelivery::Hard &&
            state.lineRelease(MixedLine) == IrqLineRelease::AfterHardStage &&
            state.enabled(MixedLine) && !state.threadedPending(MixedLine) &&
            state.completeThreadedDispatch(
                MixedLine, mixedGeneration, true),
        "removing the final threaded peer did not release mixed masking",
        Test);
    state.handlerUnregistered(MixedLine, IrqDelivery::Hard);
    passed &= check(
        state.delivery(MixedLine) == IrqDelivery::None &&
            !state.enabled(MixedLine),
        "the mixed line did not retire after its hard peer", Test);

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
          handlerCalls(0), handlerCompleted(0), genericPublications(0)
    {
    }

    Semaphore gate;
    Thread *waiter;
    Atomic<size_t> armed;
    Atomic<size_t> entered;
    Atomic<size_t> completed;
    Atomic<size_t> handlerCalls;
    Atomic<size_t> handlerCompleted;
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

class IrqReadyPublicationHandler : public IrqHandler
{
  public:
    explicit IrqReadyPublicationHandler(IrqReadyPublicationContext &context)
        : m_Context(context)
    {
    }

    IrqDisposition irq(irq_id_t) override
    {
        if (!m_Context.armed || !m_Context.handlerCalls.compareAndSwap(0, 1))
        {
            return IrqDisposition::Handled;
        }

        m_Context.gate.release();
        m_Context.handlerCompleted = 1;
        return IrqDisposition::Handled;
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

    size_t waiterStackSize = 0;
    void *waiterStackBase =
        context.waiter->getKernelStackBase(&waiterStackSize);
    const SchedulerState &waiterState = context.waiter->state();
    const bool schedulerStackPublished =
        waiterStackBase && waiterStackSize &&
        waiterState.stackBase ==
            reinterpret_cast<uintptr_t>(waiterStackBase) &&
        waiterState.stackSize == waiterStackSize;

    const irq_id_t id =
        waiterBlocked ? manager->registerIsaIrqHandler(
                            2, &handler, IrqPolicy::syntheticThreaded()) :
                        0;
    g_IrqReadyPublicationContext = &context;
    Scheduler::setGenericThreadStatusHook(observeGenericThreadStatus);
    context.armed = 1;
    const bool signalQueued = id && raise(SIGURG) == 0;
    bool handlerFinished = false;
    for (size_t attempt = 0; signalQueued && attempt < Attempts; ++attempt)
    {
        if (context.handlerCompleted == 1)
        {
            handlerFinished = true;
            break;
        }
        Scheduler::instance().yield();
    }
    handlerFinished = context.handlerCompleted == 1;
    context.armed = 0;
    Scheduler::setGenericThreadStatusHook(nullptr);
    g_IrqReadyPublicationContext = nullptr;

    if (!handlerFinished)
    {
        context.gate.release();
    }
    const bool waiterJoined = context.waiter->join();
    const bool cleaned = id && manager->unregisterHandler(id, &handler);

    bool passed = true;
    passed &= check(
        schedulerStackPublished,
        "the waiter scheduler state lost its hosted stack bounds", Test);
    passed &= check(
        waiterBlocked, "the waiter did not publish its semaphore wait", Test);
    passed &= check(id != 0, "the IRQ handler could not be registered", Test);
    passed &= check(
        signalQueued && handlerFinished && context.handlerCalls == 1,
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
        NOTICE("HOSTED-WAIT-TEST: PASS hosted-scheduler-stack-bounds");
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
    const bool replacementCleaned =
        !HostedIrqManager::containsHandlerForTest(1, &context.replacement);

    const size_t activeOriginal =
        HostedIrqManager::activeDispatchCountForTest(&context.original);
    const size_t activeReplacement =
        HostedIrqManager::activeDispatchCountForTest(&context.replacement);
    const size_t claimed =
        HostedIrqManager::claimedDispatchCountForOwnerForTest(
            Processor::information().getCurrentThread());

    bool passed = true;
    passed &= check(
        signalQueued && context.prePinCalls == 1 &&
            context.originalRemoved == 1 &&
            context.replacementRegistered == 1 && !context.committedCalls &&
            !context.replacementRemoved,
        "a pre-retirement reader pinned the replacement generation", Test);
    passed &= check(
        replacementStillPublished && replacementCleaned && !activeOriginal &&
            !activeReplacement && !claimed && failureCleanup,
        "stale generation cleanup left a publication or callback hazard", Test);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "irq-stale-generation-reuse");
    }
    return passed;
}

class AbandonmentProbeThread : public Thread
{
  public:
    using Thread::Thread;
    using Thread::armAtomicStateCleanup;
    using Thread::disarmAtomicStateCleanup;
};

struct AbandonedSignalFrameProbe
{
    AbandonedSignalFrameProbe(
        Thread *owner, Atomic<size_t> &calls, Atomic<size_t> &failures)
        : thread(owner), cleanupCalls(&calls), stateFailures(&failures),
          baselineDepth(Processor::hostedSignalFrameDepthForTest()), cleanup()
    {
    }

    Thread *thread;
    Atomic<size_t> *cleanupCalls;
    Atomic<size_t> *stateFailures;
    size_t baselineDepth;
    AtomicStateCleanupRecord cleanup;
};

void observeAbandonedSignalFrameCleanup(void *parameter)
{
    AbandonedSignalFrameProbe *probe =
        reinterpret_cast<AbandonedSignalFrameProbe *>(parameter);
    *probe->cleanupCalls += 1;
    if (Processor::information().getCurrentThread() != probe->thread ||
        probe->thread->getHostedSignalDepth() ||
        Processor::hostedSignalFrameDepthForTest() != probe->baselineDepth)
    {
        *probe->stateFailures += 1;
    }
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
          cleanupCalls(0), signalCleanupCalls(0), entered(0), returned(0),
          stateFailures(0)
    {
    }

    AbandoningIrqHandler handler;
    AbandonmentProbeThread *worker;
    AbandonedDispatchStage stage;
    Atomic<size_t> hazardCalls;
    Atomic<size_t> cleanupCalls;
    Atomic<size_t> signalCleanupCalls;
    Atomic<size_t> entered;
    Atomic<size_t> returned;
    Atomic<size_t> stateFailures;
};

bool AbandoningIrqHandler::irq(irq_id_t, InterruptState &)
{
    if (m_Context.stage != AbandonedDispatchStage::Callback ||
        Processor::information().getCurrentThread() != m_Context.worker)
    {
        return true;
    }

    m_Context.entered += 1;
    if (Processor::getInterrupts() || !Processor::inDeviceHardIrq() ||
        Processor::deviceHardIrqDepthForTest() != 1)
    {
        m_Context.stateFailures += 1;
    }
    HostedIrqManager::abandonCurrentThreadForTest();
    return true;
}

int abandonIrqDispatch(void *parameter)
{
    AbandonedDispatchContext *context =
        reinterpret_cast<AbandonedDispatchContext *>(parameter);
    AbandonedSignalFrameProbe signalProbe(
        context->worker, context->signalCleanupCalls, context->stateFailures);
    // This older cleanup runs after the signal-frame cleanup when the signal
    // abandons the worker stack, so it can verify the worker's exact baseline.
    context->worker->armAtomicStateCleanup(
        signalProbe.cleanup, observeAbandonedSignalFrameCleanup, &signalProbe);
    const int raised = raise(HardContextTestSignal);
    context->worker->disarmAtomicStateCleanup(signalProbe.cleanup);
    if (raised)
    {
        context->stateFailures += 1;
    }
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
    if (!Processor::getInterrupts() || Processor::inDeviceHardIrq() ||
        Processor::deviceHardIrqDepthForTest() != 0)
    {
        context->stateFailures += 1;
    }
    HostedIrqManager::abandonCurrentThreadForTest();
}

void abandonIrqCleanup(void *owner, bool callbackBoundaryEntered)
{
    AbandonedDispatchContext *context = g_AbandonedDispatchContext;
    if (!context || owner != context->worker)
    {
        return;
    }

    context->cleanupCalls += 1;
    const bool expectedRestore =
        context->stage == AbandonedDispatchStage::Callback;
    if (callbackBoundaryEntered != expectedRestore ||
        !Processor::getInterrupts() || Processor::inDeviceHardIrq() ||
        Processor::deviceHardIrqDepthForTest() != 0)
    {
        context->stateFailures += 1;
    }
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
    context->cleanupCalls = 0;
    context->signalCleanupCalls = 0;
    context->entered = 0;
    context->returned = 0;
    context->stateFailures = 0;
    const irq_id_t id = manager->registerHardIsaIrqHandler(
        HardContextTestIrq, &context->handler, IrqPolicy::syntheticHard());

    context->worker = new AbandonmentProbeThread(
        Scheduler::instance().getKernelProcess(), abandonIrqDispatch, context,
        nullptr, false, true, true);
    context->worker->setName("hosted abandoned IRQ publication");

    g_AbandonedDispatchContext = context;
    HostedIrqManager::setDispatchAbandonHook(abandonIrqCleanup);
    if (stage != AbandonedDispatchStage::Callback)
    {
        HostedIrqManager::setHandlerHazardHook(abandonIrqHazard);
    }
    const bool started = id && context->worker->start();
    const bool joined = started && context->worker->joinForCompletion();
    HostedIrqManager::setHandlerHazardHook(nullptr);
    HostedIrqManager::setDispatchAbandonHook(nullptr);
    g_AbandonedDispatchContext = nullptr;

    const size_t active =
        HostedIrqManager::activeDispatchCountForTest(&context->handler);
    const size_t claimed =
        HostedIrqManager::claimedDispatchCountForOwnerForTest(context->worker);
    const bool stateRestored = !context->stateFailures &&
                               !Processor::inDeviceHardIrq() &&
                               Processor::deviceHardIrqDepthForTest() == 0 &&
                               Processor::getInterrupts();
    const bool cleaned =
        id && manager->unregisterHandler(id, &context->handler);

    const bool passed = id && started && joined && !context->returned &&
                        context->hazardCalls == expectedHazardCalls &&
                        context->cleanupCalls == 1 &&
                        context->signalCleanupCalls == 1 &&
                        context->entered == expectedCallbackCalls && !active &&
                        !claimed && cleaned && stateRestored;
    return passed;
}

struct NestedAbandonedDepthContext;

class NestedAbandoningInner : public HardIrqHandler
{
  public:
    explicit NestedAbandoningInner(NestedAbandonedDepthContext &context)
        : m_Context(context)
    {
    }

    bool irq(irq_id_t, InterruptState &) override;

  private:
    NestedAbandonedDepthContext &m_Context;
};

class NestedAbandoningOuter : public HardIrqHandler
{
  public:
    explicit NestedAbandoningOuter(NestedAbandonedDepthContext &context)
        : m_Context(context)
    {
    }

    bool irq(irq_id_t, InterruptState &) override;

  private:
    NestedAbandonedDepthContext &m_Context;
};

struct NestedAbandonedDepthContext
{
    NestedAbandonedDepthContext()
        : inner(*this), outer(*this), worker(nullptr), outerEntered(0),
          innerEntered(0), signalCleanupCalls(0), returned(0), failures(0)
    {
    }

    NestedAbandoningInner inner;
    NestedAbandoningOuter outer;
    AbandonmentProbeThread *worker;
    Atomic<size_t> outerEntered;
    Atomic<size_t> innerEntered;
    Atomic<size_t> signalCleanupCalls;
    Atomic<size_t> returned;
    Atomic<size_t> failures;
};

bool NestedAbandoningInner::irq(irq_id_t, InterruptState &)
{
    if (Processor::information().getCurrentThread() != m_Context.worker)
    {
        return true;
    }

    m_Context.innerEntered += 1;
    if (Processor::getInterrupts() || !Processor::inDeviceHardIrq() ||
        Processor::deviceHardIrqDepthForTest() != 2)
    {
        m_Context.failures += 1;
    }
    HostedIrqManager::abandonCurrentThreadForTest();
    return true;
}

bool NestedAbandoningOuter::irq(irq_id_t, InterruptState &)
{
    if (Processor::information().getCurrentThread() != m_Context.worker)
    {
        return true;
    }

    m_Context.outerEntered += 1;
    if (Processor::getInterrupts() || !Processor::inDeviceHardIrq() ||
        Processor::deviceHardIrqDepthForTest() != 1)
    {
        m_Context.failures += 1;
    }

    bool handled = false;
    HostedIrqManager::dispatchHandlerForTest(
        HardContextTestIrq, &m_Context.inner, handled);
    m_Context.returned += 1;
    return true;
}

int abandonNestedIrqDispatch(void *parameter)
{
    NestedAbandonedDepthContext *context =
        reinterpret_cast<NestedAbandonedDepthContext *>(parameter);
    AbandonedSignalFrameProbe signalProbe(
        context->worker, context->signalCleanupCalls, context->failures);
    context->worker->armAtomicStateCleanup(
        signalProbe.cleanup, observeAbandonedSignalFrameCleanup, &signalProbe);
    const int raised = raise(HardContextTestSignal);
    context->worker->disarmAtomicStateCleanup(signalProbe.cleanup);
    if (raised)
    {
        context->failures += 1;
    }
    context->returned += 1;
    return 1;
}

bool abandonedNestedDispatchDepthCleanup()
{
    IrqManager *manager = Machine::instance().getIrqManager();
    static NestedAbandonedDepthContext context;
    context.outerEntered = 0;
    context.innerEntered = 0;
    context.signalCleanupCalls = 0;
    context.returned = 0;
    context.failures = 0;

    const irq_id_t outerId = manager->registerHardIsaIrqHandler(
        HardContextTestIrq, &context.outer, IrqPolicy::syntheticHard());
    const irq_id_t innerId = manager->registerHardIsaIrqHandler(
        HardContextTestIrq, &context.inner, IrqPolicy::syntheticHard());
    context.worker = new AbandonmentProbeThread(
        Scheduler::instance().getKernelProcess(), abandonNestedIrqDispatch,
        &context, nullptr, false, true, true);
    context.worker->setName("hosted nested abandoned IRQ callback");

    const bool started = outerId && innerId && context.worker->start();
    const bool joined = started && context.worker->joinForCompletion();
    const size_t outerActive =
        HostedIrqManager::activeDispatchCountForTest(&context.outer);
    const size_t innerActive =
        HostedIrqManager::activeDispatchCountForTest(&context.inner);
    const size_t claimed =
        HostedIrqManager::claimedDispatchCountForOwnerForTest(context.worker);
    const bool outerCleaned =
        outerId && manager->unregisterHandler(outerId, &context.outer);
    const bool innerCleaned =
        innerId && manager->unregisterHandler(innerId, &context.inner);

    return started && joined && context.outerEntered == 1 &&
           context.innerEntered == 1 && context.signalCleanupCalls == 1 &&
           !context.returned && !context.failures && !outerActive &&
           !innerActive && !claimed && outerCleaned && innerCleaned &&
           !Processor::inDeviceHardIrq() &&
           Processor::deviceHardIrqDepthForTest() == 0 &&
           Processor::getInterrupts();
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
    const bool nested = abandonedNestedDispatchDepthCleanup();

    bool passed = true;
    passed &= check(
        beforeClaim && claimed && committed && nested,
        "stack abandonment leaked interrupt state, a callback hazard, or "
        "hard-IRQ depth",
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
          dispatchHandled(0), pinIrqEnabled(0), callbackIrqDisabled(0),
          releaseIrqDisabled(0), returnIrqRestored(0), handlerCalls(0),
          callbacksAfterReturn(0), unregisterReturned(0),
          unregisterSucceeded(0), failures(0)
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
    Atomic<size_t> pinIrqEnabled;
    Atomic<size_t> callbackIrqDisabled;
    Atomic<size_t> releaseIrqDisabled;
    Atomic<size_t> returnIrqRestored;
    Atomic<size_t> handlerCalls;
    Atomic<size_t> callbacksAfterReturn;
    Atomic<size_t> unregisterReturned;
    Atomic<size_t> unregisterSucceeded;
    Atomic<size_t> failures;
};

bool LifetimeHandler::irq(irq_id_t, InterruptState &)
{
    m_Context.handlerCalls += 1;
    if (!Processor::getInterrupts())
    {
        m_Context.callbackIrqDisabled += 1;
    }
    else
    {
        m_Context.failures += 1;
    }
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
    if (Processor::getInterrupts())
    {
        context->pinIrqEnabled += 1;
    }
    else
    {
        context->failures += 1;
    }
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
        !current->getHostedSignalDepth() && !Processor::getInterrupts() &&
        context->phase == static_cast<size_t>(3) &&
        !context->unregisterReturned &&
        !context->remover->getWaitDebugInfo(wait) &&
        context->remover->getDebugState(debugAddress) ==
            Thread::CallbackDrain &&
        debugAddress == reinterpret_cast<uintptr_t>(&context->handler))
    {
        context->releaseObservedAtomicDrain += 1;
        context->releaseIrqDisabled += 1;
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
    if (Processor::getInterrupts())
    {
        context->returnIrqRestored += 1;
    }
    else
    {
        context->failures += 1;
    }
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
        context.pinIrqEnabled == 1 && context.callbackIrqDisabled == 1 &&
            context.releaseIrqDisabled == 1 &&
            context.returnIrqRestored == 1,
        "hard callback masking escaped its callback boundary");
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
    bool passed = deviceHardIrqContextTracking();
    passed &= deliveryModeSeparation();
    passed &= mixedDeliveryOccurrenceBinding();
    passed &= threadedOccurrenceLifetimeBinding();
    passed &= waitFreeOccurrenceCapturePreservesBoundary();
    passed &= occurrenceGraceTombstones();
    passed &= abandonedOccurrenceLeaseCleanup();
    passed &= retirementBoundaryExcludesNewOccurrences();
    passed &= actionMutationPinsSlotLifetime();
    passed &= quiescedPublicationClosesWorkerExit();
    passed &= abandonedActionMutationReleasesWriter();
    passed &= abandonedCancellationCompletesRollback();
    passed &= claimedCancellationPreservesPriorQuiesce();
    passed &= claimFinalizationArbitratesRemoval();
    passed &= threadedPostCallbackCancellation();
    passed &= threadedActionGenerationBoundaries();
    passed &= threadedQuiescedRearm();
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
