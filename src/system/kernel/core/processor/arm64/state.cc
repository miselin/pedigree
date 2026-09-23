#include "pedigree/kernel/processor/state.h"

processor_register_t Arm64InterruptState::getRegister(size_t index) const {
  if (index < 31) {
    return x[index];
  }
  if (index == 31) {
    return sp;
  }
  if (index == 32) {
    return pc;
  }
  return index == 33 ? pstate : 0;
}

void Arm64InterruptState::setRegister(size_t index, uintptr_t value) {
  if (index < 31) {
    x[index] = value;
  } else if (index == 31) {
    sp = value;
  } else if (index == 32) {
    pc = value;
  } else if (index == 33) {
    pstate = value;
  }
}

const char* Arm64InterruptState::getRegisterName(size_t index) const {
  static const char* const names[] = {
      "x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",  "x8",  "x9",    "x10", "x11",
      "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19", "x20", "x21",   "x22", "x23",
      "x24", "x25", "x26", "x27", "x28", "x29", "x30", "sp",  "pc",  "pstate"};
  return index < 34 ? names[index] : "?";
}

Arm64InterruptState* Arm64InterruptState::construct(Arm64ProcessorState& state, bool userMode) {
  uintptr_t frameAddress = (state.sp - sizeof(Arm64InterruptState)) & ~uintptr_t(15);
  auto* frame = reinterpret_cast<Arm64InterruptState*>(frameAddress);
  for (size_t i = 0; i < 31; ++i) {
    frame->x[i] = state.x[i];
  }
  frame->sp = state.sp;
  frame->pc = state.pc;
  frame->pstate = userMode ? 0 : 5;
  frame->esr = 0;
  frame->far = 0;
  frame->vector = 0;
  return frame;
}
