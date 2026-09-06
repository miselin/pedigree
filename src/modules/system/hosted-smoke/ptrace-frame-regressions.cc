/* Copyright (c) 2026, Pedigree Developers. */
#include <config.h>

#if PEDIGREE_PTRACE_TESTS && THREADS
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/UserReturnFrame.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/utilities/utility.h"

namespace {
bool check(bool condition, const char* detail) {
  if (!condition)
    ERROR("PTRACE-FRAME-CORE: FAIL " << detail);
  return condition;
}

bool equalBytes(const void* left, const void* right, size_t count) {
  const auto* a = static_cast<const unsigned char*>(left);
  const auto* b = static_cast<const unsigned char*>(right);
  for (size_t i = 0; i < count; ++i)
    if (a[i] != b[i])
      return false;
  return true;
}

bool scopes(Thread& thread) {
  SyscallState state;
  ByteSet(&state, 0, sizeof(state));
  UserReturnFrame outer(thread, state);
  bool passed = check(!thread.currentUserReturnFrame(), "initial frame slot");
  {
    Thread::UserReturnFrameScope outerScope(thread, outer);
    passed &= check(thread.currentUserReturnFrame() == &outer, "outer frame publication");
    UserReturnFrame inner(thread, state);
    {
      Thread::UserReturnFrameScope innerScope(thread, inner);
      passed &= check(thread.currentUserReturnFrame() == &inner, "same-level frame override");
    }
    passed &= check(thread.currentUserReturnFrame() == &outer, "same-level frame restoration");
    const size_t level = thread.getStateLevel();
    if (!thread.pushState())
      return false;
    passed &= check(!thread.currentUserReturnFrame(), "nested kernel state excludes outer frame");
    {
      Thread::UserReturnFrameScope nestedScope(thread, inner);
      passed &= check(thread.currentUserReturnFrame() == &inner, "nested frame publication");
      thread.abandonCurrentState(false);
      passed &= check(thread.getStateLevel() == level && thread.currentUserReturnFrame() == &outer,
                      "abandoned nested frame restored before storage retirement");
    }
    passed &= check(thread.currentUserReturnFrame() == &outer, "discarded scope destructor inert");
  }
  passed &= check(!thread.currentUserReturnFrame(), "final frame slot cleared");
  if (passed)
    NOTICE("PTRACE-FRAME-CORE: PASS scope lifetime");
  return passed;
}

bool legacyAdmission(Thread& thread) {
  const bool pin = thread.tryPinLegacyUserCallbacks();
  bool passed = check(pin, "legacy callback pin admission");
  if (!pin)
    return false;
  const bool unexpectedRequirement = thread.tryRequireSignalFrames();
  passed &= check(!unexpectedRequirement, "legacy callback excludes tracing frame requirement");
  if (unexpectedRequirement)
    thread.clearSignalFrameRequirement();
  thread.unpinLegacyUserCallbacks();
  const bool required = thread.tryRequireSignalFrames();
  passed &= check(required, "frame requirement after last callback pin");
  if (required) {
    const bool unexpectedPin = thread.tryPinLegacyUserCallbacks();
    passed &= check(!unexpectedPin && thread.requiresSignalFrames(),
                    "frame requirement excludes callback");
    if (unexpectedPin)
      thread.unpinLegacyUserCallbacks();
    thread.clearSignalFrameRequirement();
  }
  if (passed)
    NOTICE("PTRACE-FRAME-CORE: PASS legacy admission exclusion");
  return passed;
}

#if X64 && !HOSTED
extern "C" void pedigree_capture_user_entry(X64UserEntryMetadata*);
extern "C" void pedigree_restore_user_entry(const X64UserEntryMetadata*);

bool metadataRoundTrip() {
  X64UserEntryMetadata saved = {}, requested = {}, observed = {};
  bool preservedKernelBase;
  {
    const bool interruptsWereEnabled = Processor::getInterrupts();
    Processor::setInterrupts(false);
    uint32_t beforeLow, beforeHigh, afterLow, afterHigh;
    asm volatile("rdmsr" : "=a"(beforeLow), "=d"(beforeHigh) : "c"(0xc0000102));
    pedigree_capture_user_entry(&saved);
    requested.ds = requested.es = 0x23;
    requested.fsBase = 0x12345000;
    requested.gsBase = 0x23456000;
    pedigree_restore_user_entry(&requested);
    pedigree_capture_user_entry(&observed);
    // No logging, allocation or scheduling occurs with these test bases live.
    pedigree_restore_user_entry(&saved);
    asm volatile("rdmsr" : "=a"(afterLow), "=d"(afterHigh) : "c"(0xc0000102));
    preservedKernelBase = beforeLow == afterLow && beforeHigh == afterHigh;
    Processor::setInterrupts(interruptsWereEnabled);
  }
  const bool passed =
      check(equalBytes(&requested, &observed, sizeof(requested)) && preservedKernelBase,
            "actual selectors/bases restore without changing kernel GS stack");
  if (passed)
    NOTICE("PTRACE-FRAME-CORE: PASS machine metadata round trip");
  return passed;
}

bool payload(Thread& thread) {
  SyscallState state;
  ByteSet(&state, 0, sizeof(state));
  for (size_t i = 0; i < 13; ++i)
    state.setRegister(i, 0x1000 + i);
  state.setInstructionPointer(0x401234);
  state.setStackPointer(0x800000);
  state.setFlags(0x202);
  X64UserEntryMetadata metadata = {0x20, 0x21, 0x22, 0x23, 0x11110000, 0x22220000, 234};
  state.setUserEntryMetadata(metadata);
  UserReturnFrame frame(thread, state);
  UserRegisterSnapshot output;
  ByteSet(&output, 0xa5, sizeof(output));
  const UserRegisterSnapshot canary = output;
  bool passed = check(!frame.snapshot(output) && equalBytes(&output, &canary, sizeof(output)),
                      "unscoped snapshot leaves output unchanged");
  {
    Thread::UserReturnFrameScope scope(thread, frame);
    passed &= check(frame.snapshot(output), "complete syscall snapshot");
  }
  const uint64_t expected[27] = {
      0x100c, 0x100b,   0x100a,   0x1009,     0x1005,     0x1001, 0x202, 0x1008,   0x1007,
      0x1006, 0x1000,   0x401234, 0x1002,     0x1004,     0x1003, 234,   0x401234, 0x2b,
      0x202,  0x800000, 0x23,     0x11110000, 0x22220000, 0x20,   0x21,  0x22,     0x23};
  passed &= check(output.architecture == UserRegisterSnapshot::Architecture::Amd64 &&
                      equalBytes(&output.amd64, expected, sizeof(expected)),
                  "all 27 syscall ABI slots");
  const uint64_t syscallSlots[16] = {0x100c, 0x100b, 0x100a,   0x1009,  0x1008, 0x1007,
                                     0x1006, 0x1005, 0x1004,   0x1003,  0x1002, 0x1001,
                                     0x1000, 0x202,  0x401234, 0x800000};
  passed &= check(equalBytes(&state, &metadata, sizeof(metadata)) &&
                      equalBytes(reinterpret_cast<const unsigned char*>(&state) + 32, syscallSlots,
                                 sizeof(syscallSlots)),
                  "syscall storage matches assembly slots after metadata prefix");
  const UserRegisterSnapshot saved = output;
  state.setRegister(0, 0xdead);
  passed &=
      check(equalBytes(&output, &saved, sizeof(output)), "snapshot independent of live frame");
  UserReturnFrame invalid(thread, state, UserReturnFrame::Origin::Interrupt);
  {
    Thread::UserReturnFrameScope scope(thread, invalid);
    passed &= check(!invalid.snapshot(output) && equalBytes(&output, &saved, sizeof(output)),
                    "incompatible frame origin preserves output");
  }

  ProcessorState registers;
  registers.rax = 0x2000;
  registers.rbx = 0x2001;
  registers.rcx = 0x2002;
  registers.rdx = 0x2003;
  registers.rdi = 0x2004;
  registers.rsi = 0x2005;
  registers.rbp = 0x2006;
  registers.r8 = 0x2007;
  registers.r9 = 0x2008;
  registers.r10 = 0x2009;
  registers.r11 = 0x200a;
  registers.r12 = 0x200b;
  registers.r13 = 0x200c;
  registers.r14 = 0x200d;
  registers.r15 = 0x200e;
  registers.rip = 0x402345;
  alignas(16) unsigned char storage[sizeof(InterruptState)];
  registers.rsp = reinterpret_cast<uintptr_t>(storage + sizeof(storage));
  metadata.origRax = ~uint64_t(0);
  InterruptState* irq = InterruptState::construct(registers, true, metadata);
  irq->setStackPointer(0x900000);
  irq->setFlags(0x246);
  UserReturnFrame irqFrame(thread, *irq);
  {
    Thread::UserReturnFrameScope scope(thread, irqFrame);
    passed &= check(irqFrame.snapshot(output), "complete interrupt snapshot");
  }
  const uint64_t irqExpected[27] = {
      0x200e, 0x200d,   0x200c, 0x200b,     0x2006,     0x2001, 0x200a,       0x2009,   0x2008,
      0x2007, 0x2000,   0x2002, 0x2003,     0x2005,     0x2004, ~uint64_t(0), 0x402345, 0x1b,
      0x246,  0x900000, 0x23,   0x11110000, 0x22220000, 0x20,   0x21,         0x22,     0x23};
  passed &= check(equalBytes(&output.amd64, irqExpected, sizeof(irqExpected)),
                  "all 27 interrupt ABI slots");
  const uint64_t irqSlots[22] = {0x200e, 0x200d,   0x200c, 0x200b, 0x200a,   0x2009, 0x2008, 0x2007,
                                 0x2006, 0x2005,   0x2004, 0x2003, 0x2002,   0x2001, 0x2000, 0,
                                 0,      0x402345, 0x1b,   0x246,  0x900000, 0x23};
  passed &= check(
      equalBytes(irq, &metadata, sizeof(metadata)) &&
          equalBytes(reinterpret_cast<const unsigned char*>(irq) + 32, irqSlots, sizeof(irqSlots)),
      "interrupt storage matches assembly slots after metadata prefix");
  const UserRegisterSnapshot irqSaved = output;
  irq = InterruptState::construct(registers, false, metadata);
  UserReturnFrame kernel(thread, *irq);
  {
    Thread::UserReturnFrameScope scope(thread, kernel);
    passed &= check(!kernel.snapshot(output) && equalBytes(&output, &irqSaved, sizeof(output)),
                    "kernel frame rejected without partial snapshot");
  }
  if (passed)
    NOTICE("PTRACE-FRAME-CORE: PASS complete register snapshots");
  return passed;
}
#endif
}  // namespace

EXPORTED_PUBLIC bool runPtraceFrameRegressions() {
  NOTICE("PTRACE-FRAME-CORE: BEGIN");
  Thread* current = Processor::information().getCurrentThread();
  if (!check(current && Processor::getInterrupts(), "fixture context"))
    return false;
  bool passed = scopes(*current) && legacyAdmission(*current);
#if X64 && !HOSTED
  passed = passed && metadataRoundTrip() && payload(*current);
#else
  SyscallState state;
  UserReturnFrame frame(*current, state);
  UserRegisterSnapshot output;
  const auto before = output;
  Thread::UserReturnFrameScope scope(*current, frame);
  passed &= check(!frame.snapshot(output) && equalBytes(&before, &output, sizeof(output)),
                  "unsupported architecture snapshot unchanged");
  NOTICE("PTRACE-FRAME-CORE: SKIP amd64 machine metadata (hosted backend)");
#endif
  if (passed)
    NOTICE("PTRACE-FRAME-CORE: END PASS");
  return passed;
}
#endif
