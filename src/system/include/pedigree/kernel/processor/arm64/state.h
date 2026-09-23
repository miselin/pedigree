#ifndef KERNEL_PROCESSOR_ARM64_STATE_H
#define KERNEL_PROCESSOR_ARM64_STATE_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"

class Arm64ProcessorState;

// The first 296 bytes are also the exception vector's saved register image.
class Arm64InterruptState {
 public:
  uintptr_t getStackPointer() const {
    return sp;
  }
  void setStackPointer(uintptr_t value) {
    sp = value;
  }
  uintptr_t getInstructionPointer() const {
    return pc;
  }
  void setInstructionPointer(uintptr_t value) {
    pc = value;
  }
  uintptr_t getBasePointer() const {
    return x[29];
  }
  void setBasePointer(uintptr_t value) {
    x[29] = value;
  }

  size_t getRegisterCount() const {
    return 34;
  }
  processor_register_t getRegister(size_t index) const;
  void setRegister(size_t index, uintptr_t value);
  const char* getRegisterName(size_t index) const;
  size_t getRegisterSize(size_t) const {
    return sizeof(uint64_t);
  }

  bool kernelMode() const {
    return (pstate & 0xf) != 0;
  }
  size_t getInterruptNumber() const {
    return vector;
  }
  uintptr_t getErrorCode() const {
    return esr;
  }
  uint64_t getFlags() const {
    return pstate;
  }
  void setFlags(uint64_t value) {
    pstate = value;
  }

  static Arm64InterruptState* construct(Arm64ProcessorState& state, bool userMode);

  uint64_t x[31];
  uint64_t sp;
  uint64_t pc;
  uint64_t pstate;
  uint64_t esr;
  uint64_t far;
  uint64_t vector;
};

class Arm64SyscallState : public Arm64InterruptState {
 public:
  // SVC #0 is Linux; SVC #1 carries Pedigree's packed service and number.
  size_t getSyscallService() const {
    return (esr & 0xffff) ? (x[8] >> 16) & 0xffff : 0;
  }
  size_t getSyscallNumber() const {
    return (esr & 0xffff) ? x[8] & 0xffff : x[8];
  }
  uintptr_t getSyscallParameter(size_t n) const {
    return n < 6 ? x[n] : (n < 12 ? x[n - 6] : 0);
  }
  void setSyscallReturnValue(uintptr_t value) {
    x[0] = value;
  }
  void setSyscallErrno(uintptr_t value) {
    x[1] = value;
  }
};

class Arm64ProcessorState {
 public:
  Arm64ProcessorState() : x{}, sp(0), pc(0), pstate(0) {}
  Arm64ProcessorState(const Arm64InterruptState& state) {
    *this = state;
  }
  Arm64ProcessorState(const Arm64SyscallState& state) {
    *this = state;
  }

  Arm64ProcessorState& operator=(const Arm64InterruptState& state) {
    for (size_t i = 0; i < 31; ++i) {
      x[i] = state.x[i];
    }
    sp = state.sp;
    pc = state.pc;
    pstate = state.pstate;
    return *this;
  }

  uintptr_t getStackPointer() const {
    return sp;
  }
  void setStackPointer(uintptr_t value) {
    sp = value;
  }
  uintptr_t getInstructionPointer() const {
    return pc;
  }
  void setInstructionPointer(uintptr_t value) {
    pc = value;
  }
  uintptr_t getBasePointer() const {
    return x[29];
  }
  void setBasePointer(uintptr_t value) {
    x[29] = value;
  }

  uint64_t x[31];
  uint64_t sp;
  uint64_t pc;
  uint64_t pstate;
};

class Arm64SchedulerState {
 public:
  uintptr_t getInstructionPointer() const {
    return pc;
  }

  uint64_t x19ToX30[12];
  uint64_t sp;
  uint64_t pc;
  uint64_t fpcr;
  uint64_t fpsr;
  alignas(16) uint8_t q[32][16];
};

static_assert(sizeof(Arm64InterruptState) == 296);
static_assert(__builtin_offsetof(Arm64InterruptState, sp) == 248);
static_assert(__builtin_offsetof(Arm64InterruptState, pc) == 256);
static_assert(__builtin_offsetof(Arm64InterruptState, pstate) == 264);
static_assert(__builtin_offsetof(Arm64InterruptState, esr) == 272);
static_assert(__builtin_offsetof(Arm64InterruptState, far) == 280);
static_assert(__builtin_offsetof(Arm64InterruptState, vector) == 288);
static_assert(__builtin_offsetof(Arm64SchedulerState, fpcr) == 112);
static_assert(__builtin_offsetof(Arm64SchedulerState, fpsr) == 120);
static_assert(__builtin_offsetof(Arm64SchedulerState, q) == 128);

#endif
