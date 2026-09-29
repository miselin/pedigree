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

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/machine/PciConfigAccess.h"
#include "pedigree/kernel/machine/PciMessageBar.h"
#include "pedigree/kernel/processor/IoPort.h"
#include "pedigree/kernel/processor/MemoryMappedIo.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"

#if ACPI
#include "Acpi.h"
#endif
#if X64 && ACPI
#include "IntelIommu.h"
#endif
#include "Pic.h"

namespace {
IoPort configSpace("PCI config space");
Spinlock configLock(false);
Spinlock ecamLock(false);
bool configAvailable = false;
MemoryMappedIo* ecamBuses[256] = {};
PciConfigAccess<IoPort, Spinlock> config(configSpace, configLock);

MemoryMappedIo* ecamBus(uint8_t bus) {
#if ACPI
  MemoryMappedIo* mapped = __atomic_load_n(&ecamBuses[bus], __ATOMIC_ACQUIRE);
  if (mapped) {
    return mapped;
  }
  LockGuard<Spinlock> guard(ecamLock);
  mapped = __atomic_load_n(&ecamBuses[bus], __ATOMIC_ACQUIRE);
  if (mapped) {
    return mapped;
  }
  uint64_t physical = 0;
  if (!Acpi::instance().pciConfigurationAddress(bus, physical)) {
    return nullptr;
  }
  auto* candidate = new MemoryMappedIo("PCI ECAM");
  constexpr size_t BusBytes = 1U << 20;
  if (!PhysicalMemoryManager::instance().allocateRegion(
          *candidate, BusBytes / PhysicalMemoryManager::getPageSize(),
          PhysicalMemoryManager::continuous | PhysicalMemoryManager::nonRamMemory |
              PhysicalMemoryManager::force,
          VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write |
              VirtualAddressSpace::CacheDisable,
          physical)) {
    WARNING("PCI: could not map ECAM bus " << Dec << bus << " at " << Hex << physical);
    delete candidate;
    return nullptr;
  }
  __atomic_store_n(&ecamBuses[bus], candidate, __ATOMIC_RELEASE);
  return candidate;
#else
  return nullptr;
#endif
}

bool ecamAccessValid(uint8_t device, uint8_t function, uint16_t offset, uint8_t width) {
  return device < 32 && function < 8 && (width == 1 || width == 2 || width == 4) &&
         offset <= 4096 - width && !(offset & (width - 1));
}

bool readEcam(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint8_t width,
              uint32_t& value) {
  if (!ecamAccessValid(device, function, offset, width)) {
    return false;
  }
  MemoryMappedIo* io = ecamBus(bus);
  if (!io) {
    return false;
  }
  const size_t position = (size_t(device) << 15) | (size_t(function) << 12) | offset;
  if (width == 1) {
    value = io->read8(position);
  } else if (width == 2) {
    value = io->read16(position);
  } else {
    value = io->read32(position);
  }
  return true;
}

bool writeEcam(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint8_t width,
               uint32_t value) {
  if (!ecamAccessValid(device, function, offset, width)) {
    return false;
  }
  MemoryMappedIo* io = ecamBus(bus);
  if (!io) {
    return false;
  }
  const size_t position = (size_t(device) << 15) | (size_t(function) << 12) | offset;
  if (width == 1) {
    io->write8(value, position);
  } else if (width == 2) {
    io->write16(value, position);
  } else {
    io->write32(value, position);
  }
  return true;
}

struct FunctionConfig {
  Device* device;
  bool read8(uint16_t offset, uint8_t& value) {
    return PciBus::instance().readConfig8(device, offset, value);
  }
  bool read16(uint16_t offset, uint16_t& value) {
    return PciBus::instance().readConfig16(device, offset, value);
  }
  bool read32(uint16_t offset, uint32_t& value) {
    return PciBus::instance().readConfig32(device, offset, value);
  }
  bool write16(uint16_t offset, uint16_t value) {
    return PciBus::instance().writeConfig16(device, offset, value);
  }
  bool write32(uint16_t offset, uint32_t value) {
    return PciBus::instance().writeConfig32(device, offset, value);
  }
};

bool readFunction(Device* device, uint16_t offset, uint8_t width, uint32_t& value) {
  if (!device) {
    return false;
  }
  const uint8_t bus = device->getPciBusPosition();
  const uint8_t slot = device->getPciDevicePosition();
  const uint8_t function = device->getPciFunctionNumber();
  if (configAvailable && offset <= 256 - width) {
    return config.read(bus, slot, function, offset, width, value);
  }
  return readEcam(bus, slot, function, offset, width, value);
}
bool writeFunction(Device* device, uint16_t offset, uint8_t width, uint32_t value) {
  if (!device) {
    return false;
  }
  const uint8_t bus = device->getPciBusPosition();
  const uint8_t slot = device->getPciDevicePosition();
  const uint8_t function = device->getPciFunctionNumber();
  if (configAvailable && offset <= 256 - width) {
    return config.write(bus, slot, function, offset, width, value);
  }
  return writeEcam(bus, slot, function, offset, width, value);
}
}  // namespace

PciBus PciBus::m_Instance;
PciBus::PciBus() {}
PciBus::~PciBus() {}

void PciBus::initialise() {
  if (configSpace.allocate(0xCF8, 8)) {
    LockGuard<Spinlock> guard(configLock);
    configSpace.write32(0x80000000, 0);
    configAvailable = configSpace.read32(0) == 0x80000000;
  }
  // ACPI tables may be published after the early device-tree setup.
  if (!configAvailable) {
    NOTICE("PCI: legacy configuration ports unavailable; waiting for ACPI ECAM");
  }
}

uint32_t PciBus::readConfigSpace(Device* device, uint8_t offset) {
  uint32_t value = 0xffffffffU;
  readConfig32(device, uint16_t{offset} * 4, value);
  return value;
}
uint32_t PciBus::readConfigSpace(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
  uint32_t value = 0xffffffffU;
  if (configAvailable && offset < 64) {
    config.read(bus, device, function, uint16_t{offset} * 4, 4, value);
  } else {
    readEcam(bus, device, function, uint16_t{offset} * 4, 4, value);
  }
  return value;
}
void PciBus::writeConfigSpace(Device* device, uint8_t offset, uint32_t value) {
  writeConfig32(device, uint16_t{offset} * 4, value);
}
void PciBus::writeConfigSpace(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                              uint32_t value) {
  if (configAvailable && offset < 64) {
    config.write(bus, device, function, uint16_t{offset} * 4, 4, value);
  } else {
    writeEcam(bus, device, function, uint16_t{offset} * 4, 4, value);
  }
}
bool PciBus::readConfig8(Device* device, uint16_t offset, uint8_t& value) {
  uint32_t data = 0;
  if (!readFunction(device, offset, 1, data))
    return false;
  value = data;
  return true;
}
bool PciBus::readConfig16(Device* device, uint16_t offset, uint16_t& value) {
  uint32_t data = 0;
  if (!readFunction(device, offset, 2, data))
    return false;
  value = data;
  return true;
}
bool PciBus::readConfig32(Device* device, uint16_t offset, uint32_t& value) {
  return readFunction(device, offset, 4, value);
}
bool PciBus::writeConfig8(Device* device, uint16_t offset, uint8_t value) {
  return writeFunction(device, offset, 1, value);
}
bool PciBus::writeConfig16(Device* device, uint16_t offset, uint16_t value) {
  return writeFunction(device, offset, 2, value);
}
bool PciBus::writeConfig32(Device* device, uint16_t offset, uint32_t value) {
  return writeFunction(device, offset, 4, value);
}
bool PciBus::updateCommand(Device* device, uint16_t clearBits, uint16_t setBits) {
  if (!device) {
    return false;
  }
  if (configAvailable) {
    return config.updateCommand(device->getPciBusPosition(), device->getPciDevicePosition(),
                                device->getPciFunctionNumber(), clearBits, setBits);
  }
  uint16_t command = 0;
  if (!readConfig16(device, 4, command)) {
    return false;
  }
  const uint16_t desired = (command & ~clearBits) | setBits;
  return writeConfig16(device, 4, desired) && readConfig16(device, 4, command) &&
         command == desired;
}
bool PciBus::inspectFunction(Device* device, PciFunctionState::State& state,
                             bool requireLegacyInterrupt) {
  FunctionConfig function{device};
  return device &&
         PciFunctionState::inspect(function, state, requireLegacyInterrupt,
                                   device->getPhysicalFunction()) &&
         (!requireLegacyInterrupt || state.interruptLine == device->getInterruptNumber());
}
bool PciBus::disableMessageInterrupts(Device* device, const PciFunctionState::State& state) {
  FunctionConfig function{device};
  return PciFunctionState::disableMessageInterrupts(function, state);
}
bool PciBus::enableMsi(Device* device, uint64_t address, uint16_t data) {
  FunctionConfig function{device};
  PciFunctionState::State state;
  return device &&
         PciFunctionState::inspect(function, state, false, device->getPhysicalFunction()) &&
         PciFunctionState::enableMsi(function, state, address, data);
}
bool PciBus::disableMsi(Device* device) {
  FunctionConfig function{device};
  PciFunctionState::State state;
  return device &&
         PciFunctionState::inspect(function, state, false, device->getPhysicalFunction()) &&
         PciFunctionState::disableMsi(function, state);
}
bool PciBus::enableMsix(Device* device, uint64_t address, uint32_t data) {
  return enableMsixVectors(device, address, &data, 1);
}
bool PciBus::enableMsixVectors(Device* device, uint64_t address, const uint32_t* data, size_t count,
                               bool* touched, const uint64_t* addresses) {
  if (touched) {
    *touched = false;
  }
  FunctionConfig function{device};
  PciFunctionState::State state;
  PciFunctionState::MsixTable table;
  if (!device || !data || !count ||
      !PciFunctionState::inspect(function, state, false, device->getPhysicalFunction()) ||
      !PciFunctionState::msixTable(function, state, table) || count > table.vectors) {
    return false;
  }
  IoBase* io = PciFunctionState::msixTableIo(device, state, table, true);
  if (touched && io) {
    *touched = true;
  }
  return io && PciFunctionState::enableMsixVectors(function, state, *io, table.offset, address,
                                                   data, count, addresses);
}
bool PciBus::setMsixVectorMask(Device* device, size_t index, bool masked) {
  FunctionConfig function{device};
  PciFunctionState::State state;
  PciFunctionState::MsixTable table;
  if (!device ||
      !PciFunctionState::inspect(function, state, false, device->getPhysicalFunction()) ||
      !PciFunctionState::msixTable(function, state, table) || index >= table.vectors) {
    return false;
  }
  IoBase* io = PciFunctionState::msixTableIo(device, state, table, false);
  return io &&
         PciFunctionState::setMsixVectorMask(function, state, *io, table.offset, index, masked);
}
bool PciBus::disableMsix(Device* device) {
  FunctionConfig function{device};
  PciFunctionState::State state;
  PciFunctionState::MsixTable table;
  if (!device ||
      !PciFunctionState::inspect(function, state, false, device->getPhysicalFunction()) ||
      !PciFunctionState::msixTable(function, state, table)) {
    return false;
  }
  IoBase* io = PciFunctionState::msixTableIo(device, state, table, false);
  return io && PciFunctionState::disableMsix(function, state, *io, table.offset);
}
bool PciBus::resourcesUnchanged(Device* device, const PciFunctionState::State& state) {
  FunctionConfig function{device};
  return device &&
         PciFunctionState::resourcesUnchanged(function, state, device->getPhysicalFunction());
}

bool PciBus::translateAddress(uint64_t pciAddress, uint64_t, bool, uint64_t& cpuPhysical) {
  cpuPhysical = pciAddress;
  return true;
}

bool PciBus::busRange(uint8_t& first, uint8_t& last) {
  if (configAvailable) {
    first = 0;
    last = 255;
    return true;
  }
#if ACPI
  return Acpi::instance().pciBusRange(first, last);
#else
  return false;
#endif
}

uint32_t PciBus::interruptRoute(uint8_t, uint8_t, uint8_t, uint8_t) {
  return 0;
}

bool PciBus::assignBar(Device*, uint8_t, uint32_t, uint32_t, uint32_t, uint32_t) {
  return true;
}

bool PciBus::attachDmaRemapping(Device* device) {
#if X64 && ACPI
  return device && IntelIommu::instance().attach(device);
#else
  (void)device;
  return false;
#endif
}

bool PciBus::hasDmaRemapping(Device* device) const {
#if X64 && ACPI
  return device && IntelIommu::instance().attached(device);
#else
  (void)device;
  return false;
#endif
}

bool PciBus::attachIsolatedDma(Device* device) {
#if X64 && ACPI
  return device && IntelIommu::instance().attach(device, true);
#else
  (void)device;
  return false;
#endif
}

bool PciBus::detachIsolatedDma(Device* device) {
#if X64 && ACPI
  return device && IntelIommu::instance().detachIsolated(device);
#else
  (void)device;
  return false;
#endif
}

bool PciBus::isolatedDmaIdle(Device* device) const {
#if X64 && ACPI
  return device && IntelIommu::instance().isolatedIdle(device);
#else
  (void)device;
  return false;
#endif
}

bool PciBus::detachDisabledIsolatedDma(Device* device) {
#if X64 && ACPI
  Device* pf = device ? device->getPhysicalFunction() : nullptr;
  PciExtendedCapabilities::Capability capability;
  uint16_t control = 0;
  if (!pf ||
      findExtendedCapability(pf, 0x10, capability) != PciExtendedCapabilities::FindResult::Found ||
      !readConfig16(pf, capability.offset + 8, control) || (control & 9)) {
    return false;
  }
  return IntelIommu::instance().detachIsolated(device, true);
#else
  (void)device;
  return false;
#endif
}

bool PciBus::hasDmaIsolation(Device* device) const {
#if X64 && ACPI
  return device && IntelIommu::instance().isolated(device);
#else
  (void)device;
  return false;
#endif
}

bool PciBus::mapDmaPage(Device* device, physical_uintptr_t physical, size_t bytes,
                        DmaMapping& mapping) {
  constexpr uint64_t HighestDmaAddress = 0xffffffffULL;
  if (!device || mapping.m_Device || !physical || !bytes || bytes > TargetInfo::getPageSize() ||
      (physical & (TargetInfo::getPageSize() - 1))) {
    return false;
  }

  uint32_t address = 0;
  uint16_t token = 0;
  bool isolated = false;
#if X64 && ACPI
  isolated = IntelIommu::instance().isolated(device);
#endif
  if (!isolated && uint64_t{physical} <= HighestDmaAddress - (bytes - 1)) {
    address = static_cast<uint32_t>(physical);
  } else {
#if X64 && ACPI
    if (!IntelIommu::instance().mapPage(device, physical, address, token)) {
      return false;
    }
#else
    return false;
#endif
  }
  mapping.m_Device = device;
  mapping.m_Address = address;
  mapping.m_Token = token;
  return true;
}

void PciBus::unmapDmaPage(Device* device, uint16_t token) {
#if X64 && ACPI
  if (device && token) {
    IntelIommu::instance().unmapPage(device, token);
  }
#else
  (void)device;
  (void)token;
#endif
}

bool PciBus::reserveLegacyInterrupt(uint8_t irq) {
  return Machine::instance().getIrqManager() == &Pic::instance() &&
         Pic::instance().reservePciRoute(irq);
}
