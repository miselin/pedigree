#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/syscallError.h"

#include <limits.h>

#include "PosixProcess.h"
#include "modules/system/vfs/VFS.h"
#include "sandbox-state.h"
#include "system-syscalls.h"
#include "user-namespace.h"

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
    case PosixProcess::CredentialStatus::NoMemory:
      SYSCALL_ERROR(OutOfMemory);
      return -1;
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
  const bool group = type == PosixProcess::CredentialChange::SetGid ||
                     type == PosixProcess::CredentialChange::SetReGid ||
                     type == PosixProcess::CredentialChange::SetResGid;
  if (!posix_global_id(group, first, first) || !posix_global_id(group, second, second) ||
      !posix_global_id(group, third, third)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  return complete(process->changeCredentials(*Processor::information().getCurrentThread(), type,
                                             first, second, third));
}
}  // namespace

uid_t posix_getuid() {
  Process* process = Processor::information().getCurrentThread()->getParent();
  return posix_visible_id(false, process->getUserId());
}
gid_t posix_getgid() {
  return posix_visible_id(true,
                          Processor::information().getCurrentThread()->getParent()->getGroupId());
}
uid_t posix_geteuid() {
  return posix_visible_id(
      false, Processor::information().getCurrentThread()->getParent()->getEffectiveUserId());
}
gid_t posix_getegid() {
  return posix_visible_id(
      true, Processor::information().getCurrentThread()->getParent()->getEffectiveGroupId());
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
  uint32_t global;
  if (!posix_global_id(false, id, global))
    global = UINT32_MAX;
  const uint32_t previous = process->changeFilesystemId(*task, false, global);
  task->setErrno(0);
  return static_cast<long>(posix_visible_id(false, previous));
}
long posix_setfsgid(gid_t id) {
  PosixProcess* process = currentProcess();
  if (!process)
    return -1;
  Thread* task = Processor::information().getCurrentThread();
  uint32_t global;
  if (!posix_global_id(true, id, global))
    global = UINT32_MAX;
  const uint32_t previous = process->changeFilesystemId(*task, true, global);
  task->setErrno(0);
  return static_cast<long>(posix_visible_id(true, previous));
}

int posix_getresuid(uid_t* real, uid_t* effective, uid_t* saved) {
  PosixProcess* process = currentProcess();
  if (!process)
    return -1;
  auto snapshot = process->snapshotCredentials();
  snapshot.ruid = posix_visible_id(false, snapshot.ruid);
  snapshot.euid = posix_visible_id(false, snapshot.euid);
  snapshot.suid = posix_visible_id(false, snapshot.suid);
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
  auto snapshot = process->snapshotCredentials();
  snapshot.rgid = posix_visible_id(true, snapshot.rgid);
  snapshot.egid = posix_visible_id(true, snapshot.egid);
  snapshot.sgid = posix_visible_id(true, snapshot.sgid);
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
  auto space = posix_user_namespace(*Processor::information().getCurrentThread());
  if (!posix_capable(PosixCapabilities::Setgid) || (space && !space->groupsAllowed())) {
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
  for (size_t i = 0; i < count; ++i) {
    if (!posix_global_id(true, imported[i], imported[i])) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
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
  auto snapshot = process->snapshotCredentials();
  for (size_t i = 0; i < snapshot.groupCount; ++i)
    snapshot.groups[i] = posix_visible_id(true, snapshot.groups[i]);
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

namespace {
struct CapabilityHeader {
  uint32_t version;
  int32_t pid;
};
struct CapabilityData {
  uint32_t effective, permitted, inheritable;
};
int capabilityHeader(void* user, CapabilityHeader& header) {
  if (!PosixSubsystem::copyFromUser(&header, user, sizeof(header))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (header.version == 0x19980330)
    return 1;
  if (header.version == 0x20071026 || header.version == 0x20080522)
    return 2;
  const uint32_t version = 0x20080522;
  if (!PosixSubsystem::copyToUser(user, &version, sizeof(version))) {
    SYSCALL_ERROR(BadAddress);
  } else {
    SYSCALL_ERROR(InvalidArgument);
  }
  return -1;
}
}  // namespace

int posix_capget(void* hdrp, void* datap) {
  CapabilityHeader header = {};
  const int words = capabilityHeader(hdrp, header);
  if (words < 0)
    return -1;
  Thread& task = *Processor::information().getCurrentThread();
  if (header.pid < 0) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  Process::ThreadLease target;
  Thread* selected = &task;
  if (header.pid && static_cast<size_t>(header.pid) !=
                        task.getUserspaceTaskId(task.getParent()->pidNamespace().get())) {
    if (!Scheduler::instance().acquireThreadByUserspaceId(target, header.pid,
                                                          task.getParent()->pidNamespace().get())) {
      SYSCALL_ERROR(NoSuchProcess);
      return -1;
    }
    selected = target.get();
  }
  const auto credentials = posix_task_credentials(*selected);
  CapabilityData data[2] = {};
  for (int i = 0; i < words; ++i) {
    data[i].effective = credentials.effective >> (32 * i);
    data[i].permitted = credentials.permitted >> (32 * i);
    data[i].inheritable = credentials.inheritable >> (32 * i);
  }
  if (datap && !PosixSubsystem::copyToUser(datap, data, words, sizeof(data[0]))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  task.setErrno(0);
  return 0;
}

int posix_capset(void* hdrp, const void* datap) {
  TerminationDeferral lifetime;
  CapabilityHeader header = {};
  const int words = capabilityHeader(hdrp, header);
  if (words < 0)
    return -1;
  Thread& task = *Processor::information().getCurrentThread();
  if (header.pid &&
      (header.pid < 0 || static_cast<size_t>(header.pid) !=
                             task.getUserspaceTaskId(task.getParent()->pidNamespace().get()))) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }
  CapabilityData data[2] = {};
  if (!PosixSubsystem::copyFromUser(data, datap, words, sizeof(data[0]))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  auto value = posix_task_credentials(task);
  const uint64_t effective = (uint64_t(data[1].effective) << 32) | data[0].effective;
  const uint64_t permitted = (uint64_t(data[1].permitted) << 32) | data[0].permitted;
  const uint64_t inheritable = (uint64_t(data[1].inheritable) << 32) | data[0].inheritable;
  const bool setpcap = value.effective & (uint64_t(1) << PosixCapabilities::Setpcap);
  if (((effective | permitted | inheritable) & ~PosixCapabilities::All) ||
      (effective & ~permitted) || (permitted & ~value.permitted) ||
      (inheritable & ~(value.inheritable | value.bounding)) ||
      (!setpcap && (inheritable & ~(value.inheritable | value.permitted)))) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }
  value.permitted = permitted;
  value.effective = effective;
  value.inheritable = inheritable;
  value.ambient &= permitted & inheritable;
  auto next = TaskCredentialsRef::tryAllocate(value);
  if (!next) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  if (!posix_sandbox_set_credentials(task, next))
    return -1;
  task.setErrno(0);
  return 0;
}
