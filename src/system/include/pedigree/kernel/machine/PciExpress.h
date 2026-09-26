/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_PCI_EXPRESS_H
#define PEDIGREE_PCI_EXPRESS_H

#include "pedigree/kernel/processor/types.h"

namespace PciExpress {
constexpr uint8_t CapabilityId = 0x10;

enum class FindResult { Found, Absent, Unavailable, Malformed };
enum class FlrResult { Complete, Unavailable, Unsupported, Busy, Unverified };

enum class Type : uint8_t {
  Endpoint = 0,
  LegacyEndpoint = 1,
  RootPort = 4,
  UpstreamPort = 5,
  DownstreamPort = 6,
  PcieToPciBridge = 7,
  PciToPcieBridge = 8,
  IntegratedEndpoint = 9,
  EventCollector = 10,
};

struct State {
  uint8_t offset = 0;
  uint8_t version = 0;
  Type type = Type::Endpoint;
  uint8_t interruptMessage = 0;
  uint32_t deviceCapabilities = 0;
  uint16_t deviceControl = 0;
  uint16_t deviceStatus = 0;
  uint32_t linkCapabilities = 0;
  uint16_t linkControl = 0;
  uint16_t linkStatus = 0;
  uint32_t slotCapabilities = 0;
  uint16_t slotControl = 0;
  uint16_t slotStatus = 0;
  bool hasLink = false;
  bool slotImplemented = false;

  bool supportsFlr() const {
    return (type == Type::Endpoint || type == Type::LegacyEndpoint ||
            type == Type::IntegratedEndpoint) &&
           (deviceCapabilities & (1U << 28));
  }
  uint8_t linkSpeed() const {
    return linkStatus & 0xfU;
  }
  uint8_t linkWidth() const {
    return (linkStatus >> 4) & 0x3fU;
  }
  bool linkTraining() const {
    return linkStatus & (1U << 11);
  }
};

inline bool validType(uint8_t type) {
  return type <= 1 || (type >= 4 && type <= 10);
}

template <class Config>
FindResult find(Config& config, State& result) {
  uint32_t identity = 0;
  uint16_t status = 0;
  uint8_t headerType = 0;
  if (!config.read32(0, identity) || !config.read16(6, status) || !config.read8(0x0e, headerType)) {
    return FindResult::Unavailable;
  }
  if (!(identity & 0xffffU) || (identity & 0xffffU) == 0xffffU || (headerType & 0x7fU) > 1) {
    return FindResult::Unavailable;
  }
  if (!(status & 0x10U)) {
    return FindResult::Absent;
  }

  uint8_t offset = 0;
  if (!config.read8(0x34, offset)) {
    return FindResult::Unavailable;
  }
  uint64_t occupied = 0;
  bool found = false;
  State candidate;
  while (offset) {
    if (offset < 0x40 || (offset & 3U)) {
      return FindResult::Malformed;
    }
    uint8_t id = 0, next = 0;
    if (!config.read8(offset, id) || !config.read8(offset + 1, next)) {
      return FindResult::Unavailable;
    }
    if (!id || id == 0xffU || (next && (next < 0x40 || (next & 3U)))) {
      return FindResult::Malformed;
    }

    unsigned bytes = 4;
    if (id == 1) {
      bytes = 8;
    } else if (id == 5) {
      uint16_t control = 0;
      if (!config.read16(offset + 2, control)) {
        return FindResult::Unavailable;
      }
      bytes = (control & 0x80U) ? 14 : 10;
      if (control & 0x100U) {
        bytes += 10;
      }
    } else if (id == 0x11) {
      bytes = 12;
    } else if (id == CapabilityId) {
      if (found) {
        return FindResult::Malformed;
      }
      uint16_t flags = 0;
      if (!config.read16(offset + 2, flags)) {
        return FindResult::Unavailable;
      }
      const uint8_t version = flags & 0xfU;
      const uint8_t type = (flags >> 4) & 0xfU;
      if ((version != 1 && version != 2) || !validType(type)) {
        return FindResult::Malformed;
      }
      candidate.offset = offset;
      candidate.version = version;
      candidate.type = static_cast<Type>(type);
      candidate.interruptMessage = (flags >> 9) & 0x1fU;
      candidate.hasLink = type != 9 && type != 10;
      candidate.slotImplemented = (flags & (1U << 8)) && (candidate.type == Type::RootPort ||
                                                          candidate.type == Type::DownstreamPort);
      bytes = candidate.hasLink ? 0x14 : 0x0c;
      if (candidate.type == Type::RootPort) {
        bytes = 0x24;
      } else if (candidate.slotImplemented) {
        bytes = 0x1c;
      }
    }
    if (unsigned(offset) + bytes > 0x100) {
      return FindResult::Malformed;
    }
    for (unsigned slot = offset / 4; slot < (offset + bytes + 3) / 4; ++slot) {
      const uint64_t bit = uint64_t{1} << slot;
      if (occupied & bit) {
        return FindResult::Malformed;
      }
      occupied |= bit;
    }
    if (id == CapabilityId) {
      if (!config.read32(offset + 4, candidate.deviceCapabilities) ||
          !config.read16(offset + 8, candidate.deviceControl) ||
          !config.read16(offset + 0x0a, candidate.deviceStatus) ||
          (candidate.hasLink && (!config.read32(offset + 0x0c, candidate.linkCapabilities) ||
                                 !config.read16(offset + 0x10, candidate.linkControl) ||
                                 !config.read16(offset + 0x12, candidate.linkStatus))) ||
          (candidate.slotImplemented &&
           (!config.read32(offset + 0x14, candidate.slotCapabilities) ||
            !config.read16(offset + 0x18, candidate.slotControl) ||
            !config.read16(offset + 0x1a, candidate.slotStatus)))) {
        return FindResult::Unavailable;
      }
      found = true;
    }
    offset = next;
  }
  if (!found) {
    return FindResult::Absent;
  }
  result = candidate;
  return FindResult::Found;
}

/** The caller must stop the driver, disable bus mastering, and drain DMA first.
 * FLR may clear resource configuration. The owner must restore its saved
 * assignments with decoding disabled before restarting the driver.
 */
template <class Config, class Wait>
FlrResult resetFunction(Config& config, Wait wait) {
  State state;
  const FindResult found = find(config, state);
  if (found != FindResult::Found) {
    return found == FindResult::Absent ? FlrResult::Unsupported : FlrResult::Unavailable;
  }
  if (!state.supportsFlr()) {
    return FlrResult::Unsupported;
  }
  uint32_t identity = 0;
  uint16_t command = 0;
  if (!config.read32(0, identity) || !config.read16(4, command)) {
    return FlrResult::Unavailable;
  }
  if ((command & 4U) || (state.deviceStatus & (1U << 5))) {
    return FlrResult::Busy;
  }
  // Device Control and the W1C Device Status share a DWORD; never write both.
  const bool wrote = config.write16(state.offset + 8, state.deviceControl | 0x8000U);
  // The callback must actually wait at least this long before returning.
  // Even a reported write failure may have initiated reset on the device.
  const bool waited = wait(100);
  if (!wrote || !waited) {
    return FlrResult::Unverified;
  }
  uint32_t actual = 0;
  uint16_t afterCommand = 0;
  if (!config.read32(0, actual) || actual != identity || !config.read16(4, afterCommand) ||
      (afterCommand & 4U)) {
    return FlrResult::Unverified;
  }
  return FlrResult::Complete;
}
}  // namespace PciExpress

#endif
