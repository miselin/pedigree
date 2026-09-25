/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_PCI_AER_H
#define PEDIGREE_PCI_AER_H

#include "pedigree/kernel/processor/types.h"

namespace PciAer {
constexpr uint16_t CapabilityId = 0x0001;

struct Status {
  uint32_t uncorrectable = 0;
  uint32_t uncorrectableMask = 0;
  uint32_t severity = 0;
  uint32_t correctable = 0;
  uint32_t correctableMask = 0;
  uint32_t capabilities = 0;

  uint32_t fatal() const {
    return uncorrectable & ~uncorrectableMask & severity;
  }
  uint32_t nonfatal() const {
    return uncorrectable & ~uncorrectableMask & ~severity;
  }
  uint32_t maskedUncorrectable() const {
    return uncorrectable & uncorrectableMask;
  }
  uint32_t activeCorrectable() const {
    return correctable & ~correctableMask;
  }
};

template <class Config>
bool read(Config& config, uint16_t offset, uint16_t next, Status& result) {
  constexpr uint16_t RequiredBytes = 0x1c;
  if (offset < 0x100 || (offset & 3U) || offset > 0x1000 - RequiredBytes ||
      (next &&
       ((next >= offset && next < offset + RequiredBytes) || next > 0xffc || (next & 3U)))) {
    return false;
  }
  Status status;
  if (!config.read32(offset + 0x04, status.uncorrectable) ||
      !config.read32(offset + 0x08, status.uncorrectableMask) ||
      !config.read32(offset + 0x0c, status.severity) ||
      !config.read32(offset + 0x10, status.correctable) ||
      !config.read32(offset + 0x14, status.correctableMask) ||
      !config.read32(offset + 0x18, status.capabilities)) {
    return false;
  }
  result = status;
  return true;
}
}  // namespace PciAer

#endif
