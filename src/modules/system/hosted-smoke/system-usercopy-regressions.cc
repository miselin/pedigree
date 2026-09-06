/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/errors.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/utilities/lib.h"

#include <limits.h>

#include "modules/subsys/posix/PosixProcess.h"
#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/system-syscalls.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include <sys/utsname.h>

namespace {
struct UserFixture {
  utsname name;
  uid_t uid[3];
  gid_t gid[3];
  gid_t groups[3];
  unsigned long tls;
  uint32_t capVersion;
  int capPid;
  uint32_t capabilities[3];
  const char* arguments[2];
  char executable[64];
};
struct Context {
  bool passed = false;
};

int usercopyWorker(void* parameter) {
  Context* context = reinterpret_cast<Context*>(parameter);
  Thread* thread = Processor::information().getCurrentThread();
  auto* process = static_cast<PosixProcess*>(thread->getParent());
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t length = PosixSubsystem::MaximumExecArgumentBytes + 2 * pageSize;
  uintptr_t address = 0;
  if (!process->allocateUserRange(Process::UserRegion::Normal, length, address)) {
    return 1;
  }
  uintptr_t mappedAddress = address;
  MemoryMapManager& mappings = MemoryMapManager::instance();
  if (!mappings.mapAnon(mappedAddress, length,
                        MemoryMappedObject::Read | MemoryMappedObject::Write) ||
      mappedAddress != address) {
    return 1;
  }
  UserFixture initial = {};
  initial.groups[0] = 17;
  initial.groups[1] = 29;
  initial.capVersion = 0x19980330;
  StringCopy(initial.executable, "/__pedigree_missing_usercopy_fixture__");
  auto* fixture = reinterpret_cast<UserFixture*>(address);
  bool passed = PosixSubsystem::copyToUser(fixture, &initial, sizeof(initial));
  process->setUserId(101);
  process->setEffectiveUserId(102);
  process->setSavedUserId(103);
  process->setGroupId(201);
  process->setEffectiveGroupId(202);
  process->setSavedGroupId(203);
  passed &= posix_getuid() == 101 && posix_geteuid() == 102 && posix_getgid() == 201 &&
            posix_getegid() == 202;
  passed &= posix_getresuid(&fixture->uid[0], &fixture->uid[1], &fixture->uid[2]) == 0 &&
            fixture->uid[0] == 101 && fixture->uid[1] == 102 && fixture->uid[2] == 103;
  passed &= posix_getresgid(&fixture->gid[0], &fixture->gid[1], &fixture->gid[2]) == 0 &&
            fixture->gid[0] == 201 && fixture->gid[1] == 202 && fixture->gid[2] == 203;
  passed &= posix_uname(&fixture->name) == 0 && fixture->name.sysname[0] == 'P' &&
            fixture->name.nodename[0] == 'p';
  thread->setErrno(0);
  passed &= posix_uname(nullptr) == -1 && thread->getErrno() == Error::BadAddress;
  passed &= posix_setgroups(2, fixture->groups) == 0 && posix_getgroups(0, nullptr) == 2;
  thread->setErrno(0);
  passed &=
      posix_getgroups(1, fixture->groups) == -1 && thread->getErrno() == Error::InvalidArgument;
  passed &= posix_getgroups(3, fixture->groups) == 2 && fixture->groups[0] == 17 &&
            fixture->groups[1] == 29;
  thread->setErrno(0);
  passed &= posix_setgroups(NGROUPS_MAX + 1, fixture->groups) == -1 &&
            thread->getErrno() == Error::InvalidArgument;
  thread->setErrno(0);
  passed &= posix_setgroups(1, nullptr) == -1 && thread->getErrno() == Error::BadAddress;
  passed &= posix_setgroups(0, nullptr) == 0 && posix_getgroups(0, nullptr) == 0;
  const unsigned long savedTls = thread->getTlsBase();
  passed &= posix_arch_prctl(0x1003, reinterpret_cast<unsigned long>(&fixture->tls)) == 0 &&
            fixture->tls == savedTls;
  thread->setErrno(0);
  passed &= posix_arch_prctl(0x1003, 0) == -1 && thread->getErrno() == Error::BadAddress;
  thread->setErrno(0);
  passed &= posix_arch_prctl(0x1002, process->getAddressSpace()->getKernelStart()) == -1 &&
            thread->getErrno() == Error::NotEnoughPermissions && thread->getTlsBase() == savedTls;
  const unsigned long replacementTls = reinterpret_cast<unsigned long>(&fixture->tls);
  const bool changedTls = posix_arch_prctl(0x1002, replacementTls) == 0 &&
                          thread->getTlsBase() == replacementTls && fixture->tls == savedTls;
  const bool clearedTls = posix_arch_prctl(0x1002, 0) == 0 && thread->getTlsBase() == 0;
  const bool restoredTls = posix_arch_prctl(0x1002, savedTls) == 0;
  passed &= changedTls && clearedTls && restoredTls;
  passed &= posix_capget(&fixture->capVersion, fixture->capabilities) == 0 &&
            fixture->capabilities[0] == 0xFFFFFFFF && fixture->capabilities[1] == 0xFFFFFFFF &&
            fixture->capabilities[2] == 0xFFFFFFFF;
  passed &= posix_capset(&fixture->capVersion, fixture->capabilities) == 0;
  thread->setErrno(0);
  passed &=
      posix_capset(&fixture->capVersion, nullptr) == -1 && thread->getErrno() == Error::BadAddress;
  fixture->capVersion = 0;
  thread->setErrno(0);
  passed &= posix_capget(&fixture->capVersion, nullptr) == -1 &&
            thread->getErrno() == Error::InvalidArgument && fixture->capVersion == 0x19980330;

  SyscallState state = {};
  thread->setErrno(0);
  passed &= posix_execve(fixture->executable, nullptr, nullptr, state) == -1 &&
            thread->getErrno() == Error::DoesNotExist;
  fixture->arguments[0] = fixture->executable;
  thread->setErrno(0);
  passed &= posix_execve(fixture->executable, fixture->arguments, nullptr, state) == -1 &&
            thread->getErrno() == Error::DoesNotExist;
  char* oversized = reinterpret_cast<char*>(address + pageSize);
  const size_t argumentBytes = PosixSubsystem::MaximumExecArgumentBytes;
  char* content = new char[argumentBytes + 1];
  ByteSet(content, 'a', argumentBytes);
  content[argumentBytes] = 0;
  passed &= PosixSubsystem::copyToUser(oversized, content, argumentBytes + 1);
  delete[] content;
  fixture->arguments[0] = oversized;
  thread->setErrno(0);
  passed &= posix_execve(fixture->executable, fixture->arguments, nullptr, state) == -1 &&
            thread->getErrno() == Error::TooBig;

  mappings.remove(address, length);
  process->freeUserRange(Process::UserRegion::Normal, address, length);
  context->passed = passed;
  return passed ? 0 : 1;
}
}  // namespace

bool runHostedSystemUsercopyRegressions(Process* kernelProcess) {
  PosixProcess* process = new PosixProcess(kernelProcess);
  process->setSubsystem(new PosixSubsystem);
  process->publish();
  Context context;
  Thread* worker = new Thread(process, usercopyWorker, &context, nullptr, false, true);
  const bool joined = worker->joinForCompletion();
  const bool passed = joined && context.passed;
  delete process;
  if (passed) {
    NOTICE("HOSTED-SYSCALL-TEST: PASS system-usercopy-contracts");
  } else {
    ERROR("HOSTED-SYSCALL-TEST: FAIL system-usercopy-contracts");
  }
  return passed;
}
