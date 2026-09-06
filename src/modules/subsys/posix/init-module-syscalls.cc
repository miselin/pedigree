/* Copyright (c) 2026, Pedigree Developers. */
#include "init-module-syscalls.h"
#include "pedigree/kernel/linker/KernelElf.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/syscallError.h"

#include "PosixSubsystem.h"

int posix_init_module(const void* image, size_t length, const char* parameters) {
  TerminationDeferral lifetime;
  Thread* thread = Processor::information().getCurrentThread();
  if (thread->getParent()->getEffectiveUserId() != 0) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }
  // Native Pedigree modules have no Linux module-parameter parser.
  char first = 0;
  if (!PosixSubsystem::copyFromUser(&first, parameters, sizeof(first))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (first) {
    SYSCALL_ERROR(OperationNotSupported);
    return -1;
  }
  auto& kernel = KernelElf::instance();
  using Result = KernelElf::RuntimeLoadResult;
  Result result = Result::Busy;
  bool copyFailed = false;
  {
    KernelElf::RuntimeLoad load;
    result = kernel.beginRuntimeModuleLoad(length, load);
    if (result == Result::Ready) {
      copyFailed = !PosixSubsystem::copyFromUser(load.data(), image, length);
      if (!copyFailed)
        result = kernel.loadModuleRuntime(load);
    }
  }
  // Failed entry cleanup may itself call interfaces which set errno.
  if (copyFailed) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  switch (result) {
    case Result::Loaded:
      thread->setErrno(0);
      return 0;
    case Result::InvalidImage:
    case Result::UnsupportedImage:
    case Result::ImageTooLarge:
      SYSCALL_ERROR(ExecFormatError);
      break;
    case Result::NoMemory:
      SYSCALL_ERROR(OutOfMemory);
      break;
    case Result::Shutdown:
      SYSCALL_ERROR(NotEnoughPermissions);
      break;
    case Result::Duplicate:
      SYSCALL_ERROR(FileExists);
      break;
    case Result::MissingDependency:
      SYSCALL_ERROR(DoesNotExist);
      break;
    case Result::EntryFailed:
      SYSCALL_ERROR(DeviceDoesNotExist);
      break;
    case Result::ProtectionFailed:
      SYSCALL_ERROR(IoError);
      break;
    case Result::Ready:
    case Result::Busy:
      SYSCALL_ERROR(DeviceBusy);
      break;
  }
  return -1;
}
