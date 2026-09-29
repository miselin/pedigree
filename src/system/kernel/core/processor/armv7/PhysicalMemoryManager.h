#ifndef KERNEL_PROCESSOR_ARMV7_PHYSICALMEMORYMANAGER_H
#define KERNEL_PROCESSOR_ARMV7_PHYSICALMEMORYMANAGER_H

#include "pedigree/kernel/BootstrapInfo.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"

extern size_t g_AllocedPages;
extern size_t g_FreePages;

class Armv7PhysicalMemoryManager final : public PhysicalMemoryManager {
 public:
  static Armv7PhysicalMemoryManager& instance();
  void initialise(const BootstrapStruct_t& info);

  physical_uintptr_t allocatePage(size_t constraints = 0) override;
  physical_uintptr_t tryAllocatePage() override;
  physical_uintptr_t allocateAlignedPages(size_t pages);
  void freePage(physical_uintptr_t page) override;
  void pin(physical_uintptr_t page) override;
  bool copyPhysicalPageToBuffer(physical_uintptr_t page, void* buffer) override;
  bool copyPhysicalPageFromBuffer(physical_uintptr_t page, const void* buffer) override;
  MemorySnapshot memorySnapshot() const override;
  size_t freePageCount() const override;
  bool allocateRegion(MemoryRegion& region, size_t pages, size_t constraints, size_t flags,
                      physical_uintptr_t start = -1) override;

 private:
  Armv7PhysicalMemoryManager();
  void freePageUnlocked(physical_uintptr_t page) override;
  void unmapRegion(MemoryRegion* region) override;
  bool pageAvailable(size_t page) const;
  void markPage(size_t page, bool available);
  size_t pageLimit(size_t constraints) const;
  physical_uintptr_t allocatePageUnlocked(size_t constraints);
  physical_uintptr_t allocateContinuousPagesUnlocked(size_t pages, size_t constraints);

  mutable Spinlock m_Lock;
  Spinlock m_RegionLock;
  uint16_t* m_References;
  uint8_t* m_FreeBitmap;
  size_t m_MaxPage;
  size_t m_NextPage;
  size_t m_TotalPages;
  size_t m_FreePages;
  uintptr_t m_NextRegion;
};

#endif
