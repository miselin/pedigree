/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/PciDrivers.h"
#include "pedigree/kernel/machine/PciSriov.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/utilities/new"

#include "Enumeration.h"
#include "ProbeBars.h"
#include "Resources.h"

namespace {
bool readHeader(Device* device, PciBus::ConfigSpace& header) {
  auto* bytes = reinterpret_cast<uint8_t*>(&header);
  for (uint16_t offset = 0; offset < sizeof(header); offset += 4) {
    uint32_t value = 0;
    if (!PciBus::instance().readConfig32(device, offset, value)) {
      return false;
    }
    for (unsigned byte = 0; byte < 4; ++byte) {
      bytes[offset + byte] = value >> (byte * 8);
    }
  }
  return true;
}

bool writeBar(Device* device, size_t index, uint32_t value) {
  uint32_t actual = 0;
  auto& pci = PciBus::instance();
  return pci.writeConfig32(device, 0x10 + 4 * index, value) &&
         pci.readConfig32(device, 0x10 + 4 * index, actual) && actual == value;
}

Device* slotBus(Device* port, uint8_t& number) {
  uint32_t buses = 0;
  if (!port || port->getPciClassCode() != 6 || port->getPciSubclassCode() != 4 ||
      !PciBus::instance().readConfig32(port, 0x18, buses) ||
      uint8_t(buses) != port->getPciBusPosition() || uint8_t(buses >> 8) <= uint8_t(buses) ||
      uint8_t(buses >> 16) < uint8_t(buses >> 8)) {
    return nullptr;
  }
  number = buses >> 8;
  Device* result = nullptr;
  Device::TreeLockGuard guard;
  for (size_t i = 0; i < port->getNumChildren(); ++i) {
    Device* child = port->getChild(i);
    if (child->getSpecificType() == "pci") {
      if (result) {
        return nullptr;
      }
      result = child;
    }
  }
  return result;
}

bool noVirtualFunctions(Device* device) {
  PciExtendedCapabilities::Capability capability;
  auto& pci = PciBus::instance();
  const auto found = pci.findExtendedCapability(device, PciSriov::CapabilityId, capability);
  if (found == PciExtendedCapabilities::FindResult::Absent) {
    return true;
  }
  uint16_t control = 0;
  return found == PciExtendedCapabilities::FindResult::Found &&
         pci.readConfig16(device, capability.offset + 8, control) &&
         !(control & PciSriov::VfControl);
}

Device* leaf(Device* bus) {
  Device::TreeLockGuard guard;
  if (!bus || bus->getNumChildren() != 1) {
    return nullptr;
  }
  Device* device = bus->getChild(0);
  const auto header = device->getPciConfigHeader();
  return !header.header_type && !device->getPciDevicePosition() &&
                 !device->getPciFunctionNumber() && !device->getPhysicalFunction()
             ? device
             : nullptr;
}
}  // namespace

namespace PciEnumeration {
bool probeSlot(Device* port) {
  uint8_t number = 0;
  Device* bus = slotBus(port, number);
  if (!bus) {
    return false;
  }
  if (bus->getNumChildren()) {
    return leaf(bus) != nullptr;
  }
  auto& pci = PciBus::instance();
  auto* device = new Device;
  device->setPciPosition(number, 0, 0);
  device->setParent(bus);
  PciBus::ConfigSpace original{};
  if (!readHeader(device, original) || !original.vendor || original.vendor == 0xffff ||
      original.header_type || (original.command & 7U) || (original.rom_base_address & 1U) ||
      !noVirtualFunctions(device)) {
    delete device;
    return false;
  }
  device->setPciIdentifiers(original.class_code, original.subclass, original.vendor,
                            original.device, original.progif);
  PciBus::ConfigSpace assigned = original;
  bool registered = false;
  auto rollback = [&]() {
    if (!pci.updateCommand(device, 7U, 0)) {
      panic("PCI hotplug: cannot stop decoding during probe rollback");
    }
    for (size_t i = 0; i < 6; ++i) {
      if (!writeBar(device, i, original.bar[i])) {
        panic("PCI hotplug: cannot restore BARs during probe rollback");
      }
    }
    if (registered && !PciResources::removeFunction(device)) {
      panic("PCI hotplug: cannot reclaim failed probe resources");
    }
    delete device;
    return false;
  };
  // A newly inserted endpoint has no driver. Normalize stale, disabled BARs
  // before reserving space; firmware will not allocate this insertion for us.
  for (size_t i = 0; i < 6; ++i) {
    const uint32_t low = original.bar[i];
    assigned.bar[i] = low & ((low & 1U) ? 3U : 15U);
    if (!writeBar(device, i, assigned.bar[i])) {
      return rollback();
    }
    if (!(low & 1U) && (low & 6U) == 4) {
      if (++i == 6) {
        return rollback();
      }
      assigned.bar[i] = 0;
      if (!writeBar(device, i, 0)) {
        return rollback();
      }
    }
  }
  device->setPciConfigHeader(assigned);
  if (!PciResources::addFunction(device)) {
    return rollback();
  }
  registered = true;
  const auto probe = PciBar::probe(pci, device, assigned);
  if (probe.result != PciBar::ProbeResult::Success) {
    return rollback();
  }
  for (size_t i = 0; i < 6; ++i) {
    const uint32_t low = assigned.bar[i];
    const bool wide = !(low & 1U) && (low & 6U) == 4;
    if ((low & 6U) && !wide) {
      return rollback();
    }
    uint64_t mask = probe.masks[i] & ~uint32_t{15};
    if (wide) {
      if (i == 5) {
        return rollback();
      }
      mask |= uint64_t(probe.masks[i + 1]) << 32;
    }
    if (!mask) {
      if (wide) {
        ++i;
      }
      continue;
    }
    if (low & 1U) {
      return rollback();
    }
    const uint64_t width = wide ? ~uint64_t{0} : 0xffffffffU;
    const uint64_t bytes = ((~mask) & width) + 1;
    uint64_t base = 0, cpu = 0;
    if (!bytes || (bytes & (bytes - 1)) || bytes > ~size_t{0} ||
        !PciResources::allocate(device, bytes, bytes, low & 8U, width, base) ||
        !pci.translateAddress(base, bytes, false, cpu) || cpu > ~uintptr_t{0}) {
      return rollback();
    }
    assigned.bar[i] = uint32_t(base) | low;
    if (!writeBar(device, i, assigned.bar[i])) {
      return rollback();
    }
    char name[] = {'b', 'a', 'r', static_cast<char>('0' + i), 0};
    device->addresses().pushBack(new Device::Address(String(name), cpu, bytes, false));
    if (wide) {
      assigned.bar[++i] = base >> 32;
      if (!writeBar(device, i, assigned.bar[i])) {
        return rollback();
      }
    }
  }
  device->setPciConfigHeader(assigned);
  const uint32_t route = pci.interruptRoute(number, 0, 0, assigned.interrupt_pin);
  device->setInterruptNumber(route ? route : assigned.interrupt_line);
  {
    Device::TreeLockGuard guard;
    bus->addChild(device);
  }
  const bool bound = PciDrivers::attach(device);
  NOTICE("PCI hotplug: inserted " << Dec << unsigned(number) << ":0.0 driver bound=" << bound);
  return true;
}

bool prepareRemoveSlot(Device* port) {
  uint8_t number = 0;
  Device* device = leaf(slotBus(port, number));
  return device && noVirtualFunctions(device) && PciDrivers::prepareRemove(device);
}

void cancelRemoveSlot(Device* port) {
  uint8_t number = 0;
  if (Device* device = leaf(slotBus(port, number))) {
    PciDrivers::cancelRemove(device);
  }
}

bool removeSlot(Device* port) {
  uint8_t number = 0;
  Device* bus = slotBus(port, number);
  Device* device = leaf(bus);
  if (!device || !PciDrivers::remove(device)) {
    return false;
  }
  auto& pci = PciBus::instance();
  if (!pci.updateCommand(device, 7U, 0) || !PciResources::removeFunction(device)) {
    panic("PCI hotplug: cannot retire endpoint resources");
  }
  {
    Device::TreeLockGuard guard;
    bus->removeChild(device);
    device->setParent(nullptr);
  }
  delete device;
  NOTICE("PCI hotplug: removed " << Dec << unsigned(number) << ":0.0");
  return true;
}
}  // namespace PciEnumeration
