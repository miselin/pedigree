/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/processor/UserReturnFrame.h"

#include <config.h>

#if X64 && !HOSTED && THREADS
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/state.h"

static_assert(sizeof(X64UserEntryMetadata) == 32, "entry metadata prefix size");
static_assert(__builtin_offsetof(X64UserEntryMetadata, ds) == 0, "entry DS offset");
static_assert(__builtin_offsetof(X64UserEntryMetadata, es) == 2, "entry ES offset");
static_assert(__builtin_offsetof(X64UserEntryMetadata, fs) == 4, "entry FS offset");
static_assert(__builtin_offsetof(X64UserEntryMetadata, gs) == 6, "entry GS offset");
static_assert(__builtin_offsetof(X64UserEntryMetadata, fsBase) == 8, "entry FS base offset");
static_assert(__builtin_offsetof(X64UserEntryMetadata, gsBase) == 16, "entry GS base offset");
static_assert(__builtin_offsetof(X64UserEntryMetadata, origRax) == 24, "entry original RAX offset");
static_assert(sizeof(X64SyscallState) == 160, "syscall frame includes entry metadata");
static_assert(sizeof(X64InterruptState) == 208, "interrupt frame includes entry metadata");

namespace {
void copyMetadata(Amd64UserRegisters& output, const X64UserEntryMetadata& entry) {
  output.orig_rax = entry.origRax;
  output.fs_base = entry.fsBase;
  output.gs_base = entry.gsBase;
  output.ds = entry.ds;
  output.es = entry.es;
  output.fs = entry.fs;
  output.gs = entry.gs;
}

void capture(Amd64UserRegisters& output, const X64SyscallState& state) {
  output.r15 = state.getRegister(12);
  output.r14 = state.getRegister(11);
  output.r13 = state.getRegister(10);
  output.r12 = state.getRegister(9);
  output.rbp = state.getRegister(5);
  output.rbx = state.getRegister(1);
  output.r11 = state.getFlags();
  output.r10 = state.getRegister(8);
  output.r9 = state.getRegister(7);
  output.r8 = state.getRegister(6);
  output.rax = state.getRegister(0);
  output.rcx = state.getInstructionPointer();
  output.rdx = state.getRegister(2);
  output.rsi = state.getRegister(4);
  output.rdi = state.getRegister(3);
  output.rip = state.getInstructionPointer();
  // X64SyscallManager's STAR value selects these actual SYSRET selectors.
  output.cs = 0x2b;
  output.eflags = state.getFlags();
  output.rsp = state.getStackPointer();
  output.ss = 0x23;
  copyMetadata(output, state.getUserEntryMetadata());
}

void capture(Amd64UserRegisters& output, const X64InterruptState& state) {
  output.r15 = state.getRegister(14);
  output.r14 = state.getRegister(13);
  output.r13 = state.getRegister(12);
  output.r12 = state.getRegister(11);
  output.rbp = state.getRegister(6);
  output.rbx = state.getRegister(1);
  output.r11 = state.getRegister(10);
  output.r10 = state.getRegister(9);
  output.r9 = state.getRegister(8);
  output.r8 = state.getRegister(7);
  output.rax = state.getRegister(0);
  output.rcx = state.getRegister(2);
  output.rdx = state.getRegister(3);
  output.rsi = state.getRegister(5);
  output.rdi = state.getRegister(4);
  output.rip = state.getInstructionPointer();
  output.cs = state.getCodeSegment();
  output.eflags = state.getFlags();
  output.rsp = state.getStackPointer();
  output.ss = state.getStackSegment();
  copyMetadata(output, state.getUserEntryMetadata());
}
}  // namespace
#endif

bool UserReturnFrame::snapshot(UserRegisterSnapshot& output) const {
#if X64 && !HOSTED && THREADS
  // Selection, scope validation and the bounded copy observe one current task.
  // No hardware TLS read, allocator, user copy or blocking operation occurs here.
  EnsureInterrupts interrupts(false);
  if (Processor::information().getCurrentThread() != m_Owner ||
      m_Owner->currentUserReturnFrame() != this)
    return false;

  UserRegisterSnapshot next;
  if (m_Syscall) {
    if (m_Origin != Origin::Syscall && m_Origin != Origin::NewImage)
      return false;
    capture(next.amd64, *m_Syscall);
  } else if (m_Interrupt) {
    if ((m_Origin != Origin::Interrupt && m_Origin != Origin::SignalRestore) ||
        m_Interrupt->kernelMode())
      return false;
    capture(next.amd64, *m_Interrupt);
  } else {
    return false;
  }
  next.architecture = UserRegisterSnapshot::Architecture::Amd64;
  output = next;
  return true;
#else
  (void)output;
  return false;
#endif
}
