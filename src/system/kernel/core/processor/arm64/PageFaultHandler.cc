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
  const uintptr_t address = state.far;
  const uintptr_t page = address & ~(PAGE_SIZE - 1);
  const size_t faultStatus = state.esr & 0x3f;
  const size_t exceptionClass = (state.esr >> 26) & 0x3f;
  const bool fetch = exceptionClass == 0x20 || exceptionClass == 0x21;
  const bool write = !fetch && (state.esr & (1U << 6));
  const bool present = (faultStatus & 0x3c) == 0x0c;
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
  ERROR("ARM64: page fault at " << Hex << address << " PC " << state.pc << " ESR " << state.esr);
  panic("ARM64: unrecoverable page fault");
}
