#ifndef KERNEL_PROCESSOR_ARMV7_STACKFRAME_H
#define KERNEL_PROCESSOR_ARMV7_STACKFRAME_H

#include "pedigree/kernel/processor/StackFrameBase.h"

class Armv7StackFrame : public StackFrameBase {
 public:
  Armv7StackFrame(const ProcessorState& state, uintptr_t baseAddress,
                  LargeStaticString mangledSymbol)
      : StackFrameBase(state, baseAddress, mangledSymbol) {}

  static void construct(ProcessorState& state, uintptr_t returnAddress, unsigned int nParams, ...);

#if DEBUGGER
 private:
  uintptr_t getParameter(size_t n) override;
#endif
};

#endif
