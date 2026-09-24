#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Subsystem.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/InterruptManager.h"
#include "pedigree/kernel/processor/PageFaultHandler.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/state.h"

PageFaultHandler PageFaultHandler::m_Instance;

bool PageFaultHandler::initialise() {
  return InterruptManager::instance().registerInterruptHandler(14, this);
}

void PageFaultHandler::interrupt(size_t, InterruptState& state) {
  const uintptr_t address = state.faultAddress;
  const uintptr_t page = address & ~(PAGE_SIZE - 1);
  const bool fetch = state.vector == 3;
  const bool write = !fetch && (state.faultStatus & (1U << 11));
  const size_t status = (state.faultStatus & 15) | ((state.faultStatus >> 6) & 16);
  const bool present = status == 0xd || status == 0xf;
  VirtualAddressSpace& space = Processor::information().getVirtualAddressSpace();
  if (present && write &&
      space.handleCopyOnWriteFault(reinterpret_cast<void*>(page), !state.kernelMode())) {
    return;
  }
  if (dispatchHandlers(state, address, write, present)) {
    return;
  }

  Thread* thread = Processor::information().getCurrentThread();
  if (thread && !state.kernelMode()) {
    Process* process = thread->getParent();
    if (process && process->getSubsystem()) {
      const uintptr_t errorCode = (present ? 1 : 0) | (write ? 2 : 0) | 4 | (fetch ? 16 : 0);
      thread->deferSubsystemException(static_cast<size_t>(Subsystem::PageFault), address,
                                      errorCode);
      return;
    }
  }
  ERROR("ARMv7: page fault at " << Hex << address << " PC " << state.pc << " FSR "
                                << state.faultStatus);
  panic("ARMv7: unrecoverable page fault");
}
