/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "modules/system/ext2/Ext2Xattr.h"

// The hosted ext2 library expects this test-provided clock symbol.
uint32_t getUnixTimestamp() {
  return 0;
}

namespace {
constexpr size_t BlockSize = 4096;

void check(bool condition) {
  if (!condition) {
    std::abort();
  }
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > 1024) {
    return 0;
  }

  alignas(Ext2Ea::Header) std::array<uint8_t, BlockSize> block{};
  const size_t valueLength = std::min(size, size_t{128});
  bool empty = true;
  check(Ext2Ea::rebuild(nullptr, BlockSize, StringView("user.a"), data, valueLength, 0, false,
                        block.data(), empty) == XattrStatus::Success);
  check(!empty && Ext2Ea::validate(block.data(), BlockSize) == XattrStatus::Success);

  std::array<uint8_t, BlockSize> output{};
  size_t required = 0;
  check(Ext2Ea::get(block.data(), BlockSize, StringView("user.a"), output.data(), output.size(),
                    required) == XattrStatus::Success);
  check(required == valueLength &&
        (!valueLength || std::equal(data, data + valueLength, output.data())));

  alignas(Ext2Ea::Header) auto mutated = block;
  constexpr size_t Fields[] = {0,  4,  8,  12, 16, 20, 24, 28, 32, 33, 34, 35,
                               36, 40, 44, 48, 49, 50, 51, 52, 56, 60, 64};
  for (size_t i = 0; i + 1 < size; i += 2) {
    const size_t offset = (data[i] & 1) ? Fields[data[i] % (sizeof(Fields) / sizeof(Fields[0]))]
                                        : ((size_t(data[i]) << 4) | data[i + 1]) % BlockSize;
    mutated[offset] ^= data[i + 1];
  }

  const auto status = Ext2Ea::validate(mutated.data(), BlockSize);
  const auto getStatus = Ext2Ea::get(mutated.data(), BlockSize, StringView("user.a"), output.data(),
                                     output.size(), required);
  if (status != XattrStatus::Success) {
    check(getStatus == status);
    return 0;
  }
  check(getStatus == XattrStatus::Success || getStatus == XattrStatus::Missing);

  size_t listed = 0;
  check(Ext2Ea::list(mutated.data(), BlockSize, nullptr, 0, listed) == XattrStatus::Success);
  size_t copied = 0;
  const auto listStatus =
      Ext2Ea::list(mutated.data(), BlockSize, output.data(), output.size(), copied);
  check(copied == listed);
  check(listStatus == (listed <= output.size() ? XattrStatus::Success : XattrStatus::Range));

  alignas(Ext2Ea::Header) std::array<uint8_t, BlockSize> replacement{};
  const auto rebuilt = Ext2Ea::rebuild(mutated.data(), BlockSize, StringView("user.z"), data,
                                       valueLength, 0, false, replacement.data(), empty);
  if (rebuilt == XattrStatus::Success) {
    check(Ext2Ea::validate(replacement.data(), BlockSize) == XattrStatus::Success);
  }
  return 0;
}
