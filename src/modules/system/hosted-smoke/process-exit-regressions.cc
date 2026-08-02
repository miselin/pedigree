/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/utilities/ZombieQueue.h"

namespace
{
bool schedulerContains(Process *process)
{
    const size_t count = Scheduler::instance().getNumProcesses();
    for (size_t i = 0; i < count; ++i)
    {
        Scheduler::ProcessLease candidate;
        if (
            Scheduler::instance().acquireProcess(candidate, i) &&
            candidate.get() == process)
        {
            return true;
        }
    }
    return false;
}

struct PublicationProbe
{
    explicit PublicationProbe(Process *owner)
        : visibleDuringMemberConstruction(schedulerContains(owner))
    {
    }

    bool visibleDuringMemberConstruction;
};

class DeferredHostedProcess : public Process
{
  public:
    explicit DeferredHostedProcess(Process *parent)
        : Process(DeferredPublication(), parent), probe(this)
    {
    }

    ~DeferredHostedProcess() override
    {
        prepareForDestruction();
    }

    void finishAssembly()
    {
        publish();
    }

    PublicationProbe probe;
};

struct ProcessExitContext
{
    explicit ProcessExitContext(Mutex *mutex)
        : mutex(mutex), peerEntered(0), peerAcquiredMutex(0),
          unstartedPeerEntered(0)
    {
    }

    Mutex *mutex;
    Atomic<size_t> peerEntered;
    Atomic<size_t> peerAcquiredMutex;
    Atomic<size_t> unstartedPeerEntered;
};

struct JoinReaperContext
{
    JoinReaperContext(Process *process, Thread *target)
        : process(process), target(target), releaseJoiner(0, false),
          hookCalls(0), hookFailures(0), joinReturned(0), joinSucceeded(0),
          reaperEntered(0), processDeleted(0)
    {
    }

    Process *process;
    Thread *target;
    Semaphore releaseJoiner;
    Atomic<size_t> hookCalls;
    Atomic<size_t> hookFailures;
    Atomic<size_t> joinReturned;
    Atomic<size_t> joinSucceeded;
    Atomic<size_t> reaperEntered;
    Atomic<size_t> processDeleted;
};

JoinReaperContext *g_JoinReaperContext = nullptr;

bool check(bool condition, const char *detail)
{
    if (condition)
    {
        return true;
    }

    ERROR(
        "HOSTED-WAIT-TEST: FAIL process-exit-rendezvous: " << detail);
    return false;
}

bool checkJoinLease(bool condition, const char *detail)
{
    if (condition)
    {
        return true;
    }

    ERROR(
        "HOSTED-WAIT-TEST: FAIL join-reaper-lease: " << detail);
    return false;
}

int blockedProcessPeer(void *parameter)
{
    ProcessExitContext *context =
        reinterpret_cast<ProcessExitContext *>(parameter);
    context->peerEntered += 1;
    if (context->mutex->acquire())
    {
        context->peerAcquiredMutex += 1;
        context->mutex->release();
    }
    return 0;
}

int unstartedProcessPeer(void *parameter)
{
    ProcessExitContext *context =
        reinterpret_cast<ProcessExitContext *>(parameter);
    context->unstartedPeerEntered += 1;
    return 0;
}

int terminateChildProcess(void *parameter)
{
    Process *process = reinterpret_cast<Process *>(parameter);
    process->kill();
}

int immediateThreadExit(void *)
{
    return 0;
}

void joinOperationHook(Thread *target, Process *parent)
{
    JoinReaperContext *context =
        __atomic_load_n(&g_JoinReaperContext, __ATOMIC_ACQUIRE);
    if (!context)
    {
        return;
    }

    context->hookCalls += 1;
    if (target != context->target || parent != context->process)
    {
        context->hookFailures += 1;
        return;
    }

    if (!context->releaseJoiner.acquireForCompletion())
    {
        context->hookFailures += 1;
    }
}

int joinRaceTarget(void *parameter)
{
    JoinReaperContext *context =
        reinterpret_cast<JoinReaperContext *>(parameter);
    const bool joined = context->target->join();
    context->joinSucceeded = joined ? 1 : 0;
    context->joinReturned += 1;
    return 0;
}

int reapJoinProcess(void *parameter)
{
    JoinReaperContext *context =
        reinterpret_cast<JoinReaperContext *>(parameter);
    context->reaperEntered += 1;
    delete context->process;
    context->processDeleted += 1;
    return 0;
}

bool joinReaperLease(Process *kernelProcess)
{
    constexpr size_t Attempts = 10000;
    Process *process = new Process(kernelProcess);

    Thread *target = new Thread(
        process, immediateThreadExit, nullptr, nullptr, false, true, true);
    target->setName("hosted join/reaper target");

    Thread *retained = new Thread(
        process, immediateThreadExit, nullptr, nullptr, false, true, true);
    retained->setName("hosted join/reaper retained target");

    const bool targetStarted = target->start();
    const bool retainedStarted = retained->start();
    const bool targetsStarted = targetStarted && retainedStarted;

    bool retainedReapable = false;
    for (size_t attempt = 0; targetsStarted && attempt < Attempts; ++attempt)
    {
        if (retained->isReapableForHostedTest())
        {
            retainedReapable = true;
            break;
        }
        Scheduler::instance().yield();
    }

    bool passed = checkJoinLease(
        targetsStarted && retainedReapable,
        "the retained admission target did not become reapable");
    if (targetsStarted && retainedReapable)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS scheduler-same-priority-progress");
    }
    if (!retainedReapable)
    {
        return false;
    }

    JoinReaperContext context(process, target);
    Thread *joiner = new Thread(
        kernelProcess, joinRaceTarget, &context, nullptr, false, true, true);
    joiner->setName("hosted join/reaper joiner");

    __atomic_store_n(
        &g_JoinReaperContext, &context, __ATOMIC_RELEASE);
    Thread::setJoinOperationHook(joinOperationHook);

    passed &= checkJoinLease(
        joiner->start(), "the delayed joiner did not start");

    bool joinerPinned = false;
    for (size_t attempt = 0; attempt < Attempts; ++attempt)
    {
        if (context.hookCalls)
        {
            joinerPinned = true;
            break;
        }
        Scheduler::instance().yield();
    }
    passed &= checkJoinLease(
        joinerPinned && context.hookFailures == 0,
        "the joiner did not reach the leased pre-delete window");

    Thread::setJoinOperationHook(nullptr);
    __atomic_store_n(
        &g_JoinReaperContext, static_cast<JoinReaperContext *>(nullptr),
        __ATOMIC_RELEASE);

    if (!joinerPinned)
    {
        context.releaseJoiner.release();
        passed &= checkJoinLease(
            joiner->join(), "the failed-window joiner did not retire");
        passed &= checkJoinLease(
            retained->join(), "the retained target did not retire");
        delete process;
        return false;
    }

    Thread *reaper = new Thread(
        kernelProcess, reapJoinProcess, &context, nullptr, false, true, true);
    reaper->setName("hosted join/reaper process reaper");
    passed &= checkJoinLease(
        reaper->start(), "the delayed process reaper did not start");

    bool reaperBlocked = false;
    for (size_t attempt = 0; attempt < Attempts; ++attempt)
    {
        Thread::WaitDebugInfo info = {};
        if (
            context.reaperEntered && reaper->getWaitDebugInfo(info) &&
            info.queue && info.queued &&
            info.channelOwner == process &&
            reaper->getStatus() == Thread::Sleeping)
        {
            reaperBlocked = true;
            break;
        }
        Scheduler::instance().yield();
    }

    passed &= checkJoinLease(
        reaperBlocked && context.processDeleted == 0,
        "Process destruction did not wait for the active join lease");

    const bool lateJoin = retained->join();
    passed &= checkJoinLease(
        !lateJoin,
        "Process destruction admitted a new join after closing");

    context.releaseJoiner.release();

    bool completed = false;
    for (size_t attempt = 0; attempt < Attempts; ++attempt)
    {
        if (context.joinReturned && context.processDeleted)
        {
            completed = true;
            break;
        }
        Scheduler::instance().yield();
    }
    passed &= checkJoinLease(
        completed && context.joinSucceeded == 1,
        "the join lease did not release into orderly Process destruction");
    passed &= checkJoinLease(
        joiner->join(), "the joiner thread did not retire");
    passed &= checkJoinLease(
        reaper->join(), "the process reaper thread did not retire");

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS join-reaper-lease");
    }
    return passed;
}

class ExitElectionProcess : public Process
{
  public:
    explicit ExitElectionProcess(Process *parent)
        : Process(DeferredPublication(), parent), competitor(nullptr),
          cleanupCalls(0),
          cleanupBeforePeerReapable(0)
    {
        publish();
    }

    ~ExitElectionProcess() override
    {
        prepareForDestruction();
    }

    Thread *competitor;
    Atomic<size_t> cleanupCalls;
    Atomic<size_t> cleanupBeforePeerReapable;

  private:
    void processTerminated() override
    {
        if (competitor && !competitor->isReapableForHostedTest())
        {
            cleanupBeforePeerReapable += 1;
        }
        cleanupCalls += 1;
    }
};

struct ExitElectionContext
{
    explicit ExitElectionContext(ExitElectionProcess *process)
        : process(process), releaseCompetitor(0, false), hookCalls(0),
          hookFailures(0), ownerElected(0), competitorEntered(0),
          competitorElected(0), resumeSawTerminating(0), owner(nullptr)
    {
    }

    ExitElectionProcess *process;
    Semaphore releaseCompetitor;
    Atomic<size_t> hookCalls;
    Atomic<size_t> hookFailures;
    Atomic<size_t> ownerElected;
    Atomic<size_t> competitorEntered;
    Atomic<size_t> competitorElected;
    Atomic<size_t> resumeSawTerminating;
    Thread *owner;
};

ExitElectionContext *g_ExitElectionContext = nullptr;

void exitElectionHook(Process *process, Thread *)
{
    ExitElectionContext *context =
        __atomic_load_n(&g_ExitElectionContext, __ATOMIC_ACQUIRE);
    if (!context || process != context->process)
    {
        return;
    }

    context->hookCalls += 1;
}

int competingProcessExit(void *parameter)
{
    ExitElectionContext *context =
        reinterpret_cast<ExitElectionContext *>(parameter);
    if (!context->releaseCompetitor.acquireForCompletion())
    {
        context->hookFailures += 1;
    }

    // The owner has published Terminating before the hook opens this gate.
    // A concurrent resume must not restore Active and reopen the process.
    context->process->resume();
    context->resumeSawTerminating =
        context->process->getState() == Process::Terminating ? 1 : 0;

    const bool elected = context->process->beginTermination();
    context->competitorElected = elected ? 1 : 0;
    context->competitorEntered += 1;
    context->process->competitor->getScheduler()->killCurrentThread();
}

int owningProcessExit(void *parameter)
{
    ExitElectionContext *context =
        reinterpret_cast<ExitElectionContext *>(parameter);
    const bool elected = context->process->beginTermination();
    context->ownerElected = elected ? 1 : 0;
    if (!elected || !context->process->quiesceTermination())
    {
        context->hookFailures += 1;
        context->owner->getScheduler()->killCurrentThread();
    }
    context->process->finishTermination();
}

bool exitElectionQuiescence(Process *kernelProcess)
{
    ExitElectionProcess *process =
        new ExitElectionProcess(kernelProcess);
    ExitElectionContext context(process);
    Thread *competitor = new Thread(
        process, competingProcessExit, &context, nullptr, false, true, true);
    competitor->setName("hosted process-exit competitor");
    process->competitor = competitor;
    Thread *owner = new Thread(
        process, owningProcessExit, &context, nullptr, false, true, true);
    owner->setName("hosted process-exit owner");
    context.owner = owner;

    bool passed = check(
        competitor->start(), "the competing exit thread did not start");

    constexpr size_t Attempts = 10000;
    bool competitorBlocked = false;
    for (size_t attempt = 0; attempt < Attempts; ++attempt)
    {
        Thread::WaitDebugInfo info = {};
        if (
            competitor->getWaitDebugInfo(info) && info.queue && info.queued &&
            competitor->getStatus() == Thread::Sleeping)
        {
            competitorBlocked = true;
            break;
        }
        Scheduler::instance().yield();
    }
    passed &= check(
        competitorBlocked,
        "the competing exit thread did not publish its election gate");

    __atomic_store_n(
        &g_ExitElectionContext, &context, __ATOMIC_RELEASE);
    Process::setTerminationElectionHook(exitElectionHook);
    passed &= check(owner->start(), "the exit owner did not start");

    bool ownerBlocked = false;
    for (size_t attempt = 0; attempt < Attempts; ++attempt)
    {
        Thread::WaitDebugInfo info = {};
        if (
            context.hookCalls == 1 &&
            owner->getWaitDebugInfo(info) && info.queue && info.queued &&
            owner->getStatus() == Thread::Sleeping)
        {
            ownerBlocked = true;
            break;
        }
        Scheduler::instance().yield();
    }
    passed &= check(
        ownerBlocked,
        "the elected exit owner did not publish its peer rendezvous");

    context.releaseCompetitor.release();
    const bool reapable = process->waitUntilTerminationReapable();
    Process::setTerminationElectionHook(nullptr);
    __atomic_store_n(
        &g_ExitElectionContext, static_cast<ExitElectionContext *>(nullptr),
        __ATOMIC_RELEASE);

    passed &= check(
        reapable && process->getState() == Process::Terminated,
        "the competing exit process never became reapable");
    passed &= check(
        context.hookCalls == 1 && context.hookFailures == 0,
        "the deterministic election hook did not complete cleanly");
    passed &= check(
        context.ownerElected == 1 && context.competitorEntered == 1 &&
            context.competitorElected == 0,
        "more than one exiting thread won the process election");
    passed &= check(
        context.resumeSawTerminating == 1,
        "a concurrent resume downgraded a terminating process");
    passed &= check(
        process->cleanupCalls == 1 &&
            process->cleanupBeforePeerReapable == 0,
        "shared process cleanup was repeated or ran before peer quiescence");
    passed &= check(
        owner->getStatus() == Thread::AwaitingJoin &&
            competitor->getStatus() == Thread::AwaitingJoin,
        "process cleanup completed before every peer was off-stack");

    delete process;
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS process-exit-election");
        NOTICE("HOSTED-WAIT-TEST: PASS process-resume-vs-termination");
    }
    return passed;
}

struct OrphanExitContext
{
    OrphanExitContext()
        : process(nullptr), preparingCalls(0), publishedCalls(0),
          workerEntered(0), workerEnteredBeforeOwnerExit(0), reapableCalls(0),
          destructorCalls(0), cleanupCalls(0), hookFailures(0),
          ownerInPublication(0), duplicateClaimsRejected(0)
    {
    }

    Process *process;
    Atomic<size_t> preparingCalls;
    Atomic<size_t> publishedCalls;
    Atomic<size_t> workerEntered;
    Atomic<size_t> workerEnteredBeforeOwnerExit;
    Atomic<size_t> reapableCalls;
    Atomic<size_t> destructorCalls;
    Atomic<size_t> cleanupCalls;
    Atomic<size_t> hookFailures;
    Atomic<size_t> ownerInPublication;
    Atomic<size_t> duplicateClaimsRejected;
};

OrphanExitContext *g_OrphanExitContext = nullptr;

class OrphanExitProcess : public Process
{
  public:
    OrphanExitProcess(Process *parent, OrphanExitContext *context)
        : Process(DeferredPublication(), parent), m_Context(context)
    {
        makeOrphanBeforePublicationForHostedTest();
        publish();
    }

    ~OrphanExitProcess() override
    {
        prepareForDestruction();
        if (
            static_cast<size_t>(m_Context->reapableCalls) != 1 ||
            getState() != Process::Terminated)
        {
            m_Context->hookFailures += 1;
        }
        m_Context->destructorCalls += 1;
    }

  private:
    void processTerminated() override
    {
        m_Context->cleanupCalls += 1;
    }

    OrphanExitContext *m_Context;
};

void orphanPublicationHook(
    Process *process, Process::OrphanPublicationPhase phase,
    bool interruptsEnabled, bool processLockHeld)
{
    OrphanExitContext *context =
        __atomic_load_n(&g_OrphanExitContext, __ATOMIC_ACQUIRE);
    if (!context || context->process != process)
    {
        return;
    }

    if (!interruptsEnabled || processLockHeld)
    {
        context->hookFailures += 1;
    }

    if (phase == Process::OrphanPublicationPhase::Preparing)
    {
        context->preparingCalls += 1;
        context->ownerInPublication = 1;
        Process::ReaperClaim duplicate = process->tryClaimReaper();
        if (duplicate)
        {
            context->hookFailures += 1;
            duplicate.publish();
        }
        else
        {
            context->duplicateClaimsRejected += 1;
        }
        return;
    }

    context->publishedCalls += 1;
    constexpr size_t Attempts = 10000;
    for (size_t attempt = 0; attempt < Attempts; ++attempt)
    {
        if (context->workerEntered)
        {
            break;
        }
        Scheduler::instance().yield();
    }
    if (!context->workerEntered)
    {
        context->hookFailures += 1;
    }
    context->ownerInPublication = 0;
}

void orphanReapHook(Process *process, ZombieProcess::ReapPhase phase)
{
    OrphanExitContext *context =
        __atomic_load_n(&g_OrphanExitContext, __ATOMIC_ACQUIRE);
    if (!context || context->process != process)
    {
        return;
    }

    if (phase == ZombieProcess::ReapPhase::Entered)
    {
        context->workerEntered += 1;
        if (context->ownerInPublication)
        {
            context->workerEnteredBeforeOwnerExit += 1;
        }
        else
        {
            context->hookFailures += 1;
        }
        return;
    }

    context->reapableCalls += 1;
    if (process->getState() != Process::Terminated)
    {
        context->hookFailures += 1;
    }
}

bool orphanPublicationInterleaving(Process *kernelProcess)
{
    OrphanExitContext *context = new OrphanExitContext;
    OrphanExitProcess *process =
        new OrphanExitProcess(kernelProcess, context);
    context->process = process;

    __atomic_store_n(&g_OrphanExitContext, context, __ATOMIC_RELEASE);
    Process::setOrphanPublicationHook(orphanPublicationHook);
    ZombieProcess::setReapHook(orphanReapHook);

    Thread *owner = new Thread(
        process, terminateChildProcess, process, nullptr, false, true, true);
    owner->setName("hosted orphan-exit owner");
    if (!owner->start())
    {
        Process::setOrphanPublicationHook(nullptr);
        ZombieProcess::setReapHook(nullptr);
        __atomic_store_n(
            &g_OrphanExitContext, static_cast<OrphanExitContext *>(nullptr),
            __ATOMIC_RELEASE);
        delete process;
        delete context;
        return check(false, "the orphan-exit owner did not start");
    }

    constexpr size_t Attempts = 20000;
    for (size_t attempt = 0; attempt < Attempts; ++attempt)
    {
        if (context->destructorCalls)
        {
            break;
        }
        Scheduler::instance().yield();
    }

    const bool destroyed = context->destructorCalls == 1;
    const bool drained = destroyed && ZombieQueue::instance().drain();
    Process::setOrphanPublicationHook(nullptr);
    ZombieProcess::setReapHook(nullptr);
    __atomic_store_n(
        &g_OrphanExitContext, static_cast<OrphanExitContext *>(nullptr),
        __ATOMIC_RELEASE);

    if (!destroyed || !drained)
    {
        // A late Process destructor still owns this diagnostic storage.
        return check(false, "orphan destruction did not complete and drain");
    }

    bool passed = true;
    passed &= check(
        context->preparingCalls == 1 && context->publishedCalls == 1,
        "orphan publication did not cross both unlocked checkpoints");
    passed &= check(
        context->workerEntered == 1 &&
            context->workerEnteredBeforeOwnerExit == 1,
        "the ZombieQueue worker did not enter before owner stack retirement");
    passed &= check(
        context->reapableCalls == 1 && context->destructorCalls == 1 &&
            context->cleanupCalls == 1 &&
            context->duplicateClaimsRejected == 1,
        "orphan destruction was not exactly once and post-reapable");
    passed &= check(
        context->hookFailures == 0,
        "orphan publication retained its Process lock or disabled interrupts");
    delete context;

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS process-orphan-publication-handoff");
    }
    return passed;
}

struct ZombieBacklogContext
{
    ZombieBacklogContext()
        : release(0, false), entered(0), destroyed(0), failures(0)
    {
    }

    Semaphore release;
    Atomic<size_t> entered;
    Atomic<size_t> destroyed;
    Atomic<size_t> failures;
};

class HostedBacklogZombie : public ZombieObject
{
  public:
    explicit HostedBacklogZombie(ZombieBacklogContext *context)
        : m_Context(context)
    {
    }

    ~HostedBacklogZombie() override
    {
        m_Context->entered += 1;
        if (!m_Context->release.acquireForCompletion())
        {
            m_Context->failures += 1;
        }
        m_Context->destroyed += 1;
    }

  private:
    ZombieBacklogContext *m_Context;
};

bool mandatoryZombieBacklog()
{
    constexpr size_t Backlog = 300;
    ZombieBacklogContext context;
    for (size_t i = 0; i < Backlog; ++i)
    {
        ZombieQueue::instance().addObject(new HostedBacklogZombie(&context));
    }

    bool passed = check(
        context.destroyed == 0,
        "mandatory ZombieQueue work executed through its closed test gate");
    context.release.release(Backlog);
    passed &= check(
        ZombieQueue::instance().drain(),
        "mandatory ZombieQueue backlog did not drain");
    passed &= check(
        context.entered == Backlog && context.destroyed == Backlog &&
            context.failures == 0,
        "mandatory ZombieQueue work above the legacy limit was lost");
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS zombiequeue-mandatory-backlog");
    }
    return passed;
}
}  // namespace

bool runHostedProcessExitRegressions()
{
    Process *kernelProcess =
        Scheduler::instance().getKernelProcess();
    Mutex mutex;
    ProcessExitContext context(&mutex);
    bool passed = true;

    if (!joinReaperLease(kernelProcess))
    {
        return false;
    }
    passed &= mandatoryZombieBacklog();
    passed &= orphanPublicationInterleaving(kernelProcess);
    passed &= exitElectionQuiescence(kernelProcess);

    DeferredHostedProcess *publishedChild =
        new DeferredHostedProcess(kernelProcess);
    passed &= check(
        !publishedChild->probe.visibleDuringMemberConstruction,
        "a derived Process was visible while its members were constructing");
    passed &= check(
        !schedulerContains(publishedChild),
        "an incomplete derived Process was visible after its constructor");
    publishedChild->finishAssembly();
    passed &= check(
        schedulerContains(publishedChild),
        "a complete derived Process was not published");
    delete publishedChild;

    const size_t processCountBeforeAbandon =
        Scheduler::instance().getNumProcesses();
    DeferredHostedProcess *abandonedChild =
        new DeferredHostedProcess(kernelProcess);
    delete abandonedChild;
    passed &= check(
        Scheduler::instance().getNumProcesses() ==
            processCountBeforeAbandon,
        "destroying an unpublished Process changed scheduler enumeration");

    Process *terminatingParent = new Process(kernelProcess);
    terminatingParent->markTerminating();
    DeferredHostedProcess *freshChild =
        new DeferredHostedProcess(terminatingParent);
    freshChild->finishAssembly();
    passed &= check(
        freshChild->getState() == Process::Active,
        "a new child inherited its parent's terminal state");
    passed &= check(
        freshChild->getParent() != terminatingParent,
        "a terminating parent retained a newly published child");
    delete freshChild;
    delete terminatingParent;

    passed &= check(
        mutex.acquire(), "the supervisor could not hold the peer mutex");

    Process *child = new Process(kernelProcess);
    Process *orphan = new Process(child);
    Thread *peer = new Thread(
        child, blockedProcessPeer, &context, nullptr, false, true, true);
    peer->setName("hosted process-exit blocked peer");
    Thread *unstartedPeer = new Thread(
        child, unstartedProcessPeer, &context, nullptr, false, true, true);
    unstartedPeer->setName("hosted process-exit unstarted peer");
    Thread *terminator = new Thread(
        child, terminateChildProcess, child, nullptr, false, true, true);
    terminator->setName("hosted process-exit terminator");

    passed &= check(
        peer->start(), "the delayed blocked peer did not start");

    bool peerEnrolled = false;
    constexpr size_t EnrolmentAttempts = 10000;
    for (size_t attempt = 0; attempt < EnrolmentAttempts; ++attempt)
    {
        Thread::WaitDebugInfo info = {};
        if (
            peer->getWaitDebugInfo(info) && info.queue && info.queued &&
            peer->getStatus() == Thread::Sleeping)
        {
            peerEnrolled = true;
            break;
        }
        Scheduler::instance().yield();
    }
    passed &= check(
        context.peerEntered == 1 && peerEnrolled,
        "the peer did not publish its mutex wait");

    passed &= check(
        terminator->start(), "the delayed terminating thread did not start");
    const bool reapable = child->waitUntilTerminationReapable();

    passed &= check(
        reapable && child->getState() == Process::Terminated,
        "the child did not publish off-stack termination");
    passed &= check(
        peer->getStatus() == Thread::AwaitingJoin &&
            unstartedPeer->getStatus() == Thread::AwaitingJoin &&
            terminator->getStatus() == Thread::AwaitingJoin,
        "not every retained child thread reached AwaitingJoin");
    passed &= check(
        context.peerAcquiredMutex == 0,
        "the blocked peer escaped terminal wait cancellation");
    passed &= check(
        context.unstartedPeerEntered == 0,
        "the delayed unstarted peer executed during process exit");
    passed &= check(
        orphan->getParent() != child,
        "process exit left a child parented to the dead process");

    delete child;
    delete orphan;
    mutex.release();

    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS process-publication-reparent");
        NOTICE("HOSTED-WAIT-TEST: PASS process-exit-rendezvous");
    }
    return passed;
}
