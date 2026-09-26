/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/utility.h"

#include <config.h>

#include "../../core/processor/x64/utils.h"
#include "Acpi.h"
#include "IntelIommu.h"

namespace {
constexpr uint32_t TranslationEnable = 1U << 31;
constexpr uint32_t SetRootPointer = 1U << 30;
constexpr uint32_t QueuedInvalidationEnabled = 1U << 26;
constexpr uint64_t PageBytes = 4096;
constexpr uint64_t LargePageBytes = 2 * 1024 * 1024;
constexpr size_t PollLimit = 1000000;

bool includesRequester(const AcpiDmar::Info& info, Device* device) {
  if (info.hardwareUnitCount != 1 || info.segmentZeroUnitCount != 1) {
    return false;
  }
  const uint8_t bus = device->getPciBusPosition();
  const uint16_t requester =
      (uint16_t(bus) << 8) | (device->getPciDevicePosition() << 3) | device->getPciFunctionNumber();
  if (info.segmentZeroIncludeAllCount ||
      info.includesDirectEndpoint(bus, device->getPciDevicePosition(),
                                  device->getPciFunctionNumber())) {
    return true;
  }

  auto& pci = PciBus::instance();
  for (size_t index = 0; index < info.directBridgeCount; ++index) {
    const uint16_t source = info.directBridges[index];
    Device bridge;
    bridge.setPciPosition(source >> 8, (source >> 3) & 31, source & 7);
    uint16_t vendor = 0;
    uint8_t header = 0;
    uint32_t classCode = 0, buses = 0;
    if (!pci.readConfig16(&bridge, 0, vendor) || !vendor || vendor == 0xffff ||
        !pci.readConfig8(&bridge, 0x0e, header) || (header & 0x7fU) != 1 ||
        !pci.readConfig32(&bridge, 8, classCode) || (classCode >> 16) != 0x0604 ||
        !pci.readConfig32(&bridge, 0x18, buses)) {
      continue;
    }
    const uint8_t primary = buses;
    const uint8_t secondary = buses >> 8;
    const uint8_t subordinate = buses >> 16;
    if (primary != source >> 8 || secondary <= primary || subordinate < secondary) {
      continue;
    }
    // A sub-hierarchy scope includes its bridge and every downstream bus.
    if (requester == source || (bus >= secondary && bus <= subordinate)) {
      return true;
    }
  }
  return false;
}
}  // namespace

IntelIommu& IntelIommu::instance() {
  static IntelIommu iommu;
  return iommu;
}

IntelIommu::IntelIommu()
    : m_Registers("Intel VT-d registers"), m_ReservedTokens("Intel VT-d IOVA reserve") {}

void IntelIommu::flushLines(const void* address, size_t bytes) const {
  if (m_Coherent) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return;
  }
  const uintptr_t start = reinterpret_cast<uintptr_t>(address) & ~(m_CacheLine - 1);
  const uintptr_t end =
      (reinterpret_cast<uintptr_t>(address) + bytes + m_CacheLine - 1) & ~(m_CacheLine - 1);
  for (uintptr_t line = start; line < end; line += m_CacheLine) {
    asm volatile("clflush (%0)" : : "r"(line) : "memory");
  }
  asm volatile("mfence" : : : "memory");
}

bool IntelIommu::allocateTable(physical_uintptr_t& page) {
  page = PhysicalMemoryManager::instance().allocatePage(PhysicalMemoryManager::below4GB);
  if (!page) {
    return false;
  }
  if ((page & (PageBytes - 1)) || page > m_MaxPhysical - (PageBytes - 1)) {
    PhysicalMemoryManager::instance().freePage(page);
    page = 0;
    return false;
  }
  ByteSet(reinterpret_cast<void*>(physicalAddress(page)), 0, PageBytes);
  return true;
}

bool IntelIommu::waitForStatus(uint32_t bit, bool set) {
  for (size_t attempt = 0; attempt < PollLimit; ++attempt) {
    if (bool(m_Registers.read32(0x1c) & bit) == set) {
      return true;
    }
    asm volatile("pause");
  }
  return false;
}

bool IntelIommu::command(uint32_t bit, bool set) {
  const uint32_t status = m_Registers.read32(0x1c);
  constexpr uint32_t persistent =
      TranslationEnable | QueuedInvalidationEnabled | (1U << 25) | (1U << 23);
  const uint32_t next = (status & persistent & ~bit) | (set ? bit : 0);
  m_Registers.write32(next, 0x18);
  return waitForStatus(bit, set);
}

bool IntelIommu::invalidateContext() {
  m_Registers.write64((1ULL << 63) | (1ULL << 61), 0x28);
  for (size_t attempt = 0; attempt < PollLimit; ++attempt) {
    const uint64_t result = m_Registers.read64(0x28);
    if (!(result & (1ULL << 63))) {
      return ((result >> 59) & 3) != 0;
    }
    asm volatile("pause");
  }
  return false;
}

bool IntelIommu::invalidateIotlb() {
  m_Registers.write64((1ULL << 63) | (1ULL << 60), m_IotlbOffset + 8);
  for (size_t attempt = 0; attempt < PollLimit; ++attempt) {
    const uint64_t result = m_Registers.read64(m_IotlbOffset + 8);
    if (!(result & (1ULL << 63))) {
      return ((result >> 57) & 3) != 0;
    }
    asm volatile("pause");
  }
  return false;
}

bool IntelIommu::initialise() {
  const AcpiDmar::Info* info = Acpi::instance().dmarInfo();
  if (!info || info->hardwareUnitCount != 1 || info->segmentZeroUnitCount != 1 ||
      (!info->segmentZeroIncludeAllCount && !info->directEndpointCount &&
       !info->directBridgeCount) ||
      info->reservedMemoryRegions || info->firstSegmentZeroUnitRegisterPagesLog2 > 4) {
    if (info) {
      NOTICE("Intel VT-d: unsupported DMAR layout, units="
             << Dec << info->hardwareUnitCount << " include-all="
             << info->segmentZeroIncludeAllCount << " RMRR=" << info->reservedMemoryRegions);
    } else {
      NOTICE("Intel VT-d: ACPI DMAR information unavailable");
    }
    return false;
  }

  const size_t registerPages = size_t(1) << info->firstSegmentZeroUnitRegisterPagesLog2;
  if (!PhysicalMemoryManager::instance().allocateRegion(
          m_Registers, registerPages,
          PhysicalMemoryManager::continuous | PhysicalMemoryManager::nonRamMemory |
              PhysicalMemoryManager::force,
          VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write |
              VirtualAddressSpace::CacheDisable,
          info->firstSegmentZeroUnitAddress)) {
    return false;
  }
  auto abort = [&]() {
    if (m_DefaultContext) {
      PhysicalMemoryManager::instance().freePage(m_DefaultContext);
      m_DefaultContext = 0;
    }
    if (m_Root) {
      PhysicalMemoryManager::instance().freePage(m_Root);
      m_Root = 0;
    }
    if (m_ReservedTokens) {
      m_ReservedTokens.free();
    }
    m_Registers.free();
    return false;
  };

  const uint32_t version = m_Registers.read32(0);
  const uint64_t cap = m_Registers.read64(8);
  const uint64_t ecap = m_Registers.read64(0x10);
  const uint32_t status = m_Registers.read32(0x1c);
  const uint8_t mgaw = static_cast<uint8_t>(((cap >> 16) & 0x3f) + 1);
  const uint8_t domainCountEncoding = static_cast<uint8_t>(cap & 7);
  const bool aw39 = cap & (1ULL << 9);
  const bool aw48 = cap & (1ULL << 10);
  m_IotlbOffset = size_t((ecap >> 8) & 0x3ff) * 16;
  if (((version >> 4) & 0xf) == 0 || ((version >> 4) & 0xf) >= 6 || domainCountEncoding == 7 ||
      !(ecap & (1ULL << 6)) || !(cap & (1ULL << 34)) || mgaw < 32 || (!aw39 && !aw48) ||
      m_IotlbOffset + 16 > m_Registers.size() ||
      (status & (TranslationEnable | QueuedInvalidationEnabled))) {
    NOTICE("Intel VT-d: unsupported register capabilities, version="
           << Hex << version << " CAP=" << cap << " ECAP=" << ecap << " GSTS=" << status);
    return abort();
  }

  m_Aw = aw48 ? 2 : 1;
  // Pass-through contexts require the largest AGAW the unit reports.
  for (int aw = 4; aw >= 0; --aw) {
    if (cap & (1ULL << (8 + aw))) {
      m_PassThroughAw = aw;
      break;
    }
  }
  m_MaxPhysical =
      info->hostAddressWidth == 64 ? ~uint64_t(0) : (uint64_t(1) << info->hostAddressWidth) - 1;
  m_Coherent = ecap & 1;
  if (!m_Coherent) {
    uint32_t eax, ebx, ecx, edx;
    Processor::cpuid(1, 0, eax, ebx, ecx, edx);
    m_CacheLine = ((ebx >> 8) & 0xff) * 8;
    if (!(edx & (1U << 19)) || m_CacheLine < 32 || m_CacheLine > 256 ||
        (m_CacheLine & (m_CacheLine - 1))) {
      return abort();
    }
  }

  if (!PhysicalMemoryManager::instance().allocateRegion(
          m_ReservedTokens, TokenPages,
          PhysicalMemoryManager::continuous | PhysicalMemoryManager::below4GB,
          VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write)) {
    return abort();
  }
  const uint64_t tokenBase = m_ReservedTokens.physicalAddress();
  if (!tokenBase || tokenBase + TokenPages * PageBytes > (1ULL << 32)) {
    return abort();
  }

  if (!allocateTable(m_Root) || !allocateTable(m_DefaultContext)) {
    return abort();
  }
  auto* contexts = reinterpret_cast<uint64_t*>(physicalAddress(m_DefaultContext));
  for (size_t index = 0; index < 256; ++index) {
    contexts[index * 2] = (2ULL << 2) | 1;  // Pass-through for every requester.
    contexts[index * 2 + 1] = (1ULL << 8) | m_PassThroughAw;
  }
  flushLines(contexts, PageBytes);

  auto* roots = reinterpret_cast<uint64_t*>(physicalAddress(m_Root));
  for (size_t bus = 0; bus < 256; ++bus) {
    roots[bus * 2] = m_DefaultContext | 1;
  }
  flushLines(roots, PageBytes);

  m_Registers.write64(m_Root, 0x20);  // Legacy root-table mode.
  if (!command(SetRootPointer, true)) {
    return abort();
  }
  if (!invalidateContext() || !invalidateIotlb()) {
    return abort();
  }
  if (!command(TranslationEnable, true)) {
    panic("Intel VT-d: translation enable did not complete");
  }
  m_Enabled = true;
  const uint8_t tableWidth = m_Aw == 2 ? 48 : 39;
  const uint8_t iovaWidth = mgaw < tableWidth ? mgaw : tableWidth;
  NOTICE("Intel VT-d: DMA remapping enabled, " << Dec << static_cast<uint32_t>(iovaWidth)
                                               << "-bit IOVA, " << Hex << tokenBase
                                               << " token window");
  return true;
}

void IntelIommu::freeDomain(Domain& domain) {
  for (size_t i = 0; i < domain.pageCount; ++i) {
    PhysicalMemoryManager::instance().freePage(domain.pages[i]);
  }
  domain = Domain{};
}

bool IntelIommu::makeDomain(Domain& domain) {
  auto addPage = [&](physical_uintptr_t& page) {
    if (domain.pageCount == 8 || !allocateTable(page)) {
      return false;
    }
    domain.pages[domain.pageCount++] = page;
    return true;
  };

  if (!addPage(domain.root)) {
    return false;
  }
  physical_uintptr_t pdptPage = domain.root;
  if (m_Aw == 2) {
    if (!addPage(pdptPage)) {
      freeDomain(domain);
      return false;
    }
    auto* root = reinterpret_cast<uint64_t*>(physicalAddress(domain.root));
    root[0] = pdptPage | 3;
  }

  auto* pdpt = reinterpret_cast<uint64_t*>(physicalAddress(pdptPage));
  physical_uintptr_t directoryPages[4] = {};
  domain.tokenBase = domain.isolated ? IsolatedTokenBase : m_ReservedTokens.physicalAddress();
  domain.tokenCount = domain.isolated ? IsolatedTokenPages : TokenPages;
  const size_t firstGroup = domain.isolated ? domain.tokenBase / (512 * LargePageBytes) : 0;
  const size_t lastGroup = domain.isolated ? firstGroup + 1 : 4;
  for (size_t group = firstGroup; group < lastGroup; ++group) {
    if (!addPage(directoryPages[group])) {
      freeDomain(domain);
      return false;
    }
    pdpt[group] = directoryPages[group] | 3;
    if (!domain.isolated) {
      auto* directory = reinterpret_cast<uint64_t*>(physicalAddress(directoryPages[group]));
      for (size_t entry = 0; entry < 512; ++entry) {
        directory[entry] = (uint64_t(group * 512 + entry) * LargePageBytes) | (1U << 7) | 3;
      }
    }
  }

  const uint64_t firstBlock = domain.tokenBase / LargePageBytes;
  const uint64_t lastBlock =
      (domain.tokenBase + domain.tokenCount * PageBytes - 1) / LargePageBytes;
  for (uint64_t block = firstBlock; block <= lastBlock; ++block) {
    physical_uintptr_t tablePage = 0;
    if (!addPage(tablePage)) {
      freeDomain(domain);
      return false;
    }
    auto* table = reinterpret_cast<uint64_t*>(physicalAddress(tablePage));
    domain.tokenTables[block - firstBlock] = table;
    if (!domain.isolated) {
      for (size_t index = 0; index < 512; ++index) {
        table[index] = (block * LargePageBytes + index * PageBytes) | 3;
      }
    }
    for (size_t slot = 0; slot < domain.tokenCount; ++slot) {
      const uint64_t iova = domain.tokenBase + slot * PageBytes;
      if (iova / LargePageBytes == block) {
        table[(iova / PageBytes) & 511] = 0;
      }
    }
    auto* directory = reinterpret_cast<uint64_t*>(physicalAddress(directoryPages[block / 512]));
    directory[block & 511] = tablePage | 3;
  }

  for (size_t i = 0; i < domain.pageCount; ++i) {
    flushLines(reinterpret_cast<void*>(physicalAddress(domain.pages[i])), PageBytes);
  }
  return true;
}

uint64_t* IntelIommu::tokenEntry(const Domain& domain, size_t slot) {
  if (slot >= domain.tokenCount) {
    return nullptr;
  }
  const uint64_t iova = domain.tokenBase + slot * PageBytes;
  const size_t table = iova / LargePageBytes - domain.tokenBase / LargePageBytes;
  return table < 2 && domain.tokenTables[table]
             ? &domain.tokenTables[table][(iova / PageBytes) & 511]
             : nullptr;
}

IntelIommu::Domain* IntelIommu::findDomain(const Device* device) {
  for (size_t i = 0; i < MaxDomains; ++i) {
    if (m_Domains[i].device && m_Domains[i].device == device) {
      return &m_Domains[i];
    }
  }
  return nullptr;
}

const IntelIommu::Domain* IntelIommu::findDomain(const Device* device) const {
  for (size_t i = 0; i < MaxDomains; ++i) {
    if (m_Domains[i].device && m_Domains[i].device == device) {
      return &m_Domains[i];
    }
  }
  return nullptr;
}

bool IntelIommu::attach(Device* device, bool isolated) {
  if (!device || device->getPciBusPosition() > 255 || device->getPciDevicePosition() > 31 ||
      device->getPciFunctionNumber() > 7) {
    return false;
  }
  LockGuard<Mutex> guard(m_Lock);
  if (const Domain* domain = findDomain(device)) {
    return domain->isolated == isolated;
  }
  size_t slot = MaxDomains;
  for (size_t i = 0; i < MaxDomains; ++i) {
    Device* other = m_Domains[i].device;
    if (other && other->getPciBusPosition() == device->getPciBusPosition() &&
        other->getPciDevicePosition() == device->getPciDevicePosition() &&
        other->getPciFunctionNumber() == device->getPciFunctionNumber()) {
      return false;
    }
    if (!other && slot == MaxDomains) {
      slot = i;
    }
  }
  const AcpiDmar::Info* info = Acpi::instance().dmarInfo();
  if (!info || !includesRequester(*info, device)) {
    NOTICE("Intel VT-d: requester outside supported DMAR scope, PCI "
           << Dec << device->getPciBusPosition() << ":" << device->getPciDevicePosition() << "."
           << device->getPciFunctionNumber());
    return false;
  }
  uint16_t pciCommand = 0;
  const bool commandRead = PciBus::instance().readConfig16(device, 4, pciCommand);
  if (!commandRead || (pciCommand & 4)) {
    NOTICE("Intel VT-d: attach requires bus mastering disabled, PCI command=" << Hex << pciCommand);
    return false;
  }
  if (m_Failed || slot == MaxDomains) {
    return false;
  }
  if (!m_Enabled && !initialise()) {
    m_Failed = true;
    return false;
  }

  Domain domain;
  domain.device = device;
  domain.id = static_cast<uint16_t>(slot + 2);
  domain.isolated = isolated;
  if (!makeDomain(domain)) {
    return false;
  }

  const size_t bus = device->getPciBusPosition();
  const size_t function = (device->getPciDevicePosition() << 3) | device->getPciFunctionNumber();
  if (!m_BusContexts[bus]) {
    physical_uintptr_t busContext = 0;
    if (!allocateTable(busContext)) {
      freeDomain(domain);
      return false;
    }
    MemoryCopy(reinterpret_cast<void*>(physicalAddress(busContext)),
               reinterpret_cast<const void*>(physicalAddress(m_DefaultContext)), PageBytes);
    flushLines(reinterpret_cast<void*>(physicalAddress(busContext)), PageBytes);
    auto* roots = reinterpret_cast<uint64_t*>(physicalAddress(m_Root));
    roots[bus * 2] = busContext | 1;
    flushLines(&roots[bus * 2], sizeof(uint64_t));
    if (!invalidateContext() || !invalidateIotlb()) {
      panic("Intel VT-d: failed to publish bus context");
    }
    m_BusContexts[bus] = busContext;
  }

  auto* contexts = reinterpret_cast<uint64_t*>(physicalAddress(m_BusContexts[bus]));
  contexts[function * 2 + 1] = (uint64_t(domain.id) << 8) | m_Aw;
  contexts[function * 2] = domain.root | 1;
  flushLines(&contexts[function * 2], 2 * sizeof(uint64_t));
  if (!invalidateContext() || !invalidateIotlb()) {
    panic("Intel VT-d: failed to publish device context");
  }

  m_Domains[slot] = domain;
  NOTICE("Intel VT-d: attached PCI " << Dec << static_cast<uint32_t>(bus) << ":"
                                     << static_cast<uint32_t>(function >> 3) << "."
                                     << static_cast<uint32_t>(function & 7)
                                     << (isolated ? " to isolated domain " : " to domain ")
                                     << static_cast<uint32_t>(domain.id));
  return true;
}

bool IntelIommu::isolatedIdle(Device* device) const {
  LockGuard<Mutex> guard(m_Lock);
  const Domain* domain = findDomain(device);
  if (!m_Enabled || !domain || !domain->isolated) {
    return false;
  }
  uint16_t command = 0;
  if (!PciBus::instance().readConfig16(device, 4, command) || (command & 4)) {
    return false;
  }
  for (uint32_t used : domain->tokenUsed) {
    if (used) {
      return false;
    }
  }
  return true;
}

bool IntelIommu::detachIsolated(Device* device, bool requesterDisabled) {
  if (!device) {
    return false;
  }
  LockGuard<Mutex> guard(m_Lock);
  Domain* domain = findDomain(device);
  if (!m_Enabled || !domain || !domain->isolated) {
    return false;
  }
  uint16_t pciCommand = 0;
  if (!requesterDisabled &&
      (!PciBus::instance().readConfig16(device, 4, pciCommand) || (pciCommand & 4))) {
    return false;
  }
  for (uint32_t used : domain->tokenUsed) {
    if (used) {
      return false;
    }
  }

  const size_t bus = device->getPciBusPosition();
  const size_t function = (device->getPciDevicePosition() << 3) | device->getPciFunctionNumber();
  if (!m_BusContexts[bus]) {
    panic("Intel VT-d: isolated domain context missing");
  }
  auto* contexts = reinterpret_cast<uint64_t*>(physicalAddress(m_BusContexts[bus]));
  if (contexts[function * 2] != (domain->root | 1) ||
      contexts[function * 2 + 1] != ((uint64_t(domain->id) << 8) | m_Aw)) {
    panic("Intel VT-d: isolated domain context mismatch");
  }
  contexts[function * 2] = 0;
  flushLines(&contexts[function * 2], sizeof(uint64_t));
  if (!invalidateContext() || !invalidateIotlb()) {
    panic("Intel VT-d: failed to revoke isolated domain context");
  }
  contexts[function * 2 + 1] = 0;
  flushLines(&contexts[function * 2 + 1], sizeof(uint64_t));
  freeDomain(*domain);
  return true;
}

bool IntelIommu::attached(const Device* device) const {
  LockGuard<Mutex> guard(m_Lock);
  return m_Enabled && findDomain(device);
}

bool IntelIommu::isolated(const Device* device) const {
  LockGuard<Mutex> guard(m_Lock);
  const Domain* domain = findDomain(device);
  return m_Enabled && domain && domain->isolated;
}

bool IntelIommu::mapPage(Device* device, physical_uintptr_t physical, uint32_t& dmaAddress,
                         uint16_t& token) {
  if (!device || !physical || (physical & (PageBytes - 1))) {
    return false;
  }
  LockGuard<Mutex> guard(m_Lock);
  Domain* domain = findDomain(device);
  if (!m_Enabled || !domain || physical > m_MaxPhysical - (PageBytes - 1)) {
    return false;
  }

  if (!domain->isolated && physical < (1ULL << 32)) {
    if (physical >= domain->tokenBase &&
        physical < domain->tokenBase + domain->tokenCount * PageBytes) {
      return false;
    }
    dmaAddress = static_cast<uint32_t>(physical);
    token = 0;
    return true;
  }

  for (size_t slot = 0; slot < domain->tokenCount; ++slot) {
    const uint32_t mask = 1U << (slot & 31);
    if (domain->tokenUsed[slot / 32] & mask) {
      continue;
    }
    uint64_t* entry = tokenEntry(*domain, slot);
    if (!entry) {
      panic("Intel VT-d: IOVA page table missing");
    }
    *entry = physical | 3;
    flushLines(entry, sizeof(*entry));
    if (!invalidateIotlb()) {
      panic("Intel VT-d: failed to publish IOVA mapping");
    }
    domain->tokenUsed[slot / 32] |= mask;
    dmaAddress = static_cast<uint32_t>(domain->tokenBase + slot * PageBytes);
    token = static_cast<uint16_t>(slot + 1);
    if (physical >= (1ULL << 32) && !domain->loggedHighMapping) {
      NOTICE("Intel VT-d: high physical page " << Hex << physical << " mapped to IOVA "
                                               << dmaAddress << " in domain " << Dec << domain->id);
      domain->loggedHighMapping = true;
    }
    return true;
  }
  return false;
}

void IntelIommu::unmapPage(Device* device, uint16_t token) {
  if (!token) {
    return;
  }
  LockGuard<Mutex> guard(m_Lock);
  Domain* domain = findDomain(device);
  const size_t slot = token - 1;
  const uint32_t mask = 1U << (slot & 31);
  if (!m_Enabled || !domain || slot >= domain->tokenCount ||
      !(domain->tokenUsed[slot / 32] & mask)) {
    panic("Intel VT-d: invalid IOVA unmap");
  }
  uint64_t* entry = tokenEntry(*domain, slot);
  if (!entry) {
    panic("Intel VT-d: IOVA page table missing");
  }
  *entry = 0;
  flushLines(entry, sizeof(*entry));
  if (!invalidateIotlb()) {
    panic("Intel VT-d: failed to revoke IOVA mapping");
  }
  domain->tokenUsed[slot / 32] &= ~mask;
  logFault();
}

void IntelIommu::logFault() {
  const uint32_t status = m_Registers.read32(0x34);
  if (!(status & 3)) {
    return;
  }
  const uint64_t cap = m_Registers.read64(8);
  const size_t offset = size_t((cap >> 24) & 0x3ff) * 16;
  const size_t index = (status >> 8) & 0xff;
  if ((status & 2) && offset + (index + 1) * 16 <= m_Registers.size()) {
    const uint64_t low = m_Registers.read64(offset + index * 16);
    const uint64_t high = m_Registers.read64(offset + index * 16 + 8);
    WARNING("Intel VT-d fault: SID=" << Hex << (high & 0xffff) << " IOVA=" << (low & ~0xfffULL)
                                     << " reason=" << ((high >> 32) & 0xff));
    m_Registers.write64(1ULL << 63, offset + index * 16 + 8);
  }
  if (status & 1) {
    WARNING("Intel VT-d fault log overflow");
    m_Registers.write32(1, 0x34);
  }
}
