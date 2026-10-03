/* Copyright (c) 2026, Pedigree Developers. */
#include "namespace-syscalls.h"
#include "pedigree/kernel/Version.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/Uninterruptible.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/utility.h"

#include "PosixProcess.h"
#include "PosixSubsystem.h"
#include "ipc-namespace.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/MountView.h"
#include "namespace-file.h"
#include "network-namespace.h"
#include "sandbox-state.h"
#include "sysv-semaphore-syscalls.h"
#include "user-namespace.h"
#include <sys/utsname.h>

namespace {
constexpr unsigned long NewMount = 0x20000, NewUts = 0x04000000, NewIpc = 0x08000000,
                        NewUser = 0x10000000, NewPid = 0x20000000, NewNet = 0x40000000;
constexpr unsigned long KnownUnshare = 0x80 | 0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x20000 |
                                       0x40000 | 0x02000000 | NewUts | 0x08000000 | 0x10000000 |
                                       0x20000000 | 0x40000000;

class NamespaceResult {
 public:
  NamespaceResult() : m_Thread(*Processor::information().getCurrentThread()) {}
  ~NamespaceResult() {
    m_Thread.setErrno(m_Error);
  }
  int finish(int value) {
    m_Error = value < 0 ? m_Thread.getErrno() : 0;
    return value;
  }

 private:
  Thread& m_Thread;
  size_t m_Error = 0;
};

bool administrative(const UserNamespaceRef& owner) {
  if (!posix_namespace_capable(owner, PosixCapabilities::SysAdmin)) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return false;
  }
  return true;
}

SharedPointer<PosixNamespaceContext> context() {
  Process* process = Processor::information().getCurrentThread()->getParent();
  if (process->getType() != Process::Posix || !process->getSubsystem())
    return {};
  return static_cast<PosixSubsystem*>(process->getSubsystem())->namespaceContext();
}

int setName(const char* name, size_t suppliedLength, bool domain) {
  NamespaceResult result;
  Uninterruptible lifetime;
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  const int32_t length = static_cast<int32_t>(suppliedLength);
  if (length < 0 || length > 64) {
    SYSCALL_ERROR(InvalidArgument);
    return result.finish(-1);
  }
  char bytes[64] = {};
  if (length && !PosixSubsystem::copyFromUser(bytes, name, static_cast<size_t>(length))) {
    SYSCALL_ERROR(BadAddress);
    return result.finish(-1);
  }
  auto selected = context();
  UtsRef space;
  if (!selected || !selected->acquireThread(*Processor::information().getCurrentThread(), space))
    return result.finish(posix_uts_error(UtsStatus::Missing));
  if (!administrative(space->owner())) {
    return result.finish(-1);
  }
  space->setName(domain, bytes, static_cast<size_t>(length));
  return result.finish(0);
}

void copyField(char* output, size_t capacity, const char* source) {
  size_t length = 0;
  while (source[length] && length + 1 < capacity)
    ++length;
  MemoryCopy(output, source, length);
}
}  // namespace

int posix_sethostname(const char* name, size_t length) {
  return setName(name, length, false);
}
int posix_setdomainname(const char* name, size_t length) {
  return setName(name, length, true);
}

int posix_unshare(unsigned long flags) {
  NamespaceResult result;
  Uninterruptible lifetime;
  if (flags & ~KnownUnshare) {
    SYSCALL_ERROR(InvalidArgument);
    return result.finish(-1);
  }
  if (flags & ~(NewMount | NewUts | NewUser | NewPid | NewIpc | NewNet)) {
    SYSCALL_ERROR(OperationNotSupported);
    return result.finish(-1);
  }
  if (!flags) {
    return result.finish(0);
  }
  Thread& thread = *Processor::information().getCurrentThread();
  Process* process = thread.getParent();
  if ((flags & (NewMount | NewUser | NewPid)) && process->getNumThreads() != 1) {
    SYSCALL_ERROR(InvalidArgument);
    return result.finish(-1);
  }
  if (flags & NewPid) {
    if (flags != NewPid) {
      SYSCALL_ERROR(OperationNotSupported);
      return result.finish(-1);
    }
    if (!posix_capable(PosixCapabilities::SysAdmin)) {
      SYSCALL_ERROR(NotEnoughPermissions);
      return result.finish(-1);
    }
    if (!process->pidNamespaceReady()) {
      SYSCALL_ERROR(OutOfMemory);
      return result.finish(-1);
    }
    const auto active = process->pidNamespace();
    if (process->pidNamespaceForChildren().get() != active.get()) {
      SYSCALL_ERROR(InvalidArgument);
      return result.finish(-1);
    }
    if (active->depth() >= 32) {
      SYSCALL_ERROR(NoSpaceLeftOnDevice);
      return result.finish(-1);
    }
    auto prepared = SharedPointer<UserspacePidNamespace>::tryAllocate(active);
    if (!prepared) {
      SYSCALL_ERROR(OutOfMemory);
      return result.finish(-1);
    }
    if (!process->unsharePidNamespace(prepared)) {
      SYSCALL_ERROR(InvalidArgument);
      return result.finish(-1);
    }
    return result.finish(0);
  }
  TaskCredentialsRef credentials;
  if ((flags & NewUser) && !posix_user_namespace_prepare(thread, credentials)) {
    return result.finish(-1);
  }
  if (!(flags & NewUser) && !posix_capable(PosixCapabilities::SysAdmin)) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return result.finish(-1);
  }
  const auto owner = credentials ? credentials->userNamespace : posix_user_namespace(thread);
  FilesystemContextOwner filesystem;
  auto previousFilesystem = process->acquireFilesystemContext();
  if (flags & NewMount) {
    auto* view = VfsMountView::fromContext(previousFilesystem);
    if (!view ||
        !view->forkNamespace(previousFilesystem, filesystem, owner ? owner->identity() : 0)) {
      return result.finish(-1);
    }
  }
  auto selected = context();
  UtsRef source, replacement;
  if (flags & NewUts) {
    if (!selected || !selected->acquireThread(thread, source)) {
      return result.finish(posix_uts_error(UtsStatus::Missing));
    }
    const auto status = posix_uts_copy(source, replacement, owner);
    if (status != UtsStatus::Success) {
      return result.finish(posix_uts_error(status));
    }
  }
  NetworkNamespaceRef network;
  if ((flags & NewNet) && !posix_network_namespace_prepare(owner, network)) {
    return result.finish(-1);
  }
  SharedPointer<IpcNamespace> ipc;
  if (flags & NewIpc) {
    ipc = SharedPointer<IpcNamespace>::tryAllocate(owner);
    if (!ipc) {
      SYSCALL_ERROR(OutOfMemory);
      return result.finish(-1);
    }
  }
  // Keep the original immutable task state until all fallible publication is done.
  const auto previousState = thread.securityState();
  Thread::SecurityStateRef preparedState;
  if (!posix_sandbox_prepare_namespaces(thread, credentials, ipc, network, preparedState)) {
    return result.finish(-1);
  }
  thread.setSecurityState(preparedState);
  if (replacement && selected->replaceThread(thread, replacement) != UtsStatus::Success) {
    thread.setSecurityState(previousState);
    return result.finish(posix_uts_error(UtsStatus::Missing));
  }
  if (filesystem &&
      !process->replaceFilesystemContext(pedigree_std::move(filesystem), previousFilesystem)) {
    if (replacement) {
      selected->replaceThread(thread, source);
    }
    thread.setSecurityState(previousState);
    SYSCALL_ERROR(NoMoreProcesses);
    return result.finish(-1);
  }
  if (ipc) {
    posix_sem_thread_exit(&thread);
  }
  return result.finish(0);
}

int posix_setns(int fd, int type) {
  NamespaceResult result;
  Uninterruptible lifetime;
  Process* process = Processor::information().getCurrentThread()->getParent();
  auto* subsystem = static_cast<PosixSubsystem*>(process->getSubsystem());
  DescriptorLease descriptor;
  if (!subsystem || !subsystem->acquireFileDescriptor(fd, descriptor)) {
    SYSCALL_ERROR(BadFileDescriptor);
    return result.finish(-1);
  }
  UtsRef space;
  if (!posix_uts_file_namespace(descriptor->getFile(), space) ||
      (type && static_cast<unsigned int>(type) != NewUts)) {
    SYSCALL_ERROR(InvalidArgument);
    return result.finish(-1);
  }
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  if (!administrative(space->owner())) {
    return result.finish(-1);
  }
  auto selected = context();
  if (!selected)
    return result.finish(posix_uts_error(UtsStatus::Missing));
  return result.finish(posix_uts_error(
      selected->replaceThread(*Processor::information().getCurrentThread(), space)));
}

int posix_uname(struct utsname* user) {
  NamespaceResult result;
  Uninterruptible lifetime;
  auto selected = context();
  UtsRef space;
  Thread& thread = *Processor::information().getCurrentThread();
  if (!selected || !selected->acquireThread(thread, space))
    return result.finish(posix_uts_error(UtsStatus::Missing));
  const auto names = space->snapshot();
  auto* subsystem = static_cast<PosixSubsystem*>(thread.getParent()->getSubsystem());
  if (subsystem->getAbi() == PosixSubsystem::LinuxAbi) {
    struct LinuxUtsname {
      char fields[6][65];
    } snapshot = {};
    static_assert(sizeof(snapshot) == 390, "Linux utsname size");
    copyField(snapshot.fields[0], 65, "Pedigree");
    MemoryCopy(snapshot.fields[1], names.node, 65);
    copyField(snapshot.fields[2], 65, "2.6.32-generic");
    copyField(snapshot.fields[3], 65, g_pBuildRevision);
#if ARM64
    copyField(snapshot.fields[4], 65, "aarch64");
#else
    copyField(snapshot.fields[4], 65,
              thread.executionPersonality().value() == ExecutionPersonality::Linux32
                  ? "i686"
                  : g_pBuildTarget);
#endif
    MemoryCopy(snapshot.fields[5], names.domain, 65);
    if (!PosixSubsystem::copyToUser(user, &snapshot, sizeof(snapshot))) {
      SYSCALL_ERROR(BadAddress);
      return result.finish(-1);
    }
  } else {
    struct utsname snapshot = {};
    copyField(snapshot.sysname, sizeof(snapshot.sysname), "Pedigree");
    MemoryCopy(snapshot.nodename, names.node,
               sizeof(snapshot.nodename) < 65 ? sizeof(snapshot.nodename) : 65);
    copyField(snapshot.release, sizeof(snapshot.release), g_pBuildRevision);
    copyField(snapshot.version, sizeof(snapshot.version), "Foster");
    copyField(snapshot.machine, sizeof(snapshot.machine),
              thread.executionPersonality().value() == ExecutionPersonality::Linux32
                  ? "i686"
                  : g_pBuildTarget);
    if (!PosixSubsystem::copyToUser(user, &snapshot, sizeof(snapshot))) {
      SYSCALL_ERROR(BadAddress);
      return result.finish(-1);
    }
  }
  return result.finish(0);
}
