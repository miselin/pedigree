/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_PCI_FUNCTION_STATE_H
#define PEDIGREE_PCI_FUNCTION_STATE_H
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"

namespace PciFunctionState {
struct State {
  uint16_t command = 0;
  uint32_t bars[6] = {};
  uint8_t barCount = 6;
  uint8_t interruptLine = 0, interruptPin = 0;
  uint8_t pm = 0, msi = 0, msix = 0;
};

struct MsixTable {
  uint8_t bar = 0, pbaBar = 0;
  uint32_t offset = 0, pbaOffset = 0;
  uint16_t vectors = 0;
};

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

// virtualFunction is supplied by the PF owner; raw VF Vendor/Device IDs may
// read FFFF:FFFF. Header and capability checks still establish configuration.
template <class Config>
bool inspect(Config& config, State& result, bool requireLegacyInterrupt = true,
             bool virtualFunction = false) {
  State state;
  uint32_t identity = 0;
  uint16_t status = 0;
  uint8_t header = 0;
  if (!config.read32(0, identity) ||
      (((identity & 0xffff) == 0xffff || !(identity & 0xffff)) &&
       !(virtualFunction && identity == 0xffffffffU)) ||
      !config.read8(14, header) || (header & 0x7f) > (virtualFunction ? 0 : 1) ||
      !config.read16(4, state.command) || !config.read16(6, status) ||
      !config.read8(0x3c, state.interruptLine) || !config.read8(0x3d, state.interruptPin)) {
    return false;
  }
  state.barCount = (header & 0x7f) == 1 ? 2 : 6;
  if (state.interruptPin > 4 || (requireLegacyInterrupt && !state.interruptPin)) {
    return false;
  }
  // PC drivers use the programmable inputs of the legacy PIC. A valid line
  // is a routing claim, not proof of delivery on either platform.
  if (requireLegacyInterrupt &&
      (state.interruptLine >= 16 || !(0xdef8U & (1U << state.interruptLine)))) {
    return false;
  }
  for (unsigned i = 0; i < state.barCount; ++i) {
    if (!config.read32(0x10 + 4 * i, state.bars[i])) {
      return false;
    }
  }
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
  if (!requireLegacyInterrupt && !state.interruptPin && !state.msi && !state.msix)
    return false;
  result = state;
  return true;
}

template <class Config>
bool msixTable(Config& config, const State& state, MsixTable& result) {
  if (!state.msix || (state.barCount != 2 && state.barCount != 6)) {
    return false;
  }
  uint16_t control = 0;
  uint32_t table = 0, pba = 0;
  if (!config.read16(state.msix + 2, control) || !config.read32(state.msix + 4, table) ||
      !config.read32(state.msix + 8, pba) || (table & 7U) >= state.barCount ||
      (pba & 7U) >= state.barCount) {
    return false;
  }
  const uint16_t vectors = (control & 0x7ffU) + 1;
  const uint8_t bar = table & 7U, pbaBar = pba & 7U;
  const uint64_t tableEnd = uint64_t(table & ~7U) + uint64_t(vectors) * 16;
  const uint64_t pbaEnd = uint64_t(pba & ~7U) + uint64_t((vectors + 63) / 64) * 8;
  if (tableEnd > 0x100000000ULL || pbaEnd > 0x100000000ULL ||
      (bar == pbaBar && (table & ~7U) < pbaEnd && (pba & ~7U) < tableEnd))
    return false;
  result = {bar, pbaBar, table & ~7U, pba & ~7U, vectors};
  return true;
}

template <class Config>
bool enableMsi(Config& config, const State& state, uint64_t address, uint16_t data) {
  if (!state.msi || (address & 3U))
    return false;
  uint16_t control = 0, command = 0, oldData = 0, msixControl = 0;
  uint32_t oldLow = 0, oldHigh = 0, mask = 0;
  if (!config.read16(state.msi + 2, control) || !config.read16(4, command) ||
      !config.read32(state.msi + 4, oldLow) || (control & 1U) ||
      (state.msix && (!config.read16(state.msix + 2, msixControl) || (msixControl & 0x8000U))))
    return false;
  const bool wide = control & 0x80U;
  if (!wide && (address >> 32))
    return false;
  const uint16_t dataOffset = state.msi + (wide ? 12 : 8);
  const uint16_t maskOffset = state.msi + (wide ? 16 : 12);
  if ((wide && !config.read32(state.msi + 8, oldHigh)) || !config.read16(dataOffset, oldData) ||
      ((control & 0x100U) && !config.read32(maskOffset, mask)))
    return false;

  const bool programmed =
      writeVerified32(config, state.msi + 4, uint32_t(address)) &&
      (!wide || writeVerified32(config, state.msi + 8, uint32_t(address >> 32))) &&
      writeVerified16(config, dataOffset, data) &&
      (!(control & 0x100U) || writeVerified32(config, maskOffset, mask & ~1U)) &&
      writeVerified16(config, 4, command | 0x400U) &&
      writeVerified16(config, state.msi + 2, (control & ~0x70U) | 1U);
  if (programmed)
    return true;

  // Keep the source disabled while restoring the registers after a partial write.
  config.write16(state.msi + 2, control & ~1U);
  config.write32(state.msi + 4, oldLow);
  if (wide)
    config.write32(state.msi + 8, oldHigh);
  config.write16(dataOffset, oldData);
  if (control & 0x100U)
    config.write32(maskOffset, mask);
  config.write16(4, command);
  return false;
}

template <class Config>
bool disableMsi(Config& config, const State& state) {
  if (!state.msi)
    return false;
  uint16_t control = 0;
  if (!config.read16(state.msi + 2, control))
    return false;
  if (!(control & 1U))
    return true;
  bool masked = true;
  if (control & 0x100U) {
    uint32_t mask = 0;
    const uint16_t offset = state.msi + ((control & 0x80U) ? 16 : 12);
    masked = config.read32(offset, mask) && writeVerified32(config, offset, mask | 1U);
  }
  const bool disabled = writeVerified16(config, state.msi + 2, control & ~1U);
  return masked && disabled;
}

template <class Config, class Io>
bool enableMsixVectors(Config& config, const State& state, Io& table, uint32_t offset,
                       uint64_t address, const uint32_t* data, size_t count,
                       const uint64_t* addresses = nullptr) {
  constexpr size_t MaxVectors = 64;
  if (!state.msix || !data || !count || count > MaxVectors ||
      uint64_t(offset) + count * 16 > table.size())
    return false;
  for (size_t i = 0; i < count; ++i) {
    if ((addresses ? addresses[i] : address) & 3U) {
      return false;
    }
  }
  uint16_t control = 0, command = 0, msiControl = 0;
  if (!config.read16(state.msix + 2, control) || !config.read16(4, command) ||
      (control & 0x8000U) || count > (control & 0x7ffU) + 1 ||
      (state.msi && (!config.read16(state.msi + 2, msiControl) || (msiControl & 1U))))
    return false;
  uint32_t previous[MaxVectors][4];
  for (size_t i = 0; i < count; ++i) {
    const uint32_t entry = offset + i * 16;
    for (size_t word = 0; word < 4; ++word)
      previous[i][word] = table.read32(entry + word * 4);
  }
  if (!writeVerified16(config, state.msix + 2, control | 0x4000U)) {
    config.write16(state.msix + 2, control);
    return false;
  }
  bool programmed = true;
  for (size_t i = 0; i < count; ++i) {
    const uint32_t entry = offset + i * 16;
    table.write32(previous[i][3] | 1U, entry + 12);
    const uint64_t destination = addresses ? addresses[i] : address;
    table.write32(uint32_t(destination), entry);
    table.write32(uint32_t(destination >> 32), entry + 4);
    table.write32(data[i], entry + 8);
  }
  FENCE();
  for (size_t i = 0; i < count; ++i) {
    const uint32_t entry = offset + i * 16;
    const uint64_t destination = addresses ? addresses[i] : address;
    programmed = programmed && (table.read32(entry + 12) & 1U) &&
                 table.read32(entry) == uint32_t(destination) &&
                 table.read32(entry + 4) == uint32_t(destination >> 32) &&
                 table.read32(entry + 8) == data[i];
  }
  if (programmed)
    programmed = writeVerified16(config, 4, command | 0x400U) &&
                 writeVerified16(config, state.msix + 2, control | 0xc000U);
  if (programmed) {
    for (size_t i = 0; i < count; ++i)
      table.write32(previous[i][3] & ~1U, offset + i * 16 + 12);
    FENCE();
    for (size_t i = 0; i < count; ++i)
      programmed = programmed && !(table.read32(offset + i * 16 + 12) & 1U);
    if (programmed && writeVerified16(config, state.msix + 2, (control | 0x8000U) & ~0x4000U))
      return true;
  }

  // Do not rewrite table entries until the function has stopped emitting them.
  if (!writeVerified16(config, state.msix + 2, (control | 0x4000U) & ~0x8000U))
    return false;
  for (size_t i = 0; i < count; ++i) {
    const uint32_t entry = offset + i * 16;
    table.write32(previous[i][3] | 1U, entry + 12);
    for (size_t word = 0; word < 3; ++word)
      table.write32(previous[i][word], entry + word * 4);
    table.write32(previous[i][3], entry + 12);
  }
  FENCE();
  config.write16(state.msix + 2, control);
  config.write16(4, command);
  return false;
}

template <class Config, class Io>
bool enableMsix(Config& config, const State& state, Io& table, uint32_t offset, uint64_t address,
                uint32_t data) {
  return enableMsixVectors(config, state, table, offset, address, &data, 1);
}

template <class Config, class Io>
bool setMsixVectorMask(Config& config, const State& state, Io& table, uint32_t offset, size_t index,
                       bool masked) {
  uint16_t control = 0;
  if (!state.msix || !config.read16(state.msix + 2, control) || index >= (control & 0x7ffU) + 1 ||
      uint64_t(offset) + (index + 1) * 16 > table.size() || (!masked && !(control & 0x8000U)))
    return false;
  const uint32_t entryControl = table.read32(offset + index * 16 + 12);
  const uint32_t desired = masked ? entryControl | 1U : entryControl & ~1U;
  table.write32(desired, offset + index * 16 + 12);
  FENCE();
  return table.read32(offset + index * 16 + 12) == desired;
}

template <class Config, class Io>
bool disableMsix(Config& config, const State& state, Io& table, uint32_t offset) {
  if (!state.msix || uint64_t(offset) + 16 > table.size())
    return false;
  uint16_t control = 0;
  if (!config.read16(state.msix + 2, control))
    return false;
  if (!writeVerified16(config, state.msix + 2, control | 0x4000U))
    return false;
  const uint32_t entryControl = table.read32(offset + 12);
  table.write32(entryControl | 1U, offset + 12);
  FENCE();
  return (table.read32(offset + 12) & 1U) &&
         writeVerified16(config, state.msix + 2, (control | 0x4000U) & ~0x8000U);
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
bool resourcesUnchanged(Config& config, const State& state, bool virtualFunction = false) {
  if (state.barCount != 2 && state.barCount != 6) {
    return false;
  }
  uint32_t identity = 0;
  uint8_t header = 0;
  if (!config.read32(0, identity) ||
      (((identity & 0xffff) == 0xffff || !(identity & 0xffff)) &&
       !(virtualFunction && identity == 0xffffffffU)) ||
      !config.read8(14, header) || (header & 0x7f) > (virtualFunction ? 0 : 1) ||
      state.barCount != ((header & 0x7f) == 1 ? 2 : 6)) {
    return false;
  }
  for (unsigned i = 0; i < state.barCount; ++i) {
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
