#ifndef KERNEL_PROCESSOR_ARM64_STACKFRAME_H
#define KERNEL_PROCESSOR_ARM64_STACKFRAME_H

#include "pedigree/kernel/processor/StackFrameBase.h"

class Arm64StackFrame : public StackFrameBase {
 public:
  Arm64StackFrame(const ProcessorState& state, uintptr_t baseAddress,
                  LargeStaticString mangledSymbol)
      : StackFrameBase(state, baseAddress, mangledSymbol) {}

  static void construct(ProcessorState& state, uintptr_t returnAddress, unsigned int nParams, ...);

#if DEBUGGER
 private:
  uintptr_t getParameter(size_t n) override;
#endif
};

#endif
