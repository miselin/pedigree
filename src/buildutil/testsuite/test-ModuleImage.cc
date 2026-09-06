/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/linker/ModuleImage.h"

#include <fstream>
#include <iterator>
#include <vector>

#include <gtest/gtest.h>

namespace {
std::vector<uint8_t> fixture() {
#ifdef PEDIGREE_MODULE_IMAGE_FIXTURE_FILE
  std::ifstream input(PEDIGREE_MODULE_IMAGE_FIXTURE_FILE, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
#else
  return {};
#endif
}
}  // namespace

TEST(ModuleImage, BoundedEmptyInput) {
  ModuleImage plan;
  const uint8_t empty[1] = {};
  EXPECT_EQ(plan.preflight(empty, 0), ModuleImage::Result::Malformed);
  EXPECT_EQ(plan.preflight(empty, ModuleImage::MaximumImageBytes + 1),
            ModuleImage::Result::TooLarge);
}

TEST(ModuleImage, NativeFixtureAndTruncatedTables) {
#ifndef PEDIGREE_MODULE_IMAGE_FIXTURE_FILE
  GTEST_SKIP() << "Native module fixture path was not configured";
#else
  auto image = fixture();
  ASSERT_FALSE(image.empty());
  ModuleImage plan;
  ASSERT_EQ(plan.preflight(image.data(), image.size()), ModuleImage::Result::Valid);
  EXPECT_STREQ(plan.name, "runtime-fixture-0");
  EXPECT_EQ(plan.constructorCount, 1U);
  EXPECT_EQ(plan.destructorCount, 1U);
  ModuleImage::Header header;
  MemoryCopy(&header, image.data(), sizeof(header));
  EXPECT_EQ(plan.preflight(image.data(), sizeof(header) - 1), ModuleImage::Result::Malformed);
  header.shoff = image.size() - sizeof(ModuleImage::Section) + 1;
  MemoryCopy(image.data(), &header, sizeof(header));
  EXPECT_EQ(plan.preflight(image.data(), image.size()), ModuleImage::Result::Malformed);
#endif
}

TEST(ModuleImage, RejectsUnterminatedMetadataBeforeExecution) {
#ifndef PEDIGREE_MODULE_IMAGE_FIXTURE_FILE
  GTEST_SKIP() << "Native module fixture path was not configured";
#else
  auto image = fixture();
  ASSERT_FALSE(image.empty());
  ModuleImage plan;
  ASSERT_EQ(plan.preflight(image.data(), image.size()), ModuleImage::Result::Valid);
  ModuleImage::Symbol name;
  uintptr_t address = 0;
  size_t offset = 0;
  ASSERT_TRUE(plan.findSymbol("g_pModuleName", name));
  ASSERT_TRUE(plan.localPointer(name.value, address));
  ASSERT_TRUE(plan.fileOffset(address, 1, offset));
  // Limit this mutation to the existing name, removing its required terminator.
  const size_t length = StringLength(plan.name);
  image[offset + length] = '!';
  EXPECT_NE(plan.preflight(image.data(), image.size()), ModuleImage::Result::Valid);
#endif
}
