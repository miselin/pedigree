/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include <limits.h>

#include "PosixProcess.h"
#include "PosixSubsystem.h"
#include "ResolvedPath.h"
#include "accounting-record.h"
#include "file-syscalls.h"
#include "modules/system/console/Console.h"
#include "modules/system/vfs/VFS.h"
#include "process-accounting.h"
#include <sys/acct.h>

namespace {
static_assert(sizeof(PosixAccounting::Record) == sizeof(acct_v3));
static_assert(offsetof(PosixAccounting::Record, elapsedFloat) == offsetof(acct_v3, ac_etime));
static_assert(offsetof(PosixAccounting::Record, command) == offsetof(acct_v3, ac_comm));

class AccountingFile {
 public:
  ~AccountingFile() {
    TerminationDeferral lifetime;
    if (m_File) {
      m_File->decreaseRefCount(true);
      m_File->releaseVfsReference();
    }
    m_Filesystem.reset();
  }

  bool prepare(File* file) {
    if (!Process::currentFilesystemCredentials(m_Credentials))
      return false;
    if (!VFS::instance().pinFilesystem(file->getFilesystem(), m_Filesystem) ||
        !file->retainVfsReference())
      return false;
    m_File = file;
    file->increaseRefCount(true);
    return true;
  }

  void append(const PosixAccounting::Record& record) {
    Process::FilesystemAccessScope credentials(m_Credentials);
    uint64_t offset = 0;
    const uint64_t written =
        m_File->append(sizeof(record), reinterpret_cast<uintptr_t>(&record), offset);
    if (written != sizeof(record)) {
      // Exit must remain possible when the logging device fills or fails.
      // Report loss without turning process teardown into an unbounded retry.
      if (!(m_Dropped++ % 64))
        WARNING("Process accounting could not append a complete record");
    }
  }

 private:
  VFS::FilesystemPin m_Filesystem;
  File* m_File = nullptr;
  FilesystemCredentials m_Credentials;
  size_t m_Dropped = 0;
};

Mutex accountingLock;
UniquePointer<AccountingFile> accountingFile;

struct AccountingResult {
  AccountingResult(int result)
      : value(result),
        error(result < 0 ? Processor::information().getCurrentThread()->getErrno() : 0) {}
  int value;
  int error;
};

AccountingResult configure(const char* path) {
  TerminationDeferral lifetime;
  auto* process = Processor::information().getCurrentThread()->getParent();
  if (process->getEffectiveUserId() != 0) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }
  UniquePointer<AccountingFile> prepared, retired;
  if (path) {
    String copied, normalised;
    const auto result = PosixSubsystem::copyUserString(path, copied, PATH_MAX);
    if (result != PosixSubsystem::UserStringSuccess) {
      syscallError(result == PosixSubsystem::UserStringBadAddress ? Error::BadAddress
                                                                  : Error::NameTooLong);
      return -1;
    }
    if (!copied.length()) {
      SYSCALL_ERROR(DoesNotExist);
      return -1;
    }
    normalisePath(normalised, copied.cstr());
    ResolvedPath fileLease;
    File* file = findFilePath(normalised, fileLease, FilesystemPathRef(), true);
    if (!file) {
      if (!Processor::information().getCurrentThread()->getErrno())
        SYSCALL_ERROR(DoesNotExist);
      return -1;
    }
    if (!file->supportsRegularFileOperations()) {
      SYSCALL_ERROR(PermissionDenied);
      return -1;
    }
    if (file->getFilesystem()->isReadOnly()) {
      SYSCALL_ERROR(ReadOnlyFilesystem);
      return -1;
    }
    if (!VFS::checkAccess(file, false, true, false))
      return -1;
    prepared = UniquePointer<AccountingFile>::allocate();
    if (!prepared || !prepared.get()->prepare(file)) {
      SYSCALL_ERROR(OutOfMemory);
      return -1;
    }
  }
  {
    // The same lock admits append and destination changes. Returning from
    // acct(NULL) therefore also means all writes to the retired file drained.
    LockGuard<Mutex> guard(accountingLock);
    retired = pedigree_std::move(accountingFile);
    accountingFile = pedigree_std::move(prepared);
  }
  return 0;
}

uint32_t parentId(PosixProcess& process) {
  while (Process* expected = process.getParent()) {
    Scheduler::ProcessLease parent;
    if (!Scheduler::instance().acquireProcess(parent, expected)) {
      if (process.getParent() != expected)
        continue;
      return 0;
    }
    if (process.getParent() == parent.get())
      return parent->getId();
  }
  return 0;
}
}  // namespace

int posix_acct(const char* path) {
  const AccountingResult result = configure(path);
  syscallError(result.error);
  return result.value;
}

void posix_account_process_exit(PosixProcess& process, const ProcessAccountingLifetime& lifetime) {
  PosixAccounting::Record record;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  record.version |= 128;
#endif
  const auto credentials = process.snapshotCredentials();
  record.uid = credentials.ruid;
  record.gid = credentials.rgid;
  record.pid = process.getId();
  record.parentPid = parentId(process);
  const uint64_t birth = lifetime.birth / Time::Multiplier::Second;
  record.birthSeconds = birth > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(birth);
  const uint64_t now = Time::getTicks();
  record.elapsedFloat = PosixAccounting::floatBits(
      (now > lifetime.started ? now - lifetime.started : 0) / PosixAccounting::TickNanoseconds);
  record.userTicks =
      PosixAccounting::compressed(process.getUserTime() / PosixAccounting::TickNanoseconds);
  record.systemTicks =
      PosixAccounting::compressed(process.getKernelTime() / PosixAccounting::TickNanoseconds);
  record.memory = PosixAccounting::compressed(lifetime.virtualKilobytes);
  {
    Process::FileContextLease terminalLease;
    File* terminal = process.acquireCtty(terminalLease);
    if (terminal && ConsoleManager::instance().isConsole(terminal)) {
      const size_t number = static_cast<ConsoleFile*>(terminal)->getConsoleNumber();
      if (number <= 255)
        record.terminal = static_cast<uint16_t>(0x8800 | number);
    }
  }
  record.exitStatus = process.getExitStatus();
  if (record.exitStatus & 0x7f)
    record.flags |= AXSIG;
  if (lifetime.forked && !process.hasExecCommitted())
    record.flags |= AFORK;
  const auto& description = process.description();
  size_t start = 0;
  for (size_t i = 0; i < description.length(); ++i) {
    if (description[i] == '/')
      start = i + 1;
  }
  for (size_t i = 0; i < sizeof(record.command) - 1 && start + i < description.length(); ++i)
    record.command[i] = description[start + i];
  const int previousError = Processor::information().getCurrentThread()->getErrno();
  {
    LockGuard<Mutex> guard(accountingLock);
    if (accountingFile)
      accountingFile.get()->append(record);
  }
  syscallError(previousError);
}

void posix_stop_accounting() {
  UniquePointer<AccountingFile> retired;
  {
    LockGuard<Mutex> guard(accountingLock);
    retired = pedigree_std::move(accountingFile);
  }
}

void PosixProcess::snapshotAccountingMemory() {
  // The sole exit owner calls this after peer retirement and before unmapping.
  const ssize_t pages = getVirtualPageCount();
  const size_t kilobytesPerPage = PhysicalMemoryManager::getPageSize() / 1024;
  m_AccountingLifetime.virtualKilobytes =
      pages > 0 ? static_cast<uint64_t>(pages) * kilobytesPerPage : 0;
}
