/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include <vector>

#include "system/kernel/machine/mach_pc/AcpiPciRouting.h"
#include <gtest/gtest.h>

namespace {
using Bytes = std::vector<uint8_t>;

void append(Bytes& to, const Bytes& from) {
  to.insert(to.end(), from.begin(), from.end());
}

Bytes package(uint8_t op, const Bytes& body) {
  Bytes result{op};
  const size_t lengthBytes = body.size() + 1 <= 63 ? 1 : 2;
  const size_t length = body.size() + lengthBytes;
  if (lengthBytes == 1) {
    result.push_back(length);
  } else {
    result.push_back(0x40 | (length & 0xf));
    result.push_back(length >> 4);
  }
  append(result, body);
  return result;
}

Bytes route(const char* name, uint8_t slot, uint8_t pin, const Bytes& source,
            const Bytes& sourceIndex = {0}) {
  Bytes entry{4, 0x0c, 0xff, 0xff, slot, 0, 0x0a, pin};
  append(entry, source);
  append(entry, sourceIndex);
  const Bytes item = package(0x12, entry);
  Bytes table{1};
  append(table, item);
  Bytes named{0x08, uint8_t(name[0]), uint8_t(name[1]), uint8_t(name[2]), uint8_t(name[3])};
  append(named, package(0x12, table));
  return named;
}

}  // namespace

TEST(AcpiPciRouting, ParsesQ35ApicPackageAndLinkPolarity) {
  Bytes aml = route("PRTA", 2, 0, {'G', 'S', 'I', 'G'});
  const Bytes resource{0x89, 6, 0, 0x09, 1, 22, 0, 0, 0, 0x79, 0};
  Bytes buffer{0x0a, uint8_t(resource.size())};
  append(buffer, resource);
  Bytes device{'G', 'S', 'I', 'G', 0x08, '_', 'C', 'R', 'S'};
  append(device, package(0x11, buffer));
  const Bytes packed = package(0x82, device);
  aml.push_back(0x5b);
  append(aml, packed);

  AcpiPciRouting::Route routes[32][4] = {};
  ASSERT_TRUE(AcpiPciRouting::parse(aml.data(), aml.size(), routes));
  EXPECT_EQ(routes[2][0].gsi, 22U);
  EXPECT_FALSE(routes[2][0].activeLow);
  EXPECT_EQ(routes[2][1].gsi, 0U);
}

TEST(AcpiPciRouting, RejectsMissingLinkAndTruncatedPackage) {
  Bytes aml = route("PRTA", 2, 0, {'G', 'S', 'I', 'G'});
  AcpiPciRouting::Route routes[32][4] = {};
  EXPECT_FALSE(AcpiPciRouting::parse(aml.data(), aml.size(), routes));

  aml = route("_PRT", 3, 1, {0}, {0x0a, 20});
  ASSERT_TRUE(AcpiPciRouting::parse(aml.data(), aml.size(), routes));
  EXPECT_EQ(routes[3][1].gsi, 20U);
  EXPECT_TRUE(routes[3][1].activeLow);
  aml.pop_back();
  EXPECT_FALSE(AcpiPciRouting::parse(aml.data(), aml.size(), routes));
}
