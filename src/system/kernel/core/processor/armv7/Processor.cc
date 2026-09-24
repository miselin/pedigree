#include "pedigree/kernel/BootstrapInfo.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/initialiseMultitasking.h"
#include "pedigree/kernel/processor/PageFaultHandler.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/state.h"

#include "PhysicalMemoryManager.h"
#include "VirtualAddressSpace.h"
#include "../../../machine/mach_virt/DeviceTree.h"

extern "C" bool armv7SaveSchedulerState(SchedulerState* state);
extern "C" void armv7RestoreSchedulerState(SchedulerState* state,
                                           volatile uintptr_t* lock) NORETURN;
extern "C" void armv7ReturnToUser(InterruptState* state, volatile uintptr_t* lock,
                                  uintptr_t kernelStack) NORETURN;
extern "C" void armv7JumpKernel(volatile uintptr_t* lock, uintptr_t address, uintptr_t stack,
                                uintptr_t p1, uintptr_t p2, uintptr_t p3, uintptr_t p4) NORETURN;
extern "C" void armv7JumpUser(volatile uintptr_t* lock, uintptr_t address, uintptr_t stack,
                              uintptr_t p1, uintptr_t p2, uintptr_t p3, uintptr_t p4,
                              uintptr_t kernelStack) NORETURN;

namespace {
bool terminalTlbFailure = false;
}

void ProcessorBase::initialise1(const BootstrapStruct_t& info) {
  ProcessorInformation* current = &m_SafeBspProcessorInformation;
  asm volatile("mcr p15, 0, %0, c13, c0, 4" : : "r"(current) : "memory");
  if (!VirtDeviceTree::valid()) {
    virtSetDeviceTree(reinterpret_cast<const void*>(0xc0000000U));
  }
  Armv7PhysicalMemoryManager::instance().initialise(info);
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
  auto& armSpace = static_cast<Armv7VirtualAddressSpace&>(addressSpace);
  uintptr_t root = armSpace.root();
  uintptr_t current;
  asm volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(current));
  if ((current & ~uintptr_t(0x1fff)) != root) {
    asm volatile("dsb sy\n\tmcr p15, 0, %0, c2, c0, 0\n\tisb" : : "r"(root) : "memory");
    invalidate(nullptr);
  }
  information().setVirtualAddressSpace(addressSpace);
}

bool ProcessorBase::saveState(SchedulerState& state) {
  return armv7SaveSchedulerState(&state);
}

void ProcessorBase::restoreState(SchedulerState& state, volatile uintptr_t* lock) {
  armv7RestoreSchedulerState(&state, lock);
}

void ProcessorBase::restoreState(SyscallState& state, volatile uintptr_t* lock) {
  armv7ReturnToUser(&state, lock, information().getKernelStack());
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
  armv7JumpKernel(lock, address, stack, p1, p2, p3, p4);
}

void ProcessorBase::jumpUser(volatile uintptr_t* lock, uintptr_t address, uintptr_t stack,
                             uintptr_t p1, uintptr_t p2, uintptr_t p3, uintptr_t p4) {
  uintptr_t kernelStack = information().getKernelStack();
  if (!kernelStack) {
    kernelStack = getStackPointer();
  }
  armv7JumpUser(lock, address, stack, p1, p2, p3, p4, kernelStack);
}

uintptr_t ProcessorBase::getBasePointer() {
  uintptr_t value;
  asm volatile("mov %0, fp" : "=r"(value));
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
  asm volatile("mcr p15, 0, %0, c13, c0, 2" : : "r"(value) : "memory");
  asm volatile("mcr p15, 0, %0, c13, c0, 3" : : "r"(value) : "memory");
}

void ProcessorBase::setInterrupts(bool enable) {
  if (enable) {
    asm volatile("cpsie i" : : : "memory");
  } else {
    asm volatile("cpsid i" : : : "memory");
  }
}

bool ProcessorBase::getInterrupts() {
  uintptr_t cpsr;
  asm volatile("mrs %0, cpsr" : "=r"(cpsr));
  return !(cpsr & (1U << 7));
}

void ProcessorBase::breakpoint() {
  asm volatile("bkpt #0");
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

void ProcessorBase::invalidate(void* address) {
  asm volatile("dsb sy" : : : "memory");
  if (address) {
    uintptr_t value = reinterpret_cast<uintptr_t>(address);
    asm volatile("mcr p15, 0, %0, c8, c7, 1" : : "r"(value) : "memory");
  } else {
    uintptr_t zero = 0;
    asm volatile("mcr p15, 0, %0, c8, c7, 0" : : "r"(zero) : "memory");
  }
  asm volatile("dsb sy\n\tisb" : : : "memory");
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
  asm volatile("mcr p15, 0, %0, c7, c5, 1\n\tdsb sy\n\tisb" : : "r"(address) : "memory");
}

void ProcessorBase::invalidateDCache(uintptr_t address) {
  asm volatile("mcr p15, 0, %0, c7, c6, 1\n\tdsb sy" : : "r"(address) : "memory");
}

void ProcessorBase::flushDCache(uintptr_t address) {
  asm volatile("mcr p15, 0, %0, c7, c10, 1\n\tdsb sy" : : "r"(address) : "memory");
}

void ProcessorBase::flushDCacheAndInvalidateICache(uintptr_t start, uintptr_t end) {
  uintptr_t ctr;
  asm volatile("mrc p15, 0, %0, c0, c0, 1" : "=r"(ctr));
  const size_t dline = 4U << ((ctr >> 16) & 0xf);
  const size_t iline = 4U << (ctr & 0xf);
  for (uintptr_t p = start & ~(dline - 1); p < end; p += dline) {
    asm volatile("mcr p15, 0, %0, c7, c11, 1" : : "r"(p) : "memory");
  }
  asm volatile("dsb sy" : : : "memory");
  for (uintptr_t p = start & ~(iline - 1); p < end; p += iline) {
    asm volatile("mcr p15, 0, %0, c7, c5, 1" : : "r"(p) : "memory");
  }
  asm volatile("dsb sy\n\tisb" : : : "memory");
}

void ProcessorBase::identify(HugeStaticString& text) {
  uintptr_t midr;
  asm volatile("mrc p15, 0, %0, c0, c0, 0" : "=r"(midr));
  text = "ARMv7 (MIDR ";
  text.append(midr, 16, 8, '0');
  text += ")";
}

ProcessorId ProcessorBase::id() {
  return information().processorId();
}

size_t ProcessorBase::getCount() {
  return m_nProcessors;
}

extern "C" int __aeabi_idiv0(int) {
  panic("ARMv7: divide by zero");
}

extern "C" long long __aeabi_ldiv0(long long) {
  panic("ARMv7: divide by zero");
}
