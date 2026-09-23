#include "pedigree/kernel/BootstrapInfo.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/process/initialiseMultitasking.h"
#include "pedigree/kernel/processor/PageFaultHandler.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/utilities/StaticString.h"

#include "PhysicalMemoryManager.h"
#include "VirtualAddressSpace.h"

extern "C" char arm64_boot_low_l0;
extern "C" bool arm64SaveSchedulerState(SchedulerState* state);
extern "C" void arm64RestoreSchedulerState(SchedulerState* state,
                                           volatile uintptr_t* lock) NORETURN;
extern "C" void arm64ReturnToUser(InterruptState* state, volatile uintptr_t* lock,
                                  uintptr_t kernelStack) NORETURN;
extern "C" void arm64JumpKernel(volatile uintptr_t* lock, uintptr_t address, uintptr_t stack,
                                uintptr_t p1, uintptr_t p2, uintptr_t p3, uintptr_t p4) NORETURN;
extern "C" void arm64JumpUser(volatile uintptr_t* lock, uintptr_t address, uintptr_t stack,
                              uintptr_t p1, uintptr_t p2, uintptr_t p3, uintptr_t p4,
                              uintptr_t kernelStack) NORETURN;

namespace {
bool terminalTlbFailure = false;
}

void ProcessorBase::initialise1(const BootstrapStruct_t& info) {
  ProcessorInformation* current = &m_SafeBspProcessorInformation;
  asm volatile("msr tpidr_el1, %0" : : "r"(current) : "memory");
  Arm64PhysicalMemoryManager::instance().initialise(info);
  PageFaultHandler::instance().initialise();
  m_Initialised = 1;
}

void ProcessorBase::initialise2(const BootstrapStruct_t&) {
  initialiseMultitasking();
  m_Initialised = 2;
}

void ProcessorBase::initialisationDone() {
  g_pBootstrapInfo = nullptr;
}

void ProcessorBase::deinitialise() {
  shutdownMultitasking();
}

void ProcessorBase::switchAddressSpace(VirtualAddressSpace& addressSpace) {
  auto& armSpace = static_cast<Arm64VirtualAddressSpace&>(addressSpace);
  uintptr_t root = &addressSpace == &VirtualAddressSpace::getKernelAddressSpace()
                       ? reinterpret_cast<uintptr_t>(&arm64_boot_low_l0)
                       : armSpace.root();
  uintptr_t current;
  asm volatile("mrs %0, ttbr0_el1" : "=r"(current));
  if ((current & ~uintptr_t(PAGE_SIZE - 1)) != root) {
    asm volatile("dsb ishst\n\tmsr ttbr0_el1, %0\n\tisb\n\ttlbi vmalle1is\n\tdsb ish\n\tisb"
                 :
                 : "r"(root)
                 : "memory");
  }
  information().setVirtualAddressSpace(addressSpace);
}

bool ProcessorBase::saveState(SchedulerState& state) {
  return arm64SaveSchedulerState(&state);
}

void ProcessorBase::restoreState(SchedulerState& state, volatile uintptr_t* lock) {
  arm64RestoreSchedulerState(&state, lock);
}

void ProcessorBase::restoreState(SyscallState& state, volatile uintptr_t* lock) {
  arm64ReturnToUser(&state, lock, information().getKernelStack());
}

void ProcessorBase::switchState(bool interrupts, SchedulerState& previous, SchedulerState& next,
                                volatile uintptr_t* lock) {
  if (saveState(previous)) {
    return;
  }
  setInterrupts(interrupts);
  restoreState(next, lock);
}

void ProcessorBase::switchState(bool interrupts, SchedulerState& previous, SyscallState& next,
                                volatile uintptr_t* lock) {
  if (saveState(previous)) {
    return;
  }
  setInterrupts(interrupts);
  restoreState(next, lock);
}

void ProcessorBase::saveAndJumpKernel(bool interrupts, SchedulerState& state,
                                      volatile uintptr_t* lock, uintptr_t address, uintptr_t stack,
                                      uintptr_t p1, uintptr_t p2, uintptr_t p3, uintptr_t p4) {
  if (saveState(state)) {
    return;
  }
  setInterrupts(interrupts);
  jumpKernel(lock, address, stack, p1, p2, p3, p4);
}

void ProcessorBase::saveAndJumpUser(bool interrupts, SchedulerState& state,
                                    volatile uintptr_t* lock, uintptr_t address, uintptr_t stack,
                                    uintptr_t p1, uintptr_t p2, uintptr_t p3, uintptr_t p4) {
  if (saveState(state)) {
    return;
  }
  setInterrupts(interrupts);
  jumpUser(lock, address, stack, p1, p2, p3, p4);
}

void ProcessorBase::jumpKernel(volatile uintptr_t* lock, uintptr_t address, uintptr_t stack,
                               uintptr_t p1, uintptr_t p2, uintptr_t p3, uintptr_t p4) {
  arm64JumpKernel(lock, address, stack, p1, p2, p3, p4);
}

void ProcessorBase::jumpUser(volatile uintptr_t* lock, uintptr_t address, uintptr_t stack,
                             uintptr_t p1, uintptr_t p2, uintptr_t p3, uintptr_t p4) {
  uintptr_t kernelStack = information().getKernelStack();
  if (!kernelStack) {
    kernelStack = getStackPointer();
  }
  arm64JumpUser(lock, address, stack, p1, p2, p3, p4, kernelStack);
}

uintptr_t ProcessorBase::getBasePointer() {
  uintptr_t value;
  asm volatile("mov %0, x29" : "=r"(value));
  return value;
}

uintptr_t ProcessorBase::getStackPointer() {
  uintptr_t value;
  asm volatile("mov %0, sp" : "=r"(value));
  return value;
}

uintptr_t ProcessorBase::getInstructionPointer() {
  return reinterpret_cast<uintptr_t>(__builtin_return_address(0));
}

void ProcessorBase::setTlsBase(uintptr_t value) {
  asm volatile("msr tpidr_el0, %0" : : "r"(value) : "memory");
}

void ProcessorBase::setInterrupts(bool enable) {
  if (enable) {
    asm volatile("msr daifclr, #2" : : : "memory");
  } else {
    asm volatile("msr daifset, #2" : : : "memory");
  }
}

bool ProcessorBase::getInterrupts() {
  uintptr_t daif;
  asm volatile("mrs %0, daif" : "=r"(daif));
  return !(daif & (1U << 7));
}

void ProcessorBase::breakpoint() {
  asm volatile("brk #0");
}

void ProcessorBase::halt() {
  asm volatile("wfi");
}

void ProcessorBase::haltUntilInterrupt() {
  const bool enabled = getInterrupts();
  setInterrupts(true);
  asm volatile("wfi" : : : "memory");
  if (!enabled) {
    setInterrupts(false);
  }
}

void ProcessorBase::pause() {
  asm volatile("yield");
}

void ProcessorBase::reset() {
  Machine::instance().finalShutdown(Machine::ShutdownType::Restart);
  while (true) {
    halt();
  }
}

size_t ProcessorBase::getDebugBreakpointCount() {
  return 0;
}

uintptr_t ProcessorBase::getDebugBreakpoint(size_t, DebugFlags::FaultType& type, size_t& length,
                                            bool& enabled) {
  type = DebugFlags::InstructionFetch;
  length = 0;
  enabled = false;
  return 0;
}

void ProcessorBase::enableDebugBreakpoint(size_t, uintptr_t, DebugFlags::FaultType, size_t) {}
void ProcessorBase::disableDebugBreakpoint(size_t) {}
uintptr_t ProcessorBase::getDebugStatus() {
  return 0;
}
void ProcessorBase::setSingleStep(bool enable, InterruptState& state) {
  if (enable) {
    state.pstate |= 1ULL << 21;
  } else {
    state.pstate &= ~(1ULL << 21);
  }
}

void ProcessorBase::invalidate(void* address) {
  asm volatile("dsb ishst" : : : "memory");
  if (address) {
    uintptr_t page = reinterpret_cast<uintptr_t>(address) >> 12;
    asm volatile("tlbi vaae1is, %0" : : "r"(page) : "memory");
  } else {
    asm volatile("tlbi vmalle1is" : : : "memory");
  }
  asm volatile("dsb ish\n\tisb" : : : "memory");
}

TlbInvalidationResult ProcessorBase::beginTlbInvalidation(TlbInvalidationGuard& guard) {
  ExecutionContext context = executionContext();
  if (terminalTlbFailure || guard.m_Active ||
      (context != ExecutionContext::WaitableThread && context != ExecutionContext::AtomicThread)) {
    return TlbInvalidationResult::InvalidContext;
  }
  guard.m_Active = true;
  guard.m_Global = false;
  return TlbInvalidationResult::Success;
}

void ProcessorBase::endTlbInvalidation(TlbInvalidationGuard& guard) {
  guard.m_Active = false;
  guard.m_Global = false;
}

bool ProcessorBase::closeTlbInvalidationAdmissionForTerminalFailure(TlbInvalidationGuard& guard,
                                                                    TlbInvalidationResult) {
  if (!guard.m_Active || terminalTlbFailure) {
    return false;
  }
  terminalTlbFailure = true;
  return true;
}

bool ProcessorBase::tlbInvalidationFailureActive() {
  return terminalTlbFailure;
}
bool ProcessorBase::tlbInvalidationTerminal() {
  return terminalTlbFailure;
}

TlbInvalidationResult ProcessorBase::invalidateAll(void* address) {
  TlbInvalidationGuard guard;
  TlbInvalidationResult result = beginTlbInvalidation(guard);
  return result == TlbInvalidationResult::Success ? invalidateAll(address, guard) : result;
}

TlbInvalidationResult ProcessorBase::invalidateAll(void* address, TlbInvalidationGuard& guard) {
  if (!guard.m_Active) {
    return TlbInvalidationResult::InvalidContext;
  }
  if (m_nProcessors != 1) {
    return TlbInvalidationResult::UnsupportedTopology;
  }
  invalidate(address);
  return TlbInvalidationResult::Success;
}

void ProcessorBase::invalidateICache(uintptr_t address) {
  asm volatile("ic ivau, %0\n\tdsb ish\n\tisb" : : "r"(address) : "memory");
}

void ProcessorBase::invalidateDCache(uintptr_t address) {
  asm volatile("dc ivac, %0\n\tdsb ish" : : "r"(address) : "memory");
}

void ProcessorBase::flushDCache(uintptr_t address) {
  asm volatile("dc cvac, %0\n\tdsb ish" : : "r"(address) : "memory");
}

void ProcessorBase::flushDCacheAndInvalidateICache(uintptr_t start, uintptr_t end) {
  uintptr_t ctr;
  asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
  size_t dline = 4U << ((ctr >> 16) & 0xf);
  size_t iline = 4U << (ctr & 0xf);
  for (uintptr_t p = start & ~(dline - 1); p < end; p += dline) {
    asm volatile("dc cvau, %0" : : "r"(p) : "memory");
  }
  asm volatile("dsb ish" : : : "memory");
  for (uintptr_t p = start & ~(iline - 1); p < end; p += iline) {
    asm volatile("ic ivau, %0" : : "r"(p) : "memory");
  }
  asm volatile("dsb ish\n\tisb" : : : "memory");
}

void ProcessorBase::identify(HugeStaticString& text) {
  uintptr_t midr;
  asm volatile("mrs %0, midr_el1" : "=r"(midr));
  text = "ARM64 (MIDR ";
  text.append(midr, 16, 16, '0');
  text += ")";
}

ProcessorId ProcessorBase::id() {
  return information().processorId();
}
size_t ProcessorBase::getCount() {
  return m_nProcessors;
}
