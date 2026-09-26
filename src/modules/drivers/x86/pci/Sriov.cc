/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/machine/PciFirmware.h"
#include "pedigree/kernel/machine/PciVirtualFunctions.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/new"

#include "Resources.h"

namespace {
Mutex g_Lock;

struct Config {
  Device* device;
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

void settle() {
  const Time::Timestamp start = Time::getTicks();
  constexpr Time::Timestamp delay = 100 * Time::Multiplier::Millisecond;
  for (;;) {
    const Time::Timestamp elapsed = Time::getTicks() - start;
    if (elapsed >= delay) {
      return;
    }
    (void)Time::delay(delay - elapsed);
  }
}

bool readHeader(Device* device, PciBus::ConfigSpace& header) {
  auto* bytes = reinterpret_cast<uint8_t*>(&header);
  for (size_t offset = 0; offset < sizeof(header); offset += 4) {
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

bool busReachable(Device* pf, uint8_t bus) {
  auto& pci = PciBus::instance();
  uint8_t first = 0, last = 0;
  const auto* root = PciFirmware::rootForBus(pf->getPciBusPosition());
  if (!pci.busRange(first, last) || bus < first || bus > last || !root ||
      PciFirmware::rootForBus(bus) != root) {
    return false;
  }
  for (Device* parent = pf->getParent(); parent; parent = parent->getParent()) {
    if (parent->getPciClassCode() != 6 || parent->getPciSubclassCode() != 4 ||
        (parent->getPciConfigHeader().header_type & 0x7f) != 1) {
      continue;
    }
    uint32_t buses = 0;
    if (!pci.readConfig32(parent, 0x18, buses) || uint8_t(buses) != parent->getPciBusPosition() ||
        bus < uint8_t(buses >> 8) || bus > uint8_t(buses >> 16)) {
      return false;
    }
  }
  return true;
}
}  // namespace

PciVirtualFunctions::PciVirtualFunctions(Device* pf) : m_Pf(pf) {}

PciVirtualFunctions::~PciVirtualFunctions() {
  if (!disable()) {
    panic("SR-IOV: VF group destroyed with active users");
  }
}

bool PciVirtualFunctions::enable(size_t requested) {
  LockGuard<Mutex> guard(g_Lock);
  auto& pci = PciBus::instance();
  if (!m_Pf || m_Count || m_PlannedCount || !requested || requested > MaxFunctions ||
      !m_Pf->getParent() || m_Pf->getParent()->getSpecificType() != "pci" ||
      m_Pf->getPhysicalFunction() || m_Pf->getPciBusPosition() > 255 ||
      m_Pf->getPciDevicePosition() > 31 || m_Pf->getPciFunctionNumber() > 7) {
    return false;
  }
  PciExtendedCapabilities::Capability capability;
  uint16_t command = 0;
  Config config{m_Pf};
  PciSriov::State state;
  PciSriov::Plan plan;
  if (!pci.readConfig16(m_Pf, 4, command) || (command & 4U) ||
      pci.findExtendedCapability(m_Pf, PciSriov::CapabilityId, capability) !=
          PciExtendedCapabilities::FindResult::Found ||
      !PciSriov::read(config, capability.offset, capability.next, state) ||
      !PciSriov::plan(m_Pf->getPciBusPosition(), m_Pf->getPciDevicePosition(),
                      m_Pf->getPciFunctionNumber(), state, requested, plan)) {
    return false;
  }
  m_Original = state;
  m_Offset = capability.offset;
  m_NextCapability = capability.next;
  state.numVfs = requested;
  for (size_t i = 0; i < requested; ++i) {
    uint8_t bus = 0, device = 0, function = 0;
    if (!PciSriov::vfRoutingId(plan.pfBus, plan.pfDevice, plan.pfFunction, state, i, bus, device,
                               function) ||
        !busReachable(m_Pf, bus)) {
      reclaim();
      return false;
    }
    Device* vf = new Device();
    if (!vf) {
      reclaim();
      return false;
    }
    m_Functions[m_PlannedCount++] = vf;
    vf->setPciPosition(bus, device, function);
    vf->setParent(m_Pf->getParent());
    vf->setVirtualFunction(m_Pf, i);
    uint32_t identity = 0;
    uint32_t header = 0;
    // An enabled VF also returns FFFF:FFFF for its IDs, but has a real header.
    if (!pci.readConfig32(vf, 0, identity) || !pci.readConfig32(vf, 0xc, header) ||
        identity != 0xffffffffU || header != 0xffffffffU) {
      reclaim();
      return false;
    }
  }

  // VF BAR sizes may depend on the selected system page size.
  m_Touched = true;
  PciSriov::Plan selected;
  if (!PciSriov::writeVerified32(config, m_Offset + 0x20, PciSriov::PageSize4K) ||
      !PciSriov::read(config, m_Offset, m_NextCapability, state) ||
      !PciSriov::plan(plan.pfBus, plan.pfDevice, plan.pfFunction, state, requested, selected) ||
      selected.firstRid != plan.firstRid || selected.lastRid != plan.lastRid ||
      selected.firstVfOffset != plan.firstVfOffset || selected.vfStride != plan.vfStride ||
      state.totalVfs != m_Original.totalVfs || state.vfDeviceId != m_Original.vfDeviceId ||
      PciSriov::probeBars(config, m_Offset, m_NextCapability, state, m_Bars) !=
          PciSriov::Result::Success) {
    reclaim();
    return false;
  }
  plan = selected;
  for (size_t i = 0; i < 6; ++i) {
    auto& bar = m_Bars.bars[i];
    if (!bar.perVfBytes) {
      continue;
    }
    if (bar.io) {
      reclaim();
      return false;
    }
    uint64_t base = bar.base;
    if (!(base && PciResources::reserve(m_Pf, base, bar.apertureBytes, bar.prefetchable)) &&
        !PciResources::allocate(m_Pf, bar.apertureBytes, bar.perVfBytes, bar.prefetchable,
                                bar.wide ? ~uint64_t{0} : 0xffffffffU, base)) {
      WARNING("SR-IOV: no firmware aperture for VF BAR " << Dec << i);
      reclaim();
      return false;
    }
    m_Reserved[i] = true;
    bar.base = base;
    const uint32_t low = uint32_t(base) | (m_Original.bars[i] & 15U);
    if (!PciSriov::writeVerified32(config, m_Offset + 0x24 + 4 * i, low) ||
        (bar.wide &&
         !PciSriov::writeVerified32(config, m_Offset + 0x28 + 4 * i, uint32_t(base >> 32)))) {
      reclaim();
      return false;
    }
  }
  m_EnableAttempted = true;
  if (PciSriov::enable(config, m_Offset, m_NextCapability, plan) != PciSriov::Result::Success) {
    reclaim();
    return false;
  }
  settle();

  for (size_t i = 0; i < requested; ++i) {
    Device* vf = m_Functions[i];
    PciBus::ConfigSpace header{};
    if (!readHeader(vf, header) || (header.header_type & 0x7f) ||
        !((header.vendor == 0xffff && header.device == 0xffff) ||
          (header.vendor == m_Pf->getPciVendorId() && header.device == m_Original.vfDeviceId))) {
      WARNING("SR-IOV: VF " << Dec << i << " configuration unavailable after enable");
      reclaim();
      return false;
    }
    m_Present[i] = true;
    uint16_t vfCommand = 0;
    if (!pci.updateCommand(vf, 4U, 0) || !pci.readConfig16(vf, 4, vfCommand) || (vfCommand & 4U)) {
      reclaim();
      return false;
    }
    // SR-IOV 3.4.1.1/3.4.1.2 place the VF identity in its PF, not offset zero.
    header.vendor = m_Pf->getPciVendorId();
    header.device = m_Original.vfDeviceId;
    header.command = vfCommand;
    vf->setPciIdentifiers(header.class_code, header.subclass, header.vendor, header.device,
                          header.progif);
    vf->setPciConfigHeader(header);
    vf->setSpecificType(String("PCI virtual function"));
    for (size_t bar = 0; bar < 6; ++bar) {
      const auto& aperture = m_Bars.bars[bar];
      if (!aperture.perVfBytes) {
        continue;
      }
      const uint64_t base = aperture.base + i * aperture.perVfBytes;
      uint64_t cpu = 0;
      if (aperture.perVfBytes > ~size_t{0} ||
          !pci.translateAddress(base, aperture.perVfBytes, false, cpu) || cpu > ~uintptr_t{0}) {
        reclaim();
        return false;
      }
      char name[] = {'b', 'a', 'r', static_cast<char>('0' + bar), 0};
      auto* address = new Device::Address(String(name), static_cast<uintptr_t>(cpu),
                                          static_cast<size_t>(aperture.perVfBytes), false);
      if (!address || !vf->addresses().tryReserve(vf->addresses().count() + 1)) {
        delete address;
        reclaim();
        return false;
      }
      vf->addresses().pushBack(address);
    }
    if (!pci.attachIsolatedDma(vf) || !pci.hasDmaIsolation(vf)) {
      WARNING("SR-IOV: cannot isolate VF " << Dec << vf->getPciBusPosition() << ":"
                                           << vf->getPciDevicePosition() << "."
                                           << vf->getPciFunctionNumber());
      reclaim();
      return false;
    }
  }
  m_Count = requested;
  return true;
}

Device* PciVirtualFunctions::function(size_t index) const {
  LockGuard<Mutex> guard(g_Lock);
  return index < m_Count ? m_Functions[index] : nullptr;
}

size_t PciVirtualFunctions::count() const {
  LockGuard<Mutex> guard(g_Lock);
  return m_Count;
}

bool PciVirtualFunctions::disable() {
  LockGuard<Mutex> guard(g_Lock);
  auto& pci = PciBus::instance();
  for (size_t i = 0; i < m_Count; ++i) {
    Device* vf = m_Functions[i];
    uint16_t command = 0;
    if (vf->getNumChildren() || !pci.readConfig16(vf, 4, command) || (command & 4U) ||
        (pci.hasDmaIsolation(vf) && !pci.isolatedDmaIdle(vf)) ||
        (pci.hasDmaRemapping(vf) && !pci.hasDmaIsolation(vf))) {
      return false;
    }
  }
  reclaim();
  return true;
}

void PciVirtualFunctions::reclaim() {
  auto& pci = PciBus::instance();
  Config config{m_Pf};
  if (m_Touched) {
    bool quiescent = true;
    for (size_t i = 0; i < m_PlannedCount; ++i) {
      if (m_Present[i] && !pci.updateCommand(m_Functions[i], 4U, 0)) {
        quiescent = false;
      }
    }
    uint16_t control = 0;
    if (!config.read16(m_Offset + 8, control) ||
        !PciSriov::writeVerified16(config, m_Offset + 8, control & ~PciSriov::VfControl) ||
        !PciSriov::writeVerified16(config, m_Offset + 0x10, 0)) {
      panic("SR-IOV: cannot disable VF decoding");
    }
    if (m_EnableAttempted) {
      settle();
    }
    if (!quiescent) {
      panic("SR-IOV: cannot verify VF DMA quiescence");
    }
  }
  for (size_t i = 0; i < m_PlannedCount; ++i) {
    Device* vf = m_Functions[i];
    if (vf->getNumChildren() || (pci.hasDmaIsolation(vf) && !pci.detachDisabledIsolatedDma(vf)) ||
        pci.hasDmaRemapping(vf)) {
      panic("SR-IOV: VF still owns DMA resources");
    }
    delete vf;
    m_Functions[i] = nullptr;
    m_Present[i] = false;
  }
  if (m_Touched) {
    for (size_t i = 0; i < 6; ++i) {
      if (!PciSriov::writeVerified32(config, m_Offset + 0x24 + 4 * i, m_Original.bars[i])) {
        panic("SR-IOV: cannot restore VF BARs");
      }
    }
    if (!PciSriov::writeVerified32(config, m_Offset + 0x20, m_Original.systemPageSize) ||
        !PciSriov::writeVerified16(config, m_Offset + 0x10, m_Original.numVfs) ||
        !PciSriov::writeVerified16(config, m_Offset + 8, m_Original.control)) {
      panic("SR-IOV: cannot restore PF state");
    }
  }
  for (size_t i = 0; i < 6; ++i) {
    if (m_Reserved[i] &&
        !PciResources::release(m_Pf, m_Bars.bars[i].base, m_Bars.bars[i].apertureBytes)) {
      panic("SR-IOV: lost VF BAR reservation");
    }
    m_Reserved[i] = false;
  }
  m_Count = 0;
  m_PlannedCount = 0;
  m_Touched = false;
  m_EnableAttempted = false;
  m_Offset = 0;
  m_NextCapability = 0;
  m_Original = {};
  m_Bars = {};
}

bool PciVirtualFunctions::bar(Device* vf, uint8_t index, uint64_t& pciBase, uint64_t& bytes) {
  LockGuard<Mutex> guard(g_Lock);
  auto& pci = PciBus::instance();
  Device* pf = vf ? vf->getPhysicalFunction() : nullptr;
  PciExtendedCapabilities::Capability capability;
  PciSriov::State state;
  Config config{pf};
  if (!pf || index >= 6 ||
      pci.findExtendedCapability(pf, PciSriov::CapabilityId, capability) !=
          PciExtendedCapabilities::FindResult::Found ||
      !PciSriov::read(config, capability.offset, capability.next, state) ||
      (state.control & PciSriov::VfControl) != PciSriov::VfControl ||
      state.systemPageSize != PciSriov::PageSize4K ||
      vf->getVirtualFunctionIndex() >= state.numVfs) {
    return false;
  }
  uint8_t bus = 0, device = 0, function = 0;
  if (!PciSriov::vfRoutingId(pf->getPciBusPosition(), pf->getPciDevicePosition(),
                             pf->getPciFunctionNumber(), state, vf->getVirtualFunctionIndex(), bus,
                             device, function) ||
      bus != vf->getPciBusPosition() || device != vf->getPciDevicePosition() ||
      function != vf->getPciFunctionNumber()) {
    return false;
  }
  for (unsigned i = 0; i < index; ++i) {
    if (!(state.bars[i] & 1U) && (state.bars[i] & 6U) == 4) {
      if (++i == index) {
        return false;
      }
    }
  }
  const uint32_t low = state.bars[index];
  const unsigned type = (low >> 1) & 3U;
  const bool wide = type == 2;
  if ((low & 1U) || (type != 0 && !wide) || (wide && index == 5)) {
    return false;
  }
  uint32_t raw = 0;
  if (!pci.readConfig32(vf, 0x10 + 4 * index, raw) || raw ||
      (wide && (!pci.readConfig32(vf, 0x14 + 4 * index, raw) || raw))) {
    return false;
  }
  char name[] = {'b', 'a', 'r', static_cast<char>('0' + index), 0};
  Device::Address* address = nullptr;
  for (Device::Address* candidate : vf->addresses()) {
    if (candidate && candidate->m_Name == name && !candidate->m_IsIoSpace) {
      if (address) {
        return false;
      }
      address = candidate;
    }
  }
  const uint64_t size = address ? address->m_Size : 0;
  const uint64_t base = (uint64_t(wide ? state.bars[index + 1] : 0) << 32) | (low & ~15U);
  const uint64_t maximum = wide ? ~uint64_t{0} : 0xffffffffU;
  if (!base || !size || (size & (size - 1)) || (base & (size - 1)) ||
      size > ~uint64_t{0} / state.totalVfs || size * state.totalVfs - 1 > maximum - base) {
    return false;
  }
  const uint64_t slice = base + vf->getVirtualFunctionIndex() * size;
  uint64_t cpu = 0;
  if (!pci.translateAddress(slice, size, false, cpu) || cpu != address->m_Address) {
    return false;
  }
  pciBase = slice;
  bytes = size;
  return true;
}
