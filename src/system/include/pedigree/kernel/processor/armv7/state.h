#ifndef KERNEL_PROCESSOR_ARMV7_STATE_H
#define KERNEL_PROCESSOR_ARMV7_STATE_H

#include "pedigree/kernel/processor/types.h"

class Armv7ProcessorState;

class Armv7InterruptState {
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
    return r[11];
  }
  void setBasePointer(uintptr_t value) {
    r[11] = value;
  }

  size_t getRegisterCount() const {
    return 19;
  }
  processor_register_t getRegister(size_t index) const;
  void setRegister(size_t index, uintptr_t value);
  const char* getRegisterName(size_t index) const;
  size_t getRegisterSize(size_t) const {
    return sizeof(uint32_t);
  }

  bool kernelMode() const {
    return (cpsr & 0x1f) != 0x10;
  }
  size_t getInterruptNumber() const {
    return vector;
  }
  uintptr_t getErrorCode() const {
    return faultStatus;
  }
  uint32_t getFlags() const {
    return cpsr;
  }
  void setFlags(uint32_t value) {
    cpsr = value;
  }

  static Armv7InterruptState* construct(Armv7ProcessorState& state, bool userMode);

  uint32_t r[13];
  uint32_t sp;
  uint32_t lr;
  uint32_t pc;
  uint32_t cpsr;
  uint32_t faultAddress;
  uint32_t faultStatus;
  uint32_t vector;
};

class Armv7SyscallState : public Armv7InterruptState {
 public:
  size_t getSyscallService() const {
    return svcNumber ? (r[7] >> 16) & 0xffff : 0;
  }
  size_t getSyscallNumber() const {
    return svcNumber ? r[7] & 0xffff : r[7];
  }
  uintptr_t getSyscallParameter(size_t index) const {
    return index < 6 ? r[index] : (index < 13 ? r[index - 6] : 0);
  }
  void setSyscallReturnValue(uintptr_t value) {
    r[0] = value;
  }
  void setSyscallErrno(uintptr_t value) {
    r[1] = value;
  }

  uint32_t svcNumber;
};

class Armv7ProcessorState {
 public:
  Armv7ProcessorState() : r{}, sp(0), lr(0), pc(0), cpsr(0) {}
  Armv7ProcessorState(const Armv7InterruptState& state) {
    *this = state;
  }
  Armv7ProcessorState(const Armv7SyscallState& state) {
    *this = state;
  }
  Armv7ProcessorState& operator=(const Armv7InterruptState& state) {
    for (size_t i = 0; i < 13; ++i) {
      r[i] = state.r[i];
    }
    sp = state.sp;
    lr = state.lr;
    pc = state.pc;
    cpsr = state.cpsr;
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
    return r[11];
  }
  void setBasePointer(uintptr_t value) {
    r[11] = value;
  }

  uint32_t r[13];
  uint32_t sp;
  uint32_t lr;
  uint32_t pc;
  uint32_t cpsr;
};

class Armv7SchedulerState {
 public:
  uintptr_t getInstructionPointer() const {
    return pc;
  }
  uint32_t r4ToR11[8];
  uint32_t sp;
  uint32_t pc;
  uint64_t d[16];
};

static_assert(sizeof(Armv7InterruptState) == 80);
static_assert(__builtin_offsetof(Armv7InterruptState, sp) == 52);
static_assert(__builtin_offsetof(Armv7InterruptState, cpsr) == 64);

#endif
