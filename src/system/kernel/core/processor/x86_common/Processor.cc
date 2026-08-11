/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "pedigree/kernel/BootstrapInfo.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/processor/x86_common/ProcessorInformation.h"
#include "pedigree/kernel/utilities/Vector.h"

#include "../x64/VirtualAddressSpace.h"
#include "PhysicalMemoryManager.h"
#include <machine/mach_pc/LocalApic.h>
#include <machine/mach_pc/Pc.h>

void ProcessorBase::initialisationDone() {
  // Don't allow the bootstrap info to be used anymore - we're killing it here
  g_pBootstrapInfo = nullptr;

  // Unmap the identity mapping of the first MBs
  X64VirtualAddressSpace& KernelAddressSpace =
      static_cast<X64VirtualAddressSpace&>(VirtualAddressSpace::getKernelAddressSpace());
  *reinterpret_cast<uint64_t*>(KernelAddressSpace.m_PhysicalPML4) = 0;
  invalidate(0);

  X86CommonPhysicalMemoryManager::instance().initialisationDone();
}

size_t ProcessorBase::getDebugBreakpointCount() {
  return 4;
}

uintptr_t ProcessorBase::getDebugBreakpoint(size_t nBpNumber, DebugFlags::FaultType& nFaultType,
                                            size_t& nLength, bool& bEnabled) {
  uintptr_t nLinearAddress = 0;
  switch (nBpNumber) {
    case 0:
      asm volatile("mov %%db0, %0" : "=r"(nLinearAddress));
      break;
    case 1:
      asm volatile("mov %%db1, %0" : "=r"(nLinearAddress));
      break;
    case 2:
      asm volatile("mov %%db2, %0" : "=r"(nLinearAddress));
      break;
    case 3:
      asm volatile("mov %%db3, %0" : "=r"(nLinearAddress));
      break;
  }

  uintptr_t nStatus;
  asm volatile("mov %%db7, %0" : "=r"(nStatus));

  bEnabled = static_cast<bool>(nStatus & (1 << (nBpNumber * 2 + 1)));  // See intel manual 3b.
  nFaultType = static_cast<DebugFlags::FaultType>((nStatus >> (nBpNumber * 4 + 16)) & 0x3);
  switch ((nStatus >> (nBpNumber * 4 + 18)) & 0x3) {
    case 0:
      nLength = 1;
      break;
    case 1:
      nLength = 2;
      break;
    case 2:
      nLength = 8;
      break;
    case 3:
      nLength = 4;
      break;
  }

  return nLinearAddress;
}

void ProcessorBase::enableDebugBreakpoint(size_t nBpNumber, uintptr_t nLinearAddress,
                                          DebugFlags::FaultType nFaultType, size_t nLength) {
  switch (nBpNumber) {
    case 0:
      asm volatile("mov %0, %%db0" ::"r"(nLinearAddress));
      break;
    case 1:
      asm volatile("mov %0, %%db1" ::"r"(nLinearAddress));
      break;
    case 2:
      asm volatile("mov %0, %%db2" ::"r"(nLinearAddress));
      break;
    case 3:
      asm volatile("mov %0, %%db3" ::"r"(nLinearAddress));
      break;
  }

  uintptr_t nStatus;
  asm volatile("mov %%db7, %0" : "=r"(nStatus));

  size_t lengthField = 0;
  switch (nLength) {
    case 1:
      lengthField = 0;
      break;
    case 2:
      lengthField = 1;
      break;
    case 8:
      lengthField = 2;
      break;
    case 4:
      lengthField = 3;
      break;
  }

  nStatus |= 1 << (nBpNumber * 2 + 1);
  nStatus |= (nFaultType & 0x3) << (nBpNumber * 4 + 16);
  nStatus |= (lengthField & 0x3) << (nBpNumber * 4 + 18);
  asm volatile("mov %0, %%db7" ::"r"(nStatus));
}

void ProcessorBase::disableDebugBreakpoint(size_t nBpNumber) {
  uintptr_t nStatus;
  asm volatile("mov %%db7, %0" : "=r"(nStatus));

  nStatus &= ~(1 << (nBpNumber * 2 + 1));
  asm volatile("mov %0, %%db7" ::"r"(nStatus));
}

void ProcessorBase::setInterrupts(bool bEnable) {
  if (bEnable)
    asm volatile("sti");
  else
    asm volatile("cli");
}

bool ProcessorBase::getInterrupts() {
  size_t result;
  asm volatile(
      "pushf\n"
      "pop %0\n"
      "and $0x200, %0\n"
      : "=r"(result));
  return (result != 0);
}

void ProcessorBase::setSingleStep(bool bEnable, InterruptState& state) {
  uintptr_t eflags = state.getFlags();
  if (bEnable)
    eflags |= 0x100;
  else
    eflags &= ~0x100;
  state.setFlags(eflags);
}

uint64_t X86CommonProcessor::readMachineSpecificRegister(uint32_t index) {
  uint32_t eax, edx;
  asm volatile("rdmsr" : "=a"(eax), "=d"(edx) : "c"(index));
  return static_cast<uint64_t>(eax) | (static_cast<uint64_t>(edx) << 32);
}

void X86CommonProcessor::writeMachineSpecificRegister(uint32_t index, uint64_t value) {
  uint32_t eax = value, edx = value >> 32;
  asm volatile("wrmsr" ::"a"(eax), "d"(edx), "c"(index));
}

void ProcessorBase::invalidate(void* pAddress) {
  asm volatile("invlpg (%0)" ::"a"(pAddress));
}

TlbInvalidationResult ProcessorBase::beginTlbInvalidation(TlbInvalidationGuard& guard) {
  const ExecutionContext context = executionContext();
  if (guard.m_Active ||
      (context != ExecutionContext::WaitableThread && context != ExecutionContext::AtomicThread)) {
    return TlbInvalidationResult::InvalidContext;
  }

#if MULTIPROCESSOR && APIC
  if (m_Initialised == 2 && getCount() > 1) {
    Pc& pc = Pc::instance();
    if (!pc.localApicAvailable()) {
      return TlbInvalidationResult::UnsupportedTopology;
    }

    bool global = false;
    const TlbInvalidationResult result = pc.getLocalApic().beginTlbInvalidation(global);
    if (result != TlbInvalidationResult::Success) {
      return result;
    }
    guard.m_Global = global;
  }
#endif

  guard.m_Active = true;
  return TlbInvalidationResult::Success;
}

void ProcessorBase::endTlbInvalidation(TlbInvalidationGuard& guard) {
#if MULTIPROCESSOR && APIC
  if (guard.m_Global) {
    Pc::instance().getLocalApic().endTlbInvalidation();
  }
#endif
  guard.m_Global = false;
  guard.m_Active = false;
}

bool ProcessorBase::closeTlbInvalidationAdmissionForTerminalFailure(TlbInvalidationGuard& guard,
                                                                    TlbInvalidationResult result) {
  if (!guard.m_Active) {
    return false;
  }

#if MULTIPROCESSOR && APIC
  if (guard.m_Global) {
    return Pc::instance().getLocalApic().closeTlbInvalidationAdmissionForTerminalFailure(result);
  }
#endif

  return true;
}

bool ProcessorBase::tlbInvalidationFailureActive() {
#if MULTIPROCESSOR && APIC
  if (m_Initialised == 2 && getCount() > 1) {
    Pc& pc = Pc::instance();
    return pc.localApicAvailable() && pc.getLocalApic().tlbInvalidationFailureActive();
  }
#endif
  return false;
}

bool ProcessorBase::tlbInvalidationTerminal() {
#if MULTIPROCESSOR && APIC
  if (m_Initialised == 2 && getCount() > 1) {
    Pc& pc = Pc::instance();
    return pc.localApicAvailable() && pc.getLocalApic().tlbInvalidationTerminal();
  }
#endif
  return false;
}

TlbInvalidationResult ProcessorBase::invalidateAll(void* pAddress) {
  TlbInvalidationGuard guard;
  const TlbInvalidationResult result = beginTlbInvalidation(guard);
  if (result != TlbInvalidationResult::Success) {
    if (result == TlbInvalidationResult::SerialisationTimedOut && tlbInvalidationFailureActive()) {
      Processor::setInterrupts(false);
      while (true) {
        Processor::pause();
      }
    }
    return result;
  }
  const TlbInvalidationResult invalidation = invalidateAll(pAddress, guard);
  if (invalidation == TlbInvalidationResult::Success) {
    return invalidation;
  }

  // The Local APIC retains a failed generation for terminal adoption. Even
  // this non-mutating convenience call must complete that handoff rather than
  // returning with an orphaned shootdown owner.
  const bool coordinator = guard.closeAdmissionForTerminalFailure(invalidation);
  guard.retire();
  Processor::setInterrupts(false);
  if (!coordinator) {
    while (true) {
      Processor::pause();
    }
  }

  switch (invalidation) {
    case TlbInvalidationResult::InvalidContext:
      panic("TLB invalidation failed from an invalid context");
    case TlbInvalidationResult::UnsupportedTopology:
      panic("TLB invalidation has no safe all-processor route");
    case TlbInvalidationResult::SerialisationTimedOut:
      panic("TLB invalidation serialisation timed out");
    case TlbInvalidationResult::SubmissionFailed:
      panic("TLB invalidation IPI submission failed");
    case TlbInvalidationResult::AcknowledgementTimedOut:
      panic("TLB invalidation acknowledgement timed out");
    case TlbInvalidationResult::DrainTimedOut:
      panic("TLB invalidation service drain timed out");
    case TlbInvalidationResult::Success:
      break;
  }
  panic("TLB invalidation returned an unknown result");
}

TlbInvalidationResult ProcessorBase::invalidateAll(void* pAddress, TlbInvalidationGuard& guard) {
  if (!guard.m_Active) {
    return TlbInvalidationResult::InvalidContext;
  }

#if MULTIPROCESSOR && APIC
  if (guard.m_Global) {
    return Pc::instance().getLocalApic().invalidateAllProcessors(pAddress);
  }
#endif

  invalidate(pAddress);
  return TlbInvalidationResult::Success;
}

void X86CommonProcessor::cpuid(uint32_t inEax, uint32_t inEcx, uint32_t& eax, uint32_t& ebx,
                               uint32_t& ecx, uint32_t& edx) {
  asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(inEax), "c"(inEcx));
}

ProcessorId ProcessorBase::id() {
  if (m_Initialised < 2)
    return 0;

#if MULTIPROCESSOR
  Pc& pc = Pc::instance();
  if (!pc.localApicAvailable())
    return 0;

  uint8_t apicId = pc.getLocalApic().getId();

  for (size_t i = 0; i < m_ProcessorInformation.count(); i++)
    if (m_ProcessorInformation[i]->m_LocalApicId == apicId)
      return m_ProcessorInformation[i]->m_ProcessorId;
#endif

  return 0;
}

size_t ProcessorBase::index() {
  if (m_Initialised < 2)
    return 0;

#if MULTIPROCESSOR
  Pc& pc = Pc::instance();
  if (!pc.localApicAvailable())
    return 0;

  const uint8_t apicId = pc.getLocalApic().getId();
  for (size_t i = 0; i < m_ProcessorInformation.count(); ++i) {
    if (m_ProcessorInformation[i]->m_LocalApicId == apicId)
      return i;
  }

  // Never alias an unrecognised hardware identity onto the BSP slot. IRQ
  // publication treats this sentinel as a topology failure and rejects it.
  return m_ProcessorInformation.count();
#else
  return 0;
#endif
}

ProcessorInformation& ProcessorBase::information() {
#if MULTIPROCESSOR
  if (m_Initialised < 2)
    return m_SafeBspProcessorInformation;

  Pc& pc = Pc::instance();
  if (!pc.localApicAvailable())
    return m_SafeBspProcessorInformation;

  uint8_t apicId = pc.getLocalApic().getId();

  for (size_t i = 0; i < m_ProcessorInformation.count(); i++)
    if (m_ProcessorInformation[i]->m_LocalApicId == apicId)
      return *m_ProcessorInformation[i];
#endif

  return m_SafeBspProcessorInformation;
}

size_t ProcessorBase::getCount() {
#if MULTIPROCESSOR
  const size_t count = m_ProcessorInformation.count();
  return count ? count : 1;
#else
  return 1;
#endif
}

void ProcessorBase::breakpoint() {
  asm volatile("int $3");
}

void ProcessorBase::halt() {
  asm volatile("hlt");
}

void ProcessorBase::pause() {
#if MULTIPROCESSOR && APIC
  // A processor can spin on an IRQ-disabling lock held by a shootdown
  // initiator. Cooperating here prevents that lock dependency from delaying
  // the initiator's bounded acknowledgement barrier indefinitely.
  if (m_Initialised == 2) {
    Pc& pc = Pc::instance();
    if (pc.localApicAvailable()) {
      pc.getLocalApic().servicePendingTlbShootdown();
      pc.getLocalApic().servicePendingTerminalProcessorControl();
    }
  }
#endif
  asm volatile("pause");
}

void ProcessorBase::reset() {
  // Load null IDT for now
  size_t zero = 0x0;
  asm volatile("lidt %0; int $3" ::"m"(zero));
}

void ProcessorBase::haltUntilInterrupt() {
  bool bWasInterrupts = getInterrupts();
  __asm__ __volatile__("sti; hlt");
  if (!bWasInterrupts)
    setInterrupts(false);
}

void ProcessorBase::invalidateICache(uintptr_t nAddr) {
  __asm__ __volatile__("clflush (%0)" ::"a"(nAddr));
}

void ProcessorBase::invalidateDCache(uintptr_t nAddr) {
  __asm__ __volatile__("clflush (%0)" ::"a"(nAddr));
}

void ProcessorBase::flushDCache(uintptr_t nAddr) {
  __asm__ __volatile__("clflush (%0)" ::"a"(nAddr));
}

void ProcessorBase::flushDCacheAndInvalidateICache(uintptr_t startAddr, uintptr_t endAddr) {
  for (size_t i = 0; i < endAddr; ++i) {
    __asm__ __volatile__("clflush (%0)" ::"a"(startAddr + i));
  }
}
