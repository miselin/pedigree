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
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/utility.h"

#include <signal.h>

#include "modules/subsys/posix/PosixSubsystem.h"
#include "modules/subsys/posix/signal-syscalls.h"
#include "modules/system/vfs/MemoryMappedFile.h"

bool runHostedUsercopyRegressions(Process* process) {
  size_t extent = 99;
  const size_t maximum = ~static_cast<size_t>(0);
  const bool overflowRejected =
      !PosixSubsystem::checkedUserBufferSize(maximum, 2, extent) && !extent;
  const bool zeroExtentAccepted = PosixSubsystem::checkedUserBufferSize(maximum, 0, extent) &&
                                  !extent && PosixSubsystem::copyFromUser(nullptr, nullptr, 0) &&
                                  PosixSubsystem::copyToUser(nullptr, nullptr, 0);

  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t mappingLength = pageSize * 2;
  uintptr_t address = 0;
  if (!process->getSpaceAllocator().allocate(mappingLength, address)) {
    ERROR("HOSTED-SYSCALL-TEST: FAIL usercopy: could not reserve userspace range");
    return false;
  }

  uintptr_t mappedAddress = address;
  MemoryMappedObject* mapping = MemoryMapManager::instance().mapAnon(
      mappedAddress, mappingLength,
      MemoryMappedObject::Read | MemoryMappedObject::Write | MemoryMappedObject::Exec);
  if (!mapping || mappedAddress != address) {
    MemoryMapManager::instance().remove(address, mappingLength);
    process->getSpaceAllocator().free(address, mappingLength);
    ERROR("HOSTED-SYSCALL-TEST: FAIL usercopy: could not map userspace range");
    return false;
  }

  const uint8_t source[] = {0x31, 0x42, 0x53, 0x64, 0x75, 0x86};
  uint8_t result[sizeof(source)] = {};
  void* userBuffer = reinterpret_cast<void*>(address + pageSize - 3);

  const bool roundTrip = PosixSubsystem::copyToUser(userBuffer, source, sizeof(source)) &&
                         PosixSubsystem::copyFromUser(result, userBuffer, sizeof(result)) &&
                         !MemoryCompare(source, result, sizeof(source));
  const char sourceString[] = {'u', 's', 'e', 'r', 0};
  String copiedString;
  const bool stringSnapshot =
      PosixSubsystem::copyToUser(userBuffer, sourceString, sizeof(sourceString)) &&
      PosixSubsystem::copyUserString(reinterpret_cast<const char*>(userBuffer), copiedString,
                                     sizeof(sourceString)) == PosixSubsystem::UserStringSuccess &&
      copiedString == String("user");
  const bool executableUserAccepted = PosixSubsystem::checkAddress(
      reinterpret_cast<uintptr_t>(userBuffer), 1, PosixSubsystem::SafeExecute);
  const uintptr_t kernelStart = Processor::information().getVirtualAddressSpace().getKernelStart();
  const bool kernelPointerRejected =
      !PosixSubsystem::copyFromUser(result, reinterpret_cast<const void*>(kernelStart), 1) &&
      !PosixSubsystem::checkAddress(kernelStart, 1, PosixSubsystem::SafeExecute);
  const bool nullPointersRejected = !PosixSubsystem::copyFromUser(nullptr, userBuffer, 1) &&
                                    !PosixSubsystem::copyFromUser(result, nullptr, 1) &&
                                    !PosixSubsystem::copyToUser(nullptr, source, 1) &&
                                    !PosixSubsystem::copyToUser(userBuffer, nullptr, 1);

  Thread* currentThread = Processor::information().getCurrentThread();
  const uint64_t originalMask = currentThread->getSignalMask();
  const uint64_t requestedMask = (static_cast<uint64_t>(1) << (SIGUSR1 - 1)) |
                                 (static_cast<uint64_t>(1) << (SIGKILL - 1)) |
                                 (static_cast<uint64_t>(1) << (SIGSTOP - 1));
  void* userMask = reinterpret_cast<void*>(address + 64);
  void* userOldMask = reinterpret_cast<void*>(address + 128);
  uint64_t returnedMask = 0;
  const bool signalMaskSnapshots =
      PosixSubsystem::copyToUser(userMask, &requestedMask, sizeof(requestedMask)) &&
      posix_sigprocmask(SIG_SETMASK, userMask, userOldMask, sizeof(uint64_t), true) == 0 &&
      PosixSubsystem::copyFromUser(&returnedMask, userOldMask, sizeof(returnedMask)) &&
      returnedMask == originalMask &&
      currentThread->getSignalMask() ==
          (requestedMask & ~((static_cast<uint64_t>(1) << (SIGKILL - 1)) |
                             (static_cast<uint64_t>(1) << (SIGSTOP - 1))));
  currentThread->setSignalMask(originalMask);

  const Thread::AlternateSignalStack originalStack = currentThread->getAlternateSignalStack();
  stack_t disableStack = {};
  disableStack.ss_flags = SS_DISABLE;
  void* userStack = reinterpret_cast<void*>(address + 256);
  void* userOldStack = reinterpret_cast<void*>(address + 384);
  stack_t returnedStack = {};
  const int expectedOldStackFlags =
      originalStack.enabled ? (originalStack.inUse ? SS_ONSTACK : 0) : SS_DISABLE;
  const bool alternateStackSnapshots =
      PosixSubsystem::copyToUser(userStack, &disableStack, sizeof(disableStack)) &&
      posix_sigaltstack(reinterpret_cast<const stack_t*>(userStack),
                        reinterpret_cast<stack_t*>(userOldStack)) == 0 &&
      PosixSubsystem::copyFromUser(&returnedStack, userOldStack, sizeof(returnedStack)) &&
      returnedStack.ss_flags == expectedOldStackFlags &&
      (!originalStack.enabled ||
       (returnedStack.ss_sp == reinterpret_cast<void*>(originalStack.base) &&
        returnedStack.ss_size == originalStack.size)) &&
      !currentThread->getAlternateSignalStack().enabled;
  currentThread->getAlternateSignalStack() = originalStack;

  size_t checkedExtent = 99;
  const bool arrayRangeAccepted = PosixSubsystem::checkUserBuffer(
      reinterpret_cast<uintptr_t>(userBuffer), 2, 3, PosixSubsystem::SafeRead, &checkedExtent);
  const bool overflowRangeRejected =
      !PosixSubsystem::checkUserBuffer(address, maximum, 2, PosixSubsystem::SafeRead,
                                       &checkedExtent) &&
      !checkedExtent;
  const bool reducedPermissionsRejected =
      MemoryMapManager::instance().setPermissions(address, mappingLength,
                                                  MemoryMappedObject::Read) == 1 &&
      !PosixSubsystem::copyToUser(userBuffer, source, 1) &&
      PosixSubsystem::copyFromUser(result, userBuffer, 1) &&
      !PosixSubsystem::checkAddress(reinterpret_cast<uintptr_t>(userBuffer), 1,
                                    PosixSubsystem::SafeExecute);
  currentThread->setErrno(0);
  const uint64_t maskBeforeRejectedOutput = currentThread->getSignalMask();
  const bool signalMaskReadOnlyOutputRejected =
      posix_sigprocmask(SIG_SETMASK, userMask, userOldMask, sizeof(uint64_t), true) == -1 &&
      currentThread->getErrno() == Error::BadAddress &&
      currentThread->getSignalMask() == maskBeforeRejectedOutput;
  currentThread->setSignalMask(originalMask);
  currentThread->setErrno(0);
  const bool alternateStackReadOnlyOutputRejected =
      posix_sigaltstack(reinterpret_cast<const stack_t*>(userStack),
                        reinterpret_cast<stack_t*>(userOldStack)) == -1 &&
      currentThread->getErrno() == Error::BadAddress &&
      currentThread->getAlternateSignalStack().base == originalStack.base &&
      currentThread->getAlternateSignalStack().size == originalStack.size &&
      currentThread->getAlternateSignalStack().enabled == originalStack.enabled &&
      currentThread->getAlternateSignalStack().inUse == originalStack.inUse;
  currentThread->getAlternateSignalStack() = originalStack;

  const size_t removed = MemoryMapManager::instance().remove(address + pageSize, pageSize);
  const bool incompleteRangeRejected =
      removed == 1 && !PosixSubsystem::copyFromUser(result, userBuffer, sizeof(result));
  currentThread->setErrno(0);
  const uint64_t maskBeforeRejectedCopy = currentThread->getSignalMask();
  const bool signalMaskIncompleteRangeRejected =
      posix_sigprocmask(SIG_SETMASK, userBuffer, nullptr, sizeof(uint64_t), true) == -1 &&
      currentThread->getErrno() == Error::BadAddress &&
      currentThread->getSignalMask() == maskBeforeRejectedCopy;
  currentThread->setErrno(0);
  const bool alternateStackIncompleteRangeRejected =
      posix_sigaltstack(reinterpret_cast<const stack_t*>(userBuffer), nullptr) == -1 &&
      currentThread->getErrno() == Error::BadAddress &&
      currentThread->getAlternateSignalStack().base == originalStack.base &&
      currentThread->getAlternateSignalStack().size == originalStack.size &&
      currentThread->getAlternateSignalStack().enabled == originalStack.enabled &&
      currentThread->getAlternateSignalStack().inUse == originalStack.inUse;
  currentThread->setErrno(0);

  MemoryMapManager::instance().remove(address, pageSize);
  process->getSpaceAllocator().free(address, mappingLength);

  const bool passed = overflowRejected && zeroExtentAccepted && roundTrip && stringSnapshot &&
                      executableUserAccepted && kernelPointerRejected && nullPointersRejected &&
                      signalMaskSnapshots && alternateStackSnapshots && arrayRangeAccepted &&
                      checkedExtent == 0 && overflowRangeRejected && reducedPermissionsRejected &&
                      signalMaskReadOnlyOutputRejected && alternateStackReadOnlyOutputRejected &&
                      incompleteRangeRejected && signalMaskIncompleteRangeRejected &&
                      alternateStackIncompleteRangeRejected;
  if (!passed) {
    ERROR(
        "HOSTED-SYSCALL-TEST: FAIL usercopy: overflow, range, permission, or copy contract "
        "regressed");
    return false;
  }

  NOTICE("HOSTED-SYSCALL-TEST: PASS usercopy");
  return true;
}
