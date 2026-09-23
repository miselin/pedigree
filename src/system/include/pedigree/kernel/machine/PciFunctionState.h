/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_PCI_FUNCTION_STATE_H
#define PEDIGREE_PCI_FUNCTION_STATE_H
#include "pedigree/kernel/processor/types.h"

namespace PciFunctionState {
struct State {
  uint16_t command = 0;
  uint32_t bars[6] = {};
  uint8_t interruptLine = 0, interruptPin = 0;
  uint8_t pm = 0, msi = 0, msix = 0;
};

template <class Config>
bool inspect(Config& config, State& result, bool requireLegacyInterrupt = true) {
  State state;
  uint32_t identity = 0;
  uint16_t status = 0;
  uint8_t header = 0;
  if (!config.read32(0, identity) || (identity & 0xffff) == 0xffff || !(identity & 0xffff) ||
      !config.read8(14, header) || (header & 0x7f) != 0 || !config.read16(4, state.command) ||
      !config.read16(6, status) || !config.read8(0x3c, state.interruptLine) ||
      !config.read8(0x3d, state.interruptPin))
    return false;
  if (state.interruptPin < 1 || state.interruptPin > 4) {
    return false;
  }
  // PC drivers use the programmable inputs of the legacy PIC. A valid line
  // is a routing claim, not proof of delivery on either platform.
  if (requireLegacyInterrupt &&
      (state.interruptLine >= 16 || !(0xdef8U & (1U << state.interruptLine)))) {
    return false;
  }
  for (unsigned i = 0; i < 6; ++i)
    if (!config.read32(0x10 + 4 * i, state.bars[i]))
      return false;
  uint8_t cap = 0;
  if ((status & 0x10) && !config.read8(0x34, cap))
    return false;
  uint64_t occupied = 0;
  while (cap) {
    if (cap < 0x40 || (cap & 3))
      return false;
    uint8_t type = 0, next = 0;
    uint16_t control = 0;
    if (!config.read8(cap, type) || !config.read8(cap + 1, next) ||
        !config.read16(cap + 2, control))
      return false;
    unsigned bytes = 4;
    if (type == 1) {
      if (state.pm)
        return false;
      state.pm = cap;
      bytes = 8;
    } else if (type == 5) {
      if (state.msi)
        return false;
      state.msi = cap;
      bytes = (control & 0x80) ? 14 : 10;
      if (control & 0x100)
        bytes += 10;
    } else if (type == 0x11) {
      if (state.msix)
        return false;
      state.msix = cap;
      bytes = 12;
    }
    if (cap + bytes > 256)
      return false;
    for (unsigned slot = cap / 4; slot < (cap + bytes + 3) / 4; ++slot) {
      if (occupied & (1ULL << slot))
        return false;
      occupied |= 1ULL << slot;
    }
    if (type == 1) {
      uint16_t pmcsr = 0;
      if (!config.read16(cap + 4, pmcsr) || (pmcsr & 3))
        return false;
    }
    cap = next;
  }
  result = state;
  return true;
}

template <class Config>
bool disableMessageInterrupts(Config& config, const State& state) {
  const uint8_t capabilities[] = {state.msi, state.msix};
  for (uint8_t cap : capabilities) {
    if (!cap)
      continue;
    uint16_t control = 0, actual = 0;
    if (!config.read16(cap + 2, control))
      return false;
    const uint16_t desired = cap == state.msi ? control & ~1U : (control | 0x4000U) & ~0x8000U;
    if (!config.write16(cap + 2, desired) || !config.read16(cap + 2, actual) || actual != desired)
      return false;
  }
  return true;
}

template <class Config>
bool resourcesUnchanged(Config& config, const State& state) {
  for (unsigned i = 0; i < 6; ++i) {
    uint32_t bar = 0;
    if (!config.read32(0x10 + 4 * i, bar) || bar != state.bars[i])
      return false;
  }
  uint8_t line = 0, pin = 0;
  return config.read8(0x3c, line) && config.read8(0x3d, pin) && line == state.interruptLine &&
         pin == state.interruptPin;
}
}  // namespace PciFunctionState
#endif
