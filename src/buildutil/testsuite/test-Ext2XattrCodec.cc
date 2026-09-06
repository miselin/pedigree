/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/utilities/utility.h"

#include <array>

#include "modules/system/ext2/Ext2Xattr.h"
#include <gtest/gtest.h>

namespace {
using Status = XattrStatus;
struct AttributeBlock {
  AttributeBlock() {
    auto* header = reinterpret_cast<Ext2Ea::Header*>(bytes.data());
    header->magic = HOST_TO_LITTLE32(0xea020000);
    header->references = header->blocks = HOST_TO_LITTLE32(1);
    for (size_t n = 0; n < 2; ++n) {
      auto& attribute = entry(n);
      attribute.nameIndex = 1;
      attribute.nameLength = 1;
      attribute.valueLength = HOST_TO_LITTLE32(4);
      attribute.valueOffset = HOST_TO_LITTLE16(4088 + n * 4);
      *reinterpret_cast<char*>(&attribute + 1) = 'a' + n;
      bytes[4088 + n * 4] = 'x' + n;
    }
  }
  Ext2Ea::Entry& entry(size_t n) {
    return *reinterpret_cast<Ext2Ea::Entry*>(bytes.data() + 32 + 20 * n);
  }
  alignas(Ext2Ea::Header) std::array<uint8_t, 4096> bytes = {};
};

void rejectsWithoutChangingInput(AttributeBlock& block) {
  const auto before = block.bytes;
  std::array<uint8_t, 4096> replacement;
  replacement.fill(0xa5);
  bool empty = false;
  EXPECT_EQ(Ext2Ea::validate(block.bytes.data(), block.bytes.size()), Status::IoError);
  EXPECT_EQ(Ext2Ea::rebuild(block.bytes.data(), block.bytes.size(), StringView("user.a"), "new", 3,
                            0, false, replacement.data(), empty),
            Status::IoError);
  EXPECT_EQ(block.bytes, before);
  EXPECT_EQ(replacement.front(), 0xa5);
  EXPECT_EQ(replacement.back(), 0xa5);
}
}  // namespace

TEST(Ext2XattrCodec, ReservedIndexRejectsImportedBlockWithoutMutation) {
  AttributeBlock block;
  block.entry(0).nameIndex = 0;
  rejectsWithoutChangingInput(block);
}

TEST(Ext2XattrCodec, SharedOrPartiallyOverlappingValueBytesRejectImportedBlock) {
  AttributeBlock shared;
  shared.entry(1).valueOffset = shared.entry(0).valueOffset;
  rejectsWithoutChangingInput(shared);
  AttributeBlock partial;
  partial.entry(0).valueOffset = HOST_TO_LITTLE16(4084);
  partial.entry(0).valueLength = HOST_TO_LITTLE32(5);
  partial.entry(1).valueOffset = HOST_TO_LITTLE16(4088);
  partial.entry(1).valueLength = HOST_TO_LITTLE32(1);
  rejectsWithoutChangingInput(partial);
}

TEST(Ext2XattrCodec, AdjacentReverseOrderAndEmptyValuesRemainValid) {
  AttributeBlock block;
  block.entry(0).valueOffset = HOST_TO_LITTLE16(4092);
  block.entry(1).valueOffset = HOST_TO_LITTLE16(4088);
  EXPECT_EQ(Ext2Ea::validate(block.bytes.data(), block.bytes.size()), Status::Success);
  block.entry(0).valueLength = HOST_TO_LITTLE32(3);
  block.entry(1).valueLength = HOST_TO_LITTLE32(1);
  EXPECT_EQ(Ext2Ea::validate(block.bytes.data(), block.bytes.size()), Status::Success);
  block.entry(0).valueLength = 0;
  block.entry(0).valueOffset = block.entry(1).valueOffset;
  EXPECT_EQ(Ext2Ea::validate(block.bytes.data(), block.bytes.size()), Status::Success);
  std::array<uint8_t, 4096> replacement;
  bool empty = true;
  ASSERT_EQ(Ext2Ea::rebuild(block.bytes.data(), block.bytes.size(), StringView("user.a"), nullptr,
                            0, 0, false, replacement.data(), empty),
            Status::Success);
  EXPECT_FALSE(empty);
  EXPECT_EQ(Ext2Ea::validate(replacement.data(), replacement.size()), Status::Success);
  size_t required = 99;
  EXPECT_EQ(Ext2Ea::get(replacement.data(), replacement.size(), StringView("user.a"), nullptr, 0,
                        required),
            Status::Success);
  EXPECT_EQ(required, 0U);
}
