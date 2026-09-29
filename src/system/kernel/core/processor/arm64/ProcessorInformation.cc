#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/InfoBlock.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/arm64/ProcessorInformation.h"

Arm64ProcessorInformation::Arm64ProcessorInformation(ProcessorId id)
    : m_ProcessorId(id),
      m_AddressSpace(&VirtualAddressSpace::getKernelAddressSpace()),
      m_Thread(nullptr),
      m_Scheduler(nullptr),
      m_KernelStack(0),
      m_DeviceHardIrqDepth(0) {}

Arm64ProcessorInformation::~Arm64ProcessorInformation() = default;

VirtualAddressSpace& Arm64ProcessorInformation::getVirtualAddressSpace() const {
  return *m_AddressSpace;
}

void Arm64ProcessorInformation::setVirtualAddressSpace(VirtualAddressSpace& space) {
  m_AddressSpace = &space;
}

void Arm64ProcessorInformation::setCurrentThread(Thread* thread) {
  if (m_RcuState.active()) {
    panic("Context switch inside an RCU read section.");
  }
  m_Thread = thread;
  if (thread) {
    InfoBlockManager::instance().setPid(thread->getParent()->getId());
  }
}

PerProcessorScheduler& Arm64ProcessorInformation::getScheduler() {
  if (!m_Scheduler) {
    m_Scheduler = new PerProcessorScheduler();
  }
  return *m_Scheduler;
}
