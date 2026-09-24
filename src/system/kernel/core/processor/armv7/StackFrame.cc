#include "pedigree/kernel/processor/StackFrame.h"
#include "pedigree/kernel/processor/state.h"

#include <stdarg.h>

#if DEBUGGER
uintptr_t Armv7StackFrame::getParameter(size_t n) {
  if (n < 4) {
    return m_State.r[n];
  }
  return reinterpret_cast<const uintptr_t*>(m_State.sp)[n - 4];
}
#endif

void Armv7StackFrame::construct(ProcessorState& state, uintptr_t returnAddress,
                                unsigned int nParams, ...) {
  va_list args;
  va_start(args, nParams);

  const size_t stackParams = nParams > 4 ? nParams - 4 : 0;
  uintptr_t stack = state.sp & ~uintptr_t(7);
  stack -= (stackParams * sizeof(uintptr_t) + 7) & ~size_t(7);
  auto* stackArguments = reinterpret_cast<uintptr_t*>(stack);

  for (size_t i = 0; i < nParams; ++i) {
    uintptr_t value = va_arg(args, uintptr_t);
    if (i < 4) {
      state.r[i] = value;
    } else {
      stackArguments[i - 4] = value;
    }
  }

  va_end(args);
  state.lr = returnAddress;
  state.sp = stack;
}
