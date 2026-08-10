/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/core/SlamAllocator.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

class SlamAllocatorCorrectnessTest : public ::testing::Test {
 protected:
  void SetUp() override {
    SlamAllocator::instance().clearAll();
  }

  void TearDown() override {
    SlamAllocator::instance().clearAll();
  }
};

namespace {
constexpr size_t TestObjectSize = 128;

void prepareCacheAllocation(SlamCache& cache, uintptr_t object) {
  SlamAllocator::AllocHeader* header = reinterpret_cast<SlamAllocator::AllocHeader*>(object);
  header->cache = &cache;
#if OVERRUN_CHECK
  header->magic = VIGILANT_MAGIC;
  SlamAllocator::AllocFooter* footer = reinterpret_cast<SlamAllocator::AllocFooter*>(
      object + cache.objectSize() - sizeof(SlamAllocator::AllocFooter));
  footer->magic = VIGILANT_MAGIC;
#endif
}

struct SlabTransitionGate {
  explicit SlabTransitionGate(SlamAllocator::SlabTransitionForTest expected)
      : expected(expected), entered(false), release(false), address(0) {}

  SlamAllocator::SlabTransitionForTest expected;
  std::atomic<bool> entered;
  std::atomic<bool> release;
  std::atomic<uintptr_t> address;
};

void pauseSlabTransition(SlamAllocator::SlabTransitionForTest transition, uintptr_t address,
                         void* context) {
  SlabTransitionGate* gate = reinterpret_cast<SlabTransitionGate*>(context);
  if (transition != gate->expected) {
    return;
  }

  gate->address.store(address, std::memory_order_relaxed);
  gate->entered.store(true, std::memory_order_release);
  while (!gate->release.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
}

bool waitForTransition(const SlabTransitionGate& gate) {
  for (size_t attempt = 0; attempt < 100000; ++attempt) {
    if (gate.entered.load(std::memory_order_acquire)) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}
}  // namespace

TEST(PedigreeSlamAllocator, DISABLE_EmptyStartup) {
  SlamAllocator::instance().initialise();

  EXPECT_EQ(SlamAllocator::instance().heapPageCount(), (size_t)0);
}

TEST(PedigreeSlamAllocator, DISABLE_Allocation) {
  SlamAllocator::instance().initialise();

  uintptr_t alloc = SlamAllocator::instance().allocate(4);

  EXPECT_EQ(SlamAllocator::instance().heapPageCount(), (size_t)1);

  SlamAllocator::instance().free(alloc);
}

TEST(PedigreeSlamAllocator, DISABLE_Deallocate) {
  SlamAllocator::instance().initialise();

  uintptr_t alloc = SlamAllocator::instance().allocate(4);
  SlamAllocator::instance().free(alloc);

  EXPECT_EQ(SlamAllocator::instance().heapPageCount(), (size_t)1);
}

TEST(PedigreeSlamAllocator, DISABLE_Recovery) {
  SlamAllocator::instance().initialise();

  uintptr_t alloc = SlamAllocator::instance().allocate(4);
  SlamAllocator::instance().free(alloc);

  EXPECT_EQ(SlamAllocator::instance().heapPageCount(), (size_t)1);

  SlamAllocator::instance().recovery();
  EXPECT_EQ(SlamAllocator::instance().heapPageCount(), (size_t)0);
}

TEST(PedigreeSlamAllocator, DISABLE_Alignment) {
  SlamAllocator::instance().initialise();

  uintptr_t alloc = SlamAllocator::instance().allocate(4);
  EXPECT_EQ(alloc & 15, (uintptr_t)0);
  SlamAllocator::instance().free(alloc);
}

TEST_F(SlamAllocatorCorrectnessTest, ExactSizeClassBoundaryIsNotRoundedUp) {
  SlamAllocator& allocator = SlamAllocator::instance();
  const size_t framing = allocator.headerSize() + allocator.footerSize();
  const size_t requested = 128 - framing;

  uintptr_t allocation = allocator.allocate(requested);

  EXPECT_EQ(allocator.allocSize(allocation), requested);
  allocator.free(allocation);
}

TEST_F(SlamAllocatorCorrectnessTest, RecoveryPreservesOtherSlabFreeObjects) {
  SlamAllocator& allocator = SlamAllocator::instance();
  uintptr_t allocations[64] = {};

  allocations[0] = allocator.allocate(1);
  const size_t fullObjectSize =
      allocator.allocSize(allocations[0]) + allocator.headerSize() + allocator.footerSize();
  const size_t objectsPerSlab = SLAB_MINIMUM_SIZE / fullObjectSize;
  ASSERT_LT(objectsPerSlab, 64U);

  for (size_t i = 1; i <= objectsPerSlab; ++i) {
    allocations[i] = allocator.allocate(1);
  }

  for (size_t i = 0; i < objectsPerSlab; ++i) {
    allocator.free(allocations[i]);
  }

  ASSERT_EQ(allocator.heapPageCount(), 2U);
  ASSERT_EQ(allocator.recovery(1), 1U);
  ASSERT_EQ(allocator.heapPageCount(), 1U);

  uintptr_t reused = allocator.allocate(1);
  EXPECT_EQ(allocator.heapPageCount(), 1U);
  EXPECT_EQ(reused & ~(SLAB_MINIMUM_SIZE - 1),
            allocations[objectsPerSlab] & ~(SLAB_MINIMUM_SIZE - 1));

  allocator.free(reused);
  allocator.free(allocations[objectsPerSlab]);
}

TEST_F(SlamAllocatorCorrectnessTest, RecoveryHonoursGlobalSlabLimit) {
  SlamAllocator& allocator = SlamAllocator::instance();

  uintptr_t firstCache = allocator.allocate(1);
  allocator.free(firstCache);

  uintptr_t secondCache[32] = {};
  secondCache[0] = allocator.allocate(100);
  const size_t fullObjectSize =
      allocator.allocSize(secondCache[0]) + allocator.headerSize() + allocator.footerSize();
  const size_t objectsPerSlab = SLAB_MINIMUM_SIZE / fullObjectSize;
  ASSERT_LT(objectsPerSlab, 32U);
  for (size_t i = 1; i <= objectsPerSlab; ++i) {
    secondCache[i] = allocator.allocate(100);
  }
  for (size_t i = 0; i <= objectsPerSlab; ++i) {
    allocator.free(secondCache[i]);
  }

  ASSERT_EQ(allocator.heapPageCount(), 3U);
  EXPECT_EQ(allocator.recovery(2), 2U);
  EXPECT_EQ(allocator.heapPageCount(), 1U);
}

TEST_F(SlamAllocatorCorrectnessTest, ReusesAndRecoversCrossCpuFreeLists) {
  SlamAllocator& allocator = SlamAllocator::instance();
  allocator.initialise();

  SlamCache cache;
  cache.initialise(&allocator, TestObjectSize);
  const size_t objectsPerSlab = SLAB_MINIMUM_SIZE / TestObjectSize;
  uintptr_t allocations[SLAB_MINIMUM_SIZE / TestObjectSize] = {};
  const size_t pagesBefore = allocator.heapPageCount();

  cache.setListForTest(0);
  for (size_t i = 0; i < objectsPerSlab; ++i) {
    allocations[i] = cache.allocate();
    EXPECT_EQ(reinterpret_cast<SlamAllocator::AllocHeader*>(allocations[i])->cache, &cache);
    prepareCacheAllocation(cache, allocations[i]);
  }
  ASSERT_EQ(allocator.heapPageCount(), pagesBefore + 1);

  cache.setListForTest(1);
  for (size_t i = 0; i < objectsPerSlab; ++i) {
    cache.free(allocations[i]);
  }

  cache.setListForTest(0);
  uintptr_t reused = cache.allocate();
  EXPECT_EQ(reused & ~(SLAB_MINIMUM_SIZE - 1),
            allocations[0] & ~(SLAB_MINIMUM_SIZE - 1));
  EXPECT_EQ(allocator.heapPageCount(), pagesBefore + 1);
  prepareCacheAllocation(cache, reused);

  cache.setListForTest(2);
  cache.free(reused);
  cache.setListForTest(0);
  EXPECT_EQ(cache.recovery(1), 1U);
  EXPECT_EQ(allocator.heapPageCount(), pagesBefore);
}

TEST_F(SlamAllocatorCorrectnessTest, RecoveryBudgetSkipsBusySlab) {
  SlamAllocator& allocator = SlamAllocator::instance();
  allocator.initialise();

  SlamCache cache;
  cache.initialise(&allocator, TestObjectSize);
  const size_t objectsPerSlab = SLAB_MINIMUM_SIZE / TestObjectSize;
  uintptr_t allocations[2 * (SLAB_MINIMUM_SIZE / TestObjectSize)] = {};
  const size_t pagesBefore = allocator.heapPageCount();

  for (size_t i = 0; i < (2 * objectsPerSlab); ++i) {
    allocations[i] = cache.allocate();
    prepareCacheAllocation(cache, allocations[i]);
  }
  ASSERT_EQ(allocations[0] & ~(SLAB_MINIMUM_SIZE - 1),
            allocations[objectsPerSlab - 1] & ~(SLAB_MINIMUM_SIZE - 1));
  ASSERT_NE(allocations[0] & ~(SLAB_MINIMUM_SIZE - 1),
            allocations[objectsPerSlab] & ~(SLAB_MINIMUM_SIZE - 1));

  // Put the partially used first slab at the head, ahead of the completely
  // free second slab.
  for (size_t i = objectsPerSlab; i < (2 * objectsPerSlab); ++i) {
    cache.free(allocations[i]);
  }
  for (size_t i = 1; i < objectsPerSlab; ++i) {
    cache.free(allocations[i]);
  }

  ASSERT_EQ(allocator.heapPageCount(), pagesBefore + 2);
  EXPECT_EQ(cache.recovery(1), 1U);
  EXPECT_EQ(allocator.heapPageCount(), pagesBefore + 1);

  cache.free(allocations[0]);
  EXPECT_EQ(cache.recovery(1), 1U);
  EXPECT_EQ(allocator.heapPageCount(), pagesBefore);
}

TEST_F(SlamAllocatorCorrectnessTest, RecoveredPointerIsInvalid) {
  SlamAllocator& allocator = SlamAllocator::instance();
  uintptr_t allocation = allocator.allocate(1);
  allocator.free(allocation);
  ASSERT_EQ(allocator.recovery(1), 1U);

  EXPECT_FALSE(allocator.isPointerValid(allocation));
}

TEST_F(SlamAllocatorCorrectnessTest, ValidationRejectsReservedUnmappedSlab) {
  SlamAllocator& allocator = SlamAllocator::instance();
  uintptr_t stale = allocator.allocate(1);
  allocator.free(stale);
  ASSERT_EQ(allocator.recovery(1), 1U);

  SlabTransitionGate gate(SlamAllocator::SlabTransitionForTest::Reserved);
  allocator.setSlabTransitionHookForTest(pauseSlabTransition, &gate);
  uintptr_t slab = 0;
  std::thread mapper([&]() { slab = allocator.getSlab(SLAB_MINIMUM_SIZE); });

  const bool entered = waitForTransition(gate);
  EXPECT_TRUE(entered);
  std::atomic<bool> validationStarted(false);
  std::atomic<bool> validationComplete(false);
  bool valid = true;
  if (entered) {
    EXPECT_EQ(gate.address.load(std::memory_order_relaxed),
              stale & ~(SLAB_MINIMUM_SIZE - 1));
    std::thread validator([&]() {
      validationStarted.store(true, std::memory_order_release);
      valid = allocator.isPointerValid(stale);
      validationComplete.store(true, std::memory_order_release);
    });
    while (!validationStarted.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (size_t attempt = 0; attempt < 100000 &&
                             !validationComplete.load(std::memory_order_acquire);
         ++attempt) {
      std::this_thread::yield();
    }
    EXPECT_TRUE(validationComplete.load(std::memory_order_acquire));
    EXPECT_FALSE(valid);

    gate.release.store(true, std::memory_order_release);
    mapper.join();
    validator.join();
  } else {
    gate.release.store(true, std::memory_order_release);
    mapper.join();
  }
  allocator.setSlabTransitionHookForTest(nullptr, nullptr);
  ASSERT_NE(slab, 0U);
  allocator.freeSlab(slab, SLAB_MINIMUM_SIZE);
}

TEST_F(SlamAllocatorCorrectnessTest, ValidationRejectsMappedUninitialisedSlab) {
  SlamAllocator& allocator = SlamAllocator::instance();
  uintptr_t stale = allocator.allocate(1);
  allocator.free(stale);
  ASSERT_EQ(allocator.recovery(1), 1U);

  SlamCache cache;
  cache.initialise(&allocator, TestObjectSize);
  SlabTransitionGate gate(SlamAllocator::SlabTransitionForTest::Mapped);
  allocator.setSlabTransitionHookForTest(pauseSlabTransition, &gate);
  uintptr_t allocation = 0;
  std::thread mapper([&]() { allocation = cache.allocate(); });

  const bool entered = waitForTransition(gate);
  EXPECT_TRUE(entered);
  bool valid = true;
  if (entered) {
    EXPECT_EQ(gate.address.load(std::memory_order_relaxed),
              stale & ~(SLAB_MINIMUM_SIZE - 1));
    valid = allocator.isPointerValid(stale);
    EXPECT_FALSE(valid);
  }

  gate.release.store(true, std::memory_order_release);
  mapper.join();
  allocator.setSlabTransitionHookForTest(nullptr, nullptr);
  ASSERT_NE(allocation, 0U);
  prepareCacheAllocation(cache, allocation);
  cache.free(allocation);
  EXPECT_EQ(cache.recovery(1), 1U);
}

TEST_F(SlamAllocatorCorrectnessTest, ValidationPinsSlabAcrossUnmap) {
  SlamAllocator& allocator = SlamAllocator::instance();
  uintptr_t allocation = allocator.allocate(1);
  allocator.free(allocation);

  SlabTransitionGate gate(SlamAllocator::SlabTransitionForTest::Unmapped);
  allocator.setSlabTransitionHookForTest(pauseSlabTransition, &gate);
  size_t recovered = 0;
  std::thread reclaimer([&]() { recovered = allocator.recovery(1); });

  const bool entered = waitForTransition(gate);
  EXPECT_TRUE(entered);
  std::atomic<bool> validationStarted(false);
  std::atomic<bool> validationComplete(false);
  bool valid = true;
  std::thread validator([&]() {
    validationStarted.store(true, std::memory_order_release);
    valid = allocator.isPointerValid(allocation);
    validationComplete.store(true, std::memory_order_release);
  });

  while (!validationStarted.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  for (size_t attempt = 0; attempt < 1000; ++attempt) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(validationComplete.load(std::memory_order_acquire));

  gate.release.store(true, std::memory_order_release);
  reclaimer.join();
  validator.join();
  allocator.setSlabTransitionHookForTest(nullptr, nullptr);

  EXPECT_EQ(recovered, 1U);
  EXPECT_FALSE(valid);
}

TEST_F(SlamAllocatorCorrectnessTest, RejectsOverflowingAllocationSize) {
  EXPECT_DEATH(SlamAllocator::instance().allocate(~static_cast<size_t>(0)), "allocation");
}

TEST_F(SlamAllocatorCorrectnessTest, FindsFreePagesAcrossBitmapEntries) {
  SlamAllocator& allocator = SlamAllocator::instance();
  uintptr_t pages[64] = {};

  for (size_t i = 0; i < 64; ++i) {
    pages[i] = allocator.getSlab(SLAB_MINIMUM_SIZE);
  }

  allocator.freeSlab(pages[63], SLAB_MINIMUM_SIZE);
  uintptr_t twoPages = allocator.getSlab(2 * SLAB_MINIMUM_SIZE);

  EXPECT_EQ(twoPages, pages[63]);
}

TEST_F(SlamAllocatorCorrectnessTest, LargeSlabCanStartMidBitmapEntry) {
  SlamAllocator& allocator = SlamAllocator::instance();
  uintptr_t firstPage = allocator.getSlab(SLAB_MINIMUM_SIZE);

  uintptr_t largeSlab = allocator.getSlab(65 * SLAB_MINIMUM_SIZE);

  EXPECT_EQ(largeSlab, firstPage + SLAB_MINIMUM_SIZE);
}
