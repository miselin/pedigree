/* Copyright (c) 2026, Pedigree Developers. */
#include "clone3-syscalls.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/syscallError.h"

#include <sched.h>
#include <signal.h>

#include "PosixSubsystem.h"
#include "linux-clone-abi.h"
#include "system-syscalls.h"

namespace {
bool linuxUserRange(uint64_t address, uint64_t length) {
  auto& space = Processor::information().getVirtualAddressSpace();
  uint64_t limit = space.getKernelStart();
  if (limit > 0x0000800000000000ULL)
    limit = 0x0000800000000000ULL;
  return address >= space.getUserStart() && address < limit && length <= limit - address;
}
}  // namespace

long posix_clone3(SyscallState& state, const LinuxCloneArgs* userArgs, size_t size) {
  if (size > LinuxCloneAbi::MaximumSize) {
    SYSCALL_ERROR(TooBig);
    return -1;
  }
  if (size < LinuxCloneAbi::Version0Size) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  const uintptr_t userAddress = reinterpret_cast<uintptr_t>(userArgs);
  if (!linuxUserRange(userAddress, size)) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  // Linux accepts future versions only when every unknown byte is zero.
  // Read the extension in bounded pieces rather than consuming a kernel page.
  for (size_t offset = sizeof(LinuxCloneArgs); offset < size;) {
    uint8_t extension[64];
    size_t count = size - offset;
    if (count > sizeof(extension))
      count = sizeof(extension);
    if (!PosixSubsystem::copyFromUser(extension,
                                      reinterpret_cast<const void*>(userAddress + offset), count)) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    for (size_t n = 0; n < count; ++n) {
      if (extension[n]) {
        SYSCALL_ERROR(TooBig);
        return -1;
      }
    }
    offset += count;
  }

  LinuxCloneArgs args{};
  const size_t knownSize = size < sizeof(args) ? size : sizeof(args);
  if (!PosixSubsystem::copyFromUser(&args, userArgs, knownSize)) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }

  constexpr uint64_t AllowedFlags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                                    CLONE_VFORK | CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS |
                                    CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID |
                                    CLONE_CHILD_SETTID | CLONE_NEWUTS | LinuxCloneAbi::ClearSighand;
  const bool clearSignalHandlers = args.flags & LinuxCloneAbi::ClearSighand;
  const bool thread = args.flags & CLONE_THREAD;
  if ((args.flags & ~AllowedFlags) || args.pidfd || args.set_tid || args.set_tid_size ||
      args.cgroup || (clearSignalHandlers && (args.flags & (CLONE_SIGHAND | CLONE_THREAD))) ||
      args.exit_signal != static_cast<uint64_t>(thread ? 0 : SIGCHLD)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }

  const uint64_t maximumPointer = ~uintptr_t(0);
  if (((args.flags & CLONE_PARENT_SETTID) && args.parent_tid > maximumPointer) ||
      ((args.flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) &&
       args.child_tid > maximumPointer)) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if ((args.flags & CLONE_SETTLS) && args.tls > maximumPointer) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }

  uintptr_t stackTop = 0;
  if (args.stack || args.stack_size) {
    // The extent may include a guard page below the usable stack.
    if (!args.stack || !args.stack_size || !linuxUserRange(args.stack, args.stack_size)) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    stackTop = static_cast<uintptr_t>(args.stack + args.stack_size);
  }

  const unsigned long flags =
      static_cast<unsigned long>((args.flags & ~LinuxCloneAbi::ClearSighand) | args.exit_signal);
  return posix_clone(state, flags, reinterpret_cast<void*>(stackTop),
                     reinterpret_cast<int*>(static_cast<uintptr_t>(args.parent_tid)),
                     reinterpret_cast<int*>(static_cast<uintptr_t>(args.child_tid)),
                     static_cast<unsigned long>(args.tls), true, clearSignalHandlers);
}
