#ifndef KERNEL_PROCESSOR_ARMV7_PROCESSORINFORMATION_H
#define KERNEL_PROCESSOR_ARMV7_PROCESSORINFORMATION_H

#define _PROCESSOR_INFORMATION_ONLY_WANT_PROCESSORID
#include "pedigree/kernel/processor/ProcessorInformation.h"
#undef _PROCESSOR_INFORMATION_ONLY_WANT_PROCESSORID

#include "pedigree/kernel/process/RcuReadState.h"
#include "pedigree/kernel/processor/types.h"

class VirtualAddressSpace;
class PerProcessorScheduler;
class Thread;
class DeviceHardIrqContext;
class SuspendDeviceHardIrqContext;

class Armv7ProcessorInformation {
  friend class ProcessorBase;
  friend class DeviceHardIrqContext;
  friend class SuspendDeviceHardIrqContext;

 public:
  VirtualAddressSpace& getVirtualAddressSpace() const;
  void setVirtualAddressSpace(VirtualAddressSpace& space);
  uintptr_t getKernelStack() const {
    return m_KernelStack;
  }
  void setKernelStack(uintptr_t stack) {
    m_KernelStack = stack;
  }
  Thread* getCurrentThread() const {
    return m_Thread;
  }
  void setCurrentThread(Thread* thread);
  PerProcessorScheduler& getScheduler();
  RcuReadState& rcuState() {
    return m_RcuState;
  }
  ProcessorId processorId() const {
    return m_ProcessorId;
  }

 protected:
  explicit Armv7ProcessorInformation(ProcessorId id);
  ~Armv7ProcessorInformation();

 private:
  ProcessorId m_ProcessorId;
  VirtualAddressSpace* m_AddressSpace;
  Thread* m_Thread;
  PerProcessorScheduler* m_Scheduler;
  uintptr_t m_KernelStack;
  size_t m_DeviceHardIrqDepth;
  RcuReadState m_RcuState;
};

#endif
