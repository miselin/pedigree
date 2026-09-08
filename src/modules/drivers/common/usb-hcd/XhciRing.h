/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef XHCI_RING_H
#define XHCI_RING_H
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/types.h"
namespace XhciHw {
constexpr size_t PageBytes = 4096, RingEntries = 256, MaxTransfer = 65536;
constexpr size_t MaxSlots = 16, MaxPorts = 32, MaxTransactions = 64;
constexpr uint32_t Cycle = 1, Chain = 1U << 4, Ioc = 1U << 5, Isp = 1U << 2;
constexpr uint32_t PortChanges = 0x00fe0000U;
struct Trb {
  uint64_t parameter;
  uint32_t status;
  uint32_t control;
};
static_assert(sizeof(Trb) == 16);
bool allocate(MemoryRegion& region, size_t pages, bool contiguous = true);
class Ring {
 public:
  Ring();
  bool initialise();
  bool enqueue(const Trb* entries, size_t count, uint64_t* addresses);
  void retire(size_t count);
  uint64_t address() const {
    return m_Memory.physicalAddress();
  }
  uint64_t enqueuePointer() const {
    return address() + m_Tail * sizeof(Trb) + m_Cycle;
  }
  size_t wraps() const {
    return m_Wraps;
  }

 private:
  MemoryRegion m_Memory;
  size_t m_Tail;
  size_t m_Used;
  bool m_Cycle;
  size_t m_Wraps;
};
}  // namespace XhciHw
#endif
