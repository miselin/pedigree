#include "pedigree/kernel/processor/state.h"

processor_register_t Armv7InterruptState::getRegister(size_t index) const {
  if (index < 13) {
    return r[index];
  }
  if (index == 13) {
    return sp;
  }
  if (index == 14) {
    return lr;
  }
  if (index == 15) {
    return pc;
  }
  if (index == 16) {
    return cpsr;
  }
  if (index == 17) {
    return faultAddress;
  }
  return index == 18 ? faultStatus : 0;
}

void Armv7InterruptState::setRegister(size_t index, uintptr_t value) {
  if (index < 13) {
    r[index] = value;
  } else if (index == 13) {
    sp = value;
  } else if (index == 14) {
    lr = value;
  } else if (index == 15) {
    pc = value;
  } else if (index == 16) {
    cpsr = value;
  } else if (index == 17) {
    faultAddress = value;
  } else if (index == 18) {
    faultStatus = value;
  }
}

const char* Armv7InterruptState::getRegisterName(size_t index) const {
  static const char* const names[] = {"r0", "r1", "r2",   "r3",  "r4",  "r5",  "r6",
                                      "r7", "r8", "r9",   "r10", "r11", "r12", "sp",
                                      "lr", "pc", "cpsr", "far", "fsr"};
  return index < 19 ? names[index] : "?";
}

Armv7InterruptState* Armv7InterruptState::construct(Armv7ProcessorState& state, bool userMode) {
  uintptr_t frameAddress = (state.sp - sizeof(Armv7InterruptState)) & ~uintptr_t(7);
  auto* frame = reinterpret_cast<Armv7InterruptState*>(frameAddress);
  for (size_t i = 0; i < 13; ++i) {
    frame->r[i] = state.r[i];
  }
  frame->sp = state.sp;
  frame->lr = state.lr;
  frame->pc = state.pc;
  frame->cpsr = userMode ? (0x10 | (state.cpsr & 0x20)) : 0x13;
  frame->faultAddress = 0;
  frame->faultStatus = 0;
  frame->vector = 0;
  return frame;
}
