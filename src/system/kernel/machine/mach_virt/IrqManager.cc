/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "IrqManager.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
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
constexpr uintptr_t DistClearPending = 0x280;
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
      m_Its(),
      m_Version(0),
      m_IrqCount(0),
      m_MsiFirst(0),
      m_MsiLast(0),
      m_MsiAddress(0),
      m_MsiReady(false),
      m_Initialised(false) {}

bool VirtIrqManager::initialise() {
  if (m_Initialised) {
    return true;
  }
  m_Version = VirtDeviceTree::gicVersion();
  m_Initialised = m_Version == 2 ? initialiseV2() : m_Version == 3 && initialiseV3();
  if (m_Initialised) {
    m_MsiReady = initialiseMsi();
  }
  return m_Initialised;
}

bool VirtIrqManager::initialiseThreaded() {
  return m_Initialised && m_PciDispatcher.initialise();
}

bool VirtIrqManager::shutdownThreaded() {
  {
    LockGuard<Spinlock> guard(m_PciLock);
    for (const PciLine& line : m_PciLines) {
      if (line.handlers || line.reserved) {
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
  m_IrqCount = count;
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
  m_IrqCount = count;
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

bool VirtIrqManager::setEnabled(uint32_t irq, bool enabled) {
  if (!m_Initialised) {
    return false;
  }
  if (irq >= MaxIrqs) {
    return m_Its.setEnabled(irq, enabled);
  }
  uintptr_t base = VirtDeviceTree::gicDistributorBase();
  if (m_Version == 3 && irq < 32) {
    base = VirtDeviceTree::gicRedistributorBase() + RedistSgi;
  }
  write32(base, (enabled ? DistEnable : DistDisable) + (irq / 32) * 4, 1U << (irq % 32));
  barrier();
  return true;
}

void VirtIrqManager::setLevel(uint32_t irq) {
  const uintptr_t dist = VirtDeviceTree::gicDistributorBase();
  const uintptr_t offset = DistConfig + (irq / 16) * 4;
  write32(dist, offset, read32(dist, offset) & ~(2U << ((irq % 16) * 2)));
  barrier();
}

void VirtIrqManager::setEdge(uint32_t irq) {
  const uintptr_t dist = VirtDeviceTree::gicDistributorBase();
  const uintptr_t offset = DistConfig + (irq / 16) * 4;
  write32(dist, offset, read32(dist, offset) | (2U << ((irq % 16) * 2)));
  barrier();
}

bool VirtIrqManager::initialiseMsi() {
  VirtMsiController controller = {};
  if (!VirtDeviceTree::pciMsiController(controller)) {
    return false;
  }
  if (controller.type == VirtMsiController::Type::GicV3Its) {
    if (!m_Its.initialise(controller, VirtDeviceTree::gicRedistributorBase(),
                          VirtDeviceTree::gicRedistributorPhysical())) {
      return false;
    }
    m_MsiAddress = m_Its.messageAddress();
    NOTICE("PCI: GICv3 ITS LPIs " << Dec << GicIts::FirstLpi << ".."
                                  << GicIts::FirstLpi + MaxPciLines - 1);
    return true;
  }
  if (controller.type != VirtMsiController::Type::GicV2m || controller.size < 0x44 ||
      controller.base > UINT64_MAX - 0x40) {
    return false;
  }
#if ARMV7
  constexpr uintptr_t DirectMapBase = 0x80000000;
#else
  constexpr uintptr_t DirectMapBase = 0xffff000000000000ULL;
#endif
  const uint32_t typer = read32(DirectMapBase + controller.base, 0x8);
  const uint32_t first = controller.spiBase ? controller.spiBase : (typer >> 16) & 0x3ff;
  const uint32_t count = controller.spiCount ? controller.spiCount : typer & 0x3ff;
  if (first < 32 || first >= m_IrqCount || !count || count > m_IrqCount - first) {
    return false;
  }
  m_MsiFirst = first;
  m_MsiLast = first + count;
  m_MsiAddress = controller.base + 0x40;
  NOTICE("PCI: GICv2m MSI SPIs " << Dec << m_MsiFirst << ".." << m_MsiLast - 1);
  return true;
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

uint8_t VirtIrqManager::pciRegistryLine(size_t slot) const {
  return static_cast<uint8_t>(m_PciLines[slot].lpi ? slot : m_PciLines[slot].irq);
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
  if (line.removing || line.quarantined || line.reserved || line.message ||
      (line.handlers && line.hard != hard)) {
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

irq_id_t VirtIrqManager::registerPciMessageHandler(IrqHandlerBase* handler, Device* device,
                                                   bool hard, bool& allowFallback) {
  allowFallback = true;
  if (!m_MsiReady || !handler || !device || (!hard && !m_PciDispatcher.isInitialised())) {
    return 0;
  }
  PciBus& pci = PciBus::instance();
  PciFunctionState::State original;
  if (!pci.inspectFunction(device, original, false) || (!original.msi && !original.msix)) {
    return 0;
  }
  uint32_t deviceId = 0;
  if (!VirtDeviceTree::pciMsiDeviceId(device->getPciBusPosition(), device->getPciDevicePosition(),
                                      device->getPciFunctionNumber(), deviceId)) {
    return 0;
  }
  const bool lpi = m_Its.ready();
  size_t slot = MaxPciLines;
  uint32_t irq = 0;
  {
    LockGuard<Spinlock> guard(m_PciLock);
    for (const PciLine& line : m_PciLines) {
      if (line.message && line.deviceId == deviceId) {
        allowFallback = false;
        return 0;
      }
    }
    for (size_t i = 0; i < MaxPciLines; ++i) {
      if (!m_PciLines[i].irq && !m_PciLines[i].reserved && !m_PciLines[i].quarantined) {
        slot = i;
        break;
      }
    }
    if (slot == MaxPciLines) {
      return 0;
    }
    if (lpi) {
      irq = GicIts::FirstLpi + slot;
    } else {
      for (uint32_t candidate = m_MsiFirst; candidate < m_MsiLast; ++candidate) {
        if (pciLine(candidate) == MaxPciLines && !m_Hard[candidate] && !m_Scheduler[candidate]) {
          irq = candidate;
          break;
        }
      }
    }
    if (!irq) {
      return 0;
    }
    PciLine& line = m_PciLines[slot];
    line.irq = irq;
    line.reserved = true;
    line.message = true;
    line.lpi = lpi;
    line.deviceId = deviceId;
    line.device = device;
    line.hard = hard;
    line.spuriousSafe = !hard && static_cast<IrqHandler*>(handler)->acceptsSpuriousInterrupts();
    if (!lpi) {
      setEnabled(irq, false);
      setEdge(irq);
      const uintptr_t dist = VirtDeviceTree::gicDistributorBase();
      write32(dist, DistClearPending + (irq / 32) * 4, 1U << (irq % 32));
      barrier();
    }
  }

  if (lpi && !m_Its.mapDevice(slot, deviceId, irq)) {
    LockGuard<Spinlock> guard(m_PciLock);
    PciLine& line = m_PciLines[slot];
    line.reserved = false;
    if (m_Its.mappingActive(slot)) {
      line.quarantined = true;
      allowFallback = false;
    } else {
      line.irq = 0;
      line.message = false;
      line.lpi = false;
      line.device = nullptr;
    }
    return 0;
  }

  bool registered = false;
  {
    LockGuard<Spinlock> guard(m_PciLock);
    const uint8_t key = pciRegistryLine(slot);
    registered = hard ? m_PciHandlers.registerHardHandler(
                            key, static_cast<HardIrqHandler*>(handler), IrqPolicy::edgeHard())
                      : m_PciHandlers.registerThreadedHandler(
                            key, static_cast<IrqHandler*>(handler), IrqPolicy::edgeThreaded());
    if (registered) {
      PciLine& line = m_PciLines[slot];
      line.handlers = 1;
    }
  }

  const uint32_t data = lpi ? 0 : irq;
  const bool msix = registered && original.msix && pci.enableMsix(device, m_MsiAddress, data);
  const bool msi = registered && !msix && original.msi && pci.enableMsi(device, m_MsiAddress, data);
  if (msix || msi) {
    LockGuard<Spinlock> guard(m_PciLock);
    PciLine& line = m_PciLines[slot];
    line.msix = msix;
    line.reserved = false;
    if (setEnabled(irq, true)) {
      return irq;
    }
    line.quarantined = true;
  }

  uint16_t command = 0;
  const bool disabled =
      !registered ||
      (pci.disableMessageInterrupts(device, original) &&
       pci.updateCommand(device, 0x400U, original.command & 0x400U) &&
       pci.readConfig16(device, 4, command) && ((command ^ original.command) & 0x400U) == 0);
  const auto removed =
      registered ? m_PciHandlers.unregisterHandler(
                       lpi ? static_cast<uint8_t>(slot) : static_cast<uint8_t>(irq), handler)
                 : IrqHandlerRegistry::UnregisterResult::Completed;
  const bool unmapped = !lpi || (disabled && m_Its.unmapDevice(slot));
  {
    LockGuard<Spinlock> guard(m_PciLock);
    PciLine& line = m_PciLines[slot];
    line.reserved = false;
    if (disabled && unmapped && removed == IrqHandlerRegistry::UnregisterResult::Completed) {
      line.irq = 0;
      line.handlers = 0;
      line.message = false;
      line.msix = false;
      line.lpi = false;
      line.device = nullptr;
    } else {
      line.quarantined = true;
      allowFallback = false;
    }
  }
  return 0;
}

irq_id_t VirtIrqManager::registerPciMessageIrqHandler(IrqHandler* handler, Device* device,
                                                      const IrqPolicy& intxFallbackPolicy) {
  bool fallback = true;
  const irq_id_t id = registerPciMessageHandler(handler, device, false, fallback);
  return id || !fallback ? id : registerPciIrqHandler(handler, device, intxFallbackPolicy);
}

bool VirtIrqManager::registerPciMsixIrqHandlers(Device* device, IrqHandler* const* handlers,
                                                size_t count, irq_id_t* ids, bool& fallbackSafe,
                                                const size_t*) {
  fallbackSafe = true;
  if (!m_MsiReady || !m_PciDispatcher.isInitialised() || !device || !handlers || !ids || !count ||
      count > MaxPciLines) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    ids[i] = 0;
    if (!handlers[i]) {
      return false;
    }
  }

  PciBus& pci = PciBus::instance();
  PciFunctionState::State original;
  if (!pci.inspectFunction(device, original, false) || !original.msix) {
    return false;
  }
  uint32_t deviceId = 0;
  if (!VirtDeviceTree::pciMsiDeviceId(device->getPciBusPosition(), device->getPciDevicePosition(),
                                      device->getPciFunctionNumber(), deviceId)) {
    return false;
  }

  const bool lpi = m_Its.ready();
  size_t slots[MaxPciLines] = {};
  uint32_t irqs[MaxPciLines] = {};
  uint32_t data[MaxPciLines] = {};
  size_t reserved = 0;
  {
    LockGuard<Spinlock> guard(m_PciLock);
    for (const PciLine& line : m_PciLines) {
      if (line.message && line.deviceId == deviceId) {
        fallbackSafe = false;
        return false;
      }
    }
    for (; reserved < count; ++reserved) {
      size_t slot = MaxPciLines;
      for (size_t i = 0; i < MaxPciLines; ++i) {
        if (!m_PciLines[i].irq && !m_PciLines[i].reserved && !m_PciLines[i].quarantined) {
          slot = i;
          break;
        }
      }
      if (slot == MaxPciLines) {
        break;
      }
      uint32_t irq = lpi ? GicIts::FirstLpi + slot : 0;
      if (!lpi) {
        for (uint32_t candidate = m_MsiFirst; candidate < m_MsiLast; ++candidate) {
          if (pciLine(candidate) == MaxPciLines && !m_Hard[candidate] && !m_Scheduler[candidate]) {
            irq = candidate;
            break;
          }
        }
      }
      if (!irq) {
        break;
      }
      slots[reserved] = slot;
      irqs[reserved] = irq;
      data[reserved] = lpi ? static_cast<uint32_t>(reserved) : irq;
      PciLine& line = m_PciLines[slot];
      line.irq = irq;
      line.reserved = true;
      line.message = true;
      line.msix = true;
      line.msixIndex = static_cast<uint8_t>(reserved);
      line.lpi = lpi;
      line.deviceId = deviceId;
      line.device = device;
      if (!lpi) {
        setEnabled(irq, false);
        setEdge(irq);
        const uintptr_t dist = VirtDeviceTree::gicDistributorBase();
        write32(dist, DistClearPending + (irq / 32) * 4, 1U << (irq % 32));
        barrier();
      }
    }
    if (reserved != count) {
      for (size_t i = 0; i < reserved; ++i) {
        PciLine& line = m_PciLines[slots[i]];
        const size_t cookie = line.cookie;
        line = PciLine{};
        line.cookie = cookie;
      }
      return false;
    }
  }

  if (lpi && !m_Its.mapDeviceGroup(slots, count, deviceId)) {
    bool active = false;
    for (size_t i = 0; i < count; ++i) {
      active = m_Its.mappingActive(slots[i]) || active;
    }
    LockGuard<Spinlock> guard(m_PciLock);
    for (size_t i = 0; i < count; ++i) {
      PciLine& line = m_PciLines[slots[i]];
      if (active) {
        line.quarantined = true;
      } else {
        const size_t cookie = line.cookie;
        line = PciLine{};
        line.cookie = cookie;
      }
    }
    fallbackSafe = !active;
    return false;
  }

  size_t registered = 0;
  {
    LockGuard<Spinlock> guard(m_PciLock);
    for (; registered < count; ++registered) {
      const uint8_t key = pciRegistryLine(slots[registered]);
      if (!m_PciHandlers.registerThreadedHandler(key, handlers[registered],
                                                 IrqPolicy::edgeThreaded())) {
        break;
      }
      m_PciLines[slots[registered]].handlers = 1;
      m_PciLines[slots[registered]].spuriousSafe =
          handlers[registered]->acceptsSpuriousInterrupts();
    }
  }

  bool enabled = registered == count;
  if (enabled) {
    LockGuard<Spinlock> guard(m_PciLock);
    for (size_t i = 0; i < count; ++i) {
      m_PciLines[slots[i]].reserved = false;
      if (!setEnabled(irqs[i], true)) {
        enabled = false;
        break;
      }
    }
  }

  bool touched = false;
  if (enabled && pci.enableMsixVectors(device, m_MsiAddress, data, count, &touched)) {
    for (size_t i = 0; i < count; ++i) {
      ids[i] = irqs[i];
    }
    return true;
  }

  bool safe = true;
  if (enabled) {
    uint16_t command = 0;
    safe = pci.disableMessageInterrupts(device, original);
    if (touched) {
      for (size_t i = 0; i < count; ++i) {
        const bool masked = pci.setMsixVectorMask(device, i, true);
        safe = masked && safe;
      }
    }
    safe = pci.updateCommand(device, 0x400U, original.command & 0x400U) && safe;
    safe = pci.readConfig16(device, 4, command) && ((command ^ original.command) & 0x400U) == 0 &&
           safe;
  }

  size_t cookies[MaxPciLines] = {};
  {
    LockGuard<Spinlock> guard(m_PciLock);
    for (size_t i = 0; i < count; ++i) {
      PciLine& line = m_PciLines[slots[i]];
      line.removing = true;
      const bool masked = setEnabled(irqs[i], false);
      safe = masked && safe;
      cookies[i] = line.cookie;
      ++line.cookie;
      if (!line.cookie) {
        ++line.cookie;
      }
    }
  }
  for (size_t i = 0; i < registered; ++i) {
    const uint8_t key = lpi ? static_cast<uint8_t>(slots[i]) : static_cast<uint8_t>(irqs[i]);
    m_PciHandlers.invalidateThreadedLine(key, cookies[i]);
    if (m_PciHandlers.unregisterHandler(key, handlers[i]) !=
        IrqHandlerRegistry::UnregisterResult::Completed) {
      FATAL("ARM PCI MSI-X setup could not drain its handler");
    }
  }
  if (lpi) {
    for (size_t i = 0; i < count; ++i) {
      const bool unmapped = m_Its.unmapDevice(slots[i]);
      safe = unmapped && safe;
    }
  }
  {
    LockGuard<Spinlock> guard(m_PciLock);
    for (size_t i = 0; i < count; ++i) {
      PciLine& line = m_PciLines[slots[i]];
      if (safe) {
        const size_t cookie = line.cookie;
        line = PciLine{};
        line.cookie = cookie;
      } else {
        line.quarantined = true;
        line.reserved = true;
        line.handlers = 0;
      }
    }
  }
  fallbackSafe = safe;
  return false;
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

irq_id_t VirtIrqManager::registerHardPciMessageIrqHandler(HardIrqHandler* handler, Device* device,
                                                          const IrqPolicy& intxFallbackPolicy) {
  bool fallback = true;
  const irq_id_t id = registerPciMessageHandler(handler, device, true, fallback);
  return id || !fallback ? id : registerHardPciIrqHandler(handler, device, intxFallbackPolicy);
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
  if (!m_Initialised || !handler ||
      (id >= MaxIrqs &&
       (!m_Its.ready() || id < GicIts::FirstLpi || id >= GicIts::FirstLpi + MaxPciLines))) {
    return false;
  }
  size_t slot = MaxPciLines;
  Device* messageDevice = nullptr;
  bool messageMsix = false;
  bool messageLpi = false;
  bool messageLast = false;
  uint8_t msixIndex = 0;
  uint8_t registryKey = 0;
  {
    LockGuard<Spinlock> guard(m_PciLock);
    slot = pciLine(id);
    if (slot != MaxPciLines) {
      if (Processor::executionContext() != ExecutionContext::WaitableThread ||
          !Processor::getInterrupts() || m_PciDispatcher.isCurrentWorker()) {
        ERROR("PCI: IRQ " << Dec << id << " retirement requires a waitable thread");
        return false;
      }
      PciLine& line = m_PciLines[slot];
      if (line.removing || !line.handlers) {
        ERROR("PCI: IRQ " << Dec << id << " is not active for retirement");
        return false;
      }
      if (line.message) {
        for (size_t i = 0; i < MaxPciLines; ++i) {
          if (i != slot && m_PciLines[i].message && m_PciLines[i].device == line.device &&
              m_PciLines[i].removing) {
            ERROR("PCI: IRQ " << Dec << id << " has another vector retiring");
            return false;
          }
        }
      }
      line.removing = true;
      if (!setEnabled(id, false)) {
        ERROR("PCI: could not mask GIC IRQ " << Dec << id);
        line.removing = false;
        line.quarantined = true;
        return false;
      }
      registryKey = pciRegistryLine(slot);
      if (line.message) {
        messageDevice = line.device;
        messageMsix = line.msix;
        messageLpi = line.lpi;
        msixIndex = line.msixIndex;
        messageLast = true;
        for (size_t i = 0; i < MaxPciLines; ++i) {
          if (i != slot && m_PciLines[i].message && m_PciLines[i].device == messageDevice) {
            messageLast = false;
            break;
          }
        }
      }
    }
  }
  if (slot != MaxPciLines) {
    bool sourceDisabled = true;
    if (messageDevice) {
      if (messageMsix) {
        sourceDisabled = PciBus::instance().setMsixVectorMask(messageDevice, msixIndex, true);
        if (sourceDisabled && messageLast) {
          sourceDisabled = PciBus::instance().disableMsix(messageDevice);
        }
      } else {
        sourceDisabled = PciBus::instance().disableMsi(messageDevice);
      }
    }
    if (!sourceDisabled) {
      ERROR("PCI: could not disable message source for IRQ " << Dec << id);
      LockGuard<Spinlock> guard(m_PciLock);
      PciLine& line = m_PciLines[slot];
      line.removing = false;
      line.quarantined = true;
      return false;
    }
    if (messageLpi && !m_Its.unmapDevice(slot)) {
      ERROR("PCI: could not unmap ITS LPI " << Dec << id);
      LockGuard<Spinlock> guard(m_PciLock);
      PciLine& line = m_PciLines[slot];
      line.removing = false;
      line.quarantined = true;
      return false;
    }
    const auto removed = m_PciHandlers.unregisterHandler(registryKey, handler);
    size_t retiredCookie = 0;
    {
      LockGuard<Spinlock> guard(m_PciLock);
      PciLine& line = m_PciLines[slot];
      if (removed != IrqHandlerRegistry::UnregisterResult::Completed) {
        ERROR("PCI: could not retire IRQ handler " << Dec << id);
        if (removed != IrqHandlerRegistry::UnregisterResult::NotFound) {
          line.quarantined = true;
        }
        line.removing = false;
        if (line.handlers && !line.message && !line.inFlight && !line.quarantined) {
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
        if (line.message) {
          line.irq = 0;
          line.message = false;
          line.msix = false;
          line.lpi = false;
          line.device = nullptr;
        }
      }
    }
    if (retiredCookie) {
      m_PciHandlers.invalidateThreadedLine(registryKey, retiredCookie);
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
  if (id >= MaxIrqs) {
    return false;
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
  uint32_t irq = 0;
  uint8_t registryKey = 0;
  {
    LockGuard<Spinlock> guard(manager.m_PciLock);
    if (slot >= MaxPciLines || !manager.m_PciLines[slot].irq ||
        manager.m_PciLines[slot].cookie != cookie || manager.m_PciLines[slot].hard) {
      return;
    }
    irq = manager.m_PciLines[slot].irq;
    registryKey = manager.pciRegistryLine(slot);
  }

  IrqHandlerRegistry::ThreadedDispatchResult result = {};
  const bool admitted =
      manager.m_PciHandlers.dispatchThreaded(registryKey, cookie, result, nullptr, irq);
  {
    LockGuard<Spinlock> guard(manager.m_PciLock);
    PciLine& line = manager.m_PciLines[slot];
    if (line.irq != irq || line.cookie != cookie) {
      return;
    }
    line.inFlight = false;
    if (!admitted || (!result.allowRearm && !(line.message && line.spuriousSafe))) {
      line.quarantined = true;
    }
    if (line.handlers && !line.removing && !line.quarantined) {
      if (!manager.setEnabled(irq, true)) {
        line.quarantined = true;
      }
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
  if (irq >= MaxIrqs &&
      (!m_Its.ready() || irq < GicIts::FirstLpi || irq >= GicIts::FirstLpi + MaxPciLines)) {
    complete(acknowledgeValue);
    return;
  }

  SchedulerIrqHandler* scheduler = irq < MaxIrqs ? m_Scheduler[irq] : nullptr;
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
  bool pciMessage = false;
  uint8_t registryKey = 0;
  {
    LockGuard<Spinlock> guard(m_PciLock);
    pciSlot = pciLine(irq);
    if (pciSlot != MaxPciLines) {
      PciLine& line = m_PciLines[pciSlot];
      if (!setEnabled(irq, false)) {
        line.quarantined = true;
        complete(acknowledgeValue);
        return;
      }
      if (!line.handlers || line.reserved || line.inFlight || line.removing || line.quarantined) {
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
      pciMessage = line.message;
      registryKey = pciRegistryLine(pciSlot);
      if (pciMessage) {
        complete(acknowledgeValue);
      }
      if (!pciHard) {
        const bool published =
            m_PciHandlers.publishThreadedDispatch(registryKey, pciCookie) &&
            m_PciDispatcher.publishFromInterrupt(static_cast<uint8_t>(pciSlot), pciCookie);
        if (!published) {
          m_PciHandlers.invalidateThreadedGenerationFromInterrupt(registryKey, pciCookie);
          line.inFlight = false;
          line.quarantined = true;
        }
        if (!pciMessage) {
          complete(acknowledgeValue);
        }
        return;
      }
    }
  }

  if (pciSlot != MaxPciLines && pciHard) {
    HardIrqDisposition disposition = HardIrqDisposition::NotHandled;
    const bool admitted =
        m_PciHandlers.dispatchHard(registryKey, state, disposition, nullptr, 0, irq);
    LockGuard<Spinlock> guard(m_PciLock);
    PciLine& line = m_PciLines[pciSlot];
    if (!pciMessage) {
      complete(acknowledgeValue);
    }
    if (line.cookie == pciCookie) {
      line.inFlight = false;
      if (!admitted || disposition != HardIrqDisposition::Handled) {
        line.quarantined = true;
      }
      if (line.handlers && !line.removing && !line.quarantined) {
        if (!setEnabled(irq, true)) {
          line.quarantined = true;
        }
      }
    }
    return;
  }

  if (irq >= MaxIrqs) {
    complete(acknowledgeValue);
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
