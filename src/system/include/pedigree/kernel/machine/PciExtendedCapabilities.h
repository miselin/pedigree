/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_PCI_EXTENDED_CAPABILITIES_H
#define PEDIGREE_PCI_EXTENDED_CAPABILITIES_H

#include "pedigree/kernel/processor/types.h"

namespace PciExtendedCapabilities {
struct Capability {
  uint16_t id = 0;
  uint8_t version = 0;
  uint16_t offset = 0;
  uint16_t next = 0;
};

enum class ScanResult { Complete, Stopped, Unavailable, Malformed };
enum class FindResult { Found, Absent, Unavailable, Malformed };

template <class Config, class Visitor>
ScanResult walk(Config& config, Visitor visitor) {
  // One bit for each DWORD of extended configuration space. A link may move
  // backward, so an iteration limit alone would revisit capabilities.
  uint32_t visited[30] = {};
  uint16_t offset = 0x100;
  for (;;) {
    const unsigned slot = (offset - 0x100) / 4;
    const uint32_t mask = 1U << (slot % 32);
    if (visited[slot / 32] & mask) {
      return ScanResult::Malformed;
    }
    visited[slot / 32] |= mask;

    uint32_t header = 0;
    if (!config.read32(offset, header)) {
      return offset == 0x100 ? ScanResult::Unavailable : ScanResult::Malformed;
    }
    if (header == 0 || header == 0xffffffffU) {
      if (offset != 0x100) {
        return ScanResult::Malformed;
      }
      return header == 0 ? ScanResult::Complete : ScanResult::Unavailable;
    }

    Capability cap{uint16_t(header), uint8_t((header >> 16) & 0xfU), offset,
                   uint16_t(header >> 20)};
    if (!cap.id || cap.id == 0xffffU || !cap.version ||
        (cap.next && (cap.next < 0x100 || cap.next > 0xffc || (cap.next & 3U)))) {
      return ScanResult::Malformed;
    }
    if (!visitor(cap)) {
      return ScanResult::Stopped;
    }
    if (!cap.next) {
      return ScanResult::Complete;
    }
    offset = cap.next;
  }
}

template <class Config>
FindResult find(Config& config, uint16_t id, Capability& result) {
  Capability found;
  bool present = false;
  const ScanResult status = walk(config, [&](const Capability& cap) {
    if (!present && cap.id == id) {
      found = cap;
      present = true;
    }
    return true;
  });
  if (status == ScanResult::Unavailable) {
    return FindResult::Unavailable;
  }
  if (status != ScanResult::Complete) {
    return FindResult::Malformed;
  }
  if (!present) {
    return FindResult::Absent;
  }
  result = found;
  return FindResult::Found;
}
}  // namespace PciExtendedCapabilities

#endif
