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
#include "modules/system/vfs/MemoryMappedFile.h"
#include "namespace-file.h"
#include <sys/utsname.h>

namespace {
constexpr unsigned long NewUts = 0x04000000;
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

bool administrative() {
  if (Processor::information().getCurrentThread()->getParent()->getEffectiveUserId() != 0) {
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
  if (!administrative())
    return result.finish(-1);
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
  if (flags & ~NewUts) {
    SYSCALL_ERROR(OperationNotSupported);
    return result.finish(-1);
  }
  if (!flags)
    return result.finish(0);
  MemoryMapManager::OperationGuard operation(MemoryMapManager::instance());
  if (!administrative())
    return result.finish(-1);
  auto selected = context();
  UtsRef source, replacement;
  Thread& thread = *Processor::information().getCurrentThread();
  if (!selected || !selected->acquireThread(thread, source))
    return result.finish(posix_uts_error(UtsStatus::Missing));
  auto status = posix_uts_copy(source, replacement);
  if (status == UtsStatus::Success)
    status = selected->replaceThread(thread, replacement);
  return result.finish(posix_uts_error(status));
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
  if (!administrative())
    return result.finish(-1);
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
