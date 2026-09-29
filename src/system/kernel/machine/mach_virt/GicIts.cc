/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "GicIts.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"

#include <string.h>

namespace {
#if ARM64
constexpr uintptr_t DirectMapBase = 0xffff000000000000ULL;
constexpr uintptr_t ItsControl = 0x0000;
constexpr uintptr_t ItsType = 0x0008;
constexpr uintptr_t ItsCommandBase = 0x0080;
constexpr uintptr_t ItsCommandWrite = 0x0088;
constexpr uintptr_t ItsCommandRead = 0x0090;
constexpr uintptr_t ItsBaser = 0x0100;
constexpr uintptr_t RedistControl = 0x0000;
constexpr uintptr_t RedistType = 0x0008;
constexpr uintptr_t RedistPropertyBase = 0x0070;
constexpr uintptr_t RedistPendingBase = 0x0078;
constexpr uint64_t TableValid = 1ULL << 63;
constexpr uint64_t InnerShareable = 1ULL << 10;
constexpr uint64_t CacheableBaser = 7ULL << 59;
constexpr uint64_t CacheableRedist = 7ULL << 7;
constexpr size_t PageSize = 4096;
constexpr size_t LpiTableAlignment = 65536;

uint32_t read32(uintptr_t base, uintptr_t offset) {
  uint32_t value;
  asm volatile("ldr %w0, [%1]" : "=r"(value) : "r"(base + offset) : "memory");
  return value;
}

uint64_t read64(uintptr_t base, uintptr_t offset) {
  uint64_t value;
  asm volatile("ldr %0, [%1]" : "=r"(value) : "r"(base + offset) : "memory");
  return value;
}

void write32(uintptr_t base, uintptr_t offset, uint32_t value) {
  asm volatile("str %w1, [%0]" : : "r"(base + offset), "r"(value) : "memory");
}

void write64(uintptr_t base, uintptr_t offset, uint64_t value) {
  asm volatile("str %1, [%0]" : : "r"(base + offset), "r"(value) : "memory");
}

void barrier() {
  asm volatile("dsb sy\n\tisb" : : : "memory");
}
#endif
}  // namespace

GicIts::GicIts()
    : m_DeviceTable("ITS device table"),
      m_CollectionTable("ITS collection table"),
      m_CommandQueue("ITS command queue"),
      m_Properties("GIC LPI properties"),
      m_Pending("GIC LPI pending"),
      m_MappingLock(false),
      m_CommandLock(false),
      m_Mappings(),
      m_Base(0),
      m_Redistributor(0),
      m_PhysicalBase(0),
      m_Target(0),
      m_CommandVirtual(nullptr),
      m_PropertyVirtual(nullptr),
      m_CommandWrite(0),
      m_CommandSize(0),
      m_DeviceLimit(0),
      m_EventBits(0),
      m_Ready(false) {}

bool GicIts::ready() const {
  return m_Ready;
}

uint64_t GicIts::messageAddress() const {
  return m_Ready ? m_PhysicalBase + 0x10040 : 0;
}

bool GicIts::allocateAligned(MemoryRegion& region, size_t size, size_t alignment,
                             uint64_t& physical, uint8_t*& virtualAddress) {
#if ARM64
  if (!size || !alignment || (alignment & (alignment - 1)) ||
      size > SIZE_MAX - alignment + PageSize) {
    return false;
  }
  const size_t pages = (size + alignment - PageSize + PageSize - 1) / PageSize;
  if (!PhysicalMemoryManager::instance().allocateRegion(
          region, pages, PhysicalMemoryManager::continuous,
          VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write)) {
    return false;
  }
  const uint64_t base = region.physicalAddress();
  const size_t offset = (alignment - (base & (alignment - 1))) & (alignment - 1);
  if (offset > region.size() || size > region.size() - offset) {
    region.free();
    return false;
  }
  physical = base + offset;
  virtualAddress = static_cast<uint8_t*>(region.virtualAddress()) + offset;
  memset(virtualAddress, 0, size);
  barrier();
  return true;
#else
  (void)region;
  (void)size;
  (void)alignment;
  (void)physical;
  (void)virtualAddress;
  return false;
#endif
}

bool GicIts::setupBaser(uint32_t type, MemoryRegion& region, size_t requiredEntries) {
#if ARM64
  for (size_t index = 0; index < 8; ++index) {
    const uintptr_t offset = ItsBaser + index * 8;
    const uint64_t original = read64(m_Base, offset);
    if (((original >> 56) & 7) != type) {
      continue;
    }
    const size_t entrySize = ((original >> 48) & 31) + 1;
    if (requiredEntries > SIZE_MAX / entrySize) {
      return false;
    }
    const size_t bytes = requiredEntries * entrySize;
    const size_t pages = (bytes + PageSize - 1) / PageSize;
    if (!pages || pages > 256) {
      return false;
    }
    uint64_t physical = 0;
    uint8_t* virtualAddress = nullptr;
    if (!allocateAligned(region, pages * PageSize, PageSize, physical, virtualAddress)) {
      return false;
    }
    const uint64_t value = (original & ((7ULL << 56) | (31ULL << 48))) | physical | CacheableBaser |
                           InnerShareable | TableValid | (pages - 1);
    write64(m_Base, offset, value);
    barrier();
    const uint64_t actual = read64(m_Base, offset);
    const uint64_t mask =
        TableValid | (0x000ffffffffff000ULL) | (3ULL << 10) | (3ULL << 8) | (7ULL << 59) | 0xffULL;
    return (actual & mask) == (value & mask);
  }
#else
  (void)type;
  (void)region;
  (void)requiredEntries;
#endif
  return false;
}

bool GicIts::send(uint64_t word0, uint64_t word1, uint64_t word2) {
#if ARM64
  LockGuard<Spinlock> guard(m_CommandLock);
  if (!m_CommandVirtual || !m_CommandSize) {
    return false;
  }
  auto* command = reinterpret_cast<volatile uint64_t*>(m_CommandVirtual + m_CommandWrite);
  command[0] = word0;
  command[1] = word1;
  command[2] = word2;
  command[3] = 0;
  barrier();
  const size_t next = (m_CommandWrite + 32) % m_CommandSize;
  write64(m_Base, ItsCommandWrite, next);
  for (size_t wait = 0; wait < 100000; ++wait) {
    const uint64_t reader = read64(m_Base, ItsCommandRead);
    if (reader & 1U) {
      return false;
    }
    if ((reader & (m_CommandSize - 1)) == next) {
      m_CommandWrite = next;
      return true;
    }
    asm volatile("yield");
  }
#else
  (void)word0;
  (void)word1;
  (void)word2;
#endif
  return false;
}

bool GicIts::sync() {
  return send(0x05, 0, m_Target);
}

void GicIts::releaseTables() {
#if ARM64
  bool quiescent = true;
  if (m_Base) {
    write32(m_Base, ItsControl, read32(m_Base, ItsControl) & ~1U);
    size_t wait = 0;
    while (wait < 100000 && !(read32(m_Base, ItsControl) & (1U << 31))) {
      ++wait;
      asm volatile("yield");
    }
    quiescent = wait < 100000;
  }
  if (m_Redistributor) {
    write32(m_Redistributor, RedistControl, read32(m_Redistributor, RedistControl) & ~1U);
    size_t wait = 0;
    while (wait < 100000 && (read32(m_Redistributor, RedistControl) & (1U << 3))) {
      ++wait;
      asm volatile("yield");
    }
    quiescent = quiescent && wait < 100000;
  }
  if (!quiescent) {
    m_Ready = false;
    return;
  }
#endif
  if (m_DeviceTable) {
    m_DeviceTable.free();
  }
  if (m_CollectionTable) {
    m_CollectionTable.free();
  }
  if (m_CommandQueue) {
    m_CommandQueue.free();
  }
  if (m_Properties) {
    m_Properties.free();
  }
  if (m_Pending) {
    m_Pending.free();
  }
  m_Ready = false;
}

bool GicIts::initialise(const VirtMsiController& controller, uintptr_t redistributor,
                        uint64_t redistributorPhysical) {
#if ARM64
  if (m_Ready) {
    return true;
  }
  if (controller.type != VirtMsiController::Type::GicV3Its || controller.size < 0x20000 ||
      !redistributor || !redistributorPhysical || controller.base >= (64ULL << 30) ||
      controller.base > UINT64_MAX - 0x10040) {
    return false;
  }
  m_Base = DirectMapBase + controller.base;
  m_PhysicalBase = controller.base;
  m_Redistributor = redistributor;
  if ((read32(m_Base, ItsControl) & 1U) || (read32(redistributor, RedistControl) & 1U) ||
      !(read64(m_Base, ItsType) & 1U) || !(read64(redistributor, RedistType) & 1U)) {
    return false;
  }
  const uint64_t typer = read64(m_Base, ItsType);
  const uint32_t deviceBits = ((typer >> 13) & 31) + 1;
  const uint32_t eventBits = ((typer >> 8) & 31) + 1;
  const uintptr_t distributor = VirtDeviceTree::gicDistributorBase();
  if (!distributor) {
    return false;
  }
  const uint32_t lpiBits = ((read32(distributor, 0x4) >> 19) & 31) + 1;
  if (lpiBits < 14) {
    return false;
  }
  m_DeviceLimit = deviceBits < 16 ? 1U << deviceBits : 65536;
  m_EventBits = eventBits;
  const uint32_t usableLpiBits = lpiBits < 16 ? lpiBits : 16;

  uint64_t propertyPhysical = 0, pendingPhysical = 0, commandPhysical = 0;
  uint8_t* pendingVirtual = nullptr;
  if (!allocateAligned(m_Properties, LpiTableAlignment, LpiTableAlignment, propertyPhysical,
                       m_PropertyVirtual) ||
      !allocateAligned(m_Pending, LpiTableAlignment, LpiTableAlignment, pendingPhysical,
                       pendingVirtual)) {
    releaseTables();
    return false;
  }
  memset(m_PropertyVirtual, 0xa2, LpiTableAlignment);
  barrier();

  const uint64_t prop = propertyPhysical | CacheableRedist | InnerShareable | (usableLpiBits - 1);
  const uint64_t pend = pendingPhysical | CacheableRedist | InnerShareable;
  write64(redistributor, RedistPropertyBase, prop);
  write64(redistributor, RedistPendingBase, pend);
  barrier();
  const uint64_t propMask = 0x000ffffffffff000ULL | (7ULL << 7) | (3ULL << 10) | 31ULL;
  const uint64_t pendMask = 0x000fffffffff0000ULL | (7ULL << 7) | (3ULL << 10);
  if ((read64(redistributor, RedistPropertyBase) & propMask) != (prop & propMask) ||
      (read64(redistributor, RedistPendingBase) & pendMask) != (pend & pendMask) ||
      !setupBaser(1, m_DeviceTable, m_DeviceLimit) || !setupBaser(4, m_CollectionTable, 1) ||
      !allocateAligned(m_CommandQueue, PageSize, PageSize, commandPhysical, m_CommandVirtual)) {
    releaseTables();
    return false;
  }

  m_CommandSize = PageSize;
  const uint64_t cbaser = commandPhysical | CacheableBaser | InnerShareable | TableValid;
  write64(m_Base, ItsCommandBase, cbaser);
  barrier();
  const uint64_t cbaserMask =
      0x000ffffffffff000ULL | (7ULL << 59) | (3ULL << 10) | TableValid | 0xffULL;
  if ((read64(m_Base, ItsCommandBase) & cbaserMask) != (cbaser & cbaserMask)) {
    releaseTables();
    return false;
  }
  write64(m_Base, ItsCommandWrite, 0);
  m_CommandWrite = 0;
  m_Target = (typer & (1ULL << 19)) ? redistributorPhysical
                                    : ((read64(redistributor, RedistType) >> 8) & 0xffff) << 16;
  if (m_Target & 0xffff) {
    releaseTables();
    return false;
  }
  write32(redistributor, RedistControl, read32(redistributor, RedistControl) | 1U);
  write32(m_Base, ItsControl, read32(m_Base, ItsControl) | 1U);
  barrier();
  if (!(read32(redistributor, RedistControl) & 1U) || !(read32(m_Base, ItsControl) & 1U) ||
      !send(0x09, 0, m_Target | TableValid) || !send(0x0d, 0, 0) || !sync()) {
    releaseTables();
    return false;
  }
  m_Ready = true;
  return true;
#else
  (void)controller;
  (void)redistributor;
  (void)redistributorPhysical;
  return false;
#endif
}

bool GicIts::mapDevice(size_t slot, uint32_t deviceId, uint32_t lpi) {
  return lpi == FirstLpi + slot && mapDeviceGroup(&slot, 1, deviceId);
}

bool GicIts::mapDeviceGroup(const size_t* slots, size_t count, uint32_t deviceId) {
#if ARM64
  LockGuard<Spinlock> guard(m_MappingLock);
  if (!m_Ready || !slots || !count || count > MaxMappings || deviceId >= m_DeviceLimit) {
    return false;
  }
  uint32_t size = 0;
  while ((size_t(2) << size) < count) {
    ++size;
  }
  if (size >= m_EventBits) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (slots[i] >= MaxMappings || m_Mappings[slots[i]].valid) {
      return false;
    }
    for (size_t j = 0; j < i; ++j) {
      if (slots[j] == slots[i]) {
        return false;
      }
    }
  }
  const uint64_t itt = PhysicalMemoryManager::instance().tryAllocatePage();
  if (!itt) {
    return false;
  }
  memset(reinterpret_cast<void*>(DirectMapBase + itt), 0, PageSize);
  barrier();
  bool mapped = send(0x08 | (uint64_t(deviceId) << 32), size, itt | TableValid);
  for (size_t i = 0; mapped && i < count; ++i) {
    mapped = send(0x0a | (uint64_t(deviceId) << 32), (uint64_t(FirstLpi + slots[i]) << 32) | i, 0);
  }
  mapped = mapped && sync();
  if (!mapped) {
    if (send(0x08 | (uint64_t(deviceId) << 32), 0, 0) && sync()) {
      PhysicalMemoryManager::instance().freePage(itt);
    } else {
      for (size_t i = 0; i < count; ++i) {
        m_Mappings[slots[i]] = {deviceId, static_cast<uint32_t>(i), itt, true};
      }
    }
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    m_Mappings[slots[i]] = {deviceId, static_cast<uint32_t>(i), itt, true};
  }
  return true;
#else
  (void)slots;
  (void)count;
  (void)deviceId;
  return false;
#endif
}

bool GicIts::mappingActive(size_t slot) const {
  LockGuard<Spinlock> guard(m_MappingLock);
  return slot < MaxMappings && m_Mappings[slot].valid;
}

bool GicIts::unmapDevice(size_t slot) {
#if ARM64
  LockGuard<Spinlock> guard(m_MappingLock);
  if (!m_Ready || slot >= MaxMappings || !m_Mappings[slot].valid) {
    return false;
  }
  const Mapping mapping = m_Mappings[slot];
  bool last = true;
  for (size_t i = 0; i < MaxMappings; ++i) {
    if (i != slot && m_Mappings[i].valid && m_Mappings[i].deviceId == mapping.deviceId) {
      last = false;
      break;
    }
  }
  const bool removed = last ? send(0x08 | (uint64_t(mapping.deviceId) << 32), 0, 0)
                            : send(0x0f | (uint64_t(mapping.deviceId) << 32), mapping.eventId, 0);
  if (!removed || !sync()) {
    return false;
  }
  if (last) {
    PhysicalMemoryManager::instance().freePage(mapping.itt);
  }
  m_Mappings[slot] = {};
  return true;
#else
  (void)slot;
  return false;
#endif
}

bool GicIts::setEnabled(uint32_t lpi, bool enabled) {
#if ARM64
  LockGuard<Spinlock> guard(m_MappingLock);
  if (!m_Ready || lpi < FirstLpi || lpi >= FirstLpi + MaxMappings ||
      !m_Mappings[lpi - FirstLpi].valid) {
    return false;
  }
  const Mapping& mapping = m_Mappings[lpi - FirstLpi];
  m_PropertyVirtual[lpi - FirstLpi] = 0xa2 | (enabled ? 1U : 0U);
  barrier();
  return send(0x0c | (uint64_t(mapping.deviceId) << 32), mapping.eventId, 0) && sync();
#else
  (void)lpi;
  (void)enabled;
#endif
  return false;
}
