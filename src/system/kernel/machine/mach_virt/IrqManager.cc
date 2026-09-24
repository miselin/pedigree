/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "IrqManager.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/machine/SchedulerIrqHandler.h"
#include "pedigree/kernel/processor/Processor.h"

#include "DeviceTree.h"
#include "system/kernel/core/processor/DeviceHardIrqContext.h"

namespace {
constexpr uintptr_t DistControl = 0x000;
constexpr uintptr_t DistType = 0x004;
constexpr uintptr_t DistGroup = 0x080;
constexpr uintptr_t DistEnable = 0x100;
constexpr uintptr_t DistDisable = 0x180;
constexpr uintptr_t DistPriority = 0x400;
constexpr uintptr_t DistTarget = 0x800;
constexpr uintptr_t DistConfig = 0xc00;
constexpr uintptr_t DistRouter = 0x6000;
constexpr uintptr_t CpuControl = 0x000;
constexpr uintptr_t CpuPriorityMask = 0x004;
constexpr uintptr_t CpuAcknowledge = 0x00c;
constexpr uintptr_t CpuEndOfInterrupt = 0x010;
constexpr uintptr_t RedistWaker = 0x014;
constexpr uintptr_t RedistSgi = 0x10000;

// QEMU HVF needs unindexed, single-register MMIO accesses with valid syndromes.
uint32_t read32(uintptr_t base, uintptr_t offset) {
  uint32_t value;
#if ARMV7
  asm volatile("ldr %0, [%1]" : "=r"(value) : "r"(base + offset) : "memory");
#else
  asm volatile("ldr %w0, [%1]" : "=r"(value) : "r"(base + offset) : "memory");
#endif
  return value;
}

void write32(uintptr_t base, uintptr_t offset, uint32_t value) {
#if ARMV7
  asm volatile("str %1, [%0]" : : "r"(base + offset), "r"(value) : "memory");
#else
  asm volatile("str %w1, [%0]" : : "r"(base + offset), "r"(value) : "memory");
#endif
}

#if ARM64
void write64(uintptr_t base, uintptr_t offset, uint64_t value) {
  asm volatile("str %1, [%0]" : : "r"(base + offset), "r"(value) : "memory");
}
#endif

void barrier() {
  asm volatile("dsb sy\n\tisb" : : : "memory");
}

#if ARM64
uint64_t processorAffinity() {
  uint64_t mpidr;
  asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
  return (mpidr & 0xff00000000ULL) | (mpidr & 0x00ffffffULL);
}
#endif
}  // namespace

VirtIrqManager VirtIrqManager::m_Instance;

VirtIrqManager& VirtIrqManager::instance() {
  return m_Instance;
}

VirtIrqManager::VirtIrqManager()
    : m_Hard(),
      m_Scheduler(),
      m_PciHandlers(),
      m_PciDispatcher(MakeConstantString("Virt PCI IRQ"), MaxPciLines, dispatchPciLine, this),
      m_PciLock(false),
      m_PciLines(),
      m_Version(0),
      m_Initialised(false) {}

bool VirtIrqManager::initialise() {
  if (m_Initialised) {
    return true;
  }
  m_Version = VirtDeviceTree::gicVersion();
  m_Initialised = m_Version == 2 ? initialiseV2() : m_Version == 3 && initialiseV3();
  return m_Initialised;
}

bool VirtIrqManager::initialiseThreaded() {
  return m_Initialised && m_PciDispatcher.initialise();
}

bool VirtIrqManager::shutdownThreaded() {
  {
    LockGuard<Spinlock> guard(m_PciLock);
    for (const PciLine& line : m_PciLines) {
      if (line.handlers) {
        return false;
      }
    }
    for (const PciLine& line : m_PciLines) {
      if (line.irq) {
        setEnabled(line.irq, false);
      }
    }
  }
  return m_PciDispatcher.shutdown();
}

bool VirtIrqManager::initialiseV2() {
  const uintptr_t dist = VirtDeviceTree::gicDistributorBase();
  const uintptr_t cpu = VirtDeviceTree::gicCpuBase();
  if (!dist || !cpu) {
    return false;
  }

  write32(dist, DistControl, 0);
  const size_t lines = ((read32(dist, DistType) & 0x1f) + 1) * 32;
  const size_t count = lines < MaxIrqs ? lines : MaxIrqs;
  for (size_t irq = 0; irq < count; irq += 32) {
    write32(dist, DistDisable + irq / 8, 0xffffffff);
  }
  for (size_t irq = 0; irq < count; irq += 4) {
    write32(dist, DistPriority + irq, 0xa0a0a0a0);
    if (irq >= 32) {
      write32(dist, DistTarget + irq, 0x01010101);
    }
  }
  write32(cpu, CpuPriorityMask, 0xff);
  write32(cpu, CpuControl, 1);
  write32(dist, DistControl, 1);
  barrier();
  return true;
}

bool VirtIrqManager::initialiseV3() {
#if ARMV7
  return false;
#else
  const uintptr_t dist = VirtDeviceTree::gicDistributorBase();
  const uintptr_t redist = VirtDeviceTree::gicRedistributorBase();
  if (!dist || !redist) {
    return false;
  }

  write32(dist, DistControl, 0);
  for (size_t wait = 0; wait < 100000 && (read32(dist, DistControl) & (1U << 31)); ++wait) {
    asm volatile("yield");
  }
  // GICD_IROUTER is available only after affinity routing is enabled.
  write32(dist, DistControl, 1U << 4);
  for (size_t wait = 0; wait < 100000 && (read32(dist, DistControl) & (1U << 31)); ++wait) {
    asm volatile("yield");
  }
  if (!(read32(dist, DistControl) & (1U << 4))) {
    return false;
  }
  const size_t lines = ((read32(dist, DistType) & 0x1f) + 1) * 32;
  const size_t count = lines < MaxIrqs ? lines : MaxIrqs;
  for (size_t irq = 32; irq < count; irq += 32) {
    write32(dist, DistDisable + irq / 8, 0xffffffff);
    write32(dist, DistGroup + irq / 8, 0xffffffff);
  }
  for (size_t irq = 32; irq < count; irq += 4) {
    write32(dist, DistPriority + irq, 0xa0a0a0a0);
  }

  const uint64_t affinity = processorAffinity();
  for (size_t irq = 32; irq < count; ++irq) {
    write64(dist, DistRouter + irq * 8, affinity);
  }

  write32(redist, RedistWaker, read32(redist, RedistWaker) & ~(1U << 1));
  for (size_t wait = 0; wait < 100000 && (read32(redist, RedistWaker) & (1U << 2)); ++wait) {
    asm volatile("yield");
  }
  if (read32(redist, RedistWaker) & (1U << 2)) {
    return false;
  }

  const uintptr_t sgi = redist + RedistSgi;
  write32(sgi, DistDisable, 0xffffffff);
  write32(sgi, DistGroup, 0xffffffff);
  for (size_t irq = 0; irq < 32; irq += 4) {
    write32(sgi, DistPriority + irq, 0xa0a0a0a0);
  }

  uint64_t sre;
  asm volatile("mrs %0, ICC_SRE_EL1" : "=r"(sre));
  sre |= 1;
  asm volatile("msr ICC_SRE_EL1, %0\n\tisb" : : "r"(sre) : "memory");
  uint64_t cpuControl;
  asm volatile("mrs %0, ICC_CTLR_EL1" : "=r"(cpuControl));
  cpuControl &= ~(1U << 1);  // EOIR1 also deactivates the interrupt.
  asm volatile("msr ICC_CTLR_EL1, %0" : : "r"(cpuControl) : "memory");
  asm volatile("msr ICC_PMR_EL1, %0" : : "r"(uint64_t(0xff)) : "memory");
  asm volatile("msr ICC_BPR1_EL1, %0" : : "r"(uint64_t(0)) : "memory");
  asm volatile("msr ICC_IGRPEN1_EL1, %0" : : "r"(uint64_t(1)) : "memory");

  // The two Group 1 enables cover both single- and two-security-state GICs.
  write32(dist, DistControl, (1U << 4) | (1U << 1) | 1U);
  barrier();
  return true;
#endif
}

void VirtIrqManager::setEnabled(uint32_t irq, bool enabled) {
  if (!m_Initialised || irq >= MaxIrqs) {
    return;
  }
  uintptr_t base = VirtDeviceTree::gicDistributorBase();
  if (m_Version == 3 && irq < 32) {
    base = VirtDeviceTree::gicRedistributorBase() + RedistSgi;
  }
  write32(base, (enabled ? DistEnable : DistDisable) + (irq / 32) * 4, 1U << (irq % 32));
  barrier();
}

void VirtIrqManager::setLevel(uint32_t irq) {
  const uintptr_t dist = VirtDeviceTree::gicDistributorBase();
  const uintptr_t offset = DistConfig + (irq / 16) * 4;
  write32(dist, offset, read32(dist, offset) & ~(2U << ((irq % 16) * 2)));
  barrier();
}

void VirtIrqManager::enable(uint8_t irq, bool enabled) {
  setEnabled(irq, enabled);
}

irq_id_t VirtIrqManager::registerIsaIrqHandler(uint8_t, IrqHandler*, const IrqPolicy&) {
  return 0;
}

size_t VirtIrqManager::pciLine(uint32_t irq) const {
  for (size_t i = 0; i < MaxPciLines; ++i) {
    if (m_PciLines[i].irq == irq) {
      return i;
    }
  }
  return MaxPciLines;
}

irq_id_t VirtIrqManager::registerPciHandler(IrqHandlerBase* handler, Device* device,
                                            const IrqPolicy& policy, bool hard) {
  if (!m_Initialised || !device || !handler || policy.trigger() != IrqTrigger::Level ||
      (hard ? !policy.validForHard() : !policy.validForThreaded()) ||
      (!hard && !m_PciDispatcher.isInitialised())) {
    return 0;
  }
  uint8_t pin = 0;
  PciBus& pci = PciBus::instance();
  if (!pci.readConfig8(device, 0x3d, pin)) {
    return 0;
  }
  const uint32_t irq =
      pci.interruptRoute(device->getPciBusPosition(), device->getPciDevicePosition(),
                         device->getPciFunctionNumber(), pin);
  if (irq < 32 || irq >= MaxIrqs || irq != device->getInterruptNumber()) {
    return 0;
  }

  LockGuard<Spinlock> guard(m_PciLock);
  if (m_Hard[irq] || m_Scheduler[irq]) {
    return 0;
  }
  size_t slot = pciLine(irq);
  if (slot == MaxPciLines) {
    for (size_t i = 0; i < MaxPciLines; ++i) {
      if (!m_PciLines[i].irq) {
        slot = i;
        break;
      }
    }
  }
  if (slot == MaxPciLines) {
    return 0;
  }
  PciLine& line = m_PciLines[slot];
  if (line.removing || line.quarantined || (line.handlers && line.hard != hard)) {
    return 0;
  }
  const bool first = !line.handlers;
  if (first) {
    setEnabled(irq, false);
    setLevel(irq);
  }
  const bool registered =
      hard ? m_PciHandlers.registerHardHandler(static_cast<uint8_t>(irq),
                                               static_cast<HardIrqHandler*>(handler), policy)
           : m_PciHandlers.registerThreadedHandler(static_cast<uint8_t>(irq),
                                                   static_cast<IrqHandler*>(handler), policy);
  if (!registered) {
    return 0;
  }
  line.irq = irq;
  line.hard = hard;
  ++line.handlers;
  if (!line.inFlight) {
    setEnabled(irq, true);
  }
  return irq;
}

irq_id_t VirtIrqManager::registerPciIrqHandler(IrqHandler* handler, Device* device,
                                               const IrqPolicy& policy) {
  return registerPciHandler(handler, device, policy, false);
}

irq_id_t VirtIrqManager::registerHardIsaIrqHandler(uint8_t irq, HardIrqHandler* handler,
                                                   const IrqPolicy& policy) {
  if (!m_Initialised || irq < 16 || !handler || !policy.validForHard()) {
    return 0;
  }
  const bool interrupts = Processor::getInterrupts();
  Processor::setInterrupts(false);
  if (m_Hard[irq] || m_Scheduler[irq]) {
    Processor::setInterrupts(interrupts);
    return 0;
  }
  m_Hard[irq] = handler;
  setEnabled(irq, true);
  Processor::setInterrupts(interrupts);
  return irq;
}

irq_id_t VirtIrqManager::registerHardPciIrqHandler(HardIrqHandler* handler, Device* device,
                                                   const IrqPolicy& policy) {
  return registerPciHandler(handler, device, policy, true);
}

irq_id_t VirtIrqManager::registerSchedulerIrqHandler(uint8_t irq, SchedulerIrqHandler* handler,
                                                     const IrqPolicy& policy) {
  if (!m_Initialised || irq < 16 || !handler || !policy.validForHard()) {
    return 0;
  }
  const bool interrupts = Processor::getInterrupts();
  Processor::setInterrupts(false);
  if (m_Hard[irq] || m_Scheduler[irq]) {
    Processor::setInterrupts(interrupts);
    return 0;
  }
  m_Scheduler[irq] = handler;
  setEnabled(irq, true);
  Processor::setInterrupts(interrupts);
  return irq;
}

bool VirtIrqManager::unregisterSchedulerIrqHandler(irq_id_t id, SchedulerIrqHandler* handler) {
  if (!m_Initialised || id >= MaxIrqs || !handler) {
    return false;
  }
  const bool interrupts = Processor::getInterrupts();
  Processor::setInterrupts(false);
  if (m_Scheduler[id] != handler) {
    Processor::setInterrupts(interrupts);
    return false;
  }
  setEnabled(id, false);
  m_Scheduler[id] = nullptr;
  Processor::setInterrupts(interrupts);
  return true;
}

bool VirtIrqManager::unregisterHandler(irq_id_t id, IrqHandlerBase* handler) {
  if (!m_Initialised || id >= MaxIrqs || !handler) {
    return false;
  }
  size_t slot = MaxPciLines;
  {
    LockGuard<Spinlock> guard(m_PciLock);
    slot = pciLine(id);
    if (slot != MaxPciLines) {
      if (Processor::executionContext() != ExecutionContext::WaitableThread ||
          !Processor::getInterrupts() || m_PciDispatcher.isCurrentWorker()) {
        return false;
      }
      PciLine& line = m_PciLines[slot];
      if (line.removing || !line.handlers) {
        return false;
      }
      line.removing = true;
      setEnabled(id, false);
    }
  }
  if (slot != MaxPciLines) {
    const auto removed = m_PciHandlers.unregisterHandler(static_cast<uint8_t>(id), handler);
    size_t retiredCookie = 0;
    {
      LockGuard<Spinlock> guard(m_PciLock);
      PciLine& line = m_PciLines[slot];
      if (removed != IrqHandlerRegistry::UnregisterResult::Completed) {
        if (removed != IrqHandlerRegistry::UnregisterResult::NotFound) {
          line.quarantined = true;
        }
        line.removing = false;
        if (line.handlers && !line.inFlight && !line.quarantined) {
          setEnabled(id, true);
        }
        return false;
      }
      --line.handlers;
      if (!line.handlers) {
        retiredCookie = line.cookie;
        ++line.cookie;
        if (!line.cookie) {
          ++line.cookie;
        }
        line.inFlight = false;
        line.quarantined = false;
      }
    }
    if (retiredCookie) {
      m_PciHandlers.invalidateThreadedLine(static_cast<uint8_t>(id), retiredCookie);
    }
    {
      LockGuard<Spinlock> guard(m_PciLock);
      PciLine& line = m_PciLines[slot];
      line.removing = false;
      if (line.handlers && !line.inFlight && !line.quarantined) {
        setEnabled(id, true);
      }
    }
    return true;
  }
  const bool interrupts = Processor::getInterrupts();
  Processor::setInterrupts(false);
  if (static_cast<IrqHandlerBase*>(m_Hard[id]) != handler) {
    Processor::setInterrupts(interrupts);
    return false;
  }
  setEnabled(id, false);
  m_Hard[id] = nullptr;
  Processor::setInterrupts(interrupts);
  return true;
}

void VirtIrqManager::dispatchPciLine(void* context, uint8_t slot, size_t cookie) {
  auto& manager = *static_cast<VirtIrqManager*>(context);
  uint8_t irq = 0;
  {
    LockGuard<Spinlock> guard(manager.m_PciLock);
    if (slot >= MaxPciLines || !manager.m_PciLines[slot].irq ||
        manager.m_PciLines[slot].cookie != cookie || manager.m_PciLines[slot].hard) {
      return;
    }
    irq = static_cast<uint8_t>(manager.m_PciLines[slot].irq);
  }

  IrqHandlerRegistry::ThreadedDispatchResult result = {};
  const bool admitted = manager.m_PciHandlers.dispatchThreaded(irq, cookie, result);
  {
    LockGuard<Spinlock> guard(manager.m_PciLock);
    PciLine& line = manager.m_PciLines[slot];
    if (line.irq != irq || line.cookie != cookie) {
      return;
    }
    line.inFlight = false;
    if (!admitted || !result.allowRearm) {
      line.quarantined = true;
    }
    if (line.handlers && !line.removing && !line.quarantined) {
      manager.setEnabled(irq, true);
    }
  }
}

uint32_t VirtIrqManager::acknowledge() {
#if ARM64
  if (m_Version == 3) {
    uint64_t value;
    asm volatile("mrs %0, ICC_IAR1_EL1" : "=r"(value));
    asm volatile("isb" : : : "memory");
    return static_cast<uint32_t>(value);
  }
#endif
  return read32(VirtDeviceTree::gicCpuBase(), CpuAcknowledge);
}

void VirtIrqManager::complete(uint32_t value) {
#if ARM64
  if (m_Version == 3) {
    asm volatile("msr ICC_EOIR1_EL1, %0\n\tisb" : : "r"(uint64_t(value)) : "memory");
  } else {
#endif
    write32(VirtDeviceTree::gicCpuBase(), CpuEndOfInterrupt, value);
#if ARM64
  }
#endif
}

void VirtIrqManager::handle(InterruptState& state) {
  if (!m_Initialised) {
    return;
  }
  const uint32_t acknowledgeValue = acknowledge();
  const uint32_t irq = acknowledgeValue & (m_Version == 3 ? 0xffffff : 0x3ff);
  if (irq >= 1020 && irq <= 1023) {
    return;
  }
  if (irq >= MaxIrqs) {
    complete(acknowledgeValue);
    return;
  }

  SchedulerIrqHandler* scheduler = m_Scheduler[irq];
  if (scheduler) {
    if (irq == VirtDeviceTree::virtualTimerIrq()) {
#if ARMV7
      uint32_t disabled = 0;
      asm volatile("mcr p15, 0, %0, c14, c3, 1\n\tisb" : : "r"(disabled) : "memory");
#else
      asm volatile("msr cntv_ctl_el0, %0\n\tisb" : : "r"(uint64_t(0)) : "memory");
#endif
    }
    // A scheduler callback may abandon this interrupt frame permanently.
    complete(acknowledgeValue);
    scheduler->schedulerIrq(irq, state);
    return;
  }

  size_t pciSlot = MaxPciLines;
  size_t pciCookie = 0;
  bool pciHard = false;
  {
    LockGuard<Spinlock> guard(m_PciLock);
    pciSlot = pciLine(irq);
    if (pciSlot != MaxPciLines) {
      PciLine& line = m_PciLines[pciSlot];
      setEnabled(irq, false);
      if (!line.handlers || line.inFlight || line.removing || line.quarantined) {
        complete(acknowledgeValue);
        return;
      }
      line.inFlight = true;
      ++line.cookie;
      if (!line.cookie) {
        ++line.cookie;
      }
      pciCookie = line.cookie;
      pciHard = line.hard;
      if (!pciHard) {
        const bool published =
            m_PciHandlers.publishThreadedDispatch(static_cast<uint8_t>(irq), pciCookie) &&
            m_PciDispatcher.publishFromInterrupt(static_cast<uint8_t>(pciSlot), pciCookie);
        if (!published) {
          m_PciHandlers.invalidateThreadedGenerationFromInterrupt(static_cast<uint8_t>(irq),
                                                                  pciCookie);
          line.inFlight = false;
          line.quarantined = true;
        }
        complete(acknowledgeValue);
        return;
      }
    }
  }

  if (pciSlot != MaxPciLines && pciHard) {
    HardIrqDisposition disposition = HardIrqDisposition::NotHandled;
    const bool admitted = m_PciHandlers.dispatchHard(static_cast<uint8_t>(irq), state, disposition);
    LockGuard<Spinlock> guard(m_PciLock);
    PciLine& line = m_PciLines[pciSlot];
    complete(acknowledgeValue);
    if (line.cookie == pciCookie) {
      line.inFlight = false;
      if (!admitted || disposition != HardIrqDisposition::Handled) {
        line.quarantined = true;
      }
      if (line.handlers && !line.removing && !line.quarantined) {
        setEnabled(irq, true);
      }
    }
    return;
  }

  HardIrqHandler* hard = m_Hard[irq];
  if (hard) {
    size_t previousDepth = 0;
    bool restoreDepth = false;
    DeviceHardIrqContext context(previousDepth, restoreDepth);
    if (hard->irq(irq, state) == HardIrqDisposition::KeepMasked) {
      setEnabled(irq, false);
    }
  } else {
    setEnabled(irq, false);
  }
  complete(acknowledgeValue);
}

extern "C" void virtHandleIrq(InterruptState& state) {
  VirtIrqManager::instance().handle(state);
}
