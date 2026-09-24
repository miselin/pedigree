#include "InterruptManager.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/InterruptTimeAccounting.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/InterruptHandler.h"
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
    panic("ARMv7: terminal user return unexpectedly resumed");
  }
}
}  // namespace

Armv7InterruptManager::Armv7InterruptManager()
    : m_Handlers{}
#if DEBUGGER
      ,
      m_DebugHandlers{}
#endif
{
}

Armv7InterruptManager& Armv7InterruptManager::instance() {
  static Armv7InterruptManager manager;
  return manager;
}

InterruptManager& InterruptManager::instance() {
  return Armv7InterruptManager::instance();
}

bool Armv7InterruptManager::registerInterruptHandler(size_t number, InterruptHandler* handler) {
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
bool Armv7InterruptManager::registerInterruptHandlerDebugger(size_t number,
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

void Armv7InterruptManager::dispatch(size_t number, InterruptState& state) {
  if (number >= 32) {
    panic("ARMv7: invalid exception number");
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
  ERROR("ARMv7: unhandled exception " << Dec << number << " at " << Hex << state.pc);
  panic("ARMv7: unhandled exception");
}

extern "C" void armv7HandleException(InterruptState* state) {
  if (!state->kernelMode()) {
    Thread* thread = Processor::information().getCurrentThread();
    if (thread) {
      uintptr_t tls;
      asm volatile("mrc p15, 0, %0, c13, c0, 2" : "=r"(tls));
      thread->setTlsBase(tls);
    }
  }
  if (state->vector == 2) {
    Armv7SyscallManager::handle(*static_cast<SyscallState*>(state));
    return;
  }
  {
    InterruptTimeAccounting accounting(!state->kernelMode());
    if (state->vector == 5) {
      virtHandleIrq(*state);
    } else if (state->vector == 3 || state->vector == 4) {
      Armv7InterruptManager::instance().dispatch(14, *state);
    } else {
      Armv7InterruptManager::instance().dispatch(state->vector, *state);
    }
  }
  finishUserReturn(*state);
}
