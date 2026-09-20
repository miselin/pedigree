/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1

#include "modules/system/vfs/MappingList.h"
#include <gtest/gtest.h>

namespace {
struct Mapping {
  uintptr_t base;
  size_t length;
  uintptr_t address() const {
    return base;
  }
  bool matches(uintptr_t address) const {
    return address >= base && address - base < length;
  }
};
}  // namespace

TEST(PedigreeMappingList, BoundariesGapsAndAppendOrder) {
  MappingList<Mapping> mappings;
  Mapping high{0x5000, 0x1001}, low{0x1000, 0x2000}, adjacent{0x3000, 0x1000};
  EXPECT_EQ(mappings.find(0), nullptr);
  ASSERT_TRUE(mappings.tryPushBack(&high));
  ASSERT_TRUE(mappings.tryPushBack(&low));
  ASSERT_TRUE(mappings.tryPushBack(&adjacent));
  EXPECT_FALSE(mappings.tryPushBack(&low));
  EXPECT_EQ(mappings.count(), 3U);
  Mapping* expected[] = {&high, &low, &adjacent};
  size_t i = 0;
  for (auto* mapping : mappings)
    EXPECT_EQ(mapping, expected[i++]);
  EXPECT_EQ(mappings.find(0xfff), nullptr);
  EXPECT_EQ(mappings.find(0x1000), &low);
  EXPECT_EQ(mappings.find(0x2fff), &low);
  EXPECT_EQ(mappings.find(0x3000), &adjacent);
  EXPECT_EQ(mappings.find(0x4000), nullptr);
  EXPECT_EQ(mappings.find(0x5000), &high);
  EXPECT_EQ(mappings.find(0x6000), &high);
  EXPECT_EQ(mappings.find(0x6001), nullptr);
}

TEST(PedigreeMappingList, ReservedSplitCommitAndRollback) {
  MappingList<Mapping> mappings;
  Mapping prefix{0, 0x4000}, suffix{0x2000, 0x2000};
  ASSERT_TRUE(mappings.tryPushBack(&prefix));
  ASSERT_TRUE(mappings.reserveBack(suffix.base));
  EXPECT_EQ(mappings.find(0x2000), &prefix);
  EXPECT_EQ(mappings.popBack(), nullptr);
  EXPECT_EQ(mappings.count(), 1U);
  EXPECT_EQ(mappings.find(0x2000), &prefix);
  ASSERT_TRUE(mappings.reserveBack(suffix.base));
  prefix.length = 0x2000;
  mappings.publishBack(&suffix);
  EXPECT_EQ(mappings.find(0x1fff), &prefix);
  EXPECT_EQ(mappings.find(0x2000), &suffix);
  EXPECT_EQ(mappings.popBack(), &suffix);
  EXPECT_EQ(mappings.find(0x2000), nullptr);
  EXPECT_EQ(mappings.find(0), &prefix);
}

TEST(PedigreeMappingList, ReplacementAndCloneHaveIndependentIndexes) {
  MappingList<Mapping> original, replacement, child;
  Mapping old{0x1000, 0x3000}, retained{0x8000, 0x1000};
  Mapping prefix{0x1000, 0x1000}, moved{0xa000, 0x2000}, clone{0x1000, 0x3000};
  ASSERT_TRUE(original.tryPushBack(&old));
  ASSERT_TRUE(original.tryPushBack(&retained));
  ASSERT_TRUE(replacement.tryPushBack(&retained));
  ASSERT_TRUE(replacement.tryPushBack(&prefix));
  ASSERT_TRUE(replacement.tryPushBack(&moved));
  ASSERT_TRUE(child.reserveBack(clone.base));
  EXPECT_EQ(child.find(clone.base), nullptr);
  child.publishBack(&clone);
  EXPECT_EQ(original.find(0x2000), &old);
  EXPECT_EQ(replacement.find(0x2000), nullptr);
  EXPECT_EQ(replacement.find(0xa000), &moved);
  auto it = original.begin();
  original.erase(it);
  EXPECT_EQ(original.find(0x1000), nullptr);
  EXPECT_EQ(replacement.find(0x1000), &prefix);
  EXPECT_EQ(child.find(0x1000), &clone);
  EXPECT_EQ(replacement.find(0x8000), &retained);
}

TEST(PedigreeMappingList, ErasureKeepsEveryRemainingRangeSearchable) {
  Mapping objects[256];
  MappingList<Mapping> mappings;
  for (size_t i = 0; i < 256; ++i)
    objects[i] = {i * 0x2000, 0x1000};
  for (size_t i = 0; i < 256; ++i)
    ASSERT_TRUE(mappings.tryPushBack(&objects[(i * 37) % 256]));
  for (auto it = mappings.begin(); it != mappings.end();) {
    if (((*it)->base / 0x2000) % 2)
      ++it;
    else
      it = mappings.erase(it);
  }
  for (size_t i = 0; i < 256; ++i) {
    size_t visits = 99;
    EXPECT_EQ(mappings.find(i * 0x2000, &visits), i % 2 ? &objects[i] : nullptr);
    EXPECT_LE(visits, 1U);
    EXPECT_EQ(mappings.find(i * 0x2000 + 0x1000), nullptr);
  }
  for (auto it = mappings.rbegin(); it != mappings.rend();) {
    const uintptr_t address = (*it)->base;
    it = mappings.erase(it);
    EXPECT_EQ(mappings.find(address), nullptr);
  }
  EXPECT_EQ(mappings.count(), 0U);
}
