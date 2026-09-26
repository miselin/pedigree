/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_PCI_SRIOV_H
#define PEDIGREE_PCI_SRIOV_H

#include "pedigree/kernel/processor/types.h"

namespace PciSriov {
constexpr uint16_t CapabilityId = 0x0010;
constexpr uint16_t VfEnable = 1U;
constexpr uint16_t VfMemoryEnable = 8U;
constexpr uint32_t PageSize4K = 1U;
constexpr uint16_t VfControl = VfEnable | VfMemoryEnable;

enum class Result { Success, Invalid, IoError, RestoreFailed, RollbackFailed };

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

inline bool vfRoutingId(uint8_t pfBus, uint8_t pfDevice, uint8_t pfFunction, const State& state,
                        uint16_t index, uint8_t& bus, uint8_t& device, uint8_t& function);

struct Bar {
  uint64_t base = 0;
  uint64_t perVfBytes = 0;
  uint64_t apertureBytes = 0;
  bool io = false;
  bool wide = false;
  bool prefetchable = false;
};

struct Geometry {
  Bar bars[6] = {};
};

struct Plan {
  uint8_t pfBus = 0;
  uint8_t pfDevice = 0;
  uint8_t pfFunction = 0;
  uint16_t numVfs = 0;
  uint16_t firstVfOffset = 0;
  uint16_t vfStride = 0;
  uint16_t firstRid = 0;
  uint16_t lastRid = 0;
  uint32_t originalPageSize = 0;
};

inline bool validOffset(uint16_t offset, uint16_t next) {
  constexpr uint16_t RequiredBytes = 0x3c;
  return offset >= 0x100 && !(offset & 3U) && offset <= 0x1000 - RequiredBytes &&
         !(next && next >= offset && next < offset + RequiredBytes);
}

template <class Config>
bool read(Config& config, uint16_t offset, uint16_t next, State& result) {
  if (!validOffset(offset, next)) {
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

/** The caller must reject RID collisions and RIDs outside the upstream bridge's bus window. */
inline bool plan(uint8_t pfBus, uint8_t pfDevice, uint8_t pfFunction, const State& state,
                 uint16_t count, Plan& result) {
  if (pfDevice >= 32 || pfFunction >= 8 || (state.control & VfControl) || state.numVfs || !count ||
      count > state.initialVfs || count > state.totalVfs ||
      !(state.supportedPageSizes & PageSize4K)) {
    return false;
  }
  State requested = state;
  requested.numVfs = count;
  uint8_t firstBus = 0, firstDevice = 0, firstFunction = 0;
  uint8_t lastBus = 0, lastDevice = 0, lastFunction = 0;
  if (!vfRoutingId(pfBus, pfDevice, pfFunction, requested, 0, firstBus, firstDevice,
                   firstFunction) ||
      !vfRoutingId(pfBus, pfDevice, pfFunction, requested, count - 1, lastBus, lastDevice,
                   lastFunction)) {
    return false;
  }
  result = Plan{pfBus,
                pfDevice,
                pfFunction,
                count,
                state.firstVfOffset,
                state.vfStride,
                uint16_t((uint16_t(firstBus) << 8) | (firstDevice << 3) | firstFunction),
                uint16_t((uint16_t(lastBus) << 8) | (lastDevice << 3) | lastFunction),
                state.systemPageSize};
  return true;
}

template <class Config>
bool writeVerified16(Config& config, uint16_t offset, uint16_t value) {
  uint16_t actual = 0;
  return config.write16(offset, value) && config.read16(offset, actual) && actual == value;
}

template <class Config>
bool writeVerified32(Config& config, uint16_t offset, uint32_t value) {
  uint32_t actual = 0;
  return config.write32(offset, value) && config.read32(offset, actual) && actual == value;
}

/** VF BAR writes are safe only after the caller has quiesced the PF and all VFs. */
template <class Config>
Result probeBars(Config& config, uint16_t offset, uint16_t next, const State& state,
                 Geometry& result) {
  if (!validOffset(offset, next) || !state.totalVfs || (state.control & VfControl)) {
    return Result::Invalid;
  }
  uint16_t control = 0;
  uint32_t original[6] = {}, masks[6] = {};
  if (!config.read16(offset + 0x08, control)) {
    return Result::IoError;
  }
  if (control != state.control || (control & VfControl)) {
    return Result::Invalid;
  }
  for (unsigned i = 0; i < 6; ++i) {
    if (!config.read32(offset + 0x24 + 4 * i, original[i]) || original[i] != state.bars[i]) {
      return Result::Invalid;
    }
  }

  bool probed = true;
  for (unsigned i = 0; i < 6; ++i) {
    if (!config.write32(offset + 0x24 + 4 * i, 0xffffffffU)) {
      probed = false;
      break;
    }
  }
  if (probed) {
    for (unsigned i = 0; i < 6; ++i) {
      if (!config.read32(offset + 0x24 + 4 * i, masks[i])) {
        probed = false;
        break;
      }
    }
  }
  bool restored = true;
  for (unsigned i = 0; i < 6; ++i) {
    if (!config.write32(offset + 0x24 + 4 * i, original[i])) {
      restored = false;
    }
  }
  for (unsigned i = 0; i < 6; ++i) {
    uint32_t actual = 0;
    if (!config.read32(offset + 0x24 + 4 * i, actual) || actual != original[i]) {
      restored = false;
    }
  }
  if (!restored) {
    return writeVerified16(config, offset + 0x08, control & ~VfControl) ? Result::RestoreFailed
                                                                        : Result::RollbackFailed;
  }
  if (!probed) {
    return Result::IoError;
  }

  Geometry geometry;
  for (unsigned i = 0; i < 6; ++i) {
    const uint32_t low = original[i];
    const uint32_t maskLow = masks[i];
    if (!low && !maskLow) {
      continue;
    }
    const bool io = low & 1U;
    const unsigned type = (low >> 1) & 3U;
    if ((io && ((low ^ maskLow) & 3U)) ||
        (!io && (((low ^ maskLow) & 15U) || type == 1 || type == 3))) {
      return Result::Invalid;
    }
    const bool wide = !io && type == 2;
    if (wide && i == 5) {
      return Result::Invalid;
    }
    const uint64_t base = wide ? ((uint64_t(original[i + 1]) << 32) | (low & ~15U))
                               : uint64_t(low & (io ? ~3U : ~15U));
    const uint64_t mask = wide ? ((uint64_t(masks[i + 1]) << 32) | (maskLow & ~15U))
                               : uint64_t(maskLow & (io ? ~3U : ~15U));
    const uint64_t bytes = wide ? ~mask + 1 : uint64_t(~uint32_t(mask)) + 1;
    if (!bytes || (bytes & (bytes - 1)) || bytes > ~uint64_t{0} / state.totalVfs ||
        (base && (base & (bytes - 1)))) {
      return Result::Invalid;
    }
    const uint64_t aperture = bytes * state.totalVfs;
    const uint64_t limit = io ? 0xffffU : wide ? ~uint64_t{0} : 0xffffffffU;
    if (base > limit || aperture - 1 > limit - base) {
      return Result::Invalid;
    }
    geometry.bars[i] = Bar{base, bytes, aperture, io, wide, !io && bool(low & 8U)};
    if (wide) {
      ++i;
    }
  }
  result = geometry;
  return Result::Success;
}

/** The caller owns VF resources, driver binding, and the delay before VF enumeration. */
template <class Config>
Result enable(Config& config, uint16_t offset, uint16_t next, const Plan& request) {
  State state;
  if (!read(config, offset, next, state)) {
    return Result::IoError;
  }
  Plan expected;
  if (!plan(request.pfBus, request.pfDevice, request.pfFunction, state, request.numVfs, expected) ||
      expected.firstRid != request.firstRid || expected.lastRid != request.lastRid ||
      expected.firstVfOffset != request.firstVfOffset || expected.vfStride != request.vfStride ||
      expected.originalPageSize != request.originalPageSize) {
    return Result::Invalid;
  }

  if (writeVerified32(config, offset + 0x20, PageSize4K) &&
      writeVerified16(config, offset + 0x10, request.numVfs)) {
    State staged;
    if (read(config, offset, next, staged) && staged.firstVfOffset == request.firstVfOffset &&
        staged.vfStride == request.vfStride && !(staged.control & VfControl) &&
        writeVerified16(config, offset + 0x08, state.control | VfControl)) {
      State enabled;
      if (read(config, offset, next, enabled) && enabled.enabled() &&
          (enabled.control & VfMemoryEnable) && enabled.numVfs == request.numVfs &&
          enabled.systemPageSize == PageSize4K) {
        return Result::Success;
      }
    }
  }
  const bool controlRestored = writeVerified16(config, offset + 0x08, state.control & ~VfControl);
  const bool countRestored = writeVerified16(config, offset + 0x10, 0);
  const bool pageRestored = writeVerified32(config, offset + 0x20, state.systemPageSize);
  return controlRestored && countRestored && pageRestored ? Result::IoError
                                                          : Result::RollbackFailed;
}

/** The caller must quiesce VFs and wait before reusing their resources. */
template <class Config>
Result disable(Config& config, uint16_t offset, uint16_t next) {
  State state;
  if (!read(config, offset, next, state)) {
    return Result::RollbackFailed;
  }
  if (!writeVerified16(config, offset + 0x08, state.control & ~VfControl)) {
    return Result::RollbackFailed;
  }
  return writeVerified16(config, offset + 0x10, 0) ? Result::Success : Result::IoError;
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
