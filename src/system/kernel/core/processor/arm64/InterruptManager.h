#ifndef KERNEL_PROCESSOR_ARM64_INTERRUPTMANAGER_H
#define KERNEL_PROCESSOR_ARM64_INTERRUPTMANAGER_H

#include "pedigree/kernel/processor/InterruptManager.h"
#include "pedigree/kernel/processor/state_forward.h"

class Arm64InterruptManager final : public InterruptManager {
 public:
  static Arm64InterruptManager& instance();
  bool registerInterruptHandler(size_t number, InterruptHandler* handler) override;
#if DEBUGGER
  bool registerInterruptHandlerDebugger(size_t number, InterruptHandler* handler) override;
  size_t getBreakpointInterruptNumber() override {
    return 3;
  }
  size_t getDebugInterruptNumber() override {
    return 1;
  }
#endif
  void dispatch(size_t number, InterruptState& state);

 private:
  Arm64InterruptManager();
  InterruptHandler* m_Handlers[32];
#if DEBUGGER
  InterruptHandler* m_DebugHandlers[32];
#endif
};

#endif
