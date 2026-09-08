/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "Xhci.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/machine/PciFunctionState.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/utility.h"
using namespace XhciHw;
Xhci::Xhci(Device* device)
    : UsbHub(device),
      RequestQueue(MakeConstantString("xHCI")),
      m_Pci(this),
      m_Events("xHCI events"),
      m_Erst("xHCI ERST"),
      m_Dcbaa("xHCI DCBAA"),
      m_Input("xHCI input context"),
      m_ScratchPointers("xHCI scratchpad pointers") {}
Xhci::~Xhci() {
  shutdown();
}
uint32_t Xhci::read(size_t offset) const {
  return m_Registers->read32(offset);
}
void Xhci::write(size_t offset, uint32_t value) {
  m_Registers->write32(value, offset);
}
void Xhci::write64(size_t offset, uint64_t value) {
  write(offset, value);
  write(offset + 4, value >> 32);
}
bool Xhci::wait(size_t offset, uint32_t mask, uint32_t expected, size_t milliseconds) {
  const auto deadline = Time::getTicks() + milliseconds * Time::Multiplier::Millisecond;
  do {
    const uint32_t value = read(offset);
    if (value != 0xffffffffU && (value & mask) == expected)
      return true;
    Time::delay(Time::Multiplier::Millisecond);
  } while (Time::getTicks() < deadline);
  return false;
}
bool Xhci::parseCapabilities(uint32_t hcc) {
  size_t offset = (hcc >> 16) * 4U;
  for (size_t count = 0; offset && count < 256; ++count) {
    if (offset + 16 > m_Registers->size())
      return false;
    const uint32_t cap = read(offset);
    if ((cap & 255U) == 1) {
      write(offset, cap | (1U << 24));
      if (!wait(offset, (1U << 16) | (1U << 24), 1U << 24, 5000))
        return false;
      write(offset + 4, (read(offset + 4) & 0x000e1feeU) | 0xe0000000U);
      if (read(offset + 4) & 0x0000e011U)
        return false;
    } else if ((cap & 255U) == 2) {
      const uint32_t ports = read(offset + 8);
      const size_t first = ports & 255U, count = (ports >> 8) & 255U;
      const size_t psiCount = ports >> 28;
      const uint8_t major = cap >> 24;
      if (!first || first - 1 + count > m_PortCount || (major != 2 && major != 3) ||
          read(offset + 4) != 0x20425355U || offset + 16 + psiCount * 4 > m_Registers->size())
        return false;
      for (size_t port = first - 1; port < first - 1 + count; ++port) {
        auto& target = m_Ports[port];
        if (target.major)
          return false;
        target.major = major;
        target.slotType = read(offset + 12) & 31U;
        if (!psiCount) {
          if (major == 2) {
            target.speeds[1] = FullSpeed;
            target.speeds[2] = LowSpeed;
            target.speeds[3] = HighSpeed;
            target.validSpeed[1] = target.validSpeed[2] = target.validSpeed[3] = true;
          } else {
            const uint8_t minor = (cap >> 16) & 255U;
            const size_t last = minor >= 0x20 ? 7 : minor >= 0x10 ? 5 : 4;
            for (size_t id = 4; id <= last; ++id) {
              target.speeds[id] = SuperSpeed;
              target.validSpeed[id] = true;
            }
          }
        }
        for (size_t i = 0; i < psiCount; ++i) {
          const uint32_t psi = read(offset + 16 + i * 4);
          const uint8_t id = psi & 15U;
          if (!id || (psi & 0xc0U))
            return false;
          uint64_t rate = psi >> 16;
          for (size_t exponent = (psi >> 4) & 3U; exponent; --exponent)
            rate *= 1000;
          UsbSpeed speed;
          if (rate == 1500000)
            speed = LowSpeed;
          else if (rate == 12000000)
            speed = FullSpeed;
          else if (rate == 480000000)
            speed = HighSpeed;
          else if (rate >= 5000000000ULL)
            speed = SuperSpeed;
          else
            return false;
          target.speeds[id] = speed;
          target.validSpeed[id] = true;
        }
      }
    }
    const size_t next = (cap >> 8) & 255U;
    if (!next)
      return true;
    offset += next * 4;
  }
  return !offset;
}
bool Xhci::initialiseController() {
#if PEDIGREE_USB_SMOKE_TESTS
  if (!completionRegressions())
    return false;
#endif
  auto& pci = PciBus::instance();
  PciFunctionState::State pciState{};
  if (!pci.inspectFunction(m_Pci, pciState)) {
    ERROR("xHCI: unsupported inherited PCI function state");
    return false;
  }
  const uint32_t bar = pciState.bars[0];
  if ((bar & 1U) || ((bar & 6U) != 0 && (bar & 6U) != 4))
    return false;
  uint64_t base = bar & ~15U;
  if (bar & 4U)
    base |= uint64_t{pciState.bars[1]} << 32;
  Device::Address* mapping = nullptr;
  for (auto* address : m_Pci->addresses())
    if (address->m_Name == "bar0" && base && !address->m_IsIoSpace && address->m_Address == base)
      mapping = address;
  if (!mapping || mapping->m_Size < 0x500)
    return false;
  // Keep firmware DMA live until its ownership semaphore has been released.
  if (!pci.updateCommand(m_Pci, 0, 2))
    return false;
  mapping->map();
  m_Registers = mapping->m_Io;
  if (!m_Registers)
    return false;
  const uint32_t version = read(0);
  m_Op = version & 255U;
  const uint32_t hcs1 = read(4), hcs2 = read(8), hcc = read(16);
  const size_t portCount = hcs1 >> 24;
  if (!portCount || portCount > MaxPorts)
    return false;
  m_PortCount = portCount;
  m_SlotCount = (hcs1 & 255U) < MaxSlots ? hcs1 & 255U : MaxSlots;
  m_ContextSize = hcc & 4U ? 64 : 32;
  m_Doorbells = read(20) & ~3U;
  m_Runtime = read(24) & ~31U;
  if ((version >> 16) < 0x100 || m_Op < 0x20 || !m_SlotCount || !m_PortCount ||
      m_PortCount > MaxPorts || m_Op + 0x400 + m_PortCount * 16 > m_Registers->size() ||
      m_Runtime + 0x40 > m_Registers->size() ||
      m_Doorbells + (m_SlotCount + 1) * 4 > m_Registers->size() || !parseCapabilities(hcc))
    return false;
  for (size_t i = 0; i < m_PortCount; ++i)
    if (!m_Ports[i].major)
      return false;
  if (!wait(m_Op + 4, 1U << 11, 0, 1000))
    return false;
  m_HardwareOwned = true;
  write(m_Op, read(m_Op) & ~5U);
  if (!wait(m_Op + 4, 1, 1, 1000))
    return false;
  write(m_Op, read(m_Op) | 2U);
  if (!wait(m_Op, 2, 0, 1000) || !wait(m_Op + 4, 1U << 11, 0, 1000) || !(read(m_Op + 8) & 1U))
    return false;
  if (!pci.updateCommand(m_Pci, 4, 2 | 0x400) || !pci.disableMessageInterrupts(m_Pci, pciState)) {
    ERROR("xHCI: could not establish masked PCI interrupt state");
    return false;
  }
  if (!m_Commands.initialise() || !allocate(m_Events, 1) || !allocate(m_Erst, 1) ||
      !allocate(m_Dcbaa, 1) || !allocate(m_Input, 1))
    return false;
  const size_t scratchpads = ((hcs2 >> 27) & 31U) | (((hcs2 >> 21) & 31U) << 5);
  if (scratchpads > 32)
    return false;
  auto* dcbaa = static_cast<uint64_t*>(m_Dcbaa.virtualAddress());
  if (scratchpads) {
    if (!allocate(m_ScratchPointers, 1))
      return false;
    dcbaa[0] = m_ScratchPointers.physicalAddress();
    auto* pointers = static_cast<uint64_t*>(m_ScratchPointers.virtualAddress());
    for (size_t i = 0; i < scratchpads; ++i) {
      m_Scratch[i] = new MemoryRegion("xHCI scratchpad");
      if (!allocate(*m_Scratch[i], 1))
        return false;
      pointers[i] = m_Scratch[i]->physicalAddress();
    }
  }
  auto* erst = static_cast<uint64_t*>(m_Erst.virtualAddress());
  erst[0] = m_Events.physicalAddress();
  erst[1] = RingEntries;
  // Controllers may fetch the segment table as soon as ERSTBA is programmed.
  FENCE();
  if (!pci.resourcesUnchanged(m_Pci, pciState) || !pci.updateCommand(m_Pci, 0, 6 | 0x400)) {
    ERROR("xHCI: PCI resources or DMA command changed during handoff");
    return false;
  }
  write64(m_Op + 0x30, m_Dcbaa.physicalAddress());
  write64(m_Op + 0x18, m_Commands.address() | 1U);
  write(m_Op + 0x38, m_SlotCount);
  write(m_Runtime + 0x28, 1);
  write64(m_Runtime + 0x30, m_Erst.physicalAddress());
  write64(m_Runtime + 0x38, m_Events.physicalAddress());
  write(m_Runtime + 0x24, 0);
  write(m_Runtime + 0x20, 1);
  write(m_Op + 4, 0x1c);
  if (read(m_Op + 4) & ((1U << 12) | 4U))
    return false;
  FENCE();
  RequestQueue::initialise();
  for (size_t port = 0; port < m_PortCount; ++port)
    if (!m_PortChanges[port].configure(*this, 0, port))
      return false;
  m_DeliveryThread =
      new Thread(Processor::information().getCurrentThread()->getParent(), deliveryWorker, this);
  m_Irq = Machine::instance().getIrqManager()->registerPciIrqHandler(this, m_Pci,
                                                                     IrqPolicy::pciIntxThreaded());
  if (!m_Irq)
    return false;
  {
    LockGuard<Mutex> lock(m_Lock);
    if (!pci.updateCommand(m_Pci, 0x400, 6))
      return false;
    m_Online = true;
    write(m_Runtime + 0x20, 3);
    write(m_Op, 13);
    (void)read(m_Op);
  }
  if (!wait(m_Op + 4, 1, 0, 1000))
    return false;
  for (size_t port = 0; port < m_PortCount; ++port) {
    LockGuard<Mutex> lock(m_Lock);
    const size_t reg = m_Op + 0x400 + port * 16;
    const uint32_t status = read(reg);
    write(reg, (status & 0x0e00c200U) | (1U << 9) | (status & PortChanges));
    (void)read(reg);
    if (status & 1U)
      notifyPortLocked(port);
  }
  NOTICE("xHCI: controller ready, " << Dec << m_PortCount << " ports, " << m_SlotCount
                                    << " slots, shared INTx" << Hex);
  return true;
}
bool Xhci::command(Trb trb, uint8_t* returnedSlot) {
  LockGuard<Mutex> serial(m_CommandLock);
  {
    LockGuard<Mutex> lock(m_Lock);
    if (!m_Online)
      return false;
    const size_t drained = m_CommandDone.drainAvailable();
    (void)drained;
    m_CommandCode = 0;
    if (!m_Commands.enqueue(&trb, 1, &m_CommandAddress))
      return false;
    write(m_Doorbells, 0);
    (void)read(m_Op + 4);
  }
  const auto deadline = Time::getTicks() + 5 * Time::Multiplier::Second;
  while (!m_CommandDone.acquireForCompletion(1, 0, 10000)) {
    LockGuard<Mutex> lock(m_Lock);
    collectEventsLocked(false);
    if (!m_Online)
      return false;
    if (Time::getTicks() >= deadline) {
      failLocked();
      return false;
    }
  }
  LockGuard<Mutex> lock(m_Lock);
  if (returnedSlot)
    *returnedSlot = m_CommandSlot;
  if (m_CommandCode != 1)
    WARNING("xHCI: command " << ((trb.control >> 10) & 63U) << " failed: " << m_CommandCode);
  return m_Online && m_CommandCode == 1;
}
