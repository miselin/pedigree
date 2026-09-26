/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/machine/PciMessageBar.h"
#include "pedigree/kernel/processor/MemoryMappedIo.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"

#include "DeviceTree.h"

namespace {
MemoryMappedIo ecam("PCI ECAM");
Spinlock configLock(false);
VirtPciHost host = {};
bool configAvailable = false;
VirtPciWindow windows[8] = {};
uint64_t nextAddress[8] = {};
size_t windowCount = 0;

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

bool valid(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint8_t width) {
  return configAvailable && bus >= host.firstBus && bus <= host.lastBus && device < 32 &&
         function < 8 && (width == 1 || width == 2 || width == 4) && offset <= 4096 - width &&
         !(offset & (width - 1));
}

size_t configOffset(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset) {
  return (size_t(bus - host.firstBus) << 20) | (size_t(device) << 15) | (size_t(function) << 12) |
         offset;
}

bool read(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint8_t width,
          uint32_t& value) {
  if (!valid(bus, device, function, offset, width)) {
    return false;
  }
  const size_t position = configOffset(bus, device, function, offset);
  if (position > ecam.size() - width) {
    return false;
  }
  if (width == 1) {
    value = ecam.read8(position);
  } else if (width == 2) {
    value = ecam.read16(position);
  } else {
    value = ecam.read32(position);
  }
  return true;
}

bool write(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint8_t width,
           uint32_t value) {
  if (!valid(bus, device, function, offset, width)) {
    return false;
  }
  const size_t position = configOffset(bus, device, function, offset);
  if (position > ecam.size() - width) {
    return false;
  }
  if (width == 1) {
    ecam.write8(static_cast<uint8_t>(value), position);
  } else if (width == 2) {
    ecam.write16(static_cast<uint16_t>(value), position);
  } else {
    ecam.write32(value, position);
  }
  return true;
}

bool readFunction(Device* device, uint16_t offset, uint8_t width, uint32_t& value) {
  return device && read(device->getPciBusPosition(), device->getPciDevicePosition(),
                        device->getPciFunctionNumber(), offset, width, value);
}

bool writeFunction(Device* device, uint16_t offset, uint8_t width, uint32_t value) {
  return device && write(device->getPciBusPosition(), device->getPciDevicePosition(),
                         device->getPciFunctionNumber(), offset, width, value);
}

struct UpstreamBridge {
  uint8_t bus;
  uint8_t device;
  uint8_t function;
};

struct BridgeWindow {
  uint64_t base = 0;
  uint64_t limit = 0;
  bool open = false;

  bool contains(uint64_t address, uint64_t bytes) const {
    return open && address >= base && address <= limit && bytes - 1 <= limit - address;
  }
};

struct BridgeWindows {
  BridgeWindow io;
  BridgeWindow memory;
  BridgeWindow prefetch;
};

bool findUpstreamBridge(uint8_t childBus, UpstreamBridge& upstream) {
  bool found = false;
  for (uint32_t bus = host.firstBus; bus < childBus; ++bus) {
    for (uint8_t device = 0; device < 32; ++device) {
      uint32_t identity = 0;
      if (!read(bus, device, 0, 0, 4, identity) || (identity & 0xffffU) == 0xffffU ||
          !(identity & 0xffffU)) {
        continue;
      }
      uint32_t header = 0;
      if (!read(bus, device, 0, 0x0c, 4, header)) {
        continue;
      }
      const uint8_t functions = (header & 0x00800000U) ? 8 : 1;
      for (uint8_t function = 0; function < functions; ++function) {
        if (function && (!read(bus, device, function, 0, 4, identity) ||
                         (identity & 0xffffU) == 0xffffU || !(identity & 0xffffU))) {
          continue;
        }
        uint32_t classCode = 0;
        uint32_t busNumbers = 0;
        if (!read(bus, device, function, 0x08, 4, classCode) || (classCode >> 16) != 0x0604U ||
            !read(bus, device, function, 0x18, 4, busNumbers) || (busNumbers & 0xffU) != bus ||
            ((busNumbers >> 8) & 0xffU) != childBus || ((busNumbers >> 16) & 0xffU) < childBus) {
          continue;
        }
        if (found) {
          return false;
        }
        upstream = {static_cast<uint8_t>(bus), device, function};
        found = true;
      }
    }
  }
  return found;
}

bool readBridgeWindows(const UpstreamBridge& bridge, BridgeWindows& windows) {
  uint32_t header = 0, command = 0, io = 0, memory = 0, prefetch = 0;
  uint32_t ioUpper = 0, prefetchBaseUpper = 0, prefetchLimitUpper = 0;
  if (!read(bridge.bus, bridge.device, bridge.function, 0x0e, 1, header) || (header & 0x7f) != 1 ||
      !read(bridge.bus, bridge.device, bridge.function, 0x04, 2, command) ||
      !read(bridge.bus, bridge.device, bridge.function, 0x1c, 4, io) ||
      !read(bridge.bus, bridge.device, bridge.function, 0x20, 4, memory) ||
      !read(bridge.bus, bridge.device, bridge.function, 0x24, 4, prefetch) ||
      !read(bridge.bus, bridge.device, bridge.function, 0x28, 4, prefetchBaseUpper) ||
      !read(bridge.bus, bridge.device, bridge.function, 0x2c, 4, prefetchLimitUpper) ||
      !read(bridge.bus, bridge.device, bridge.function, 0x30, 4, ioUpper)) {
    return false;
  }

  const uint8_t ioBase = io & 0xff;
  const uint8_t ioLimit = (io >> 8) & 0xff;
  const uint8_t ioType = ioBase & 0x0f;
  if ((command & 1U) && ioType == (ioLimit & 0x0f) && ioType <= 1) {
    windows.io.base = uint64_t(ioBase & 0xf0) << 8;
    windows.io.limit = (uint64_t(ioLimit & 0xf0) << 8) | 0xfff;
    if (ioType == 1) {
      windows.io.base |= uint64_t(ioUpper & 0xffff) << 16;
      windows.io.limit |= uint64_t(ioUpper >> 16) << 16;
    }
    windows.io.open = windows.io.base <= windows.io.limit;
  }

  if (command & 2U) {
    windows.memory.base = uint64_t(memory & 0xfff0) << 16;
    windows.memory.limit = (uint64_t((memory >> 16) & 0xfff0) << 16) | 0xfffff;
    windows.memory.open = windows.memory.base <= windows.memory.limit;

    const uint16_t prefetchBase = prefetch & 0xffff;
    const uint16_t prefetchLimit = prefetch >> 16;
    const uint8_t prefetchType = prefetchBase & 0x0f;
    if (prefetchType == (prefetchLimit & 0x0f) && prefetchType <= 1) {
      windows.prefetch.base = uint64_t(prefetchBase & 0xfff0) << 16;
      windows.prefetch.limit = (uint64_t(prefetchLimit & 0xfff0) << 16) | 0xfffff;
      if (prefetchType == 1) {
        windows.prefetch.base |= uint64_t(prefetchBaseUpper) << 32;
        windows.prefetch.limit |= uint64_t(prefetchLimitUpper) << 32;
      }
      windows.prefetch.open = windows.prefetch.base <= windows.prefetch.limit;
    }
  }
  return true;
}

bool bridgesForward(uint8_t bus, uint64_t address, uint64_t bytes, bool io, bool prefetch) {
  while (bus != host.firstBus) {
    UpstreamBridge bridge = {};
    BridgeWindows windows;
    if (!findUpstreamBridge(bus, bridge) || !readBridgeWindows(bridge, windows)) {
      return false;
    }
    bool forwarded = false;
    if (io) {
      forwarded = windows.io.contains(address, bytes);
    } else if (prefetch) {
      forwarded =
          windows.prefetch.contains(address, bytes) || windows.memory.contains(address, bytes);
    } else {
      forwarded = windows.memory.contains(address, bytes);
    }
    if (!forwarded) {
      return false;
    }
    bus = bridge.bus;
  }
  return true;
}
}  // namespace

PciBus PciBus::m_Instance;

PciBus::PciBus() {}

PciBus::~PciBus() {}

void PciBus::initialise() {
  if (!VirtDeviceTree::pciHost(host)) {
    return;
  }
  const size_t pages = host.size / PhysicalMemoryManager::getPageSize();
  if (!PhysicalMemoryManager::instance().allocateRegion(
          ecam, pages, PhysicalMemoryManager::nonRamMemory | PhysicalMemoryManager::force,
          VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write |
              VirtualAddressSpace::CacheDisable,
          host.base)) {
    ERROR("PCI: could not map ECAM host at " << Hex << host.base);
    return;
  }
  configAvailable = true;
  VirtPciWindow window;
  while (windowCount < 8 && VirtDeviceTree::pciWindow(windowCount, window)) {
    windows[windowCount] = window;
    nextAddress[windowCount] = window.pciBase;
    ++windowCount;
  }
  NOTICE("PCI: ECAM host " << Hex << host.base << ".." << host.base + host.size << Dec << " buses "
                           << host.firstBus << ".." << host.lastBus);
}

uint32_t PciBus::readConfigSpace(Device* device, uint8_t offset) {
  uint32_t value = 0xffffffffU;
  readConfig32(device, uint16_t(offset) * 4, value);
  return value;
}

uint32_t PciBus::readConfigSpace(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
  uint32_t value = 0xffffffffU;
  read(bus, device, function, uint16_t(offset) * 4, 4, value);
  return value;
}

void PciBus::writeConfigSpace(Device* device, uint8_t offset, uint32_t value) {
  writeConfig32(device, uint16_t(offset) * 4, value);
}

void PciBus::writeConfigSpace(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                              uint32_t value) {
  write(bus, device, function, uint16_t(offset) * 4, 4, value);
}

bool PciBus::readConfig8(Device* device, uint16_t offset, uint8_t& value) {
  uint32_t data = 0;
  if (!readFunction(device, offset, 1, data)) {
    return false;
  }
  value = data;
  return true;
}

bool PciBus::readConfig16(Device* device, uint16_t offset, uint16_t& value) {
  uint32_t data = 0;
  if (!readFunction(device, offset, 2, data)) {
    return false;
  }
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

bool PciBus::attachDmaRemapping(Device*) {
  return false;
}

bool PciBus::hasDmaRemapping(Device*) const {
  return false;
}

bool PciBus::mapDmaPage(Device* device, physical_uintptr_t physical, size_t bytes,
                        DmaMapping& mapping) {
  if (!device || mapping.m_Device || !physical || !bytes || bytes > TargetInfo::getPageSize() ||
      (physical & (TargetInfo::getPageSize() - 1)) ||
      uint64_t{physical} > 0xffffffffULL - (bytes - 1)) {
    return false;
  }
  mapping.m_Device = device;
  mapping.m_Address = static_cast<uint32_t>(physical);
  return true;
}

void PciBus::unmapDmaPage(Device*, uint16_t) {}

bool PciBus::reserveLegacyInterrupt(uint8_t) {
  return false;
}

bool PciBus::updateCommand(Device* device, uint16_t clearBits, uint16_t setBits) {
  if (!device) {
    return false;
  }
  LockGuard<Spinlock> guard(configLock);
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
  if (!device || !PciFunctionState::inspect(function, state, false))
    return false;
  return !requireLegacyInterrupt || state.interruptPin;
}

bool PciBus::disableMessageInterrupts(Device* device, const PciFunctionState::State& state) {
  FunctionConfig function{device};
  return PciFunctionState::disableMessageInterrupts(function, state);
}
bool PciBus::enableMsi(Device* device, uint64_t address, uint16_t data) {
  FunctionConfig function{device};
  PciFunctionState::State state;
  return device && PciFunctionState::inspect(function, state, false) &&
         PciFunctionState::enableMsi(function, state, address, data);
}
bool PciBus::disableMsi(Device* device) {
  FunctionConfig function{device};
  PciFunctionState::State state;
  return device && PciFunctionState::inspect(function, state, false) &&
         PciFunctionState::disableMsi(function, state);
}
bool PciBus::enableMsix(Device* device, uint64_t address, uint32_t data) {
  return enableMsixVectors(device, address, &data, 1);
}
bool PciBus::enableMsixVectors(Device* device, uint64_t address, const uint32_t* data,
                               size_t count, bool* touched) {
  if (touched) {
    *touched = false;
  }
  FunctionConfig function{device};
  PciFunctionState::State state;
  PciFunctionState::MsixTable table;
  if (!device || !data || !count || !PciFunctionState::inspect(function, state, false) ||
      !PciFunctionState::msixTable(function, state, table) || count > table.vectors) {
    return false;
  }
  IoBase* io = PciFunctionState::msixTableIo(device, state, table, true);
  if (touched && io) {
    *touched = true;
  }
  return io && PciFunctionState::enableMsixVectors(function, state, *io, table.offset, address,
                                                   data, count);
}
bool PciBus::setMsixVectorMask(Device* device, size_t index, bool masked) {
  FunctionConfig function{device};
  PciFunctionState::State state;
  PciFunctionState::MsixTable table;
  if (!device || !PciFunctionState::inspect(function, state, false) ||
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
  if (!device || !PciFunctionState::inspect(function, state, false) ||
      !PciFunctionState::msixTable(function, state, table))
    return false;
  IoBase* io = PciFunctionState::msixTableIo(device, state, table, false);
  return io && PciFunctionState::disableMsix(function, state, *io, table.offset);
}

bool PciBus::resourcesUnchanged(Device* device, const PciFunctionState::State& state) {
  FunctionConfig function{device};
  return PciFunctionState::resourcesUnchanged(function, state);
}

bool PciBus::translateAddress(uint64_t pciAddress, uint64_t bytes, bool io, uint64_t& cpuPhysical) {
  return VirtDeviceTree::pciTranslate(pciAddress, bytes, io, cpuPhysical);
}

bool PciBus::busRange(uint8_t& first, uint8_t& last) {
  if (!configAvailable) {
    return false;
  }
  first = static_cast<uint8_t>(host.firstBus);
  last = static_cast<uint8_t>(host.lastBus);
  return true;
}

uint32_t PciBus::interruptRoute(uint8_t bus, uint8_t device, uint8_t function, uint8_t pin) {
  if (!configAvailable || bus < host.firstBus || bus > host.lastBus || device >= 32 ||
      function >= 8 || pin < 1 || pin > 4) {
    return 0;
  }
  while (bus != host.firstBus) {
    UpstreamBridge bridge = {};
    if (!findUpstreamBridge(bus, bridge)) {
      return 0;
    }
    pin = ((pin - 1 + device) & 3U) + 1;
    bus = bridge.bus;
    device = bridge.device;
    function = bridge.function;
  }
  return VirtDeviceTree::pciInterrupt(bus, device, function, pin);
}

bool PciBus::assignBar(Device* device, uint8_t index, uint32_t low, uint32_t high, uint32_t maskLow,
                       uint32_t maskHigh) {
  if (!device || index >= 6) {
    return false;
  }
  const bool io = low & 1U;
  const uint32_t type = (low >> 1) & 3U;
  if (io && type) {
    return false;
  }
  if (!io && type == 3) {
    return false;
  }
  const bool wide = !io && type == 2;
  if (wide && index == 5) {
    return false;
  }
  const uint64_t width = io          ? 0xffffULL
                         : type == 1 ? 0xfffffULL
                         : wide      ? UINT64_MAX
                                     : 0xffffffffULL;
  const uint64_t addressMask = width & (io ? ~uint64_t{3} : ~uint64_t{15});
  const uint64_t original = ((wide ? uint64_t(high) << 32 : 0) | low) & addressMask;
  const uint64_t mask = ((wide ? uint64_t(maskHigh) << 32 : 0) | maskLow) & addressMask;
  const uint64_t bytes = ((~mask) & width) + 1;
  if (!bytes || (bytes & (bytes - 1))) {
    return false;
  }
  const uint8_t bus = device->getPciBusPosition();
  const bool behindBridge = bus != host.firstBus;
  const bool prefetchable = !io && (low & 8U);
  if (behindBridge && !original) {
    // The host cursor does not reserve resources for sibling bridge windows.
    return false;
  }

  LockGuard<Spinlock> guard(configLock);
  for (size_t i = 0; i < windowCount; ++i) {
    VirtPciWindow& window = windows[i];
    if ((window.space == 0x01000000) != io || original < window.pciBase ||
        original - window.pciBase >= window.size ||
        bytes > window.size - (original - window.pciBase) ||
        (behindBridge && !io && !prefetchable && window.space == 0x03000000) ||
        (behindBridge && original && !bridgesForward(bus, original, bytes, io, prefetchable))) {
      continue;
    }
    if (original) {
      const uint64_t end = original + bytes;
      if (nextAddress[i] < end) {
        nextAddress[i] = end;
      }
      return true;
    }
  }
  if (original) {
    return false;
  }

  for (size_t i = 0; i < windowCount; ++i) {
    const VirtPciWindow& window = windows[i];
    if ((window.space == 0x01000000) != io) {
      continue;
    }
    const uint64_t cursor = nextAddress[i] ? nextAddress[i] : bytes;
    const uint64_t candidate = (cursor + bytes - 1) & ~(bytes - 1);
    if (candidate < cursor || candidate < window.pciBase ||
        candidate - window.pciBase >= window.size ||
        bytes > window.size - (candidate - window.pciBase) || candidate > width ||
        bytes - 1 > width - candidate) {
      continue;
    }
    uint16_t command = 0;
    if (!readConfig16(device, 4, command) || !writeConfig16(device, 4, command & ~3U)) {
      return false;
    }
    const uint32_t assignedLow =
        static_cast<uint32_t>((candidate & addressMask) | (low & ~uint32_t(addressMask)));
    const uint16_t offset = 0x10 + index * 4;
    uint32_t actualLow = 0;
    uint32_t actualHigh = 0;
    const bool assigned =
        writeConfig32(device, offset, assignedLow) &&
        (!wide || writeConfig32(device, offset + 4, candidate >> 32)) &&
        readConfig32(device, offset, actualLow) &&
        (!wide || readConfig32(device, offset + 4, actualHigh)) &&
        (actualLow & uint32_t(addressMask)) == (assignedLow & uint32_t(addressMask)) &&
        (!wide || actualHigh == static_cast<uint32_t>(candidate >> 32));
    if (!assigned) {
      writeConfig32(device, offset, low);
      if (wide) {
        writeConfig32(device, offset + 4, high);
      }
    }
    if (!writeConfig16(device, 4, command)) {
      return false;
    }
    if (!assigned) {
      return false;
    }
    nextAddress[i] = candidate + bytes;
    return true;
  }
  return false;
}
