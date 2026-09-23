#include "InterruptManager.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/InterruptTimeAccounting.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/InterruptHandler.h"
#include "pedigree/kernel/processor/PageFaultHandler.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/state.h"

#include "SyscallManager.h"

extern "C" void virtHandleIrq(InterruptState& state);

namespace {
void finishUserReturn(InterruptState& state) {
  if (state.kernelMode()) {
    return;
  }
  Thread* thread = Processor::information().getCurrentThread();
  if (!thread) {
    return;
  }

  PerProcessorScheduler& scheduler = Processor::information().getScheduler();
  bool terminal = false;
  Processor::setInterrupts(true);
  while (true) {
    terminal = scheduler.serviceUserReturnWork(state, UserReturnFrame::Origin::Interrupt);
    if (terminal) {
      break;
    }
    bool waited = false;
    terminal = thread->completeAffinityAtSafePoint(&waited) == AffinityResult::Terminal;
    if (terminal || !waited) {
      break;
    }
  }
  Processor::setInterrupts(false);
  InterruptTimeAccounting::finishUserReturn(thread);
  if (terminal) {
    Processor::setInterrupts(true);
    scheduler.commitUserReturnTerminalState();
    panic("ARM64: terminal user return unexpectedly resumed");
  }
}
}  // namespace

Arm64InterruptManager::Arm64InterruptManager()
    : m_Handlers{}
#if DEBUGGER
      ,
      m_DebugHandlers{}
#endif
{
}

Arm64InterruptManager& Arm64InterruptManager::instance() {
  static Arm64InterruptManager manager;
  return manager;
}

InterruptManager& InterruptManager::instance() {
  return Arm64InterruptManager::instance();
}

bool Arm64InterruptManager::registerInterruptHandler(size_t number, InterruptHandler* handler) {
  if (number >= 32) {
    return false;
  }
  InterruptHandler* expected = handler ? nullptr : m_Handlers[number];
  if (!expected && !handler) {
    return false;
  }
  return __atomic_compare_exchange_n(&m_Handlers[number], &expected, handler, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

#if DEBUGGER
bool Arm64InterruptManager::registerInterruptHandlerDebugger(size_t number,
                                                             InterruptHandler* handler) {
  if (number >= 32) {
    return false;
  }
  InterruptHandler* expected = handler ? nullptr : m_DebugHandlers[number];
  if (!expected && !handler) {
    return false;
  }
  return __atomic_compare_exchange_n(&m_DebugHandlers[number], &expected, handler, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
#endif

void Arm64InterruptManager::dispatch(size_t number, InterruptState& state) {
  if (number >= 32) {
    ERROR("ARM64: exception class " << Hex << number << " vector " << state.vector << " ESR "
                                    << state.esr << " FAR " << state.far << " ELR "
                                    << state.getInstructionPointer());
    panic("ARM64: invalid exception number");
  }
#if DEBUGGER
  InterruptHandler* debugger = __atomic_load_n(&m_DebugHandlers[number], __ATOMIC_ACQUIRE);
  if (debugger) {
    debugger->interrupt(number, state);
  }
#endif
  InterruptHandler* handler = __atomic_load_n(&m_Handlers[number], __ATOMIC_ACQUIRE);
  if (handler) {
    handler->interrupt(number, state);
    return;
  }
  ERROR("ARM64: unhandled exception " << Dec << number << " at " << Hex
                                      << state.getInstructionPointer() << " ESR " << state.esr);
  panic("ARM64: unhandled exception");
}

extern "C" void arm64HandleException(InterruptState* state) {
  if (!state->kernelMode()) {
    Thread* thread = Processor::information().getCurrentThread();
    if (thread) {
      // EL0 may write TPIDR_EL0 directly, without a syscall to update Thread.
      uintptr_t tls;
      asm volatile("mrs %0, tpidr_el0" : "=r"(tls));
      thread->setTlsBase(tls);
    }
  }
  const size_t vector = state->vector;
  const size_t exceptionClass = (state->esr >> 26) & 0x3f;
  if ((vector & 3) == 0 && exceptionClass == 0x15 && !state->kernelMode()) {
    Arm64SyscallManager::handle(*static_cast<SyscallState*>(state));
    return;
  }
  {
    InterruptTimeAccounting accounting(!state->kernelMode());
    if ((vector & 3) == 1) {
      virtHandleIrq(*state);
    } else if (exceptionClass == 0x20 || exceptionClass == 0x21 || exceptionClass == 0x24 ||
               exceptionClass == 0x25) {
      Arm64InterruptManager::instance().dispatch(14, *state);
    } else {
      Arm64InterruptManager::instance().dispatch(exceptionClass, *state);
    }
  }
  finishUserReturn(*state);
}
