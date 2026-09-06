/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include "PosixProcess.h"
#include "wait-state.h"

namespace PosixWait {
namespace {
bool eligible(PosixProcess* parent, Process* child, const Request& request) {
  if (!child || child == parent || child->getType() != Process::Posix ||
      child->getParent() != parent || child->getState() == Process::Reaped) {
    return false;
  }
  if (request.selector == Selector::All)
    return true;
  if (request.selector == Selector::Pid)
    return child->getId() == static_cast<size_t>(request.id);
  size_t group = 0;
  return request.id >= 0 && static_cast<PosixProcess*>(child)->getProcessGroupId(group) &&
         group == static_cast<size_t>(request.id);
}

void terminalStatus(int encoded, Report& report) {
  if (!(encoded & 0x7F)) {
    report.cause = Exit;
    report.status = (encoded >> 8) & 0xFF;
  } else {
    report.cause = (encoded & 0x80) ? Dumped : Killed;
    report.status = encoded & 0x7F;
  }
}

void snapshotTimes(Process* child, Report& report) {
  report.userNanoseconds = child->getUserTime() + child->getReapedChildrenUserTime();
  report.kernelNanoseconds = child->getKernelTime() + child->getReapedChildrenKernelTime();
}
}  // namespace

int collect(const Request& request, Report& report) {
  report = Report();
  Thread* thread = Processor::information().getCurrentThread();
  thread->retainTemporarySignalWaitInterruptionOrClear();
  struct InterruptionScope {
    Thread* thread;
    ~InterruptionScope() {
      // Preserve an enclosing temporary-mask wait's interruption ownership.
      thread->retainTemporarySignalWaitInterruptionOrClear();
    }
  } interruptionScope{thread};
  auto* parent = static_cast<PosixProcess*>(thread->getParent());
  WaitQueue::WakeReason previousWake = WaitQueue::WakeReason::Signalled;

  while (true) {
    Process::ReaperClaim reaper;
    Scheduler::ProcessLease observer;
    Process* terminalChild = nullptr;
    Report selected;
    bool hasResult = false;
    {
      // This guard serializes competing reapers and joins the predicate to
      // sleep. No child pointer survives a sleep on this parent queue.
      auto guard = parent->acquireChildStateWait();
      bool hasEligibleChild = false;
      for (size_t i = 0;; ++i) {
        Process* child = Scheduler::instance().getChildProcess(parent, i);
        if (!child)
          break;
        if (!eligible(parent, child, request))
          continue;

        if (child->getState() == Process::Terminated) {
          // An exited child cannot produce a later stop or continue event.
          if (!(request.events & Exited))
            continue;
          if (request.noWait) {
            // Acquire into an empty lease while enumeration is protected;
            // dropping the last lease can wake a destructor and must occur
            // outside the parent guard.
            if (!Scheduler::instance().acquireProcess(observer, child))
              continue;
          } else {
            child->reap();
            reaper = child->tryClaimReaper();
            if (!reaper) {
              FATAL("child wait lost sole reaper ownership for pid " << Dec << child->getId());
            }
          }
          terminalStatus(child->getExitStatus(), selected);
          terminalChild = child;
          hasResult = true;
        } else {
          hasEligibleChild = true;
          Process::ChildTransition transition;
          if (!child->selectPendingChildTransition(request.events & Stopped,
                                                   request.events & Continued, !request.noWait,
                                                   transition)) {
            continue;
          }
          if (transition.kind == Process::ChildTransitionKind::Stopped) {
            selected.cause = Stop;
            selected.status = transition.stopSignal;
          } else {
            selected.cause = Continue;
            selected.status = 18;  // Linux SIGCONT, independent of hosted libc.
          }
          snapshotTimes(child, selected);
          hasResult = true;
        }
        selected.pid = static_cast<int32_t>(child->getId());
        selected.uid = static_cast<PosixProcess*>(child)->snapshotCredentials().ruid;
        break;
      }

      if (!hasResult) {
        if (!hasEligibleChild) {
          SYSCALL_ERROR(NoChildren);
          return -1;
        }
        if (request.noHang) {
          thread->setErrno(0);
          return 0;
        }
        // A reportable child status wins over a caught signal at this scan.
        if (thread->getInterruptionReason() == Thread::InterruptedBySignal ||
            previousWake == WaitQueue::WakeReason::Unwinding ||
            previousWake == WaitQueue::WakeReason::Terminating) {
          SYSCALL_ERROR(Interrupted);
          return -1;
        }
        previousWake = guard.wait(WaitQueue::Channel(), Thread::ProcessWait,
                                  reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
      }
    }

    if (!hasResult)
      continue;
    if (terminalChild) {
      // Terminated precedes the exiting thread's final scheduler accounting.
      // Either ownership form pins the Process through this off-stack barrier.
      if (!terminalChild->waitUntilTerminationReapable()) {
        FATAL("child wait attempted to observe its own terminating process");
      }
      if (reaper) {
        parent->accountReapedChild(terminalChild, selected.userNanoseconds,
                                   selected.kernelNanoseconds);
        reaper.publish();
      } else {
        snapshotTimes(terminalChild, selected);
        observer.reset();
      }
    }
    report = selected;
    thread->setErrno(0);
    return 1;
  }
}
}  // namespace PosixWait
