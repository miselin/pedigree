/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Pci.h"
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
      (!info->segmentZeroIncludeAllCount && !info->directEndpointCount) ||
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
  const bool aw39 = cap & (1ULL << 9);
  const bool aw48 = cap & (1ULL << 10);
  m_IotlbOffset = size_t((ecap >> 8) & 0x3ff) * 16;
  if (((version >> 4) & 0xf) == 0 || ((version >> 4) & 0xf) >= 6 || !(ecap & (1ULL << 6)) ||
      !(cap & (1ULL << 34)) || mgaw < 32 || (!aw39 && !aw48) ||
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
    FATAL("Intel VT-d: translation enable did not complete");
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
  for (size_t group = 0; group < 4; ++group) {
    if (!addPage(directoryPages[group])) {
      freeDomain(domain);
      return false;
    }
    pdpt[group] = directoryPages[group] | 3;
    auto* directory = reinterpret_cast<uint64_t*>(physicalAddress(directoryPages[group]));
    for (size_t entry = 0; entry < 512; ++entry) {
      directory[entry] = (uint64_t(group * 512 + entry) * LargePageBytes) | (1U << 7) | 3;
    }
  }

  const uint64_t tokenBase = m_ReservedTokens.physicalAddress();
  const uint64_t firstBlock = tokenBase / LargePageBytes;
  const uint64_t lastBlock = (tokenBase + TokenPages * PageBytes - 1) / LargePageBytes;
  for (uint64_t block = firstBlock; block <= lastBlock; ++block) {
    physical_uintptr_t tablePage = 0;
    if (!addPage(tablePage)) {
      freeDomain(domain);
      return false;
    }
    auto* table = reinterpret_cast<uint64_t*>(physicalAddress(tablePage));
    for (size_t index = 0; index < 512; ++index) {
      table[index] = (block * LargePageBytes + index * PageBytes) | 3;
    }
    for (size_t slot = 0; slot < TokenPages; ++slot) {
      const uint64_t iova = tokenBase + slot * PageBytes;
      if (iova / LargePageBytes == block) {
        domain.tokenEntries[slot] = &table[(iova / PageBytes) & 511];
        *domain.tokenEntries[slot] = 0;
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

IntelIommu::Domain* IntelIommu::findDomain(const Device* device) {
  for (size_t i = 0; i < m_DomainCount; ++i) {
    if (m_Domains[i].device == device) {
      return &m_Domains[i];
    }
  }
  return nullptr;
}

const IntelIommu::Domain* IntelIommu::findDomain(const Device* device) const {
  for (size_t i = 0; i < m_DomainCount; ++i) {
    if (m_Domains[i].device == device) {
      return &m_Domains[i];
    }
  }
  return nullptr;
}

bool IntelIommu::attach(Device* device) {
  if (!device || device->getPciBusPosition() > 255 || device->getPciDevicePosition() > 31 ||
      device->getPciFunctionNumber() > 7) {
    return false;
  }
  LockGuard<Mutex> guard(m_Lock);
  if (findDomain(device)) {
    return true;
  }
  const AcpiDmar::Info* info = Acpi::instance().dmarInfo();
  if (!info ||
      (!info->segmentZeroIncludeAllCount &&
       !info->includesDirectEndpoint(device->getPciBusPosition(), device->getPciDevicePosition(),
                                     device->getPciFunctionNumber()))) {
    return false;
  }
  uint16_t pciCommand = 0;
  const bool commandRead = PciBus::instance().readConfig16(device, 4, pciCommand);
  if (!commandRead || (pciCommand & 4)) {
    NOTICE("Intel VT-d: attach requires bus mastering disabled, PCI command=" << Hex << pciCommand);
    return false;
  }
  if (m_Failed || m_DomainCount == MaxDomains) {
    return false;
  }
  if (!m_Enabled && !initialise()) {
    m_Failed = true;
    return false;
  }

  Domain domain;
  domain.device = device;
  domain.id = static_cast<uint16_t>(m_DomainCount + 2);
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
      FATAL("Intel VT-d: failed to publish bus context");
    }
    m_BusContexts[bus] = busContext;
  }

  auto* contexts = reinterpret_cast<uint64_t*>(physicalAddress(m_BusContexts[bus]));
  contexts[function * 2 + 1] = (uint64_t(domain.id) << 8) | m_Aw;
  contexts[function * 2] = domain.root | 1;
  flushLines(&contexts[function * 2], 2 * sizeof(uint64_t));
  if (!invalidateContext() || !invalidateIotlb()) {
    FATAL("Intel VT-d: failed to publish device context");
  }

  m_Domains[m_DomainCount++] = domain;
  NOTICE("Intel VT-d: attached PCI " << Dec << static_cast<uint32_t>(bus) << ":"
                                     << static_cast<uint32_t>(function >> 3) << "."
                                     << static_cast<uint32_t>(function & 7) << " to domain "
                                     << static_cast<uint32_t>(domain.id));
  return true;
}

bool IntelIommu::attached(const Device* device) const {
  LockGuard<Mutex> guard(m_Lock);
  return m_Enabled && findDomain(device);
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

  const uint64_t tokenBase = m_ReservedTokens.physicalAddress();
  if (physical < (1ULL << 32)) {
    if (physical >= tokenBase && physical < tokenBase + TokenPages * PageBytes) {
      return false;
    }
    dmaAddress = static_cast<uint32_t>(physical);
    token = 0;
    return true;
  }

  for (size_t slot = 0; slot < TokenPages; ++slot) {
    if (domain->tokenUsed[slot]) {
      continue;
    }
    uint64_t* entry = domain->tokenEntries[slot];
    *entry = physical | 3;
    flushLines(entry, sizeof(*entry));
    if (!invalidateIotlb()) {
      FATAL("Intel VT-d: failed to publish IOVA mapping");
    }
    domain->tokenUsed[slot] = true;
    dmaAddress = static_cast<uint32_t>(tokenBase + slot * PageBytes);
    token = static_cast<uint16_t>(slot + 1);
    if (!domain->loggedHighMapping) {
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
  if (!m_Enabled || !domain || token > TokenPages || !domain->tokenUsed[token - 1]) {
    FATAL("Intel VT-d: invalid IOVA unmap");
  }
  uint64_t* entry = domain->tokenEntries[token - 1];
  *entry = 0;
  flushLines(entry, sizeof(*entry));
  if (!invalidateIotlb()) {
    FATAL("Intel VT-d: failed to revoke IOVA mapping");
  }
  domain->tokenUsed[token - 1] = false;
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
