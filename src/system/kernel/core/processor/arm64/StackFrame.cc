#include "pedigree/kernel/processor/StackFrame.h"
#include "pedigree/kernel/processor/state.h"

#include <stdarg.h>

#if DEBUGGER
uintptr_t Arm64StackFrame::getParameter(size_t n) {
  if (n < 8) {
    return m_State.x[n];
  }
  return reinterpret_cast<const uintptr_t*>(m_State.sp)[n - 8];
}
#endif

void Arm64StackFrame::construct(ProcessorState& state, uintptr_t returnAddress,
                                unsigned int nParams, ...) {
  va_list args;
  va_start(args, nParams);

  uintptr_t stack = state.sp & ~uintptr_t(15);
  const size_t stackParams = nParams > 8 ? nParams - 8 : 0;
  stack -= ((stackParams * sizeof(uintptr_t) + 15) & ~size_t(15));
  auto* stackArguments = reinterpret_cast<uintptr_t*>(stack);

  for (size_t i = 0; i < nParams; ++i) {
    uintptr_t value = va_arg(args, uintptr_t);
    if (i < 8) {
      state.x[i] = value;
    } else {
      stackArguments[i - 8] = value;
    }
  }

  va_end(args);
  state.x[30] = returnAddress;
  state.sp = stack;
}
