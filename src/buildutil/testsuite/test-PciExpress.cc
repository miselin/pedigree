#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/machine/PciExpress.h"

#include <array>
#include <vector>

#include <gtest/gtest.h>

namespace {
struct Config {
  std::array<uint8_t, 256> bytes{};
  std::vector<uint16_t> writes;
  uint16_t failReadAt = 0xffff;
  bool failWrite = false;
  bool changeIdentityOnFlr = false;

  void set16(unsigned offset, uint16_t value) {
    bytes[offset] = value;
    bytes[offset + 1] = value >> 8;
  }
  void set32(unsigned offset, uint32_t value) {
    set16(offset, value);
    set16(offset + 2, value >> 16);
  }
  bool read8(uint16_t offset, uint8_t& value) {
    if (offset >= bytes.size() || offset == failReadAt) {
      return false;
    }
    value = bytes[offset];
    return true;
  }
  bool read16(uint16_t offset, uint16_t& value) {
    if (offset > 254 || (offset & 1U) || offset == failReadAt) {
      return false;
    }
    value = bytes[offset] | (uint16_t{bytes[offset + 1]} << 8);
    return true;
  }
  bool read32(uint16_t offset, uint32_t& value) {
    if (offset > 252 || (offset & 3U) || offset == failReadAt) {
      return false;
    }
    value = bytes[offset] | (uint32_t{bytes[offset + 1]} << 8) |
            (uint32_t{bytes[offset + 2]} << 16) | (uint32_t{bytes[offset + 3]} << 24);
    return true;
  }
  bool write16(uint16_t offset, uint16_t value) {
    if (offset > 254 || (offset & 1U) || failWrite) {
      return false;
    }
    writes.push_back(offset);
    set16(offset, value);
    if (changeIdentityOnFlr && (value & 0x8000U)) {
      set16(2, 0x9876);
    }
    return true;
  }
};

Config endpoint() {
  Config config;
  config.set32(0, 0x56781234);
  config.set16(6, 0x10);
  config.bytes[0x34] = 0x40;
  config.bytes[0x40] = 1;
  config.bytes[0x41] = 0x50;
  config.bytes[0x50] = PciExpress::CapabilityId;
  config.bytes[0x51] = 0x80;
  config.set16(0x52, 2U | (3U << 9));
  config.set32(0x54, 1U << 28);
  config.set16(0x58, 0x1210);
  config.set16(0x5a, 0xf);
  config.set32(0x5c, 0x84);
  config.set16(0x60, 0x101);
  config.set16(0x62, 0x43);
  config.bytes[0x80] = 0x11;
  return config;
}
}  // namespace

TEST(PciExpress, FindsEndpointAndReadsSeparateDeviceAndLinkRegisters) {
  Config config = endpoint();
  PciExpress::State state;
  ASSERT_EQ(PciExpress::find(config, state), PciExpress::FindResult::Found);
  EXPECT_EQ(state.offset, 0x50);
  EXPECT_EQ(state.version, 2);
  EXPECT_EQ(state.type, PciExpress::Type::Endpoint);
  EXPECT_EQ(state.interruptMessage, 3);
  EXPECT_TRUE(state.supportsFlr());
  EXPECT_EQ(state.deviceControl, 0x1210);
  EXPECT_EQ(state.deviceStatus, 0xf);
  EXPECT_TRUE(state.hasLink);
  EXPECT_EQ(state.linkSpeed(), 3);
  EXPECT_EQ(state.linkWidth(), 4);

  config.bytes[0x0e] = 1;
  config.set16(0x52, 1U | (4U << 4) | (1U << 8));
  config.set32(0x64, 0x8007b);
  config.set16(0x68, 0x13cf);
  config.set16(0x6a, 0x151);
  ASSERT_EQ(PciExpress::find(config, state), PciExpress::FindResult::Found);
  EXPECT_EQ(state.type, PciExpress::Type::RootPort);
  EXPECT_TRUE(state.slotImplemented);
  EXPECT_EQ(state.slotCapabilities, 0x8007bU);
  EXPECT_EQ(state.slotControl, 0x13cfU);
  EXPECT_EQ(state.slotStatus, 0x151U);
  EXPECT_FALSE(state.supportsFlr());

  config.bytes[0x0e] = 0;
  config.set16(0x52, 1U | (9U << 4));
  config.failReadAt = 0x5c;
  ASSERT_EQ(PciExpress::find(config, state), PciExpress::FindResult::Found);
  EXPECT_FALSE(state.hasLink);
  EXPECT_EQ(state.linkCapabilities, 0U);
}

TEST(PciExpress, RejectsBadChainsAndLeavesResultUnchangedOnFailure) {
  Config config = endpoint();
  PciExpress::State state;
  state.offset = 0xaa;
  config.bytes[0x41] = 0x44;
  EXPECT_EQ(PciExpress::find(config, state), PciExpress::FindResult::Malformed);
  config = endpoint();
  config.bytes[0x81] = 0x40;
  EXPECT_EQ(PciExpress::find(config, state), PciExpress::FindResult::Malformed);
  config = endpoint();
  config.bytes[0x51] = 0x60;
  EXPECT_EQ(PciExpress::find(config, state), PciExpress::FindResult::Malformed);
  config = endpoint();
  config.set16(0x52, 1U | (4U << 4));
  config.bytes[0x51] = 0x6c;
  config.bytes[0x6c] = 1;
  EXPECT_EQ(PciExpress::find(config, state), PciExpress::FindResult::Malformed);
  config = endpoint();
  config.set16(0x52, 3);
  EXPECT_EQ(PciExpress::find(config, state), PciExpress::FindResult::Malformed);
  config = endpoint();
  config.bytes[0x80] = PciExpress::CapabilityId;
  EXPECT_EQ(PciExpress::find(config, state), PciExpress::FindResult::Malformed);
  config = endpoint();
  config.set16(0x52, 1U | (4U << 4) | (1U << 8));
  config.failReadAt = 0x6a;
  EXPECT_EQ(PciExpress::find(config, state), PciExpress::FindResult::Unavailable);
  config = endpoint();
  config.failReadAt = 0x5a;
  EXPECT_EQ(PciExpress::find(config, state), PciExpress::FindResult::Unavailable);
  EXPECT_EQ(state.offset, 0xaa);
}

TEST(PciExpress, FlrRequiresQuiescenceAndWaitsBeforeVerifyingIdentity) {
  Config config = endpoint();
  unsigned waited = 0;
  auto wait = [&](unsigned milliseconds) {
    waited = milliseconds;
    return true;
  };
  ASSERT_EQ(PciExpress::resetFunction(config, wait), PciExpress::FlrResult::Complete);
  EXPECT_EQ(waited, 100U);
  EXPECT_EQ(config.writes, (std::vector<uint16_t>{0x58}));
  EXPECT_EQ(config.bytes[0x5a], 0xf);
  EXPECT_EQ(config.bytes[0x5b], 0);

  config = endpoint();
  waited = 0;
  config.set16(4, 4);
  EXPECT_EQ(PciExpress::resetFunction(config, wait), PciExpress::FlrResult::Busy);
  EXPECT_TRUE(config.writes.empty());
  EXPECT_EQ(waited, 0U);
  config.set16(4, 0);
  config.set16(0x5a, 0x20);
  EXPECT_EQ(PciExpress::resetFunction(config, wait), PciExpress::FlrResult::Busy);
  EXPECT_TRUE(config.writes.empty());

  config = endpoint();
  config.changeIdentityOnFlr = true;
  EXPECT_EQ(PciExpress::resetFunction(config, wait), PciExpress::FlrResult::Unverified);
  EXPECT_EQ(config.writes, (std::vector<uint16_t>{0x58}));

  config = endpoint();
  config.failWrite = true;
  waited = 0;
  EXPECT_EQ(PciExpress::resetFunction(config, wait), PciExpress::FlrResult::Unverified);
  EXPECT_EQ(waited, 100U);
}
