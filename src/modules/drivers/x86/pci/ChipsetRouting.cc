/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "ChipsetRouting.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"

#include "IntelPirq.h"

namespace {
struct LpcConfig {
  Device* device;
  bool read8(uint16_t offset, uint8_t& value) {
    return PciBus::instance().readConfig8(device, offset, value);
  }
  bool write8(uint16_t offset, uint8_t value) {
    return PciBus::instance().writeConfig8(device, offset, value);
  }
};
uint16_t hpetExclusions(uint32_t hptc) {
  // AE gates register decoding, not the timers. With no readable registers,
  // leave IRQ11 alone: HPET timer 2 can use it independently of the PIC drivers.
  if (hptc == 0xffffffff || !(hptc & 0x80))
    return 1U << 11;
  MemoryRegion hpet("QM67 HPET interrupt audit");
  if (!PhysicalMemoryManager::instance().allocateRegion(
          hpet, 1,
          PhysicalMemoryManager::continuous | PhysicalMemoryManager::nonRamMemory |
              PhysicalMemoryManager::force,
          VirtualAddressSpace::KernelMode | VirtualAddressSpace::CacheDisable,
          0xfed00000U + ((hptc & 3) << 12)))
    return 1U << 11;
  auto* registers = static_cast<volatile uint32_t*>(hpet.virtualAddress());
  const uint32_t capabilities = registers[0];
  const uint32_t general = registers[0x10 / 4];
  const uint32_t pending = registers[0x20 / 4];
  const unsigned lastTimer = (capabilities >> 8) & 31;
  if ((capabilities >> 16) != 0x8086 || lastTimer < 2 || general == 0xffffffff ||
      pending == 0xffffffff)
    return 1U << 11;
  uint16_t excluded = 0;
  for (unsigned timer = 0; timer <= lastTimer; ++timer)
    excluded |= IntelPirq::hpetExclusion(registers[(0x100 + 0x20 * timer) / 4], pending, timer,
                                         general & 2);
  return excluded;
}
}  // namespace

void routeChipsetInterrupts(const Vector<Device*>& devices) {
  auto& pci = PciBus::instance();
  Device* lpc = nullptr;
  for (size_t i = 0; i < devices.size(); ++i) {
    auto* device = devices[i];
    const auto header = device->getPciConfigHeader();
    if (device->getPciBusPosition() == 0 && device->getPciDevicePosition() == 31 &&
        device->getPciFunctionNumber() == 0 && header.vendor == 0x8086 && header.device == 0x1c4f &&
        header.class_code == 6 && header.subclass == 1)
      lpc = device;
  }
  if (!lpc)
    return;

  uint32_t rcba = 0;
  uint8_t acpiControl = 0;
  if (!pci.readConfig32(lpc, 0xf0, rcba) || rcba == 0xffffffff || !(rcba & 1) ||
      !(rcba & 0xffffc000U) || !pci.readConfig8(lpc, 0x44, acpiControl) || acpiControl == 0xff ||
      (acpiControl & 7) == 3) {
    ERROR("PCI: QM67 routing unavailable: invalid RCBA or SCI selection");
    return;
  }
  const uint8_t sci = (acpiControl & 7) < 3 ? 9 + (acpiControl & 7) : 16 + (acpiControl & 7);
  uint16_t excluded = 0;

  MemoryRegion registers("QM67 interrupt routing");
  const auto pageSize = PhysicalMemoryManager::getPageSize();
  if (pageSize != 4096 ||
      !PhysicalMemoryManager::instance().allocateRegion(
          registers, 1,
          PhysicalMemoryManager::continuous | PhysicalMemoryManager::nonRamMemory |
              PhysicalMemoryManager::force,
          VirtualAddressSpace::KernelMode | VirtualAddressSpace::CacheDisable,
          (rcba & 0xffffc000U) + 0x3000)) {
    ERROR("PCI: cannot map QM67 interrupt routing registers");
    return;
  }
  auto* mapped = static_cast<volatile uint8_t*>(registers.virtualAddress());
  // Chapter 10 requires dword accesses even for its byte/word-sized fields.
  auto read32 = [mapped](uint16_t offset) {
    return *reinterpret_cast<volatile uint32_t*>(mapped + ((offset & ~3U) - 0x3000));
  };
  auto read16 = [&read32](uint16_t offset) {
    return static_cast<uint16_t>(read32(offset) >> (8 * (offset & 2)));
  };
  const uint8_t tcoControl = read32(0x3000);
  if (!IntelPirq::chipsetExclusions(acpiControl, tcoControl, excluded)) {
    ERROR("PCI: QM67 SCI/TCO routing state invalid");
    return;
  }
  excluded |= hpetExclusions(read32(0x3404));
  // IRQ12 is the fixed PS/2 mouse line. If firmware put a PIRQ there, move the
  // shared PCI route to a reserved level line before registering PCI handlers.
  excluded |= uint16_t{1} << 12;
  NOTICE("PCI: QM67 native INTx routing: RCBA=" << Hex << rcba << " OIC=" << read16(0x31fe)
                                                << " SCI=" << Dec << sci << " excluded=" << Hex
                                                << excluded);

  static const char* reasons[] = {"ready",
                                  "cannot read route",
                                  "invalid or reserved IRQ",
                                  "no compatible PIC line",
                                  "write failed; original restored",
                                  "route restoration failed"};
  LpcConfig config{lpc};
  uint8_t routes[8] = {};
  bool attempted[8] = {};
  bool ready[8] = {};
  bool downstream = false;
  for (size_t i = 0; i < devices.size(); ++i) {
    auto* device = devices[i];
    auto header = device->getPciConfigHeader();
    if (!header.interrupt_pin)
      continue;
    if (device->getPciBusPosition() != 0) {
      downstream |= header.interrupt_line == 0xff;
      continue;
    }
    const uint8_t slot = device->getPciDevicePosition();
    const uint8_t function = device->getPciFunctionNumber();
    const uint16_t offset = IntelPirq::routeOffset(slot);
    if (header.vendor != 0x8086 || !offset || function > 7)
      continue;
    uint8_t pirq = 0;
    // No driver may bind to a stale firmware IRQ after an attempted route repair.
    device->setInterruptNumber(0xff);
    auto unpublished = header;
    unpublished.interrupt_line = 0xff;
    device->setPciConfigHeader(unpublished);
    const uint32_t pins = read32(IntelPirq::pinOffset(slot));
    if (!IntelPirq::nativePinMatches(pins, function, header.interrupt_pin) ||
        !IntelPirq::decode(read16(offset), header.interrupt_pin, pirq)) {
      ERROR("PCI: QM67 pin/route mismatch for 0:" << Dec << slot << ":" << function);
      continue;
    }
    if (!attempted[pirq]) {
      attempted[pirq] = true;
      uint8_t before = 0xff;
      config.read8(IntelPirq::configOffset(pirq), before);
      const auto result = IntelPirq::establish(
          config, [&pci](uint8_t irq) { return pci.reserveLegacyInterrupt(irq); }, pirq, excluded,
          routes[pirq], true);
      if (result == IntelPirq::Result::RestoreFailed)
        panic("PCI: cannot restore QM67 PIRQ route after failed write");
      ready[pirq] = result == IntelPirq::Result::Ready;
      if (ready[pirq])
        NOTICE("PCI: QM67 PIRQ" << char('A' + pirq) << "=" << Hex << before << " -> PIC IRQ " << Dec
                                << routes[pirq]);
      else
        ERROR("PCI: QM67 PIRQ" << char('A' + pirq) << "=" << Hex << before
                               << " unavailable: " << reasons[static_cast<unsigned>(result)]);
    }
    if (!ready[pirq])
      continue;
    uint8_t actual = 0, currentPin = 0;
    if (!pci.readConfig8(device, 0x3d, currentPin) || currentPin != header.interrupt_pin ||
        !pci.writeConfig8(device, 0x3c, routes[pirq]) || !pci.readConfig8(device, 0x3c, actual) ||
        actual != routes[pirq]) {
      ERROR("PCI: cannot publish QM67 IRQ for 0:" << Dec << slot << ":" << function);
      continue;
    }
    NOTICE("PCI: 0:" << Dec << slot << ":" << function << " " << Hex << header.vendor << ":"
                     << header.device << " INT" << char('A' + header.interrupt_pin - 1)
                     << " -> PIRQ" << char('A' + pirq) << " -> IRQ " << Dec << actual);
    header.interrupt_line = actual;
    device->setPciConfigHeader(header);
    device->setInterruptNumber(actual);
  }
  if (downstream)
    NOTICE("PCI: unassigned downstream INTx routes require ACPI bridge routing");
}
