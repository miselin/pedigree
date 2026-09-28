/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef KERNEL_MACHINE_VIRT_GICITS_H
#define KERNEL_MACHINE_VIRT_GICITS_H

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/processor/MemoryRegion.h"

#include "DeviceTree.h"

class GicIts {
 public:
  static constexpr uint32_t FirstLpi = 8192;
  static constexpr size_t MaxMappings = 16;

  GicIts();
  bool initialise(const VirtMsiController& controller, uintptr_t redistributor,
                  uint64_t redistributorPhysical);
  bool ready() const;
  uint64_t messageAddress() const;
  bool mapDevice(size_t slot, uint32_t deviceId, uint32_t lpi);
  bool mapDeviceGroup(const size_t* slots, size_t count, uint32_t deviceId);
  bool mappingActive(size_t slot) const;
  bool unmapDevice(size_t slot);
  bool setEnabled(uint32_t lpi, bool enabled);

 private:
  struct Mapping {
    uint32_t deviceId;
    uint32_t eventId;
    uint64_t itt;
    bool valid;
  };

  bool allocateAligned(MemoryRegion& region, size_t size, size_t alignment, uint64_t& physical,
                       uint8_t*& virtualAddress);
  bool setupBaser(uint32_t type, MemoryRegion& region, size_t requiredEntries);
  bool send(uint64_t word0, uint64_t word1, uint64_t word2);
  bool sync();
  void releaseTables();

  MemoryRegion m_DeviceTable;
  MemoryRegion m_CollectionTable;
  MemoryRegion m_CommandQueue;
  MemoryRegion m_Properties;
  MemoryRegion m_Pending;
  mutable NoIrqSpinlock m_MappingLock;
  NoIrqSpinlock m_CommandLock;
  Mapping m_Mappings[MaxMappings];
  uintptr_t m_Base;
  uintptr_t m_Redistributor;
  uint64_t m_PhysicalBase;
  uint64_t m_Target;
  uint8_t* m_CommandVirtual;
  uint8_t* m_PropertyVirtual;
  size_t m_CommandWrite;
  size_t m_CommandSize;
  uint32_t m_DeviceLimit;
  uint32_t m_EventBits;
  bool m_Ready;
};

#endif
