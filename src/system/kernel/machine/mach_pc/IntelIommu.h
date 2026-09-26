/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_PC_INTEL_IOMMU_H
#define PEDIGREE_PC_INTEL_IOMMU_H

#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/processor/MemoryMappedIo.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/types.h"

class Device;

class IntelIommu {
 public:
  static IntelIommu& instance();

  bool attach(Device* device, bool isolated = false);
  bool attached(const Device* device) const;
  bool isolated(const Device* device) const;
  bool mapPage(Device* device, physical_uintptr_t physical, uint32_t& dmaAddress, uint16_t& token);
  void unmapPage(Device* device, uint16_t token);

 private:
  static constexpr size_t MaxDomains = 8;
  static constexpr size_t TokenPages = 32;
  static constexpr size_t IsolatedTokenPages = 1024;
  static constexpr uint32_t IsolatedTokenBase = 0x10000000U;

  struct Domain {
    Device* device = nullptr;
    uint16_t id = 0;
    physical_uintptr_t root = 0;
    physical_uintptr_t pages[8] = {};
    size_t pageCount = 0;
    uint64_t tokenBase = 0;
    uint16_t tokenCount = 0;
    uint64_t* tokenTables[2] = {};
    uint32_t tokenUsed[IsolatedTokenPages / 32] = {};
    bool isolated = false;
    bool loggedHighMapping = false;
  };

  IntelIommu();
  bool initialise();
  bool allocateTable(physical_uintptr_t& page);
  bool makeDomain(Domain& domain);
  void freeDomain(Domain& domain);
  bool invalidateContext();
  bool invalidateIotlb();
  bool command(uint32_t bit, bool set);
  bool waitForStatus(uint32_t bit, bool set);
  void flushLines(const void* address, size_t bytes) const;
  void logFault();
  static uint64_t* tokenEntry(const Domain& domain, size_t slot);
  Domain* findDomain(const Device* device);
  const Domain* findDomain(const Device* device) const;

  mutable Mutex m_Lock;
  MemoryMappedIo m_Registers;
  MemoryRegion m_ReservedTokens;
  physical_uintptr_t m_Root = 0;
  physical_uintptr_t m_DefaultContext = 0;
  physical_uintptr_t m_BusContexts[256] = {};
  Domain m_Domains[MaxDomains];
  size_t m_DomainCount = 0;
  size_t m_IotlbOffset = 0;
  uint64_t m_MaxPhysical = 0;
  uint8_t m_Aw = 0;
  uint8_t m_PassThroughAw = 0;
  size_t m_CacheLine = 64;
  bool m_Coherent = false;
  bool m_Enabled = false;
  bool m_Failed = false;
};

#endif
