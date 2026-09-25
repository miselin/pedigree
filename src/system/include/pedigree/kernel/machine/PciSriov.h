/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_PCI_SRIOV_H
#define PEDIGREE_PCI_SRIOV_H

#include "pedigree/kernel/processor/types.h"

namespace PciSriov {
constexpr uint16_t CapabilityId = 0x0010;

struct State {
  uint16_t control = 0;
  uint16_t initialVfs = 0;
  uint16_t totalVfs = 0;
  uint16_t numVfs = 0;
  uint16_t firstVfOffset = 0;
  uint16_t vfStride = 0;
  uint16_t vfDeviceId = 0;
  uint32_t supportedPageSizes = 0;
  uint32_t systemPageSize = 0;
  uint32_t bars[6] = {};

  bool enabled() const {
    return control & 1U;
  }
};

template <class Config>
bool read(Config& config, uint16_t offset, uint16_t next, State& result) {
  constexpr uint16_t RequiredBytes = 0x3c;
  if (offset < 0x100 || (offset & 3U) || offset > 0x1000 - RequiredBytes ||
      (next && next >= offset && next < offset + RequiredBytes)) {
    return false;
  }
  State state;
  if (!config.read16(offset + 0x08, state.control) ||
      !config.read16(offset + 0x0c, state.initialVfs) ||
      !config.read16(offset + 0x0e, state.totalVfs) ||
      !config.read16(offset + 0x10, state.numVfs) ||
      !config.read16(offset + 0x14, state.firstVfOffset) ||
      !config.read16(offset + 0x16, state.vfStride) ||
      !config.read16(offset + 0x1a, state.vfDeviceId) ||
      !config.read32(offset + 0x1c, state.supportedPageSizes) ||
      !config.read32(offset + 0x20, state.systemPageSize)) {
    return false;
  }
  for (unsigned i = 0; i < 6; ++i) {
    if (!config.read32(offset + 0x24 + 4 * i, state.bars[i])) {
      return false;
    }
  }
  if (!state.totalVfs || state.initialVfs > state.totalVfs || state.numVfs > state.totalVfs ||
      !state.supportedPageSizes ||
      (state.systemPageSize && (state.systemPageSize & (state.systemPageSize - 1) ||
                                !(state.systemPageSize & state.supportedPageSizes))) ||
      (state.enabled() && (!state.numVfs || !state.systemPageSize))) {
    return false;
  }
  result = state;
  return true;
}

inline bool vfRoutingId(uint8_t pfBus, uint8_t pfDevice, uint8_t pfFunction, const State& state,
                        uint16_t index, uint8_t& bus, uint8_t& device, uint8_t& function) {
  if (pfDevice >= 32 || pfFunction >= 8 || index >= state.numVfs || !state.firstVfOffset ||
      (state.numVfs > 1 && !state.vfStride)) {
    return false;
  }
  const uint32_t rid = (uint32_t(pfBus) << 8) + (uint32_t(pfDevice) << 3) + pfFunction +
                       state.firstVfOffset + uint32_t(index) * state.vfStride;
  if (rid > 0xffffU) {
    return false;
  }
  bus = rid >> 8;
  device = (rid >> 3) & 31U;
  function = rid & 7U;
  return true;
}
}  // namespace PciSriov

#endif
