/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "Resources.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/PciFirmware.h"
#include "pedigree/kernel/machine/PciSriov.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/utilities/Vector.h"

namespace {
enum class Kind : uint8_t { Bar, MemoryWindow, PrefetchWindow, Assigned };

struct Interval {
  const PciFirmware::Root* root;
  uint32_t route;
  uint64_t base;
  uint64_t last;
  Kind kind;
  uint8_t bar;
  bool prefetchable;
};

struct Staged {
  Interval interval;
  Device* owner;
};

struct Function {
  const PciFirmware::Root* root;
  uint32_t route;
};

Mutex g_Lock;
Vector<Interval> g_Intervals;
Vector<Function> g_Functions;
bool g_Ready = false;

uint32_t routeId(Device* device) {
  return (device->getPciBusPosition() << 8) | (device->getPciDevicePosition() << 3) |
         device->getPciFunctionNumber();
}

bool isPciFunction(Device* device) {
  Device* parent = device->getParent();
  if (!parent || parent->getSpecificType() != "pci") {
    return false;
  }
  const auto config = device->getPciConfigHeader();
  return config.vendor && config.vendor != 0xffff;
}

bool isBridge(Device* device) {
  return device->getPciClassCode() == 6 && device->getPciSubclassCode() == 4 &&
         (device->getPciConfigHeader().header_type & 0x7f) == 1;
}

bool ancestor(uint32_t route, Device* device) {
  for (Device* parent = device->getParent(); parent; parent = parent->getParent()) {
    if (isPciFunction(parent) && routeId(parent) == route) {
      return true;
    }
  }
  return false;
}

bool lastAddress(uint64_t base, uint64_t size, uint64_t& last) {
  if (!size || size - 1 > ~uint64_t{0} - base) {
    return false;
  }
  last = base + size - 1;
  return true;
}

bool overlap(const Interval& a, const Interval& b) {
  return a.root == b.root && a.base <= b.last && b.base <= a.last;
}

bool bridgeWindow(const Interval& entry) {
  return entry.kind == Kind::MemoryWindow || entry.kind == Kind::PrefetchWindow;
}

bool usableWindow(const PciFirmware::Window& window, bool prefetchable) {
  if (window.io || window.prefetchable != prefetchable) {
    return false;
  }
#if X86_COMMON
  if (window.pciBase != window.cpuBase) {
    WARNING("PCI resources: translated host window unsupported");
    return false;
  }
#endif
  return true;
}

template <typename T>
bool append(Vector<T>& entries, const T& entry) {
  if (entries.count() == entries.size() && !entries.tryReserve(entries.count() + 1)) {
    return false;
  }
  entries.pushBack(entry);
  return true;
}

bool addWindow(Device* owner, const PciFirmware::Root* root, uint32_t registerValue,
               uint32_t highBase, uint32_t highLimit, bool prefetchable, Vector<Staged>& staged) {
  const uint16_t baseRegister = registerValue;
  const uint16_t limitRegister = registerValue >> 16;
  const uint16_t type = baseRegister & 0xf;
  if (prefetchable) {
    if (type != (limitRegister & 0xf) || type > 1) {
      return false;
    }
  } else if (type || (limitRegister & 0xf)) {
    return false;
  }

  uint64_t base = uint64_t(baseRegister & 0xfff0U) << 16;
  uint64_t last = (uint64_t(limitRegister & 0xfff0U) << 16) | 0xfffffU;
  if (prefetchable && type == 1) {
    base |= uint64_t(highBase) << 32;
    last |= uint64_t(highLimit) << 32;
  }
  if (base > last) {
    return true;  // Disabled window.
  }
  return append(staged,
                {{root, routeId(owner), base, last,
                  prefetchable ? Kind::PrefetchWindow : Kind::MemoryWindow, 0, prefetchable},
                 owner});
}

bool sriovDisabled(Device* device) {
  auto& pci = PciBus::instance();
  PciExtendedCapabilities::Capability capability;
  const auto discovery = pci.findExtendedCapability(device, PciSriov::CapabilityId, capability);
  if (discovery == PciExtendedCapabilities::FindResult::Malformed) {
    WARNING("PCI resources: malformed extended capabilities");
    return false;
  }
  if (discovery == PciExtendedCapabilities::FindResult::Found) {
    struct Config {
      PciBus& pci;
      Device* device;
      bool read16(uint16_t offset, uint16_t& value) {
        return pci.readConfig16(device, offset, value);
      }
      bool read32(uint16_t offset, uint32_t& value) {
        return pci.readConfig32(device, offset, value);
      }
    } access{pci, device};
    PciSriov::State state;
    if (!PciSriov::read(access, capability.offset, capability.next, state) ||
        (state.control & PciSriov::VfControl)) {
      // Active VF BARs cannot be sized without disrupting inherited users.
      WARNING("PCI resources: SR-IOV apertures are active or unavailable");
      return false;
    }
  }
  return true;
}

bool stoppedLeaf(Device* device) {
  if (!device || !isPciFunction(device) || device->getPhysicalFunction() ||
      device->getNumChildren() || device->getPciBusPosition() > 255 ||
      device->getPciDevicePosition() > 31 || device->getPciFunctionNumber() > 7) {
    return false;
  }
  const auto config = device->getPciConfigHeader();
  auto& pci = PciBus::instance();
  uint32_t identity = 0;
  uint32_t rom = 0;
  uint16_t command = 0;
  uint8_t header = 0xff;
  return !(config.header_type & 0x7f) && pci.readConfig32(device, 0, identity) &&
         identity == ((uint32_t(config.device) << 16) | config.vendor) &&
         pci.readConfig8(device, 0xe, header) && !(header & 0x7f) &&
         pci.readConfig16(device, 4, command) && !(command & 7U) &&
         pci.readConfig32(device, 0x30, rom) && !(rom & 1U) && !pci.hasDmaRemapping(device) &&
         sriovDisabled(device);
}

bool collect(Device* device, Vector<Staged>& staged, Vector<Function>& functions) {
  if (!isPciFunction(device)) {
    return true;
  }
  if (device->getPciBusPosition() > 255 || device->getPciDevicePosition() > 31 ||
      device->getPciFunctionNumber() > 7 || !sriovDisabled(device)) {
    return false;
  }
  const auto* root = PciFirmware::rootForBus(device->getPciBusPosition());
  if (!root || root->segment != 0 || root->windowCount > PciFirmware::MaxWindows ||
      !append(functions, {root, routeId(device)})) {
    return false;
  }

  const auto config = device->getPciConfigHeader();
  const uint8_t header = config.header_type & 0x7f;
  if (header > 1) {
    return false;
  }
  // Expansion ROM sizing is not available in the enumerated BAR inventory.
  if ((header == 0 && (config.rom_base_address & 1U)) || (header == 1 && (config.reserved1 & 1U))) {
    return false;
  }
  const size_t barCount = header == 0 ? 6 : 2;
  for (size_t bar = 0; bar < barCount; ++bar) {
    const uint32_t low = config.bar[bar];
    if (low & 1U) {
      continue;  // This manager owns MMIO only.
    }
    const unsigned type = (low >> 1) & 3U;
    if (type == 3 || (type == 2 && bar + 1 == barCount)) {
      return false;
    }
    const bool wide = type == 2;
    uint64_t base = low & (type == 1 ? 0xffff0U : ~uint32_t{15});
    if (wide) {
      base |= uint64_t(config.bar[bar + 1]) << 32;
    }
    if (base) {
      char name[] = {'b', 'a', 'r', static_cast<char>('0' + bar), 0};
      Device::Address* address = nullptr;
      for (Device::Address* candidate : device->addresses()) {
        if (candidate && candidate->m_Name == name) {
          address = candidate;
          break;
        }
      }
      uint64_t last = 0;
      if (!address || address->m_IsIoSpace || !lastAddress(base, address->m_Size, last) ||
          !append(staged, {{root, routeId(device), base, last, Kind::Bar, static_cast<uint8_t>(bar),
                            bool(low & 8U)},
                           device})) {
        return false;
      }
    }
    if (wide) {
      ++bar;
    }
  }

  if (isBridge(device) && (config.command & 2U)) {
    const uint32_t prefetchLimitHigh = (uint32_t(config.subsys_id) << 16) | config.subsys_vendor;
    if (!addWindow(device, root, config.bar[4], 0, 0, false, staged) ||
        !addWindow(device, root, config.bar[5], config.cardbus_pointer, prefetchLimitHigh, true,
                   staged)) {
      return false;
    }
  }
  return true;
}

bool scoped(Device* owner, const PciFirmware::Root* root, bool prefetchable, uint64_t& base,
            uint64_t& last) {
  if (owner->getPciBusPosition() != root->firstBus) {
    bool behindBridge = false;
    for (Device* parent = owner->getParent(); parent; parent = parent->getParent()) {
      if (isPciFunction(parent) && isBridge(parent)) {
        behindBridge = true;
        break;
      }
    }
    if (!behindBridge) {
      return false;  // Unlinked secondary bus: forwarding path is ambiguous.
    }
  }
  for (Device* parent = owner->getParent(); parent; parent = parent->getParent()) {
    if (!isPciFunction(parent) || !isBridge(parent)) {
      continue;
    }
    const Kind kind = prefetchable ? Kind::PrefetchWindow : Kind::MemoryWindow;
    const Interval* window = nullptr;
    for (const Interval& entry : g_Intervals) {
      if (entry.root == root && entry.route == routeId(parent) && entry.kind == kind) {
        window = &entry;
        break;
      }
    }
    if (!window) {
      return false;
    }
    if (base < window->base) {
      base = window->base;
    }
    if (last > window->last) {
      last = window->last;
    }
    if (base > last) {
      return false;
    }
  }
  return true;
}

bool ownerRoot(Device* owner, const PciFirmware::Root*& root) {
  if (!g_Ready || !owner || !isPciFunction(owner) || owner->getPciBusPosition() > 255) {
    return false;
  }
  root = PciFirmware::rootForBus(owner->getPciBusPosition());
  if (!root) {
    return false;
  }
  for (const Function& function : g_Functions) {
    if (function.root == root && function.route == routeId(owner)) {
      return true;
    }
  }
  return false;
}

const Interval* occupied(Device* owner, const Interval& candidate) {
  for (const Interval& entry : g_Intervals) {
    if (!overlap(entry, candidate)) {
      continue;
    }
    if (bridgeWindow(entry) && ancestor(entry.route, owner)) {
      continue;
    }
    return &entry;
  }
  return nullptr;
}

bool alignUp(uint64_t value, uint64_t alignment, uint64_t& aligned) {
  if (!alignment || (alignment & (alignment - 1)) || value > ~uint64_t{0} - (alignment - 1)) {
    return false;
  }
  aligned = (value + alignment - 1) & ~(alignment - 1);
  return true;
}
}  // namespace

namespace PciResources {
bool initialize(Device* root) {
  LockGuard<Mutex> guard(g_Lock);
  if (g_Ready) {
    return true;
  }
  Vector<Staged> staged;
  Vector<Function> functions;
  bool valid = !root || collect(root, staged, functions);
  auto visit = [&](Device* device) -> Device* {
    if (valid) {
      valid = collect(device, staged, functions);
    }
    return device;
  };
  auto callback = pedigree_std::make_callable(visit);
  Device::foreach (callback, root);
  if (!valid || !functions.count()) {
    return false;
  }

  for (size_t i = 0; i < staged.count(); ++i) {
    const Staged& a = staged[i];
    for (size_t j = 0; j < i; ++j) {
      const Staged& b = staged[j];
      if (!overlap(a.interval, b.interval)) {
        continue;
      }
      if ((bridgeWindow(a.interval) && ancestor(a.interval.route, b.owner)) ||
          (bridgeWindow(b.interval) && ancestor(b.interval.route, a.owner))) {
        continue;
      }
      return false;
    }
  }

  Vector<Interval> intervals;
  for (const Staged& entry : staged) {
    if (!append(intervals, entry.interval)) {
      return false;
    }
  }
  g_Intervals.swap(intervals);
  g_Functions.swap(functions);
  g_Ready = true;
  return true;
}

bool addFunction(Device* device) {
  LockGuard<Mutex> guard(g_Lock);
  if (!g_Ready || !stoppedLeaf(device)) {
    return false;
  }
  const auto* root = PciFirmware::rootForBus(device->getPciBusPosition());
  if (!root || root->segment != 0 || root->windowCount > PciFirmware::MaxWindows) {
    return false;
  }
  const uint32_t route = routeId(device);
  for (const Function& function : g_Functions) {
    if (function.root == root && function.route == route) {
      return false;
    }
  }

  const auto config = device->getPciConfigHeader();
  auto& pci = PciBus::instance();
  for (size_t bar = 0; bar < 6; ++bar) {
    uint32_t low = 0;
    if (!pci.readConfig32(device, 0x10 + bar * 4, low) || low != config.bar[bar]) {
      return false;
    }
    if (low & 1U) {
      if (low & ~uint32_t{3}) {
        return false;
      }
      continue;
    }
    const unsigned type = (low >> 1) & 3U;
    if (type == 3 || (low & ~uint32_t{15})) {
      return false;
    }
    if (type == 2) {
      uint32_t high = 0;
      if (++bar == 6 || !pci.readConfig32(device, 0x10 + bar * 4, high) || high ||
          high != config.bar[bar]) {
        return false;
      }
    }
  }
  return append(g_Functions, {root, route});
}

bool removeFunction(Device* device) {
  LockGuard<Mutex> guard(g_Lock);
  const PciFirmware::Root* root = nullptr;
  if (!ownerRoot(device, root) || !stoppedLeaf(device)) {
    return false;
  }
  const uint32_t route = routeId(device);
  for (const Interval& entry : g_Intervals) {
    if (entry.root == root && entry.route == route && bridgeWindow(entry)) {
      return false;
    }
  }
  for (size_t i = g_Intervals.count(); i; --i) {
    const Interval& entry = g_Intervals[i - 1];
    if (entry.root == root && entry.route == route) {
      g_Intervals.erase(i - 1);
    }
  }
  for (size_t i = 0; i < g_Functions.count(); ++i) {
    if (g_Functions[i].root == root && g_Functions[i].route == route) {
      g_Functions.erase(i);
      return true;
    }
  }
  return false;
}

bool reserve(Device* owner, uint64_t base, uint64_t size, bool prefetchable) {
  LockGuard<Mutex> guard(g_Lock);
  const PciFirmware::Root* root = nullptr;
  uint64_t last = 0;
  if (!ownerRoot(owner, root) || !base || !lastAddress(base, size, last)) {
    return false;
  }
  bool forwarded = false;
  for (size_t i = 0; i < root->windowCount; ++i) {
    const auto& window = root->windows[i];
    uint64_t windowLast = 0;
    if (!usableWindow(window, prefetchable) ||
        !lastAddress(window.pciBase, window.size, windowLast) || base < window.pciBase ||
        last > windowLast) {
      continue;
    }
    uint64_t allowedBase = base;
    uint64_t allowedLast = last;
    if (scoped(owner, root, prefetchable, allowedBase, allowedLast) && allowedBase == base &&
        allowedLast == last) {
      forwarded = true;
      break;
    }
  }
  if (!forwarded) {
    return false;
  }
  Interval candidate{root, routeId(owner), base, last, Kind::Assigned, 0, prefetchable};
  return !occupied(owner, candidate) && append(g_Intervals, candidate);
}

bool allocate(Device* owner, uint64_t size, uint64_t alignment, bool prefetchable,
              uint64_t maxAddress, uint64_t& outBase) {
  LockGuard<Mutex> guard(g_Lock);
  const PciFirmware::Root* root = nullptr;
  if (!ownerRoot(owner, root) || !size || !alignment || (alignment & (alignment - 1)) ||
      size - 1 > maxAddress) {
    return false;
  }
  for (size_t i = 0; i < root->windowCount; ++i) {
    const auto& window = root->windows[i];
    uint64_t windowLast = 0;
    if (!usableWindow(window, prefetchable) ||
        !lastAddress(window.pciBase, window.size, windowLast)) {
      continue;
    }
    uint64_t low = window.pciBase > 0 ? window.pciBase : 1;
    uint64_t high = windowLast < maxAddress ? windowLast : maxAddress;
    if (low > high || !scoped(owner, root, prefetchable, low, high)) {
      continue;
    }
    uint64_t base = 0;
    if (!alignUp(low, alignment, base)) {
      continue;
    }
    while (base <= high && size - 1 <= high - base) {
      Interval candidate{root, routeId(owner), base, base + size - 1, Kind::Assigned,
                         0,    prefetchable};
      const Interval* conflict = occupied(owner, candidate);
      if (!conflict) {
        if (!append(g_Intervals, candidate)) {
          return false;
        }
        outBase = base;
        return true;
      }
      if (conflict->last == ~uint64_t{0} || !alignUp(conflict->last + 1, alignment, base)) {
        break;
      }
    }
  }
  return false;
}

bool release(Device* owner, uint64_t base, uint64_t size) {
  LockGuard<Mutex> guard(g_Lock);
  const PciFirmware::Root* root = nullptr;
  uint64_t last = 0;
  if (!ownerRoot(owner, root) || !lastAddress(base, size, last)) {
    return false;
  }
  for (size_t i = 0; i < g_Intervals.count(); ++i) {
    const Interval& entry = g_Intervals[i];
    if (entry.root == root && entry.route == routeId(owner) && entry.kind == Kind::Assigned &&
        entry.base == base && entry.last == last) {
      g_Intervals.erase(i);
      return true;
    }
  }
  return false;
}
}  // namespace PciResources
