/* Copyright (c) 2026, Pedigree Developers. */
#include "module-syscalls.h"
#include "pedigree/kernel/linker/KernelElf.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/Uninterruptible.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include "PosixSubsystem.h"

int posix_delete_module(const char* name, unsigned int flags) {
  Uninterruptible lifetime;
  Thread* thread = Processor::information().getCurrentThread();
  if (thread->getParent()->getEffectiveUserId() != 0) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }
  constexpr unsigned int nonblocking = 0x800;
  constexpr unsigned int force = 0x200;
  if (flags & ~(nonblocking | force)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (flags & force) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }
  // Linux includes the terminator in MODULE_NAME_LEN.
  constexpr size_t nameBytes = 64 - sizeof(unsigned long);
  String copiedName;
  const auto copied = PosixSubsystem::copyUserString(name, copiedName, nameBytes);
  if (copied != PosixSubsystem::UserStringSuccess) {
    syscallError(copied == PosixSubsystem::UserStringTooLong ? Error::DoesNotExist
                                                             : Error::BadAddress);
    return -1;
  }
  if (!copiedName.length()) {
    SYSCALL_ERROR(DoesNotExist);
    return -1;
  }

  using Result = KernelElf::RuntimeUnloadResult;
  const auto result = KernelElf::instance().unloadModuleRuntime(copiedName.cstr());
  switch (result) {
    case Result::Unloaded:
      // Module cleanup may use kernel interfaces which set the caller's errno.
      thread->setErrno(0);
      return 0;
    case Result::NotFound:
      SYSCALL_ERROR(DoesNotExist);
      break;
    case Result::Busy:
    case Result::Pinned:
      SYSCALL_ERROR(DeviceBusy);
      break;
    case Result::DependedOn:
      SYSCALL_ERROR(NoMoreProcesses);
      break;
    case Result::Shutdown:
      SYSCALL_ERROR(NotEnoughPermissions);
      break;
  }
  return -1;
}
