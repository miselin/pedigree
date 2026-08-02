/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/IrqDiagnosticSnapshotStore.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/ThreadedIrqDispatcher.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
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
constexpr const char *Test = "irq-threaded-dispatcher-coalescing";

static_assert(
    __is_trivial(IrqLineDiagnosticSnapshot),
    "IRQ debugger snapshots must remain plain data");
static_assert(
    __is_standard_layout(IrqLineDiagnosticSnapshot),
    "IRQ debugger snapshots must remain detached plain data");

bool check(bool condition, const char *detail, const char *test = Test)
{
    if (condition)
    {
        return true;
    }

    ERROR("HOSTED-WAIT-TEST: FAIL " << test << ": " << detail);
    return false;
}

bool irqDiagnosticSnapshotPublication()
{
    constexpr const char *SnapshotTest = "irq-diagnostic-snapshot-publication";
    IrqDiagnosticSnapshotStore<1> store;
    IrqLineDiagnosticSnapshot initial = {};
    bool passed = check(
        store.snapshot(0, initial) && initial.snapshotGeneration == 1,
        "the initial immutable bank was not readable", SnapshotTest);

    size_t heldBank = 0;
    const bool readerHeld = store.claimPublishedBankForTest(0, heldBank);
    passed &= check(
        readerHeld, "the published bank could not be held by a reader",
        SnapshotTest);

    size_t bank = 0;
    IrqLineDiagnosticSnapshot *next = store.beginPublication(0, bank);
    passed &=
        check(next != nullptr, "a private bank was unavailable", SnapshotTest);
    if (!next)
    {
        if (readerHeld)
        {
            store.releasePublishedBankForTest(0, heldBank);
        }
        return false;
    }

    *next = {};
    next->line = 0;
    next->configured = true;
    next->handlerCount = 3;
    next->delivery = IrqDelivery::Threaded;
    next->trigger = IrqTrigger::Level;
    next->controllerAck = IrqControllerAck::AfterHardStage;
    next->lineRelease = IrqLineRelease::AfterThreadedCompletion;
    next->effectiveMasked = true;
    next->maskReasons = IrqMaskAwaitingThreadedCompletion;
    next->dispatchGeneration = 0x1234;
    next->publicationCookie = 0x5678;

    IrqLineDiagnosticSnapshot whilePublishing = {};
    passed &= check(
        store.snapshot(0, whilePublishing) &&
            whilePublishing.snapshotGeneration == initial.snapshotGeneration &&
            !whilePublishing.configured,
        "an incomplete private bank escaped to a reader", SnapshotTest);

    size_t nestedBank = 0;
    passed &= check(
        !store.beginPublication(0, nestedBank) &&
            store.missedPublications(0) == 1,
        "a nested writer waited or entered the active publication",
        SnapshotTest);

    store.finishPublication(0, bank);
    IrqLineDiagnosticSnapshot published = {};
    passed &= check(
        store.snapshot(0, published) && published.snapshotGeneration == 2 &&
            published.configured && published.handlerCount == 3 &&
            published.delivery == IrqDelivery::Threaded &&
            published.trigger == IrqTrigger::Level &&
            published.lineRelease == IrqLineRelease::AfterThreadedCompletion &&
            published.effectiveMasked &&
            published.maskReasons == IrqMaskAwaitingThreadedCompletion &&
            published.dispatchGeneration == 0x1234 &&
            published.publicationCookie == 0x5678,
        "the committed bank was internally inconsistent", SnapshotTest);

    size_t rotatedBank = 0;
    IrqLineDiagnosticSnapshot *rotated = store.beginPublication(0, rotatedBank);
    passed &= check(
        rotated && rotatedBank != bank && rotatedBank != heldBank,
        "the writer did not rotate to the third immutable bank", SnapshotTest);
    if (rotated)
    {
        *rotated = published;
        rotated->handlerCount = 4;
        store.finishPublication(0, rotatedBank);
    }

    size_t contendedBank = 0;
    IrqLineDiagnosticSnapshot *contended =
        store.beginPublication(0, contendedBank);
    passed &= check(
        contended && contendedBank != heldBank,
        "a held reader bank blocked or was reused by the writer", SnapshotTest);
    if (contended)
    {
        *contended = published;
        contended->handlerCount = 5;
        store.finishPublication(0, contendedBank);
    }

    IrqLineDiagnosticSnapshot rotatedSnapshot = {};
    passed &= check(
        store.snapshot(0, rotatedSnapshot) &&
            rotatedSnapshot.snapshotGeneration == 4 &&
            rotatedSnapshot.handlerCount == 5,
        "held-reader contention lost the newest complete publication",
        SnapshotTest);
    if (readerHeld)
    {
        store.releasePublishedBankForTest(0, heldBank);
    }

    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "irq-diagnostic-snapshot-publication");
    }
    return passed;
}

struct DispatcherContext
{
    DispatcherContext()
        : firstEntered(0), releaseFirst(0), secondEntered(0),
          publisher(nullptr), dispatcher(nullptr), calls(0), failures(0),
          publicationHooks(0), nestedPublished(false),
          selfShutdownRejected(false), lines(), cookies(), workers()
    {
    }

    Semaphore firstEntered;
    Semaphore releaseFirst;
    Semaphore secondEntered;
    Thread *publisher;
    ThreadedIrqDispatcher *dispatcher;
    Atomic<size_t> calls;
    Atomic<size_t> failures;
    Atomic<size_t> publicationHooks;
    bool nestedPublished;
    bool selfShutdownRejected;
    uint8_t lines[2];
    size_t cookies[2];
    Thread *workers[2];
};

DispatcherContext *g_NestedPublicationContext = nullptr;

void publishNewerFromNestedHook(
    ThreadedIrqDispatcher *dispatcher, uint8_t line, size_t cookie,
    size_t pending)
{
    DispatcherContext *context = g_NestedPublicationContext;
    dispatcher->setPublicationObservedHookForTest(nullptr);
    if (!context || dispatcher != context->dispatcher || line != 1 ||
        cookie != 2 || pending != 0)
    {
        if (context)
        {
            context->failures += 1;
        }
        return;
    }

    context->publicationHooks += 1;
    context->nestedPublished = dispatcher->publishFromInterrupt(line, 3) &&
                               dispatcher->publishFromInterrupt(line, 4);
}

void dispatchBatch(void *opaque, uint8_t line, size_t cookie)
{
    DispatcherContext *context = reinterpret_cast<DispatcherContext *>(opaque);
    const size_t call = (context->calls += 1) - 1;
    if (call >= 2)
    {
        context->failures += 1;
        return;
    }

    Thread *current = Processor::information().getCurrentThread();
    context->lines[call] = line;
    context->cookies[call] = cookie;
    context->workers[call] = current;
    if (!current || current == context->publisher ||
        !Processor::getInterrupts() || current->getHostedSignalDepth())
    {
        context->failures += 1;
    }

    if (!call)
    {
        context->selfShutdownRejected =
            context->dispatcher && !context->dispatcher->shutdown();
        context->firstEntered.release();
        if (!context->releaseFirst.acquireForCompletion())
        {
            context->failures += 1;
        }
    }
    else
    {
        context->secondEntered.release();
    }
}

bool threadedDispatcherCoalescing()
{
    DispatcherContext context;
    context.publisher = Processor::information().getCurrentThread();
    ThreadedIrqDispatcher dispatcher(
        MakeConstantString("hosted threaded IRQ regression"), 2, dispatchBatch,
        &context);
    context.dispatcher = &dispatcher;
    const bool initialised = dispatcher.initialise();

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    const bool firstPublished =
        initialised && dispatcher.publishFromInterrupt(1, 1);
    const bool firstDoorbell =
        PerProcessorScheduler::currentIrqWorkDoorbellPendingForTest();
    Processor::setInterrupts(interruptsWereEnabled);
    PerProcessorScheduler::serviceCurrentIrqWorkDoorbellForTest();

    const bool firstObserved =
        firstPublished && context.firstEntered.acquireForCompletion(1, 2, 0);
    const bool firstActive =
        dispatcher.callbackActive(1) && dispatcher.activeCookie(1) == 1 &&
        dispatcher.pendingCookie(1) == 0 &&
        dispatcher.completedCookie(1) == 0 && dispatcher.workerIdentity(1) != 0;
    bool laterPublished = false;
    bool laterDoorbell = false;
    if (firstObserved)
    {
        Processor::setInterrupts(false);
        g_NestedPublicationContext = &context;
        dispatcher.setPublicationObservedHookForTest(
            publishNewerFromNestedHook);
        laterPublished = dispatcher.publishFromInterrupt(1, 2) &&
                         context.nestedPublished;
        dispatcher.setPublicationObservedHookForTest(nullptr);
        g_NestedPublicationContext = nullptr;
        laterDoorbell =
            PerProcessorScheduler::currentIrqWorkDoorbellPendingForTest();
        Processor::setInterrupts(interruptsWereEnabled);
        PerProcessorScheduler::serviceCurrentIrqWorkDoorbellForTest();
    }

    context.releaseFirst.release();
    const bool secondObserved =
        laterPublished && context.secondEntered.acquireForCompletion(1, 2, 0);
    const bool stopped = dispatcher.shutdown();

    bool passed = true;
    passed &= check(initialised, "the dispatcher did not initialise");
    passed &= check(firstPublished, "wake-before-block publication failed");
    passed &= check(
        firstDoorbell && laterDoorbell,
        "hard publication did not leave an atomic IRQ-work doorbell", Test);
    passed &= check(firstObserved, "the first worker batch did not enter");
    passed &= check(
        firstActive,
        "active worker diagnostics lost the claimed callback window", Test);
    passed &= check(laterPublished, "a coalesced publication was rejected");
    passed &= check(
        context.publicationHooks == 1,
        "the nested producer interleave did not run exactly once", Test);
    passed &= check(secondObserved, "the coalesced worker batch did not enter");
    passed &= check(stopped, "the dispatcher did not drain and join");
    passed &= check(
        context.calls == 2 && context.failures == 0,
        "callbacks did not run exactly twice in ordinary thread context");
    passed &= check(
        context.selfShutdownRejected,
        "a callback mutated its own dispatcher before rejecting self-join");
    passed &= check(
        context.lines[0] == 1 && context.lines[1] == 1 &&
            context.cookies[0] == 1 && context.cookies[1] == 4,
        "the pending cookies were not coalesced to the newest occurrence");
    passed &= check(
        context.workers[0] && context.workers[0] == context.workers[1],
        "one physical line did not retain one stable worker");
    passed &= check(
        dispatcher.completedBatches(1) == 2 &&
            dispatcher.completedCookie(1) == 4 &&
            !dispatcher.callbackActive(1) && dispatcher.publicationClosed(1),
        "completion generation did not follow callback return");
    passed &= check(
        !dispatcher.publicationClosed(2) && !dispatcher.callbackActive(2) &&
            dispatcher.workerIdentity(2) == 0,
        "an invalid diagnostic line reported live dispatcher state");

    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "irq-threaded-dispatcher-coalescing");
    }
    return passed;
}

bool hostedSyntheticIrqMasking()
{
    constexpr const char *MaskTest = "hosted-synthetic-irq-masking";
    struct sigaction action = {};
    const bool actionRead = sigaction(SIGURG, nullptr, &action) == 0;
    const bool actionMasksIrqs =
        actionRead && !(action.sa_flags & SA_NODEFER) &&
        sigismember(&action.sa_mask, SIGUSR1) == 1 &&
        sigismember(&action.sa_mask, SIGUSR2) == 1 &&
        sigismember(&action.sa_mask, SIGURG) == 1;

    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    sigset_t disabledMask;
    const bool disabledMaskRead =
        sigprocmask(0, nullptr, &disabledMask) == 0;
    const bool disabledMasksIrqs =
        disabledMaskRead && sigismember(&disabledMask, SIGUSR1) == 1 &&
        sigismember(&disabledMask, SIGUSR2) == 1 &&
        sigismember(&disabledMask, SIGURG) == 1;

    Processor::setInterrupts(true);
    sigset_t enabledMask;
    const bool enabledMaskRead = sigprocmask(0, nullptr, &enabledMask) == 0;
    const bool enabledUnmasksIrqs =
        enabledMaskRead && sigismember(&enabledMask, SIGUSR1) == 0 &&
        sigismember(&enabledMask, SIGUSR2) == 0 &&
        sigismember(&enabledMask, SIGURG) == 0;
    Processor::setInterrupts(interruptsWereEnabled);

    const bool passed = check(
        actionMasksIrqs && disabledMasksIrqs && enabledUnmasksIrqs,
        "SIGURG did not follow hosted IRQ action and mask semantics", MaskTest);
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS hosted-synthetic-irq-masking");
    }
    return passed;
}

class HostedThreadedHandler : public IrqHandler
{
  public:
    HostedThreadedHandler()
        : entered(0), publisher(nullptr), hardReturned(0), calls(0),
          callbacksBeforeHardReturn(0), failures(0)
    {
    }

    IrqDisposition irq(irq_id_t) override
    {
        calls += 1;
        if (!hardReturned)
        {
            callbacksBeforeHardReturn += 1;
        }
        Thread *current = Processor::information().getCurrentThread();
        if (!current || current == publisher || !Processor::getInterrupts() ||
            current->getHostedSignalDepth())
        {
            failures += 1;
        }
        entered.release();
        return IrqDisposition::Handled;
    }

    Semaphore entered;
    Thread *publisher;
    Atomic<size_t> hardReturned;
    Atomic<size_t> calls;
    Atomic<size_t> callbacksBeforeHardReturn;
    Atomic<size_t> failures;
};

class HostedStalledIrqHandler : public IrqHandler
{
  public:
    HostedStalledIrqHandler()
        : entered(0), releaseCallback(0), calls(0), failures(0)
    {
    }

    IrqDisposition irq(irq_id_t) override
    {
        calls += 1;
        entered.release();
        if (!releaseCallback.acquireForCompletion())
        {
            failures += 1;
        }
        return IrqDisposition::Handled;
    }

    Semaphore entered;
    Semaphore releaseCallback;
    Atomic<size_t> calls;
    Atomic<size_t> failures;
};

class HostedHardDiagnosticHandler : public HardIrqHandler
{
  public:
    explicit HostedHardDiagnosticHandler(IrqManager *irqManager)
        : manager(irqManager), observed(false), activeGeneration(0)
    {
    }

    bool irq(irq_id_t, InterruptState &) override
    {
        IrqLineDiagnosticSnapshot lines[3] = {};
        if (manager && manager->snapshotIrqLines(lines, 3) == 3)
        {
            const IrqLineDiagnosticSnapshot &line = lines[2];
            observed =
                line.configured && line.delivery == IrqDelivery::Hard &&
                line.hardStageActive && line.activeHardDispatchCount == 1 &&
                line.activeHardDispatchGeneration != 0 &&
                line.activeHardDispatchGeneration == line.dispatchGeneration;
            activeGeneration = line.activeHardDispatchGeneration;
        }
        return true;
    }

    IrqManager *manager;
    bool observed;
    size_t activeGeneration;
};

class HostedDeferredDiagnosticHandler : public HardIrqHandler
{
  public:
    explicit HostedDeferredDiagnosticHandler(IrqManager *irqManager)
        : manager(irqManager), id(0), removalReturned(true), observed(false),
          activeGeneration(0)
    {
    }

    bool irq(irq_id_t, InterruptState &) override
    {
        removalReturned = manager && manager->unregisterHandler(id, this);
        IrqLineDiagnosticSnapshot lines[3] = {};
        if (manager && manager->snapshotIrqLines(lines, 3) == 3)
        {
            const IrqLineDiagnosticSnapshot &line = lines[2];
            observed =
                !line.configured && line.handlerCount == 0 &&
                line.delivery == IrqDelivery::None && line.hardStageActive &&
                line.activeHardDispatchCount == 1 &&
                line.activeHardDispatchGeneration != 0 &&
                line.activeHardDispatchGeneration == line.dispatchGeneration;
            activeGeneration = line.activeHardDispatchGeneration;
        }
        return true;
    }

    IrqManager *manager;
    irq_id_t id;
    bool removalReturned;
    bool observed;
    size_t activeGeneration;
};

class HostedPolicyDiagnosticHandler : public HardIrqHandler
{
  public:
    bool irq(irq_id_t, InterruptState &) override
    {
        return true;
    }
};

struct StaleDiagnosticPublicationContext
{
    explicit StaleDiagnosticPublicationContext(IrqManager *irqManager)
        : manager(irqManager), oldId(0), snapshotCaptured(0),
          resumePublisher(0), armed(0), hookCalls(0), oldRemoved(0),
          capturedGeneration(0)
    {
    }

    IrqManager *manager;
    HostedPolicyDiagnosticHandler oldHandler;
    irq_id_t oldId;
    Semaphore snapshotCaptured;
    Semaphore resumePublisher;
    Atomic<size_t> armed;
    Atomic<size_t> hookCalls;
    Atomic<size_t> oldRemoved;
    size_t capturedGeneration;
};

StaleDiagnosticPublicationContext *g_StaleDiagnosticPublicationContext =
    nullptr;

void holdStaleDiagnosticPublication(uint8_t irq, size_t mutationGeneration)
{
    StaleDiagnosticPublicationContext *context =
        g_StaleDiagnosticPublicationContext;
    if (!context || irq != 2)
    {
        return;
    }

    context->hookCalls += 1;
    if (context->armed.compareAndSwap(1, 2))
    {
        context->capturedGeneration = mutationGeneration;
        context->snapshotCaptured.release();
        if (!context->resumePublisher.acquireForCompletion())
        {
            context->armed = 3;
        }
    }
}

int unregisterBeforeStaleDiagnosticPublication(void *parameter)
{
    StaleDiagnosticPublicationContext *context =
        reinterpret_cast<StaleDiagnosticPublicationContext *>(parameter);
    if (context->manager->unregisterHandler(
            context->oldId, &context->oldHandler))
    {
        context->oldRemoved = 1;
    }
    return 0;
}

struct HeldMutationDiagnosticContext
{
    explicit HeldMutationDiagnosticContext(IrqManager *irqManager)
        : manager(irqManager), id(0)
    {
    }

    IrqManager *manager;
    HostedPolicyDiagnosticHandler handler;
    irq_id_t id;
};

HeldMutationDiagnosticContext *g_HeldMutationDiagnosticContext = nullptr;

void registerWhileMutationEpochHeld()
{
    HeldMutationDiagnosticContext *context = g_HeldMutationDiagnosticContext;
    if (context)
    {
        context->id = context->manager->registerHardIsaIrqHandler(
            2, &context->handler, IrqPolicy::edgeHard());
    }
}

class HostedLifetimeHandler : public IrqHandler
{
  public:
    HostedLifetimeHandler() : entered(0), calls(0)
    {
    }

    IrqDisposition irq(irq_id_t) override
    {
        calls += 1;
        entered.release();
        return IrqDisposition::Handled;
    }

    Semaphore entered;
    Atomic<size_t> calls;
};

struct HostedLineOwnershipHookContext
{
    HostedLineOwnershipHookContext()
        : stateCheckEntered(0), releaseStateCheck(0), cookieAdvanceEntered(0),
          releaseCookieAdvance(0), workerEntered(0), releaseWorker(0),
          admissionRejected(0), holdStateCheck(0), holdCookieAdvance(0),
          holdWorker(0), rejectedAdmissions(0), hookFailures(0)
    {
    }

    Semaphore stateCheckEntered;
    Semaphore releaseStateCheck;
    Semaphore cookieAdvanceEntered;
    Semaphore releaseCookieAdvance;
    Semaphore workerEntered;
    Semaphore releaseWorker;
    Semaphore admissionRejected;
    Atomic<size_t> holdStateCheck;
    Atomic<size_t> holdCookieAdvance;
    Atomic<size_t> holdWorker;
    Atomic<size_t> rejectedAdmissions;
    Atomic<size_t> hookFailures;
};

HostedLineOwnershipHookContext *g_HostedLineOwnershipHookContext = nullptr;

void observeHostedLineOwnership(
    uint8_t irq, HostedIrqManager::LineOwnershipStage stage, size_t)
{
    HostedLineOwnershipHookContext *context = g_HostedLineOwnershipHookContext;
    if (!context || irq != 2)
    {
        return;
    }

    switch (stage)
    {
        case HostedIrqManager::LineOwnershipStage::BeforeFinalStateCheck:
            if (context->holdStateCheck.compareAndSwap(1, 2))
            {
                context->stateCheckEntered.release();
                if (!context->releaseStateCheck.acquireForCompletion())
                {
                    context->hookFailures += 1;
                }
            }
            break;
        case HostedIrqManager::LineOwnershipStage::BeforeFinalCookieAdvance:
            if (context->holdCookieAdvance.compareAndSwap(1, 2))
            {
                context->cookieAdvanceEntered.release();
                if (!context->releaseCookieAdvance.acquireForCompletion())
                {
                    context->hookFailures += 1;
                }
            }
            break;
        case HostedIrqManager::LineOwnershipStage::AdmissionRejected:
            context->rejectedAdmissions += 1;
            context->admissionRejected.release();
            break;
        case HostedIrqManager::LineOwnershipStage::
            BeforeThreadedCookieValidation:
            if (context->holdWorker.compareAndSwap(1, 2))
            {
                context->workerEntered.release();
                if (!context->releaseWorker.acquireForCompletion())
                {
                    context->hookFailures += 1;
                }
            }
            break;
    }
}

struct HostedLineUnregisterContext
{
    HostedLineUnregisterContext(
        IrqManager *irqManager, irq_id_t irqId, IrqHandlerBase *irqHandler)
        : manager(irqManager), id(irqId), handler(irqHandler), removed(0)
    {
    }

    IrqManager *manager;
    irq_id_t id;
    IrqHandlerBase *handler;
    Atomic<size_t> removed;
};

int unregisterHostedLineLifetime(void *parameter)
{
    HostedLineUnregisterContext *context =
        reinterpret_cast<HostedLineUnregisterContext *>(parameter);
    if (context->manager->unregisterHandler(context->id, context->handler))
    {
        context->removed = 1;
    }
    return 0;
}

bool waitForHostedCookieCompletion(
    IrqManager *manager, size_t cookie, IrqLineDiagnosticSnapshot &completed)
{
    const Time::Timestamp deadline =
        Time::getTicks() + 2 * Time::Multiplier::Second;
    while (Time::getTicks() < deadline)
    {
        IrqLineDiagnosticSnapshot lines[3] = {};
        if (manager->snapshotIrqLines(lines, 3) == 3 &&
            lines[2].completedCookie == cookie && !lines[2].dispatcherActive)
        {
            completed = lines[2];
            return true;
        }
        Processor::pause();
    }
    return false;
}

bool hostedThreadedSignalDelivery()
{
    constexpr const char *SignalTest = "irq-threaded-hosted-signal";
    IrqManager *manager = Machine::instance().getIrqManager();
    HostedThreadedHandler handler;
    handler.publisher = Processor::information().getCurrentThread();

    const irq_id_t id = manager->registerIsaIrqHandler(
        2, &handler, IrqPolicy::syntheticThreaded());
    IrqLineDiagnosticSnapshot registeredLines[3] = {};
    const size_t registeredCount =
        manager->snapshotIrqLines(registeredLines, 3);
    const IrqLineDiagnosticSnapshot registered = registeredLines[2];
    const bool irqEnabled = Processor::getInterrupts();
    const bool raised = id && irqEnabled && raise(SIGURG) == 0;
    IrqLineDiagnosticSnapshot publishedLines[3] = {};
    const size_t publishedCount = manager->snapshotIrqLines(publishedLines, 3);
    const IrqLineDiagnosticSnapshot published = publishedLines[2];
    handler.hardReturned = 1;
    const bool deferredUntilHardReturn = handler.calls == 0;
    const bool doorbellPending =
        PerProcessorScheduler::currentIrqWorkDoorbellPendingForTest();
    const Time::Timestamp deadline =
        Time::getTicks() + 2 * Time::Multiplier::Second;
    while (!handler.calls && Time::getTicks() < deadline)
    {
        // Only the natural scheduler tick may move this CPU to the worker.
        Processor::pause();
    }
    const bool observed = raised && handler.calls == 1 &&
                          handler.entered.acquireForCompletion(1, 2, 0);
    IrqLineDiagnosticSnapshot completed = {};
    const Time::Timestamp completionDeadline =
        Time::getTicks() + 2 * Time::Multiplier::Second;
    while (Time::getTicks() < completionDeadline)
    {
        IrqLineDiagnosticSnapshot lines[3] = {};
        if (manager->snapshotIrqLines(lines, 3) == 3 &&
            lines[2].completedCookie == published.publicationCookie &&
            !lines[2].dispatcherActive)
        {
            completed = lines[2];
            break;
        }
        Processor::pause();
    }
    const bool removed = id && manager->unregisterHandler(id, &handler);
    IrqLineDiagnosticSnapshot removedLines[3] = {};
    const size_t removedCount = manager->snapshotIrqLines(removedLines, 3);
    const IrqLineDiagnosticSnapshot removedSnapshot = removedLines[2];

    bool passed = true;
    passed &=
        check(id != 0, "the threaded handler was not registered", SignalTest);
    passed &= check(
        registeredCount == 3 && registered.configured &&
            registered.handlerCount == 1 &&
            registered.delivery == IrqDelivery::Threaded &&
            registered.trigger == IrqTrigger::Synthetic &&
            registered.controllerAck == IrqControllerAck::None &&
            registered.lineRelease == IrqLineRelease::AfterHardStage &&
            !registered.effectiveMasked && registered.dispatcherInitialised &&
            registered.workerIdentity != 0,
        "registration did not publish its typed line policy", SignalTest);
    passed &= check(raised, "the hosted IRQ signal was not raised", SignalTest);
    passed &= check(
        publishedCount == 3 && published.dispatchGeneration != 0 &&
            published.publicationCookie != 0 &&
            published.pendingCookie == published.publicationCookie &&
            !published.dispatcherActive,
        "hard publication was not visible before worker service", SignalTest);
    passed &= check(
        deferredUntilHardReturn && doorbellPending &&
            handler.callbacksBeforeHardReturn == 0,
        "the bottom half ran before the hard IRQ frame returned", SignalTest);
    passed &= check(
        observed && handler.calls == 1 && handler.failures == 0,
        "the handler did not run once in an enabled ordinary thread",
        SignalTest);
    passed &= check(
        completed.completedCookie == published.publicationCookie &&
            completed.completedBatches != 0 && !completed.dispatcherActive,
        "worker completion was not visible in the detached snapshot",
        SignalTest);
    passed &=
        check(removed, "the threaded handler was not removed", SignalTest);
    passed &= check(
        removedCount == 3 && !removedSnapshot.configured &&
            removedSnapshot.handlerCount == 0 &&
            removedSnapshot.delivery == IrqDelivery::None &&
            removedSnapshot.effectiveMasked &&
            (removedSnapshot.maskReasons & IrqMaskNoHandler),
        "final unregister did not publish an empty masked line", SignalTest);
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS irq-threaded-hosted-signal");
    }
    return passed;
}

bool hostedThreadedStallDiagnostics()
{
    constexpr const char *StallTest = "irq-threaded-stall-diagnostics";
    IrqManager *manager = Machine::instance().getIrqManager();
    HostedStalledIrqHandler handler;
    const irq_id_t id = manager->registerIsaIrqHandler(
        2, &handler, IrqPolicy::syntheticThreaded());

    IrqLineDiagnosticSnapshot registeredLines[3] = {};
    const bool registeredSnapshot =
        manager->snapshotIrqLines(registeredLines, 3) == 3;
    const IrqLineDiagnosticSnapshot registered = registeredLines[2];

    const bool irqEnabled = Processor::getInterrupts();
    const bool raised = id && irqEnabled && raise(SIGURG) == 0;
    IrqLineDiagnosticSnapshot publishedLines[3] = {};
    const bool publishedSnapshot =
        manager->snapshotIrqLines(publishedLines, 3) == 3;
    const IrqLineDiagnosticSnapshot published = publishedLines[2];
    const bool entered =
        raised && handler.entered.acquireForCompletion(1, 2, 0);
    IrqLineDiagnosticSnapshot active = {};
    bool activeObserved = false;
    const Time::Timestamp deadline =
        Time::getTicks() + 2 * Time::Multiplier::Second;
    while (entered && Time::getTicks() < deadline)
    {
        IrqLineDiagnosticSnapshot lines[3] = {};
        if (manager->snapshotIrqLines(lines, 3) == 3)
        {
            const IrqLineDiagnosticSnapshot &line = lines[2];
            if (line.activeThreadedDispatchCount == 1 &&
                line.activeThreadedHandlerIdentity ==
                    reinterpret_cast<uintptr_t>(&handler) &&
                line.workerDiagnosticAvailable && line.workerWaitActive &&
                line.workerWaitQueued &&
                line.workerDebugState == IrqWorkerDebugState::SemaphoreWait &&
                line.workerWaitReason == IrqWorkerWaitReason::Waiting &&
                line.workerWaitChannelOwner ==
                    reinterpret_cast<uintptr_t>(&handler.releaseCallback))
            {
                active = line;
                activeObserved = true;
                break;
            }
        }
        Processor::pause();
    }

    handler.releaseCallback.release();
    IrqLineDiagnosticSnapshot completed = {};
    const bool completionObserved =
        published.publicationCookie &&
        waitForHostedCookieCompletion(
            manager, published.publicationCookie, completed);
    const bool removed = id && manager->unregisterHandler(id, &handler);

    bool passed = true;
    passed &= check(
        id && registeredSnapshot && registered.configured &&
            registered.delivery == IrqDelivery::Threaded,
        "the stalled threaded handler was not registered", StallTest);
    passed &= check(
        raised && publishedSnapshot && published.publicationCookie &&
            published.pendingCookie == published.publicationCookie &&
            published.pendingSinceTimestamp != 0,
        "the stalled occurrence did not publish timing state", StallTest);
    passed &= check(
        entered && activeObserved &&
            active.activeCookie == published.publicationCookie &&
            active.activeCallbackStartedTimestamp != 0 &&
            active.observationTimestamp >=
                active.activeCallbackStartedTimestamp &&
            active.maximumWakeLatency >= active.lastWakeLatency &&
            active.workerWaitQueue != 0,
        "the blocked callback was not attributable to its handler and wait",
        StallTest);
    passed &= check(
        completionObserved &&
            completed.completedBatches == registered.completedBatches + 1 &&
            completed.maximumCallbackRuntime >= completed.lastCallbackRuntime &&
            !completed.activeThreadedDispatchCount &&
            !completed.activeThreadedHandlerIdentity &&
            !completed.activeCallbackStartedTimestamp &&
            !completed.workerWaitActive,
        "callback completion did not publish runtime and clear stall state",
        StallTest);
    passed &= check(
        handler.calls == 1 && handler.failures == 0 && removed,
        "the stalled handler did not resume and unregister cleanly", StallTest);
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS irq-threaded-stall-diagnostics");
    }
    return passed;
}

bool hostedHardStageDiagnostics()
{
    constexpr const char *HardTest = "irq-hosted-hard-stage-diagnostics";
    IrqManager *manager = Machine::instance().getIrqManager();
    HostedHardDiagnosticHandler handler(manager);
    const irq_id_t id = manager->registerHardIsaIrqHandler(
        2, &handler, IrqPolicy::syntheticHard());

    const bool irqEnabled = Processor::getInterrupts();
    const bool raised = id && irqEnabled && raise(SIGURG) == 0;
    IrqLineDiagnosticSnapshot afterLines[3] = {};
    const size_t afterCount = manager->snapshotIrqLines(afterLines, 3);
    const IrqLineDiagnosticSnapshot after = afterLines[2];
    const bool removed = id && manager->unregisterHandler(id, &handler);

    bool passed = true;
    passed &= check(id != 0, "the hard handler was not registered", HardTest);
    passed &= check(raised, "the hosted hard IRQ was not raised", HardTest);
    passed &= check(
        handler.observed && handler.activeGeneration != 0,
        "the callback could not observe its active hard-stage generation",
        HardTest);
    passed &= check(
        afterCount == 3 && !after.hardStageActive &&
            after.activeHardDispatchCount == 0 &&
            after.activeHardDispatchGeneration == 0 &&
            after.dispatchGeneration == handler.activeGeneration,
        "hard-stage diagnostics did not clear immediately after return",
        HardTest);
    passed &= check(removed, "the hard handler was not removed", HardTest);
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS irq-hosted-hard-stage-diagnostics");
    }
    return passed;
}

bool hostedDeferredRetiringDiagnostics()
{
    constexpr const char *RetiringTest =
        "irq-hosted-deferred-retiring-diagnostics";
    IrqManager *manager = Machine::instance().getIrqManager();
    HostedDeferredDiagnosticHandler handler(manager);
    handler.id = manager->registerHardIsaIrqHandler(
        2, &handler, IrqPolicy::syntheticHard());

    const bool irqEnabled = Processor::getInterrupts();
    const bool raised = handler.id && irqEnabled && raise(SIGURG) == 0;
    IrqLineDiagnosticSnapshot afterLines[3] = {};
    const size_t afterCount = manager->snapshotIrqLines(afterLines, 3);
    const IrqLineDiagnosticSnapshot after = afterLines[2];

    // Deferred self-removal closes future admission before the current hazard
    // retires. Keep that transient state visible instead of inventing a live
    // configured handler for the callback already on the stack.
    const irq_id_t reusedId = manager->registerHardIsaIrqHandler(
        2, &handler, IrqPolicy::syntheticHard());
    const bool cleaned =
        reusedId && manager->unregisterHandler(reusedId, &handler);

    bool passed = true;
    passed &= check(
        handler.id != 0 && raised, "the self-removing hard handler did not run",
        RetiringTest);
    passed &= check(
        !handler.removalReturned && handler.observed &&
            handler.activeGeneration != 0,
        "deferred self-removal hid its retiring hard hazard", RetiringTest);
    passed &= check(
        afterCount == 3 && !after.configured &&
            after.delivery == IrqDelivery::None && !after.hardStageActive &&
            after.activeHardDispatchCount == 0 &&
            after.activeHardDispatchGeneration == 0,
        "the retired hard hazard remained visible after callback return",
        RetiringTest);
    passed &= check(
        reusedId != 0 && cleaned, "the deferred slot did not retire for reuse",
        RetiringTest);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "irq-hosted-deferred-retiring-diagnostics");
    }
    return passed;
}

bool hostedDiagnosticPolicyLifecycle()
{
    constexpr const char *PolicyTest = "irq-hosted-diagnostic-policy-lifecycle";
    IrqManager *manager = Machine::instance().getIrqManager();
    HostedPolicyDiagnosticHandler first;
    HostedPolicyDiagnosticHandler survivor;
    HostedPolicyDiagnosticHandler incompatible;

    const irq_id_t firstId = manager->registerHardIsaIrqHandler(
        2, &first, IrqPolicy::syntheticHard());
    const irq_id_t survivorId = manager->registerHardIsaIrqHandler(
        2, &survivor, IrqPolicy::syntheticHard());
    const irq_id_t incompatibleId = manager->registerHardIsaIrqHandler(
        2, &incompatible, IrqPolicy::edgeHard());
    const bool firstRemoved =
        firstId && manager->unregisterHandler(firstId, &first);

    IrqLineDiagnosticSnapshot survivorLines[3] = {};
    const bool survivorSnapshotted =
        manager->snapshotIrqLines(survivorLines, 3) == 3;
    const IrqLineDiagnosticSnapshot survivorLine = survivorLines[2];
    const bool survivorRemoved =
        survivorId && manager->unregisterHandler(survivorId, &survivor);
    const bool incompatibleRemoved =
        incompatibleId &&
        manager->unregisterHandler(incompatibleId, &incompatible);

    StaleDiagnosticPublicationContext context(manager);
    context.oldId = manager->registerHardIsaIrqHandler(
        2, &context.oldHandler, IrqPolicy::syntheticHard());
    Thread *unregisterer = new Thread(
        Scheduler::instance().getKernelProcess(),
        unregisterBeforeStaleDiagnosticPublication, &context, nullptr, false,
        true, true);
    unregisterer->setName("hosted stale IRQ-diagnostic publisher");

    g_StaleDiagnosticPublicationContext = &context;
    HostedIrqManager::setDiagnosticPublicationHook(
        holdStaleDiagnosticPublication);
    context.armed = 1;
    const bool started = context.oldId && unregisterer->start();
    const bool staleSnapshotHeld =
        started && context.snapshotCaptured.acquireForCompletion(1, 2, 0);

    HostedPolicyDiagnosticHandler replacement;
    const irq_id_t replacementId =
        staleSnapshotHeld ? manager->registerHardIsaIrqHandler(
                                2, &replacement, IrqPolicy::edgeHard()) :
                            0;
    IrqLineDiagnosticSnapshot beforeResumeLines[3] = {};
    const bool beforeResumeSnapshotted =
        manager->snapshotIrqLines(beforeResumeLines, 3) == 3;
    const IrqLineDiagnosticSnapshot beforeResume = beforeResumeLines[2];

    context.resumePublisher.release();
    const bool joined = started && unregisterer->joinForCompletion();
    HostedIrqManager::setDiagnosticPublicationHook(nullptr);
    g_StaleDiagnosticPublicationContext = nullptr;

    IrqLineDiagnosticSnapshot afterResumeLines[3] = {};
    const bool afterResumeSnapshotted =
        manager->snapshotIrqLines(afterResumeLines, 3) == 3;
    const IrqLineDiagnosticSnapshot afterResume = afterResumeLines[2];
    const bool replacementRemoved =
        replacementId &&
        manager->unregisterHandler(replacementId, &replacement);
    const bool oldCleanup =
        context.oldRemoved ||
        (context.oldId &&
         manager->unregisterHandler(context.oldId, &context.oldHandler));

    bool passed = true;
    passed &= check(
        firstId && survivorId && !incompatibleId,
        "an occupied line accepted a different policy", PolicyTest);
    passed &= check(
        firstRemoved && survivorSnapshotted && survivorLine.configured &&
            survivorLine.handlerCount == 1 &&
            survivorLine.delivery == IrqDelivery::Hard &&
            survivorLine.trigger == IrqTrigger::Synthetic &&
            survivorLine.controllerAck == IrqControllerAck::None &&
            survivorLine.lineRelease == IrqLineRelease::AfterHardStage,
        "removing one handler discarded the surviving handler policy",
        PolicyTest);
    passed &= check(
        survivorRemoved && !incompatibleRemoved,
        "the policy compatibility setup did not clean up", PolicyTest);
    passed &= check(
        started && staleSnapshotHeld && replacementId &&
            context.capturedGeneration != 0,
        "the final-unregister publisher was not held after its empty snapshot",
        PolicyTest);
    passed &= check(
        beforeResumeSnapshotted && beforeResume.configured &&
            beforeResume.handlerCount == 1 &&
            beforeResume.trigger == IrqTrigger::Edge,
        "the replacement policy was not published during the interleaving",
        PolicyTest);
    passed &= check(
        joined && context.oldRemoved && context.hookCalls >= 3 &&
            afterResumeSnapshotted && afterResume.configured &&
            afterResume.handlerCount == 1 &&
            afterResume.delivery == IrqDelivery::Hard &&
            afterResume.trigger == IrqTrigger::Edge &&
            afterResume.controllerAck == IrqControllerAck::BeforeHardStage &&
            afterResume.lineRelease == IrqLineRelease::AfterHardStage,
        "a stale final-unregister publication overwrote the replacement line",
        PolicyTest);
    passed &= check(
        replacementRemoved && oldCleanup,
        "the lifecycle race handlers did not clean up", PolicyTest);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "irq-hosted-diagnostic-policy-lifecycle");
    }
    return passed;
}

bool hostedDiagnosticMissedRegistrySnapshot()
{
    constexpr const char *MissedTest =
        "irq-hosted-diagnostic-missed-registry-snapshot";
    IrqManager *manager = Machine::instance().getIrqManager();
    IrqLineDiagnosticSnapshot beforeLines[3] = {};
    const bool beforeSnapshotted =
        manager->snapshotIrqLines(beforeLines, 3) == 3;
    const IrqLineDiagnosticSnapshot before = beforeLines[2];

    HeldMutationDiagnosticContext context(manager);
    g_HeldMutationDiagnosticContext = &context;
    HostedIrqManager::withRegistryMutationEpochForTest(
        registerWhileMutationEpochHeld);
    g_HeldMutationDiagnosticContext = nullptr;

    IrqLineDiagnosticSnapshot missedLines[3] = {};
    const bool missedSnapshotted =
        manager->snapshotIrqLines(missedLines, 3) == 3;
    const IrqLineDiagnosticSnapshot missed = missedLines[2];

    HostedPolicyDiagnosticHandler second;
    const irq_id_t secondId = context.id ?
                                  manager->registerHardIsaIrqHandler(
                                      2, &second, IrqPolicy::edgeHard()) :
                                  0;
    IrqLineDiagnosticSnapshot repairedLines[3] = {};
    const bool repairedSnapshotted =
        manager->snapshotIrqLines(repairedLines, 3) == 3;
    const IrqLineDiagnosticSnapshot repaired = repairedLines[2];

    const bool secondRemoved =
        secondId && manager->unregisterHandler(secondId, &second);
    IrqLineDiagnosticSnapshot consumedLines[3] = {};
    const bool consumedSnapshotted =
        manager->snapshotIrqLines(consumedLines, 3) == 3;
    const IrqLineDiagnosticSnapshot consumed = consumedLines[2];
    const bool firstRemoved =
        context.id && manager->unregisterHandler(context.id, &context.handler);

    bool passed = true;
    passed &= check(
        beforeSnapshotted && !before.configured && before.handlerCount == 0,
        "the missed-publication test did not begin with an empty line",
        MissedTest);
    passed &= check(
        context.id && missedSnapshotted &&
            missed.snapshotGeneration == before.snapshotGeneration &&
            !missed.configured && missed.handlerCount == 0,
        "an incoherent registry read replaced the older complete bank",
        MissedTest);
    passed &= check(
        missed.diagnosticPublicationFailures >
            before.diagnosticPublicationFailures,
        "bounded registry snapshot failures were not recorded", MissedTest);
    passed &= check(
        secondId && repairedSnapshotted && repaired.configured &&
            repaired.handlerCount == 2 &&
            repaired.delivery == IrqDelivery::Hard &&
            repaired.trigger == IrqTrigger::Edge &&
            repaired.snapshotGeneration > missed.snapshotGeneration,
        "a later lifecycle publication did not repair the stale bank",
        MissedTest);
    passed &= check(
        secondRemoved && consumedSnapshotted && consumed.configured &&
            consumed.handlerCount == 1 &&
            consumed.snapshotGeneration == repaired.snapshotGeneration + 1,
        "the repaired publication did not consume the retained dirty state",
        MissedTest);
    passed &= check(
        firstRemoved, "the missed-publication handlers did not clean up",
        MissedTest);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "irq-hosted-diagnostic-missed-registry-snapshot");
    }
    return passed;
}

bool hostedOldWorkLifetimeIsolation()
{
    constexpr const char *LifetimeTest =
        "irq-hosted-old-work-lifetime-isolation";
    IrqManager *manager = Machine::instance().getIrqManager();
    HostedLifetimeHandler oldHandler;
    HostedLifetimeHandler replacement;
    const irq_id_t oldId = manager->registerIsaIrqHandler(
        2, &oldHandler, IrqPolicy::syntheticThreaded());

    HostedLineOwnershipHookContext hooks;
    g_HostedLineOwnershipHookContext = &hooks;
    HostedIrqManager::setLineOwnershipHook(observeHostedLineOwnership);

    hooks.holdWorker = 1;
    const bool raised = oldId && raise(SIGURG) == 0;
    const bool workerHeld =
        raised && hooks.workerEntered.acquireForCompletion(1, 2, 0);
    IrqLineDiagnosticSnapshot activeLines[3] = {};
    const bool activeSnapshotted =
        manager->snapshotIrqLines(activeLines, 3) == 3;
    const IrqLineDiagnosticSnapshot active = activeLines[2];
    const size_t oldCookie = active.activeCookie;

    HostedLineUnregisterContext unregisterContext(manager, oldId, &oldHandler);
    Thread *unregisterer = nullptr;
    bool started = false;
    if (workerHeld)
    {
        hooks.holdStateCheck = 1;
        unregisterer = new Thread(
            Scheduler::instance().getKernelProcess(),
            unregisterHostedLineLifetime, &unregisterContext, nullptr, false,
            true, true);
        unregisterer->setName("hosted old IRQ-lifetime unregister");
        started = unregisterer->start();
    }
    const bool stateCheckHeld =
        started && hooks.stateCheckEntered.acquireForCompletion(1, 2, 0);

    // Without manager-level lifetime ownership, this registration can publish
    // a new handler before the old removal checks whether the line is empty.
    // The old queued cookie would then be accepted by the replacement.
    const irq_id_t racedReplacementId =
        stateCheckHeld ? manager->registerIsaIrqHandler(
                             2, &replacement, IrqPolicy::syntheticThreaded()) :
                         0;
    const bool admissionRejected =
        !racedReplacementId && stateCheckHeld &&
        hooks.admissionRejected.acquireForCompletion(1, 2, 0);

    hooks.releaseStateCheck.release();
    const bool joined = started && unregisterer->joinForCompletion();
    const irq_id_t replacementId =
        racedReplacementId ?
            racedReplacementId :
            (joined ? manager->registerIsaIrqHandler(
                          2, &replacement, IrqPolicy::syntheticThreaded()) :
                      0);

    IrqLineDiagnosticSnapshot replacementLines[3] = {};
    const bool replacementSnapshotted =
        manager->snapshotIrqLines(replacementLines, 3) == 3;
    const IrqLineDiagnosticSnapshot replacementLine = replacementLines[2];

    hooks.releaseWorker.release();
    IrqLineDiagnosticSnapshot completed = {};
    const bool oldBatchCompleted =
        oldCookie &&
        waitForHostedCookieCompletion(manager, oldCookie, completed);

    HostedIrqManager::setLineOwnershipHook(nullptr);
    g_HostedLineOwnershipHookContext = nullptr;

    const bool replacementRemoved =
        replacementId &&
        manager->unregisterHandler(replacementId, &replacement);
    const bool oldRemoved =
        unregisterContext.removed ||
        (oldId && manager->unregisterHandler(oldId, &oldHandler));

    bool passed = true;
    passed &= check(
        oldId && raised && workerHeld && activeSnapshotted &&
            active.configured && active.handlerCount == 1 &&
            active.delivery == IrqDelivery::Threaded && oldCookie &&
            active.publicationCookie == oldCookie && active.dispatcherActive,
        "the old lifetime's worker batch was not held before validation",
        LifetimeTest);
    passed &= check(
        stateCheckHeld && admissionRejected && !racedReplacementId &&
            hooks.rejectedAdmissions == 1 && hooks.hookFailures == 0,
        "a replacement entered before final line state was established",
        LifetimeTest);
    passed &= check(
        joined && unregisterContext.removed && replacementId &&
            replacementSnapshotted && replacementLine.configured &&
            replacementLine.handlerCount == 1 &&
            replacementLine.delivery == IrqDelivery::Threaded &&
            replacementLine.activeCookie == oldCookie &&
            replacementLine.publicationCookie != oldCookie,
        "the replacement did not receive a distinct diagnostic lifetime",
        LifetimeTest);
    passed &= check(
        oldBatchCompleted && completed.completedCookie == oldCookie &&
            completed.publicationCookie == replacementLine.publicationCookie &&
            replacement.calls == 0 && oldHandler.calls == 0,
        "queued work from the retired lifetime reached a replacement handler",
        LifetimeTest);
    passed &= check(
        replacementRemoved && oldRemoved,
        "the old-work lifetime handlers did not clean up", LifetimeTest);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "irq-hosted-old-work-lifetime-isolation");
    }
    return passed;
}

bool hostedNewWorkLifetimeIsolation()
{
    constexpr const char *LifetimeTest =
        "irq-hosted-new-work-lifetime-isolation";
    IrqManager *manager = Machine::instance().getIrqManager();
    HostedLifetimeHandler oldHandler;
    HostedLifetimeHandler replacement;
    const irq_id_t oldId = manager->registerIsaIrqHandler(
        2, &oldHandler, IrqPolicy::syntheticThreaded());
    IrqLineDiagnosticSnapshot registeredLines[3] = {};
    const bool registeredSnapshotted =
        manager->snapshotIrqLines(registeredLines, 3) == 3;
    const IrqLineDiagnosticSnapshot registered = registeredLines[2];

    HostedLineOwnershipHookContext hooks;
    g_HostedLineOwnershipHookContext = &hooks;
    HostedIrqManager::setLineOwnershipHook(observeHostedLineOwnership);
    hooks.holdCookieAdvance = 1;

    HostedLineUnregisterContext unregisterContext(manager, oldId, &oldHandler);
    Thread *unregisterer = new Thread(
        Scheduler::instance().getKernelProcess(), unregisterHostedLineLifetime,
        &unregisterContext, nullptr, false, true, true);
    unregisterer->setName("hosted new IRQ-lifetime unregister");
    const bool started = oldId && unregisterer->start();
    const bool cookieAdvanceHeld =
        started && hooks.cookieAdvanceEntered.acquireForCompletion(1, 2, 0);

    // This is the inverse ownership window: without serialisation, a new
    // handler can publish work after the old removal decided the line was empty
    // but before it advances the cookie, allowing the old removal to invalidate
    // work which belongs to the replacement.
    const irq_id_t racedReplacementId =
        cookieAdvanceHeld ?
            manager->registerIsaIrqHandler(
                2, &replacement, IrqPolicy::syntheticThreaded()) :
            0;
    const bool admissionRejected =
        !racedReplacementId && cookieAdvanceHeld &&
        hooks.admissionRejected.acquireForCompletion(1, 2, 0);

    hooks.releaseCookieAdvance.release();
    const bool joined = started && unregisterer->joinForCompletion();
    IrqLineDiagnosticSnapshot closedLines[3] = {};
    const bool closedSnapshotted =
        manager->snapshotIrqLines(closedLines, 3) == 3;
    const IrqLineDiagnosticSnapshot closed = closedLines[2];

    const irq_id_t replacementId =
        racedReplacementId ?
            racedReplacementId :
            (joined ? manager->registerIsaIrqHandler(
                          2, &replacement, IrqPolicy::syntheticThreaded()) :
                      0);
    IrqLineDiagnosticSnapshot reopenedLines[3] = {};
    const bool reopenedSnapshotted =
        manager->snapshotIrqLines(reopenedLines, 3) == 3;
    const IrqLineDiagnosticSnapshot reopened = reopenedLines[2];

    const bool irqEnabled = Processor::getInterrupts();
    const bool raised = replacementId && irqEnabled && raise(SIGURG) == 0;
    IrqLineDiagnosticSnapshot publishedLines[3] = {};
    const bool publishedSnapshotted =
        manager->snapshotIrqLines(publishedLines, 3) == 3;
    const IrqLineDiagnosticSnapshot published = publishedLines[2];
    const bool replacementEntered =
        raised && replacement.entered.acquireForCompletion(1, 2, 0);
    IrqLineDiagnosticSnapshot completed = {};
    const bool replacementCompleted =
        published.publicationCookie &&
        waitForHostedCookieCompletion(
            manager, published.publicationCookie, completed);

    HostedIrqManager::setLineOwnershipHook(nullptr);
    g_HostedLineOwnershipHookContext = nullptr;

    const bool replacementRemoved =
        replacementId &&
        manager->unregisterHandler(replacementId, &replacement);
    const bool oldRemoved =
        unregisterContext.removed ||
        (oldId && manager->unregisterHandler(oldId, &oldHandler));

    bool passed = true;
    passed &= check(
        oldId && registeredSnapshotted && registered.configured &&
            registered.handlerCount == 1 &&
            registered.delivery == IrqDelivery::Threaded,
        "the new-work test did not establish its original lifetime",
        LifetimeTest);
    passed &= check(
        cookieAdvanceHeld && admissionRejected && !racedReplacementId &&
            hooks.rejectedAdmissions == 1 && hooks.hookFailures == 0,
        "a replacement entered after the old lifetime chose its final cookie",
        LifetimeTest);
    passed &= check(
        joined && unregisterContext.removed && closedSnapshotted &&
            !closed.configured && closed.handlerCount == 0 &&
            closed.delivery == IrqDelivery::None &&
            closed.publicationCookie != registered.publicationCookie &&
            closed.snapshotGeneration > registered.snapshotGeneration,
        "final removal did not publish one closed lifetime boundary",
        LifetimeTest);
    passed &= check(
        replacementId && reopenedSnapshotted && reopened.configured &&
            reopened.handlerCount == 1 &&
            reopened.delivery == IrqDelivery::Threaded &&
            reopened.publicationCookie == closed.publicationCookie &&
            reopened.snapshotGeneration > closed.snapshotGeneration,
        "the replacement did not open beyond the closed lifetime boundary",
        LifetimeTest);
    passed &= check(
        raised && publishedSnapshotted && published.configured &&
            published.snapshotGeneration == reopened.snapshotGeneration &&
            published.publicationCookie != reopened.publicationCookie &&
            published.pendingCookie == published.publicationCookie,
        "new work was not published in the replacement lifetime", LifetimeTest);
    passed &= check(
        replacementEntered && replacementCompleted && replacement.calls == 1 &&
            completed.completedCookie == published.publicationCookie &&
            completed.publicationCookie == published.publicationCookie,
        "the old unregister invalidated work from the replacement lifetime",
        LifetimeTest);
    passed &= check(
        replacementRemoved && oldRemoved,
        "the new-work lifetime handlers did not clean up", LifetimeTest);
    if (passed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS "
            "irq-hosted-new-work-lifetime-isolation");
    }
    return passed;
}

bool picThreadedTriggerPolicy()
{
    constexpr const char *PolicyTest = "pic-threaded-trigger-policy";
    PicIrqState level;
    level.setAllEnabled(false);
    level.handlerRegistered(10, IrqPolicy::levelThreaded());

    const size_t handledGeneration = level.beginDispatch(10);
    level.beginThreadedDispatch(10);
    bool passed = check(
        !level.enabled(10) && level.threadedPending(10),
        "a level line was not masked before its bottom half", PolicyTest);
    passed &= check(
        level.completeThreadedDispatch(10, handledGeneration, true) &&
            level.enabled(10) && !level.threadedPending(10),
        "a handled level batch did not rearm its line", PolicyTest);

    const size_t unhandledGeneration = level.beginDispatch(10);
    level.beginThreadedDispatch(10);
    passed &= check(
        level.completeThreadedDispatch(10, unhandledGeneration, false) &&
            !level.enabled(10) && level.threadedPending(10),
        "an unhandled level batch reopened an interrupt storm", PolicyTest);
    level.handlerUnregistered(10);
    level.completeThreadedDispatch(10, unhandledGeneration, true);
    passed &= check(
        level.handlerCount(10) == 0 && !level.enabled(10) &&
            !level.threadedPending(10),
        "final unregister or stale completion reopened the line", PolicyTest);

    level.handlerRegistered(10, IrqPolicy::levelThreaded());
    const size_t administrativelyDisabled = level.beginDispatch(10);
    level.beginThreadedDispatch(10);
    level.setEnabled(10, false);
    passed &= check(
        level.completeThreadedDispatch(10, administrativelyDisabled, true) &&
            !level.enabled(10) && !level.threadedPending(10),
        "bottom-half completion overrode an administrative mask", PolicyTest);
    level.setEnabled(10, true);

    const size_t administrativelyReenabled = level.beginDispatch(10);
    level.beginThreadedDispatch(10);
    level.setEnabled(10, false);
    level.setEnabled(10, true);
    passed &= check(
        !level.enabled(10) && level.threadedPending(10),
        "administrative enable overrode an active bottom-half mask",
        PolicyTest);
    passed &= check(
        level.completeThreadedDispatch(10, administrativelyReenabled, true) &&
            level.enabled(10),
        "a completed re-enabled line remained masked", PolicyTest);

    const size_t staleGeneration = level.beginDispatch(10);
    level.beginThreadedDispatch(10);
    const size_t currentGeneration = level.beginDispatch(10);
    level.beginThreadedDispatch(10);
    passed &= check(
        !level.completeThreadedDispatch(10, staleGeneration, true) &&
            !level.enabled(10) && level.threadedPending(10),
        "a stale bottom half rearmed a newer level occurrence", PolicyTest);
    passed &= check(
        level.completeThreadedDispatch(10, currentGeneration, true) &&
            level.enabled(10) && !level.threadedPending(10),
        "the newest level occurrence could not rearm", PolicyTest);

    PicIrqState edge;
    edge.setAllEnabled(false);
    edge.handlerRegistered(5, IrqPolicy::edgeThreaded());
    const uint16_t enabledEdgeMask = edge.mask();
    const size_t edgeGeneration = edge.beginDispatch(5);
    edge.beginThreadedDispatch(5);
    passed &= check(
        edge.enabled(5) && !edge.threadedPending(5),
        "an edge line was masked while its worker ran", PolicyTest);
    passed &= check(
        edge.completeThreadedDispatch(5, edgeGeneration, false) &&
            edge.enabled(5) && edge.mask() == enabledEdgeMask,
        "an edge disposition incorrectly controlled line masking", PolicyTest);
    edge.setEnabled(5, false);
    const uint16_t disabledEdgeMask = edge.mask();
    const size_t disabledEdgeGeneration = edge.beginDispatch(5);
    edge.beginThreadedDispatch(5);
    passed &= check(
        edge.completeThreadedDispatch(5, disabledEdgeGeneration, true) &&
            !edge.enabled(5) && edge.mask() == disabledEdgeMask,
        "edge completion overrode an administrative mask", PolicyTest);

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS pic-threaded-trigger-policy");
    }
    return passed;
}
}  // namespace

bool runHostedThreadedIrqRegressions()
{
    bool passed = irqDiagnosticSnapshotPublication();
    passed &= hostedSyntheticIrqMasking();
    passed &= threadedDispatcherCoalescing();
    passed &= hostedThreadedSignalDelivery();
    passed &= hostedThreadedStallDiagnostics();
    passed &= hostedHardStageDiagnostics();
    passed &= hostedDeferredRetiringDiagnostics();
    passed &= hostedDiagnosticPolicyLifecycle();
    passed &= hostedDiagnosticMissedRegistrySnapshot();
    passed &= hostedOldWorkLifetimeIsolation();
    passed &= hostedNewWorkLifetimeIsolation();
    passed &= picThreadedTriggerPolicy();
    return passed;
}
