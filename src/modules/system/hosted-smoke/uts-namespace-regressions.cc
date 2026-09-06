/* Copyright (c) 2026, Pedigree Developers. */
#include <config.h>
#if PEDIGREE_UTS_NAMESPACE_TESTS && THREADS
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

#include "modules/subsys/posix/PosixProcess.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/uts-namespace.h"

namespace {
bool check(bool condition, const char* detail) {
  if (!condition)
    ERROR("UTS-NAMESPACE-TEST: FAIL " << detail);
  return condition;
}

int forbiddenEntry(void* parameter) {
  *static_cast<bool*>(parameter) = true;
  return 0;
}

bool terminalPublication(PosixNamespaceContext& context, Process& process, const UtsRef& source) {
  const size_t references = source.refcount();
  UniquePointer<PreparedUtsThread> prepared;
  if (!check(posix_uts_prepare_thread(source, false, prepared) == UtsStatus::Success,
             "terminal preparation"))
    return false;
  bool entered = false;
  Thread* peer = new Thread(&process, forbiddenEntry, &entered, nullptr, false, true, true);
  if (!check(peer != nullptr, "terminal thread allocation"))
    return false;
  const size_t taskId = peer->getTaskId();
  peer->setUnwindState(Thread::TerminateThread);
  // The real unstarted shutdown may already have run the subsystem hook.
  // Either ordering must leave this prepared binding outside the live context.
  context.publishThread(prepared, *peer, false);
  UtsRef absent;
  PosixUtsTarget target;
  bool passed = check(bool(prepared) && !context.acquireThread(*peer, absent) &&
                          !context.taskTarget(taskId, target),
                      "terminal publication consumed or exposed a binding");
  prepared.reset();
  passed &= check(source.refcount() == references, "rejected preparation retained namespace");
  if (!peer->joinForCompletion())
    FATAL("UTS-NAMESPACE-TEST: terminal thread could not be joined safely");
  passed &= check(!entered, "terminal thread executed its entry point");
  if (passed)
    NOTICE("UTS-NAMESPACE-TEST: PASS terminal publication rollback");
  return passed;
}

struct PeerState {
  Semaphore ready{0}, release{0};
  bool released = false;
};

int controlledEntry(void* parameter) {
  auto& state = *static_cast<PeerState*>(parameter);
  state.ready.release();
  state.released = state.release.acquire(1, 10);
  return 0;
}

bool targetRetirement(PosixNamespaceContext& context, Process& process, const UtsRef& source) {
  UniquePointer<PreparedUtsThread> prepared;
  if (!check(posix_uts_prepare_thread(source, false, prepared) == UtsStatus::Success,
             "live preparation"))
    return false;
  PeerState state;
  Thread* peer = new Thread(&process, controlledEntry, &state, nullptr, false, true, true);
  if (!check(peer != nullptr, "live thread allocation"))
    return false;
  const size_t taskId = peer->getTaskId();
  context.publishThread(prepared, *peer, false);
  const bool started = peer->start();
  const bool ready = started && state.ready.acquire(1, 10);
  PosixUtsTarget retained;
  UtsRef captured;
  bool passed = check(ready && !prepared && context.taskTarget(taskId, retained) &&
                          posix_uts_acquire_target(retained, captured) == UtsStatus::Success &&
                          captured->identity() == source->identity(),
                      "live task namespace capture");
  state.release.release();
  if (!started)
    peer->setUnwindState(Thread::TerminateThread);
  if (!peer->joinForCompletion())
    FATAL("UTS-NAMESPACE-TEST: controlled thread could not be joined safely");
  UtsRef missing;
  PosixUtsTarget absent;
  passed &= check(state.released && !context.taskTarget(taskId, absent) &&
                      posix_uts_acquire_target(retained, missing) == UtsStatus::Missing &&
                      !missing && captured && captured->identity() == source->identity(),
                  "retained target survived task retirement or lost namespace ownership");
  if (passed)
    NOTICE("UTS-NAMESPACE-TEST: PASS retained task retirement");
  return passed;
}

int coordinator(void* parameter) {
  bool& passed = *static_cast<bool*>(parameter);
  Thread& current = *Processor::information().getCurrentThread();
  auto* process = current.getParent();
  auto context = static_cast<PosixSubsystem*>(process->getSubsystem())->namespaceContext();
  UtsRef source;
  if (!check(context && context->acquireThread(current, source), "coordinator binding"))
    return 0;
  passed = terminalPublication(*context, *process, source) &&
           targetRetirement(*context, *process, source);
  PosixUtsTarget leader;
  passed &= check(context->leaderTarget(leader), "leader capture before close");
  context->close();
  context->close();
  UtsRef absent;
  PosixUtsTarget noLeader;
  passed &= check(!context->acquireThread(current, absent) && !context->leaderTarget(noLeader) &&
                      posix_uts_acquire_target(leader, absent) == UtsStatus::Missing && !absent,
                  "closed context retained a live binding");
  if (passed)
    NOTICE("UTS-NAMESPACE-TEST: PASS closed context");
  return 0;
}
}  // namespace

bool utsNamespaceRegression() {
  NOTICE("UTS-NAMESPACE-TEST: BEGIN");
  auto* process = new PosixProcess(Scheduler::instance().getKernelProcess(), true);
  if (!check(process != nullptr, "isolated process allocation"))
    return false;
  auto* subsystem = new PosixSubsystem;
  if (!subsystem) {
    delete process;
    return check(false, "isolated subsystem allocation");
  }
  process->setSubsystem(subsystem);
  auto context = subsystem->namespaceContext();
  UtsRef initial, source;
  UniquePointer<PreparedUtsThread> prepared;
  if (!check(context && context->valid() && posix_uts_initial(initial) == UtsStatus::Success &&
                 posix_uts_copy(initial, source) == UtsStatus::Success &&
                 posix_uts_prepare_thread(source, false, prepared) == UtsStatus::Success,
             "isolated namespace preparation")) {
    delete process;
    return false;
  }
  bool passed = false;
  Thread* worker = new Thread(process, coordinator, &passed, nullptr, false, true, true);
  if (!worker) {
    delete process;
    return check(false, "coordinator allocation");
  }
  context->publishThread(prepared, *worker, true);
  process->publish();
  const bool started = worker->start();
  if (!started)
    worker->setUnwindState(Thread::TerminateThread);
  const bool joined = worker->joinForCompletion();
  if (!joined)
    FATAL("UTS-NAMESPACE-TEST: coordinator could not be joined safely");
  delete process;
  passed = check(started && joined && passed, "isolated coordinator") && passed;
  if (passed)
    NOTICE("UTS-NAMESPACE-TEST: END PASS");
  return passed;
}
#endif
