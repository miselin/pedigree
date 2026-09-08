/*
 * Copyright (c) 2026, Pedigree Developers
 * SPDX-License-Identifier: ISC
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "AhciController.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/new"

#include "AhciDisk.h"
#include "AhciPort.h"
#include "Registers.h"
#include "modules/drivers/common/InterruptProbe.h"

using namespace Ahci;

AhciController::AhciController(Device* pci)
    : ScsiController(),
      m_Pci(pci),
      m_Registers(nullptr),
      m_Ports{},
      m_Irq(0),
      m_Implemented(0),
      m_OriginalCommand(0),
      m_PciChanged(false),
      m_HardwareOwned(false),
      m_Interrupts(false),
      m_Stopping(false),
      m_Shutdown(false) {
  setSpecificType(String("ahci-controller"));
}
AhciController::~AhciController() {
  shutdown();
  for (size_t i = 0; i < 32; ++i)
    delete m_Ports[i];
}
bool AhciController::claimOwnership(uint32_t version) {
  if (version < 0x00010200 || !(m_Registers->read32(Cap2) & 1U))
    return true;
  uint32_t ownership = m_Registers->read32(Bohc);
  m_Registers->write32((ownership & ~(1U << 3)) | (1U << 1), Bohc);
  const auto start = Time::getTicks();
  bool busy = (ownership & (1U << 4)) != 0;
  do {
    ownership = m_Registers->read32(Bohc);
    busy |= (ownership & (1U << 4)) != 0;
    if (!(ownership & ((1U << 4) | 1U)))
      return true;
    Time::delay(Time::Multiplier::Millisecond);
  } while (Time::getTicks() - start < 25 * Time::Multiplier::Millisecond);
  // AHCI 10.6.3 permits takeover after 25 ms when firmware never declared busy.
  if (!busy)
    return true;
  const auto deadline = start + 2 * Time::Multiplier::Second + 25 * Time::Multiplier::Millisecond;
  do {
    ownership = m_Registers->read32(Bohc);
    if (!(ownership & ((1U << 4) | 1U)))
      return true;
    Time::delay(Time::Multiplier::Millisecond);
  } while (Time::getTicks() < deadline);
  ERROR("AHCI: firmware did not release controller ownership");
  return false;
}
bool AhciController::reset() {
  m_Registers->write32(Enable, Ghc);
  m_Registers->write32(Enable | Reset, Ghc);
  const auto deadline = Time::getTicks() + Time::Multiplier::Second;
  while (m_Registers->read32(Ghc) & Reset) {
    if (Time::getTicks() >= deadline)
      return false;
    Time::delay(Time::Multiplier::Millisecond);
  }
  m_Registers->write32(Enable, Ghc);
  return (m_Registers->read32(Ghc) & Enable) != 0;
}
bool AhciController::initialiseController() {
  if (!m_Pci || m_Pci->getPciClassCode() != 1 || m_Pci->getPciSubclassCode() != 6 ||
      m_Pci->getPciProgInterface() != 1)
    return false;
  auto& pci = PciBus::instance();
  PciFunctionState::State inherited;
  if (!pci.inspectFunction(m_Pci, inherited)) {
    ERROR("AHCI: unsupported PCI state (D0, valid capabilities and PIC INTx required)");
    return false;
  }
  m_OriginalCommand = inherited.command;
  const uint32_t bar = pci.readConfigSpace(m_Pci, 9);
  if ((bar & 7U) || !((bar & ~15U)))
    return false;
  Device::Address* mapping = nullptr;
  for (auto* address : m_Pci->addresses()) {
    if (address->m_Name == "bar5" && !address->m_IsIoSpace && address->m_Address == (bar & ~15U)) {
      mapping = address;
      break;
    }
  }
  if (!mapping || mapping->m_Size < PortBase + PortStride)
    return false;
  // Keep the original PCI node responsible for its BAR mapping.
  m_PciChanged = true;
  if (!pci.updateCommand(m_Pci, 0, 2U | 0x400U))
    return false;
  mapping->map();
  m_Registers = mapping->m_Io;
  if (!m_Registers || m_Registers->size() < mapping->m_Size)
    return false;
  const uint32_t version = m_Registers->read32(Vs);
  if (version < 0x00010000 || version == 0xffffffffU || !claimOwnership(version))
    return false;
  m_HardwareOwned = true;
  m_Implemented = m_Registers->read32(Pi);
  if (!m_Implemented)
    return false;
  uint32_t speedLimits[32] = {};
  for (size_t i = 0; i < 32; ++i) {
    if (!(m_Implemented & (1U << i)))
      continue;
    if (mapping->m_Size < PortBase + (i + 1) * PortStride)
      return false;
    speedLimits[i] = m_Registers->read32(PortBase + i * PortStride + Sctl) & 0xf0U;
  }
  if (!reset()) {
    ERROR("AHCI: HBA reset did not complete within one second");
    return false;
  }
  const uint32_t capabilities = m_Registers->read32(Cap);
  const uint32_t extended = version >= 0x00010200 ? m_Registers->read32(Cap2) : 0;
  if (capabilities & (1U << 7))
    m_Registers->write32(0, CccCtl);
  // DMA addresses are below 4 GiB even on controllers advertising S64A.
  if (!pci.disableMessageInterrupts(m_Pci, inherited) ||
      !pci.resourcesUnchanged(m_Pci, inherited) || !pci.updateCommand(m_Pci, 0, 6U | 0x400U))
    return false;
  for (size_t i = 0; i < 32; ++i) {
    if (!(m_Implemented & (1U << i)))
      continue;
    m_Registers->write32(speedLimits[i], PortBase + i * PortStride + Sctl);
    auto* port = new AhciPort(m_Registers, i);
    m_Ports[i] = port;
    if (!port->initialise(capabilities, version, extended)) {
      delete port;
      m_Ports[i] = nullptr;
      continue;
    }
    auto* disk = new AhciDisk(this, i);
    if (disk->initialise()) {
      addChild(disk);
    } else {
      delete disk;
      delete port;
      m_Ports[i] = nullptr;
    }
  }
  if (!getNumChildren())
    return false;
  m_Registers->write32(m_Implemented, Is);
  m_Irq = Machine::instance().getIrqManager()->registerPciIrqHandler(this, m_Pci,
                                                                     IrqPolicy::pciIntxThreaded());
  if (!m_Irq) {
    ERROR("AHCI: could not register PCI INTx");
    return false;
  }
  {
    LockGuard<Mutex> irqLock(m_IrqLock);
    for (auto* port : m_Ports)
      if (port)
        port->enableInterrupts();
    m_Registers->write32(m_Implemented, Is);
    m_Interrupts = true;
    if (!pci.updateCommand(m_Pci, 0x400U, 6U))
      return false;
    m_Registers->write32(Enable | InterruptEnable, Ghc);
    (void)m_Registers->read32(Ghc);
  }
  uint16_t identifyData[256]{};
  for (size_t i = 0; i < 32; ++i) {
    if (!m_Ports[i])
      continue;
    if (!InterruptProbe::run([&] { return identify(i, identifyData, true); },
                             [&] { return m_Ports[i]->interruptCompletions(); })) {
      ERROR("AHCI: interrupt delivery probe failed on port " << i);
      return false;
    }
  }
  for (size_t i = 0; i < getNumChildren(); ++i) {
    auto* disk = static_cast<AhciDisk*>(getChild(i));
    disk->publishEndpoint();
    NOTICE("AHCI: SATA disk ready on port " << disk->port());
  }
  NOTICE("AHCI: controller ready with " << getNumChildren() << " SATA disks, shared INTx");
  return true;
}
IrqDisposition AhciController::irq(irq_id_t) {
  LockGuard<Mutex> irqLock(m_IrqLock);
  if (m_Stopping)
    return IrqDisposition::Quiesced;
  if (!m_Interrupts)
    return IrqDisposition::NotHandled;
  const uint32_t pending = m_Registers->read32(Is) & m_Implemented;
  bool handled = pending != 0;
  for (size_t i = 0; i < 32; ++i) {
    if (m_Ports[i])
      handled |= m_Ports[i]->interrupt(pending & (1U << i));
    else if (pending & (1U << i)) {
      const size_t base = PortBase + i * PortStride;
      m_Registers->write32(m_Registers->read32(base + Serr), base + Serr);
      m_Registers->write32(m_Registers->read32(base + PortIs), base + PortIs);
    }
  }
  if (pending)
    m_Registers->write32(pending, Is);
  (void)m_Registers->read32(Is);
  return handled ? IrqDisposition::Handled : IrqDisposition::NotHandled;
}
bool AhciController::identify(size_t port, uint16_t* words, bool interruptProbe) {
  return port < 32 && m_Ports[port] &&
         m_Ports[port]->command(0xec, 0, 0, words, 512, false, m_Interrupts, interruptProbe);
}
bool AhciController::readWrite(size_t port, uint64_t lba, uint16_t sectors, void* buffer,
                               size_t bytes, bool writing) {
  if (port >= 32 || !m_Ports[port] || !sectors ||
      bytes != static_cast<size_t>(sectors) * m_Ports[port]->sectorBytes() || lba >= (1ULL << 48) ||
      sectors > (1ULL << 48) - lba)
    return false;
  return port < 32 && m_Ports[port] &&
         m_Ports[port]->command(writing ? 0x35 : 0x25, lba, sectors, buffer, bytes, writing,
                                m_Interrupts);
}
void AhciController::configureDisk(size_t port, size_t sectorBytes, size_t queueDepth) {
  if (port < 32 && m_Ports[port])
    m_Ports[port]->configureDisk(sectorBytes, queueDepth);
}
bool AhciController::flush(size_t port, bool extended) {
  return port < 32 && m_Ports[port] &&
         m_Ports[port]->command(extended ? 0xea : 0xe7, 0, 0, nullptr, 0, false, m_Interrupts);
}
size_t AhciController::interruptCompletions() const {
  size_t count = 0;
  for (auto* port : m_Ports)
    if (port)
      count += port->interruptCompletions();
  return count;
}
void AhciController::shutdown() {
  if (m_Shutdown)
    return;
  // Cache writeback and queued requests still need both hardware and interrupts.
  shutdownDiskCaches();
  RequestQueue::destroy();
#if !CRIPPLE_HDD
  // Previously evicted pages can still reside in the drive's volatile cache.
  for (size_t i = 0; i < getNumChildren(); ++i) {
    auto* disk = static_cast<AhciDisk*>(getChild(i));
    if (!disk->doSync(ScsiDisk::SyncWholeDevice))
      ERROR("AHCI: final cache flush failed on port " << disk->port());
  }
#endif
  {
    LockGuard<Mutex> irqLock(m_IrqLock);
    m_Stopping = true;
    m_Interrupts = false;
    if (m_HardwareOwned) {
      m_Registers->write32(Enable, Ghc);
      (void)m_Registers->read32(Ghc);
    }
  }
  if (m_Irq && !Machine::instance().getIrqManager()->unregisterHandler(m_Irq, this))
    panic("AHCI: synchronous interrupt retirement failed");
  m_Irq = 0;
  for (auto* port : m_Ports)
    if (port)
      port->shutdown();
  if (m_PciChanged) {
    // Reset replaced firmware's command state. Never resume its old bus mastering.
    const uint16_t command =
        m_HardwareOwned ? (m_OriginalCommand & ~4U) | 0x400U : m_OriginalCommand;
    if (!PciBus::instance().updateCommand(m_Pci, 0x407U, command & 0x407U))
      panic("AHCI: failed to verify PCI state during shutdown");
  }
  m_Shutdown = true;
}

size_t AhciController::maximumOutstanding(size_t port) const {
  return port < 32 && m_Ports[port] ? m_Ports[port]->maximumOutstanding() : 0;
}
