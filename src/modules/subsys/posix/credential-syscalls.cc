#include "pedigree/kernel/syscallError.h"

#include <limits.h>

#include "PosixProcess.h"
#include "modules/system/vfs/VFS.h"
#include "system-syscalls.h"

namespace {
PosixProcess* currentProcess() {
  Process* process = Processor::information().getCurrentThread()->getParent();
  if (process->getType() != Process::Posix) {
    SYSCALL_ERROR(InvalidArgument);
    return nullptr;
  }
  return static_cast<PosixProcess*>(process);
}
int complete(PosixProcess::CredentialStatus status) {
  switch (status) {
    case PosixProcess::CredentialStatus::Denied:
      SYSCALL_ERROR(NotEnoughPermissions);
      return -1;
    case PosixProcess::CredentialStatus::Invalid:
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    case PosixProcess::CredentialStatus::Success:
      Processor::information().getCurrentThread()->setErrno(0);
      return 0;
  }
  SYSCALL_ERROR(InvalidArgument);
  return -1;
}
int change(PosixProcess::CredentialChange type, uint32_t first, uint32_t second = UINT32_MAX,
           uint32_t third = UINT32_MAX) {
  PosixProcess* process = currentProcess();
  if (!process)
    return -1;
  return complete(process->changeCredentials(*Processor::information().getCurrentThread(), type,
                                             first, second, third));
}
}  // namespace

uid_t posix_getuid() {
  Process* process = Processor::information().getCurrentThread()->getParent();
  if (process->getType() == Process::Posix)
    return static_cast<PosixProcess*>(process)->getUserId();
  return process->getUserId();
}
gid_t posix_getgid() {
  return Processor::information().getCurrentThread()->getParent()->getGroupId();
}
uid_t posix_geteuid() {
  return Processor::information().getCurrentThread()->getParent()->getEffectiveUserId();
}
gid_t posix_getegid() {
  return Processor::information().getCurrentThread()->getParent()->getEffectiveGroupId();
}
int posix_setuid(uid_t id) {
  return change(PosixProcess::CredentialChange::SetUid, id);
}
int posix_setgid(gid_t id) {
  return change(PosixProcess::CredentialChange::SetGid, id);
}
int posix_seteuid(uid_t id) {
  return posix_setresuid(UINT32_MAX, id, UINT32_MAX);
}
int posix_setegid(gid_t id) {
  return posix_setresgid(UINT32_MAX, id, UINT32_MAX);
}
int posix_setreuid(uid_t real, uid_t effective) {
  return change(PosixProcess::CredentialChange::SetReUid, real, effective);
}
int posix_setregid(gid_t real, gid_t effective) {
  return change(PosixProcess::CredentialChange::SetReGid, real, effective);
}
int posix_setresuid(uid_t real, uid_t effective, uid_t saved) {
  return change(PosixProcess::CredentialChange::SetResUid, real, effective, saved);
}
int posix_setresgid(gid_t real, gid_t effective, gid_t saved) {
  return change(PosixProcess::CredentialChange::SetResGid, real, effective, saved);
}

long posix_setfsuid(uid_t id) {
  PosixProcess* process = currentProcess();
  if (!process)
    return -1;
  Thread* task = Processor::information().getCurrentThread();
  const uint32_t previous = process->changeFilesystemId(*task, false, id);
  task->setErrno(0);
  return static_cast<long>(previous);
}
long posix_setfsgid(gid_t id) {
  PosixProcess* process = currentProcess();
  if (!process)
    return -1;
  Thread* task = Processor::information().getCurrentThread();
  const uint32_t previous = process->changeFilesystemId(*task, true, id);
  task->setErrno(0);
  return static_cast<long>(previous);
}

int posix_getresuid(uid_t* real, uid_t* effective, uid_t* saved) {
  PosixProcess* process = currentProcess();
  if (!process)
    return -1;
  const auto snapshot = process->snapshotCredentials();
  if (!PosixSubsystem::copyToUser(real, &snapshot.ruid, sizeof(uid_t)) ||
      !PosixSubsystem::copyToUser(effective, &snapshot.euid, sizeof(uid_t)) ||
      !PosixSubsystem::copyToUser(saved, &snapshot.suid, sizeof(uid_t))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return complete(PosixProcess::CredentialStatus::Success);
}
int posix_getresgid(gid_t* real, gid_t* effective, gid_t* saved) {
  PosixProcess* process = currentProcess();
  if (!process)
    return -1;
  const auto snapshot = process->snapshotCredentials();
  if (!PosixSubsystem::copyToUser(real, &snapshot.rgid, sizeof(gid_t)) ||
      !PosixSubsystem::copyToUser(effective, &snapshot.egid, sizeof(gid_t)) ||
      !PosixSubsystem::copyToUser(saved, &snapshot.sgid, sizeof(gid_t))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return complete(PosixProcess::CredentialStatus::Success);
}
int posix_setgroups(size_t count, const gid_t* groups) {
  PosixProcess* process = currentProcess();
  if (!process)
    return -1;
  if (process->snapshotCredentials().euid) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }
  if (count > FilesystemCredentials::MaximumGroups) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  uint32_t imported[FilesystemCredentials::MaximumGroups] = {};
  if (count && !PosixSubsystem::copyFromUser(imported, groups, count, sizeof(gid_t))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  return complete(
      process->replaceGroups(*Processor::information().getCurrentThread(), imported, count));
}
int posix_getgroups(size_t count, gid_t* groups) {
  PosixProcess* process = currentProcess();
  if (!process)
    return -1;
  if (count > INT_MAX) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  const auto snapshot = process->snapshotCredentials();
  if (count) {
    if (count < snapshot.groupCount) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    if (snapshot.groupCount &&
        !PosixSubsystem::copyToUser(groups, snapshot.groups, snapshot.groupCount, sizeof(gid_t))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
  }
  Processor::information().getCurrentThread()->setErrno(0);
  return snapshot.groupCount;
}

bool posix_exec_file_readable(File* file) {
  Thread* task = Processor::information().getCurrentThread();
  const int error = task->getErrno();
  const bool readable = file && VFS::checkAccess(file, true, false, false);
  task->setErrno(error);
  return readable;
}
