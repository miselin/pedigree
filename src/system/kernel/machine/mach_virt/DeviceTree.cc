/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "DeviceTree.h"

#include "Acpi.h"

namespace {
constexpr uint32_t FdtMagic = 0xd00dfeed;
constexpr uint32_t BeginNode = 1;
constexpr uint32_t EndNode = 2;
constexpr uint32_t Property = 3;
constexpr uint32_t Nop = 4;
constexpr uint32_t End = 9;
constexpr size_t MaxDtbSize = 2 * 1024 * 1024;
constexpr size_t MaxDepth = 16;
constexpr size_t MaxMemoryRegions = 8;
constexpr size_t MaxVirtioMmio = 32;
constexpr size_t MaxPciWindows = 8;
constexpr uintptr_t DirectMapBase = 0xffff000000000000ULL;

struct PlatformInfo {
  VirtMemoryRegion memory[MaxMemoryRegions];
  size_t memoryCount;
  VirtMmioDevice virtioMmio[MaxVirtioMmio];
  size_t virtioMmioCount;
  VirtPciHost pciHost;
  VirtPciWindow pciWindows[MaxPciWindows];
  size_t pciWindowCount;
  const uint8_t* pciInterruptMap;
  size_t pciInterruptMapSize;
  uint32_t pciInterruptMask[4];
  uint32_t acpiPciIrq[32][4];
  uint32_t gicPhandle;
  uint32_t gicAddressCells;
  uintptr_t uart;
  uintptr_t rtc;
  uintptr_t gicDistributor;
  uintptr_t gicCpu;
  uintptr_t gicRedistributor;
  uint32_t gicVersion;
  uint32_t uartIrq;
  uint32_t physicalTimerIrq;
  uint32_t virtualTimerIrq;
  uint64_t initrdStart;
  uint64_t initrdEnd;
  const char* bootargs;
  size_t blobSize;
  bool psciAvailable;
  bool psciHvc;
  bool acpi;
  bool valid;
};

struct Node {
  uint32_t addressCells;
  uint32_t sizeCells;
  const uint8_t* reg;
  size_t regSize;
  const uint8_t* interrupts;
  size_t interruptsSize;
  const uint8_t* ranges;
  size_t rangesSize;
  const uint8_t* interruptMap;
  size_t interruptMapSize;
  const uint8_t* interruptMapMask;
  size_t interruptMapMaskSize;
  uint32_t firstBus;
  uint32_t lastBus;
  uint32_t phandle;
  bool memory;
  bool uart;
  bool rtc;
  bool gic;
  bool gicV3;
  bool timer;
  bool psci;
  bool psciHvc;
  bool virtioMmio;
  bool chosen;
  bool pciHost;
  bool busRange;
  uint64_t initrdStart;
  uint64_t initrdEnd;
  const char* bootargs;
};

PlatformInfo g_Platform;

uint32_t read32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

bool equal(const char* a, const char* b) {
  while (*a && *b && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

bool hasString(const uint8_t* data, size_t size, const char* wanted) {
  size_t offset = 0;
  while (offset < size) {
    size_t length = 0;
    while (offset + length < size && data[offset + length]) {
      ++length;
    }
    if (offset + length == size) {
      return false;
    }
    if (equal(reinterpret_cast<const char*>(data + offset), wanted)) {
      return true;
    }
    offset += length + 1;
  }
  return false;
}

bool readCells(const uint8_t* data, size_t size, uint32_t count, uint64_t& value) {
  if (!count || count > 2 || size < count * 4) {
    return false;
  }
  value = count == 2 ? read32(data) : 0;
  value = (value << 32) | read32(data + (count - 1) * 4);
  return true;
}

bool firstReg(const Node& node, const Node& parent, uint64_t& base, uint64_t& size) {
  const uint32_t addressBytes = parent.addressCells * 4;
  const uint32_t sizeBytes = parent.sizeCells * 4;
  if (!node.reg || node.regSize < addressBytes + sizeBytes ||
      !readCells(node.reg, node.regSize, parent.addressCells, base) ||
      !readCells(node.reg + addressBytes, node.regSize - addressBytes, parent.sizeCells, size)) {
    return false;
  }
  return size && base <= UINT64_MAX - size;
}

uint32_t gicIrq(const uint8_t* data, size_t size) {
  if (!data || size < 12) {
    return 0;
  }
  const uint32_t type = read32(data);
  const uint32_t number = read32(data + 4);
  if (type == 0 && number < 988) {
    return number + 32;
  }
  if (type == 1 && number < 16) {
    return number + 16;
  }
  return 0;
}

void finishNode(const Node& node, const Node& parent, PlatformInfo& out) {
  uint64_t base = 0;
  uint64_t size = 0;
  if (node.memory && node.reg && parent.addressCells && parent.sizeCells &&
      parent.addressCells <= 2 && parent.sizeCells <= 2) {
    const size_t stride = (parent.addressCells + parent.sizeCells) * 4;
    for (size_t offset = 0; offset + stride <= node.regSize && out.memoryCount < MaxMemoryRegions;
         offset += stride) {
      Node entry = node;
      entry.reg = node.reg + offset;
      entry.regSize = stride;
      if (firstReg(entry, parent, base, size)) {
        out.memory[out.memoryCount++] = {base, size};
      }
    }
  }

  if (node.uart && firstReg(node, parent, base, size)) {
    out.uart = static_cast<uintptr_t>(base);
    out.uartIrq = gicIrq(node.interrupts, node.interruptsSize);
  }
  if (node.rtc && firstReg(node, parent, base, size)) {
    out.rtc = static_cast<uintptr_t>(base);
  }

  if (node.gic && firstReg(node, parent, base, size)) {
    out.gicDistributor = static_cast<uintptr_t>(base);
    out.gicVersion = node.gicV3 ? 3 : 2;
    out.gicPhandle = node.phandle;
    out.gicAddressCells = node.addressCells;
    const size_t stride = (parent.addressCells + parent.sizeCells) * 4;
    if (node.regSize >= stride * 2) {
      Node cpu = node;
      cpu.reg += stride;
      cpu.regSize -= stride;
      if (firstReg(cpu, parent, base, size)) {
        if (node.gicV3) {
          out.gicRedistributor = static_cast<uintptr_t>(base);
        } else {
          out.gicCpu = static_cast<uintptr_t>(base);
        }
      }
    }
  }

  if (node.timer && node.interruptsSize >= 36) {
    out.physicalTimerIrq = gicIrq(node.interrupts + 12, node.interruptsSize - 12);
    out.virtualTimerIrq = gicIrq(node.interrupts + 24, node.interruptsSize - 24);
  }
  if (node.virtioMmio && out.virtioMmioCount < MaxVirtioMmio &&
      firstReg(node, parent, base, size)) {
    out.virtioMmio[out.virtioMmioCount++] = {base, size,
                                             gicIrq(node.interrupts, node.interruptsSize)};
  }
  if (node.pciHost && !out.pciHost.size && firstReg(node, parent, base, size) &&
      node.addressCells == 3 && node.sizeCells == 2) {
    const uint64_t buses = size >> 20;
    const uint32_t firstBus = node.busRange ? node.firstBus : 0;
    const uint32_t lastBus = node.busRange ? node.lastBus : 255;
    if (!(base & 0xfffff) && !(size & 0xfffff) && buses && firstBus <= lastBus && lastBus < 256 &&
        uint64_t(lastBus - firstBus) < buses) {
      out.pciHost = {base, size, firstBus, lastBus};
      if (node.interruptMap && node.interruptMapMask && node.interruptMapMaskSize == 16 &&
          !(node.interruptMapSize % 4)) {
        out.pciInterruptMap = node.interruptMap;
        out.pciInterruptMapSize = node.interruptMapSize;
        for (size_t i = 0; i < 4; ++i) {
          out.pciInterruptMask[i] = read32(node.interruptMapMask + i * 4);
        }
      }
      if (node.ranges && parent.addressCells <= 2) {
        const size_t stride = (node.addressCells + parent.addressCells + node.sizeCells) * 4;
        for (size_t offset = 0;
             offset + stride <= node.rangesSize && out.pciWindowCount < MaxPciWindows;
             offset += stride) {
          const uint8_t* entry = node.ranges + offset;
          const uint32_t space = read32(entry) & 0x03000000;
          const uint64_t pciBase = (uint64_t(read32(entry + 4)) << 32) | read32(entry + 8);
          uint64_t cpuBase = 0;
          uint64_t length = 0;
          if ((space == 0x01000000 || space == 0x02000000 || space == 0x03000000) &&
              readCells(entry + 12, stride - 12, parent.addressCells, cpuBase) &&
              readCells(entry + 12 + parent.addressCells * 4, stride - 12 - parent.addressCells * 4,
                        node.sizeCells, length) &&
              length && pciBase <= UINT64_MAX - length && cpuBase <= UINT64_MAX - length) {
            out.pciWindows[out.pciWindowCount++] = {space, pciBase, cpuBase, length};
          }
        }
      }
    }
  }
  if (node.psci) {
    out.psciAvailable = true;
    out.psciHvc = node.psciHvc;
  }
  if (node.chosen) {
    out.initrdStart = node.initrdStart;
    out.initrdEnd = node.initrdEnd;
    out.bootargs = node.bootargs;
  }
}

bool parse(const uint8_t* dtb, PlatformInfo& out) {
  if (!dtb || read32(dtb) != FdtMagic) {
    return false;
  }

  const size_t total = read32(dtb + 4);
  const size_t structOffset = read32(dtb + 8);
  const size_t stringsOffset = read32(dtb + 12);
  const size_t stringsSize = read32(dtb + 32);
  const size_t structSize = read32(dtb + 36);
  if (total < 40 || total > MaxDtbSize || structOffset > total ||
      structSize > total - structOffset || stringsOffset > total ||
      stringsSize > total - stringsOffset) {
    return false;
  }

  const uint8_t* structure = dtb + structOffset;
  const uint8_t* strings = dtb + stringsOffset;
  size_t cursor = 0;
  Node stack[MaxDepth] = {};
  size_t depth = 0;

  while (cursor + 4 <= structSize) {
    const uint32_t token = read32(structure + cursor);
    cursor += 4;
    if (token == BeginNode) {
      if (depth == MaxDepth) {
        return false;
      }
      const size_t start = cursor;
      while (cursor < structSize && structure[cursor]) {
        ++cursor;
      }
      if (cursor == structSize) {
        return false;
      }
      const size_t nameLength = cursor - start;
      cursor = (cursor + 4) & ~size_t(3);
      if (cursor > structSize) {
        return false;
      }
      Node& node = stack[depth];
      node = {};
      node.addressCells = depth ? stack[depth - 1].addressCells : 2;
      node.sizeCells = depth ? stack[depth - 1].sizeCells : 1;
      node.memory = depth == 1 && nameLength >= 6 && structure[start] == 'm' &&
                    structure[start + 1] == 'e' && structure[start + 2] == 'm' &&
                    structure[start + 3] == 'o' && structure[start + 4] == 'r' &&
                    structure[start + 5] == 'y';
      node.chosen = depth == 1 && equal(reinterpret_cast<const char*>(structure + start), "chosen");
      ++depth;
    } else if (token == Property) {
      if (!depth || cursor + 8 > structSize) {
        return false;
      }
      const size_t length = read32(structure + cursor);
      const size_t nameOffset = read32(structure + cursor + 4);
      cursor += 8;
      if (nameOffset >= stringsSize || length > structSize - cursor) {
        return false;
      }
      size_t nameEnd = nameOffset;
      while (nameEnd < stringsSize && strings[nameEnd]) {
        ++nameEnd;
      }
      if (nameEnd == stringsSize) {
        return false;
      }
      const char* name = reinterpret_cast<const char*>(strings + nameOffset);
      const uint8_t* data = structure + cursor;
      Node& node = stack[depth - 1];
      if (equal(name, "#address-cells") && length == 4) {
        node.addressCells = read32(data);
      } else if (equal(name, "#size-cells") && length == 4) {
        node.sizeCells = read32(data);
      } else if (equal(name, "reg")) {
        node.reg = data;
        node.regSize = length;
      } else if (equal(name, "interrupts")) {
        node.interrupts = data;
        node.interruptsSize = length;
      } else if (equal(name, "ranges")) {
        node.ranges = data;
        node.rangesSize = length;
      } else if (equal(name, "interrupt-map")) {
        node.interruptMap = data;
        node.interruptMapSize = length;
      } else if (equal(name, "interrupt-map-mask")) {
        node.interruptMapMask = data;
        node.interruptMapMaskSize = length;
      } else if (equal(name, "bus-range") && length == 8) {
        node.firstBus = read32(data);
        node.lastBus = read32(data + 4);
        node.busRange = true;
      } else if (equal(name, "phandle") && length == 4) {
        node.phandle = read32(data);
      } else if (equal(name, "device_type") && hasString(data, length, "memory")) {
        node.memory = true;
      } else if (equal(name, "compatible")) {
        node.uart = hasString(data, length, "arm,pl011");
        node.rtc = hasString(data, length, "arm,pl031");
        node.gic = hasString(data, length, "arm,cortex-a15-gic") ||
                   hasString(data, length, "arm,gic-400") || hasString(data, length, "arm,gic-v3");
        node.gicV3 = hasString(data, length, "arm,gic-v3");
        node.timer = hasString(data, length, "arm,armv8-timer");
        node.psci =
            hasString(data, length, "arm,psci-1.0") || hasString(data, length, "arm,psci-0.2");
        node.virtioMmio = hasString(data, length, "virtio,mmio");
        node.pciHost = hasString(data, length, "pci-host-ecam-generic");
      } else if (equal(name, "method")) {
        node.psciHvc = hasString(data, length, "hvc");
      } else if (node.chosen && equal(name, "linux,initrd-start")) {
        if (length == 4 || length == 8) {
          readCells(data, length, length / 4, node.initrdStart);
        }
      } else if (node.chosen && equal(name, "linux,initrd-end")) {
        if (length == 4 || length == 8) {
          readCells(data, length, length / 4, node.initrdEnd);
        }
      } else if (node.chosen && equal(name, "bootargs") && length && data[length - 1] == 0) {
        node.bootargs = reinterpret_cast<const char*>(data);
      }
      cursor = (cursor + length + 3) & ~size_t(3);
      if (cursor > structSize) {
        return false;
      }
    } else if (token == EndNode) {
      if (!depth) {
        return false;
      }
      if (depth > 1) {
        finishNode(stack[depth - 1], stack[depth - 2], out);
      }
      --depth;
    } else if (token == End) {
      return depth == 0 && out.memoryCount && out.uart && out.gicDistributor &&
             ((out.gicVersion == 2 && out.gicCpu) ||
              (out.gicVersion == 3 && out.gicRedistributor)) &&
             out.physicalTimerIrq && out.virtualTimerIrq;
    } else if (token != Nop) {
      return false;
    }
  }
  return false;
}
}  // namespace

bool VirtDeviceTree::initialise(const void* dtb) {
  PlatformInfo parsed = {};
  if (!parse(static_cast<const uint8_t*>(dtb), parsed)) {
    g_Platform = {};
    return false;
  }
  parsed.blobSize = read32(static_cast<const uint8_t*>(dtb) + 4);
  parsed.valid = true;
  g_Platform = parsed;
  return true;
}

bool VirtDeviceTree::initialiseAcpi(uint64_t rsdpPhysical,
                                    const BootstrapStruct_t::MemoryMapEntry* memoryMap,
                                    size_t memoryMapCount) {
  VirtAcpiInfo acpi = {};
  if (!virtParseAcpi(rsdpPhysical, memoryMap, memoryMapCount, acpi)) {
    g_Platform = {};
    return false;
  }
  PlatformInfo platform = {};
  platform.pciHost = acpi.pciHost;
  platform.pciWindowCount = acpi.pciWindowCount;
  for (size_t i = 0; i < acpi.pciWindowCount; ++i) {
    platform.pciWindows[i] = acpi.pciWindows[i];
  }
  for (size_t slot = 0; slot < 32; ++slot) {
    for (size_t pin = 0; pin < 4; ++pin) {
      platform.acpiPciIrq[slot][pin] = acpi.pciIrq[slot][pin];
    }
  }
  platform.uart = acpi.uart;
  platform.uartIrq = acpi.uartIrq;
  platform.rtc = acpi.rtc;
  platform.gicDistributor = acpi.gicDistributor;
  platform.gicCpu = acpi.gicCpu;
  platform.gicRedistributor = acpi.gicRedistributor;
  platform.gicVersion = acpi.gicVersion;
  platform.physicalTimerIrq = acpi.physicalTimerIrq;
  platform.virtualTimerIrq = acpi.virtualTimerIrq;
  platform.psciAvailable = acpi.psciAvailable;
  platform.psciHvc = acpi.psciHvc;
  platform.acpi = true;
  platform.valid = true;
  g_Platform = platform;
  return true;
}

bool VirtDeviceTree::valid() {
  return g_Platform.valid;
}

bool VirtDeviceTree::memoryRegion(size_t index, VirtMemoryRegion& region) {
  if (!g_Platform.valid || index >= g_Platform.memoryCount) {
    return false;
  }
  region = g_Platform.memory[index];
  return true;
}

bool VirtDeviceTree::virtioMmio(size_t index, VirtMmioDevice& device) {
  if (!g_Platform.valid || index >= g_Platform.virtioMmioCount) {
    return false;
  }
  device = g_Platform.virtioMmio[index];
  return true;
}

bool VirtDeviceTree::pciHost(VirtPciHost& host) {
  if (!g_Platform.valid || !g_Platform.pciHost.size) {
    return false;
  }
  host = g_Platform.pciHost;
  return true;
}

bool VirtDeviceTree::pciWindow(size_t index, VirtPciWindow& window) {
  if (!g_Platform.valid || index >= g_Platform.pciWindowCount) {
    return false;
  }
  window = g_Platform.pciWindows[index];
  return true;
}

bool VirtDeviceTree::pciTranslate(uint64_t address, uint64_t size, bool io, uint64_t& physical) {
  if (!g_Platform.valid || !size) {
    return false;
  }
  for (size_t i = 0; i < g_Platform.pciWindowCount; ++i) {
    const VirtPciWindow& window = g_Platform.pciWindows[i];
    if ((window.space == 0x01000000) != io || address < window.pciBase ||
        address - window.pciBase >= window.size ||
        size > window.size - (address - window.pciBase)) {
      continue;
    }
    physical = window.cpuBase + (address - window.pciBase);
    return true;
  }
  return false;
}

uint32_t VirtDeviceTree::pciInterrupt(uint8_t bus, uint8_t device, uint8_t function, uint8_t pin) {
  if (g_Platform.acpi) {
    return bus == g_Platform.pciHost.firstBus && device < 32 && function < 8 && pin >= 1 && pin <= 4
               ? g_Platform.acpiPciIrq[device][pin - 1]
               : 0;
  }
  if (!g_Platform.valid || !g_Platform.gicPhandle || !g_Platform.pciInterruptMap ||
      g_Platform.gicAddressCells > 2 || device >= 32 || function >= 8 || pin < 1 || pin > 4) {
    return 0;
  }
  const size_t stride = (8 + g_Platform.gicAddressCells) * 4;
  if (g_Platform.pciInterruptMapSize % stride) {
    return 0;
  }
  const uint32_t address =
      (uint32_t(bus) << 16) | (uint32_t(device) << 11) | (uint32_t(function) << 8);
  for (size_t offset = 0; offset < g_Platform.pciInterruptMapSize; offset += stride) {
    const uint8_t* entry = g_Platform.pciInterruptMap + offset;
    if ((read32(entry) & g_Platform.pciInterruptMask[0]) !=
            (address & g_Platform.pciInterruptMask[0]) ||
        (read32(entry + 4) & g_Platform.pciInterruptMask[1]) ||
        (read32(entry + 8) & g_Platform.pciInterruptMask[2]) ||
        (read32(entry + 12) & g_Platform.pciInterruptMask[3]) !=
            (pin & g_Platform.pciInterruptMask[3]) ||
        read32(entry + 16) != g_Platform.gicPhandle) {
      continue;
    }
    return gicIrq(entry + 20 + g_Platform.gicAddressCells * 4, 12);
  }
  return 0;
}

uintptr_t VirtDeviceTree::uartBase() {
  return g_Platform.uart ? DirectMapBase + g_Platform.uart : 0;
}

uint32_t VirtDeviceTree::uartIrq() {
  return g_Platform.uartIrq;
}

uintptr_t VirtDeviceTree::rtcBase() {
  return g_Platform.rtc ? DirectMapBase + g_Platform.rtc : 0;
}

uint32_t VirtDeviceTree::gicVersion() {
  return g_Platform.gicVersion;
}

uintptr_t VirtDeviceTree::gicDistributorBase() {
  return g_Platform.gicDistributor ? DirectMapBase + g_Platform.gicDistributor : 0;
}

uintptr_t VirtDeviceTree::gicCpuBase() {
  return g_Platform.gicCpu ? DirectMapBase + g_Platform.gicCpu : 0;
}

uintptr_t VirtDeviceTree::gicRedistributorBase() {
  return g_Platform.gicRedistributor ? DirectMapBase + g_Platform.gicRedistributor : 0;
}

uint32_t VirtDeviceTree::physicalTimerIrq() {
  return g_Platform.physicalTimerIrq;
}

uint32_t VirtDeviceTree::virtualTimerIrq() {
  return g_Platform.virtualTimerIrq;
}

bool VirtDeviceTree::psciUsesHvc() {
  return g_Platform.psciHvc;
}

bool VirtDeviceTree::psciAvailable() {
  return g_Platform.psciAvailable;
}

bool VirtDeviceTree::initrd(uint64_t& start, uint64_t& end) {
  if (!g_Platform.valid || !g_Platform.initrdStart ||
      g_Platform.initrdEnd <= g_Platform.initrdStart) {
    return false;
  }
  start = g_Platform.initrdStart;
  end = g_Platform.initrdEnd;
  return true;
}

const char* VirtDeviceTree::bootargs() {
  return g_Platform.valid ? g_Platform.bootargs : nullptr;
}

size_t VirtDeviceTree::blobSize() {
  return g_Platform.valid ? g_Platform.blobSize : 0;
}

extern "C" void virtSetDeviceTree(const void* dtb) {
  VirtDeviceTree::initialise(dtb);
}

extern "C" bool virtGetMemoryRegion(size_t index, uint64_t* base, uint64_t* size) {
  VirtMemoryRegion region;
  if (!base || !size || !VirtDeviceTree::memoryRegion(index, region)) {
    return false;
  }
  *base = region.base;
  *size = region.size;
  return true;
}

extern "C" bool virtGetVirtioMmio(size_t index, uint64_t* base, uint64_t* size, uint32_t* irq) {
  VirtMmioDevice device;
  if (!base || !size || !irq || !VirtDeviceTree::virtioMmio(index, device)) {
    return false;
  }
  *base = device.base;
  *size = device.size;
  *irq = device.irq;
  return true;
}

extern "C" bool virtGetInitrd(uint64_t* start, uint64_t* end) {
  return start && end && VirtDeviceTree::initrd(*start, *end);
}

extern "C" bool virtGetBootargs(const char** text) {
  if (!text) {
    return false;
  }
  *text = VirtDeviceTree::bootargs();
  return *text != nullptr;
}
