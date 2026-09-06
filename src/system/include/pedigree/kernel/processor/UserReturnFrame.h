/* Copyright (c) 2026, Pedigree Developers. */
#ifndef KERNEL_PROCESSOR_USER_RETURN_FRAME_H
#define KERNEL_PROCESSOR_USER_RETURN_FRAME_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"

class Thread;

struct Amd64UserRegisters {
  uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
  uint64_t rax, rcx, rdx, rsi, rdi, orig_rax, rip;
  uint64_t cs, eflags, rsp, ss, fs_base, gs_base, ds, es, fs, gs;
};

static_assert(sizeof(Amd64UserRegisters) == 27 * sizeof(uint64_t), "amd64 register count");
static_assert(sizeof(Amd64UserRegisters) == 216, "amd64 NT_PRSTATUS payload size");
static_assert(__builtin_offsetof(Amd64UserRegisters, orig_rax) == 120, "orig_rax ABI offset");
static_assert(__builtin_offsetof(Amd64UserRegisters, rip) == 128, "rip ABI offset");
static_assert(__builtin_offsetof(Amd64UserRegisters, fs_base) == 168, "fs_base ABI offset");
static_assert(__builtin_offsetof(Amd64UserRegisters, gs) == 208, "last register ABI offset");

struct UserRegisterSnapshot {
  enum class Architecture : uint64_t { Unavailable, Amd64 };
  Architecture architecture = Architecture::Unavailable;
  // Only this payload is the amd64 ABI, not the architecture tag or wrapper.
  Amd64UserRegisters amd64{};
};

class EXPORTED_PUBLIC UserReturnFrame {
  friend class Thread;
  friend class PerProcessorScheduler;

 public:
  enum class Origin { Syscall, Interrupt, SignalRestore, NewImage };

  // The source must come from a metadata-aware entry or synthetic-frame path.
  UserReturnFrame(Thread& owner, const SyscallState& state, Origin origin = Origin::Syscall)
      : m_Owner(&owner), m_Origin(origin), m_Syscall(&state), m_Interrupt(nullptr) {}
  UserReturnFrame(Thread& owner, const InterruptState& state, Origin origin = Origin::Interrupt)
      : m_Owner(&owner), m_Origin(origin), m_Syscall(nullptr), m_Interrupt(&state) {}

  UserReturnFrame(const UserReturnFrame&) = delete;
  UserReturnFrame& operator=(const UserReturnFrame&) = delete;
  UserReturnFrame(UserReturnFrame&&) = delete;
  UserReturnFrame& operator=(UserReturnFrame&&) = delete;

  Origin origin() const {
    return m_Origin;
  }

  // Requires the owning Thread's active lexical frame scope. Failure leaves
  // output unchanged; no live state pointer or partial register set escapes.
  bool snapshot(UserRegisterSnapshot& output) const;

 private:
  Thread* const m_Owner;
  const Origin m_Origin;
  const SyscallState* const m_Syscall;
  const InterruptState* const m_Interrupt;
  bool m_Terminal = false;
};

#endif
