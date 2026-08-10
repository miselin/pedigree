/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"

#include <gtest/gtest.h>

namespace {
class TestPhysicalMemoryManager final : public PhysicalMemoryManager {
 public:
  physical_uintptr_t allocatePage(size_t) override {
    return 0;
  }

  void freePage(physical_uintptr_t) override {}
  void pin(physical_uintptr_t) override {}

  bool allocateRegion(MemoryRegion&, size_t, size_t, size_t, physical_uintptr_t) override {
    return false;
  }

  void track(MemoryRegion& region) {
    m_MemoryRegions.pushBack(&region);
  }

  size_t trackedCount() const {
    return m_MemoryRegions.count();
  }

 private:
  void freePageUnlocked(physical_uintptr_t) override {}

  void unmapRegion(MemoryRegion* region) override {
    for (Vector<MemoryRegion*>::Iterator it = m_MemoryRegions.begin(); it != m_MemoryRegions.end();
         ++it) {
      if (*it == region) {
        m_MemoryRegions.erase(it);
        return;
      }
    }
  }
};

TestPhysicalMemoryManager& testPhysicalMemoryManager() {
  static TestPhysicalMemoryManager manager;
  return manager;
}
}  // namespace

PhysicalMemoryManager& PhysicalMemoryManager::instance() {
  return testPhysicalMemoryManager();
}

TEST(MemoryRegion, AnonymousRegionsRetainOwnershipWithoutDiagnosticVisibility) {
  TestPhysicalMemoryManager& manager = testPhysicalMemoryManager();
  ASSERT_EQ(manager.trackedCount(), 0U);

  {
    MemoryRegion visible("Visible region");
    MemoryRegion anonymous("Anonymous region");
    anonymous.setAnonymous(true);
    manager.track(visible);
    manager.track(anonymous);

    Vector<PhysicalMemoryManager::MemoryRegionInfo*> regions;
    manager.allocateMemoryRegionList(regions);
    ASSERT_EQ(regions.count(), 1U);
    EXPECT_STREQ(regions[0]->pName, "Visible region");
    manager.freeMemoryRegionList(regions);

    anonymous.free();
    EXPECT_EQ(manager.trackedCount(), 1U);
  }

  EXPECT_EQ(manager.trackedCount(), 0U);
}
