#ifndef KERNEL_PROCESSOR_ARM64_SYSCALLMANAGER_H
#define KERNEL_PROCESSOR_ARM64_SYSCALLMANAGER_H

#include "pedigree/kernel/processor/SyscallManager.h"

class Arm64SyscallManager : public SyscallManager {
 public:
  static Arm64SyscallManager& instance() {
    return m_Instance;
  }

  bool registerSyscallHandler(Service_t service, SyscallHandler* handler,
                              Registration& registration, FastEntry entry = nullptr) override;
  uintptr_t syscall(Service_t service, uintptr_t function, uintptr_t p1 = 0, uintptr_t p2 = 0,
                    uintptr_t p3 = 0, uintptr_t p4 = 0, uintptr_t p5 = 0) override;

  static void handle(SyscallState& state);

 private:
  Arm64SyscallManager() = default;
  ~Arm64SyscallManager() override = default;

  static Arm64SyscallManager m_Instance;
};

#endif
