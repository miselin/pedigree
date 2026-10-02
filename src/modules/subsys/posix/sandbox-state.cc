/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/SyscallManager.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/Pointers.h"

#include <signal.h>

#include "PosixSubsystem.h"
#include "landlock.h"
#include "sandbox-state.h"
#include "seccomp-filter.h"
#include "syscalls/translate.h"

namespace {
constexpr uint32_t Allow = 0x7fff0000;
constexpr uint32_t Errno = 0x00050000;
constexpr uint32_t KillThread = 0;
constexpr uint32_t ActionMask = 0xffff0000;
constexpr size_t MaximumFilterPath = 32768;
constexpr size_t MaximumFilters = 64;

struct SeccompProgram {
  UniqueArray<PosixSeccomp::Instruction> instructions;
  size_t length = 0;
  size_t totalLength = 0;
  size_t depth = 0;
  SharedPointer<SeccompProgram> previous;
};

class SandboxState final : public Thread::SecurityState {
 public:
  bool noNewPrivileges = false;
  SharedPointer<SeccompProgram> filters;
  SharedPointer<LandlockDomain> domain;
  bool interceptSyscall(SyscallState& state, uintptr_t& result) const override;
};

Thread& current() {
  return *Processor::information().getCurrentThread();
}

Thread::SecurityStateRef snapshot() {
  return current().securityState();
}

Thread::SecurityStateRef copyState(const Thread::SecurityStateRef& previous) {
  auto* replacement = new SandboxState;
  if (!replacement) {
    SYSCALL_ERROR(OutOfMemory);
    return {};
  }
  if (previous) {
    *replacement = *static_cast<SandboxState*>(previous.get());
  }
  auto owner = Thread::SecurityStateRef::tryAdopt(replacement);
  if (!owner) {
    SYSCALL_ERROR(OutOfMemory);
  }
  return owner;
}

uint32_t architecture() {
#if ARM64
  return 0xc00000b7;
#elif ARMV7
  return 0x40000028;
#else
  // Personality affects uname, not the x86-64 syscall entry convention.
  return 0xc000003e;
#endif
}

bool restrictedOperation(uint64_t number) {
  switch (posix_translate_syscall(number)) {
    case POSIX_MOUNT:
    case POSIX_UMOUNT2:
    case POSIX_PIVOT_ROOT:
    case POSIX_PTRACE:
    case POSIX_PROCESS_VM_READV:
    case POSIX_PROCESS_VM_WRITEV:
    case POSIX_OPEN_BY_HANDLE_AT:
    case POSIX_INIT_MODULE:
    case POSIX_DELETE_MODULE:
    case POSIX_SWAPON:
    case POSIX_SWAPOFF:
    case POSIX_REBOOT:
    case POSIX_ACCT:
    case POSIX_QUOTACTL:
      return true;
    default:
      return false;
  }
}

bool SandboxState::interceptSyscall(SyscallState& state, uintptr_t& result) const {
  if (!filters && !domain) {
    return false;
  }
  // The Linux filter is a policy over Linux numbers and argument layouts.
  // Other services must not provide a second, unfiltered way to perform I/O.
  if (state.getSyscallService() != linuxCompat) {
    result = uintptr_t(-1);
    SYSCALL_ERROR(NotEnoughPermissions);
    return true;
  }
  uint32_t decision = Allow;
  if (filters) {
    PosixSeccomp::Data data = {};
    data.nr = static_cast<int32_t>(state.getSyscallNumber());
    data.arch = architecture();
    data.instructionPointer = state.getInstructionPointer();
    for (size_t i = 0; i < 6; ++i) {
      data.args[i] = state.getSyscallParameter(6 + i);
    }
    for (auto filter = filters; filter; filter = filter->previous) {
      const uint32_t value =
          PosixSeccomp::evaluate(filter->instructions.get(), filter->length, data);
      // Linux action precedence is the signed ordering of the action word.
      if (static_cast<int32_t>(value & ActionMask) < static_cast<int32_t>(decision & ActionMask)) {
        decision = value;
      }
    }
  }
  if ((decision & ActionMask) == Allow) {
    if (!domain || !restrictedOperation(state.getSyscallNumber())) {
      return false;
    }
    SYSCALL_ERROR(NotEnoughPermissions);
    result = uintptr_t(-1);
    return true;
  }
  if ((decision & ActionMask) == Errno) {
    const unsigned int error = decision & 0xffff;
    current().setErrno(error > 4095 ? 4095 : error);
    result = error ? uintptr_t(-1) : 0;
    return true;
  }
  // Unsupported actions fail closed. No listener/tracer action is advertised.
  if ((decision & ActionMask) == KillThread && !current().getParent()->prepareThreadExit()) {
    if (!SyscallManager::instance().requestThreadExit()) {
      FATAL("seccomp thread exit could not be dispatched");
    }
  } else {
    current().deferSignalExit(SIGSYS);
  }
  current().setErrno(0);
  result = 0;
  return true;
}
}  // namespace

bool posix_no_new_privs() {
  auto owner = snapshot();
  return owner && static_cast<SandboxState*>(owner.get())->noNewPrivileges;
}

int posix_set_no_new_privs() {
  TerminationDeferral lifetime;
  auto old = snapshot();
  if (old && static_cast<SandboxState*>(old.get())->noNewPrivileges) {
    current().setErrno(0);
    return 0;
  }
  auto next = copyState(old);
  if (!next) {
    return -1;
  }
  static_cast<SandboxState*>(next.get())->noNewPrivileges = true;
  current().setSecurityState(next);
  current().setErrno(0);
  return 0;
}

int posix_seccomp_mode() {
  auto owner = snapshot();
  current().setErrno(0);
  return owner && static_cast<SandboxState*>(owner.get())->filters ? 2 : 0;
}

void posix_sandbox_inherit(Thread& child, Thread& parent) {
  child.setSecurityState(parent.securityState());
}

SharedPointer<LandlockDomain> posix_sandbox_domain() {
  auto owner = snapshot();
  return owner ? static_cast<SandboxState*>(owner.get())->domain : SharedPointer<LandlockDomain>();
}

bool posix_sandbox_restrict(const SharedPointer<LandlockDomain>& domain) {
  TerminationDeferral lifetime;
  if (!domain || !posix_no_new_privs()) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return false;
  }
  auto next = copyState(snapshot());
  if (!next) {
    return false;
  }
  static_cast<SandboxState*>(next.get())->domain = domain;
  current().setSecurityState(next);
  return true;
}

int posix_seccomp(unsigned int operation, unsigned int flags, const void* args) {
  TerminationDeferral lifetime;
  if (flags) {
    // TSYNC and notification listeners need separate lifecycle support.
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (operation == 2) {  // SECCOMP_GET_ACTION_AVAIL
    uint32_t action = 0;
    if (!PosixSubsystem::copyFromUser(&action, args, sizeof(action))) {
      SYSCALL_ERROR(BadAddress);
      return -1;
    }
    if (action != Allow && action != Errno && action != KillThread &&
        action != PosixSeccomp::KillProcess) {
      SYSCALL_ERROR(OperationNotSupported);
      return -1;
    }
    current().setErrno(0);
    return 0;
  }
  if (operation != 1) {  // SECCOMP_SET_MODE_FILTER
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  if (!posix_no_new_privs()) {
    SYSCALL_ERROR(PermissionDenied);
    return -1;
  }
  struct Program {
    uint16_t length;
    const PosixSeccomp::Instruction* instructions;
  } program = {};
  if (!PosixSubsystem::copyFromUser(&program, args, sizeof(program))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (!program.length || program.length > PosixSeccomp::MaximumInstructions) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  auto old = snapshot();
  auto previous = static_cast<SandboxState*>(old.get())->filters;
  const size_t total = program.length + (previous ? previous->totalLength + 4 : 0);
  const size_t depth = previous ? previous->depth + 1 : 1;
  if (total > MaximumFilterPath || depth > MaximumFilters) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  auto filter = SharedPointer<SeccompProgram>::tryAllocate();
  if (!filter) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  filter->instructions = UniqueArray<PosixSeccomp::Instruction>::allocate(program.length);
  if (!filter->instructions) {
    SYSCALL_ERROR(OutOfMemory);
    return -1;
  }
  if (!PosixSubsystem::copyFromUser(filter->instructions.get(), program.instructions,
                                    program.length * sizeof(PosixSeccomp::Instruction))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (!PosixSeccomp::validate(filter->instructions.get(), program.length)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  filter->length = program.length;
  filter->totalLength = total;
  filter->depth = depth;
  filter->previous = previous;
  auto next = copyState(old);
  if (!next) {
    return -1;
  }
  static_cast<SandboxState*>(next.get())->filters = filter;
  current().setSecurityState(next);
  current().setErrno(0);
  return 0;
}
