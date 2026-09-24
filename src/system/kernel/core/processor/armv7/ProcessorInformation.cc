#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/InfoBlock.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/armv7/ProcessorInformation.h"

Armv7ProcessorInformation::Armv7ProcessorInformation(ProcessorId id)
    : m_ProcessorId(id),
      m_AddressSpace(&VirtualAddressSpace::getKernelAddressSpace()),
      m_Thread(nullptr),
      m_Scheduler(nullptr),
      m_KernelStack(0),
      m_DeviceHardIrqDepth(0) {}

Armv7ProcessorInformation::~Armv7ProcessorInformation() = default;

VirtualAddressSpace& Armv7ProcessorInformation::getVirtualAddressSpace() const {
  return *m_AddressSpace;
}

void Armv7ProcessorInformation::setVirtualAddressSpace(VirtualAddressSpace& space) {
  m_AddressSpace = &space;
}

void Armv7ProcessorInformation::setCurrentThread(Thread* thread) {
  if (m_RcuState.active()) {
    panic("Context switch inside an RCU read section.");
  }
  m_Thread = thread;
  if (thread) {
    InfoBlockManager::instance().setPid(thread->getParent()->getId());
  }
}

PerProcessorScheduler& Armv7ProcessorInformation::getScheduler() {
  if (!m_Scheduler) {
    m_Scheduler = new PerProcessorScheduler();
  }
  return *m_Scheduler;
}
