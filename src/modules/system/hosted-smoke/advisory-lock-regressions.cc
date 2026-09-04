/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/errors.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"

#include <fcntl.h>

#include "modules/subsys/posix/FileDescriptor.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/file-syscalls.h"
#include "modules/system/vfs/File.h"
#include <sys/file.h>

namespace {
constexpr size_t LockDescriptor = 96;
constexpr size_t FcntlCommandCount = 3;
constexpr size_t ValidFlockOperationCount = 6;
constexpr size_t InvalidFlockOperationCount = 5;

const int FcntlCommands[FcntlCommandCount] = {F_GETLK, F_SETLK, F_SETLKW};
const int ValidFlockOperations[ValidFlockOperationCount] = {
    LOCK_SH, LOCK_EX, LOCK_UN, LOCK_SH | LOCK_NB, LOCK_EX | LOCK_NB, LOCK_UN | LOCK_NB};
const int InvalidFlockOperations[InvalidFlockOperationCount] = {
    0, LOCK_NB, LOCK_SH | LOCK_EX, LOCK_SH | LOCK_UN, LOCK_EX | 0x1000};

struct AdvisoryLockContext {
  AdvisoryLockContext()
      : fcntlResults{-2, -2, -2},
        fcntlErrors{0, 0, 0},
        badFcntlResults{-2, -2, -2},
        badFcntlErrors{0, 0, 0},
        validFlockResults{-2, -2, -2, -2, -2, -2},
        validFlockErrors{0, 0, 0, 0, 0, 0},
        invalidFlockResults{-2, -2, -2, -2, -2},
        invalidFlockErrors{0, 0, 0, 0, 0},
        badFlockResult(-2),
        badFlockError(0),
        invalidBadFlockResult(-2),
        invalidBadFlockError(0),
        queryUnchanged(false),
        returned(0) {}

  int fcntlResults[FcntlCommandCount];
  int fcntlErrors[FcntlCommandCount];
  int badFcntlResults[FcntlCommandCount];
  int badFcntlErrors[FcntlCommandCount];
  int validFlockResults[ValidFlockOperationCount];
  int validFlockErrors[ValidFlockOperationCount];
  int invalidFlockResults[InvalidFlockOperationCount];
  int invalidFlockErrors[InvalidFlockOperationCount];
  int badFlockResult;
  int badFlockError;
  int invalidBadFlockResult;
  int invalidBadFlockError;
  bool queryUnchanged;
  Atomic<size_t> returned;
};

bool closeDescriptor(PosixSubsystem* subsystem, size_t fd) {
  DescriptorLease descriptor;
  return subsystem->acquireFileDescriptor(fd, descriptor) &&
         subsystem->closeFileDescriptor(fd, descriptor);
}

int advisoryLockWorker(void* parameter) {
  AdvisoryLockContext* context = reinterpret_cast<AdvisoryLockContext*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();

  struct flock request = {};
  request.l_type = F_WRLCK;
  request.l_whence = SEEK_SET;
  request.l_start = 7;
  request.l_len = 13;
  request.l_pid = 41;
  const struct flock original = request;

  for (size_t i = 0; i < FcntlCommandCount; ++i) {
    thread->setErrno(0);
    context->fcntlResults[i] =
        posix_fcntl(static_cast<int>(LockDescriptor), FcntlCommands[i], &request);
    context->fcntlErrors[i] = thread->getErrno();

    thread->setErrno(0);
    context->badFcntlResults[i] = posix_fcntl(-1, FcntlCommands[i], &request);
    context->badFcntlErrors[i] = thread->getErrno();
  }
  context->queryUnchanged = request.l_type == original.l_type &&
                            request.l_whence == original.l_whence &&
                            request.l_start == original.l_start &&
                            request.l_len == original.l_len && request.l_pid == original.l_pid;

  for (size_t i = 0; i < ValidFlockOperationCount; ++i) {
    thread->setErrno(0);
    context->validFlockResults[i] =
        posix_flock(static_cast<int>(LockDescriptor), ValidFlockOperations[i]);
    context->validFlockErrors[i] = thread->getErrno();
  }
  for (size_t i = 0; i < InvalidFlockOperationCount; ++i) {
    thread->setErrno(0);
    context->invalidFlockResults[i] =
        posix_flock(static_cast<int>(LockDescriptor), InvalidFlockOperations[i]);
    context->invalidFlockErrors[i] = thread->getErrno();
  }

  thread->setErrno(0);
  context->badFlockResult = posix_flock(-1, LOCK_EX | LOCK_NB);
  context->badFlockError = thread->getErrno();
  thread->setErrno(0);
  context->invalidBadFlockResult = posix_flock(-1, 0);
  context->invalidBadFlockError = thread->getErrno();

  context->returned += 1;
  return 0;
}

bool advisoryLocksFailClosed(Process* kernelProcess) {
  Process* process = new Process(kernelProcess);
  PosixSubsystem* subsystem = new PosixSubsystem;
  process->setSubsystem(subsystem);

  File file(String("advisory-lock-probe"), 0, 0, 0, 1, nullptr, 0, nullptr);
  subsystem->addFileDescriptor(LockDescriptor,
                               new FileDescriptor(&file, 0, LockDescriptor, 0, O_RDWR));

  AdvisoryLockContext context;
  Thread* worker = new Thread(process, advisoryLockWorker, &context, nullptr, false, true, true);
  worker->setName("hosted advisory lock fail-closed");
  const bool started = worker->start();
  const bool joined = started && worker->joinForCompletion();
  if (!started) {
    delete worker;
  }

  bool passed = started && joined && context.returned == 1 && context.queryUnchanged;
  for (size_t i = 0; i < FcntlCommandCount; ++i) {
    passed = context.fcntlResults[i] == -1 && context.fcntlErrors[i] == Error::Unimplemented &&
             context.badFcntlResults[i] == -1 &&
             context.badFcntlErrors[i] == Error::BadFileDescriptor && passed;
  }
  for (size_t i = 0; i < ValidFlockOperationCount; ++i) {
    passed = context.validFlockResults[i] == -1 &&
             context.validFlockErrors[i] == Error::Unimplemented && passed;
  }
  for (size_t i = 0; i < InvalidFlockOperationCount; ++i) {
    passed = context.invalidFlockResults[i] == -1 &&
             context.invalidFlockErrors[i] == Error::InvalidArgument && passed;
  }
  passed = context.badFlockResult == -1 && context.badFlockError == Error::BadFileDescriptor &&
           context.invalidBadFlockResult == -1 &&
           context.invalidBadFlockError == Error::InvalidArgument && passed;

  passed = closeDescriptor(subsystem, LockDescriptor) && passed;
  delete process;

  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL advisory-lock-fail-closed: "
        "unsupported locks succeeded or validation returned the wrong error");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS advisory-lock-fail-closed");
  return true;
}
}  // namespace

bool runHostedAdvisoryLockRegressions(Process* process) {
  return advisoryLocksFailClosed(process);
}
