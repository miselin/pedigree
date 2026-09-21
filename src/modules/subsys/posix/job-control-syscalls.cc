/* Copyright (c) 2026, Pedigree Developers. */
#include "job-control-syscalls.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include "PosixProcess.h"

namespace {
PosixProcess* caller() {
  Process* process = Processor::information().getCurrentThread()->getParent();
  return process->getType() == Process::Posix ? static_cast<PosixProcess*>(process) : nullptr;
}

int identity(int pid, bool session) {
  int result = -1;
  {
    TerminationDeferral lifetime;
    Scheduler::ProcessLease lease;
    Process::ThreadLease task;
    PosixProcess* target = nullptr;
    if (!pid)
      target = caller();
    else if (pid > 0 && Scheduler::instance().acquireProcessByUserspaceId(lease, pid) &&
             lease->getType() == Process::Posix)
      target = static_cast<PosixProcess*>(lease.get());
    else if (pid > 0 && Scheduler::instance().acquireThreadByTaskId(task, pid) &&
             task->getParent()->getType() == Process::Posix)
      target = static_cast<PosixProcess*>(task->getParent());
    if (target) {
      size_t id = 0;
      if (session)
        id = target->getSessionId();
      else
        target->getProcessGroupId(id);
      if (id)
        result = id;
    }
  }
  syscallError(result < 0 ? Error::NoSuchProcess : 0);
  return result;
}
}  // namespace

int posix_getsid(int pid) {
  return identity(pid, true);
}
int posix_getpgid(int pid) {
  return identity(pid, false);
}
int posix_getpgrp() {
  return identity(0, false);
}

int posix_setsid() {
  TerminationDeferral lifetime;
  PosixProcess* process = caller();
  if (!process) {
    SYSCALL_ERROR(NoSuchProcess);
    return -1;
  }
  return process->createSession();
}

int posix_setpgid(int pid, int pgid) {
  int result = -1;
  int error = 0;
  {
    TerminationDeferral lifetime;
    PosixProcess* current = caller();
    Scheduler::ProcessLease lease;
    Process::ThreadLease task;
    PosixProcess* target = current;
    if (pid < 0 || !current)
      error = Error::NoSuchProcess;
    else if (pgid < 0)
      error = Error::InvalidArgument;
    else {
      if (pid && static_cast<size_t>(pid) != current->getUserspaceId()) {
        if (!Scheduler::instance().acquireProcessByUserspaceId(lease, pid) ||
            lease->getType() != Process::Posix) {
          error = Scheduler::instance().acquireThreadByTaskId(task, pid) ? Error::InvalidArgument
                                                                         : Error::NoSuchProcess;
          target = nullptr;
        } else
          target = static_cast<PosixProcess*>(lease.get());
      }
      if (target) {
        result = target->changeProcessGroup(*current, pgid ? pgid : target->getUserspaceId());
        error = result < 0 ? Processor::information().getCurrentThread()->getErrno() : 0;
      }
    }
  }
  syscallError(error);
  return result;
}
