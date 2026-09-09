/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_CHIPSET_INTEL_PIRQ_H
#define PEDIGREE_CHIPSET_INTEL_PIRQ_H
#include "pedigree/kernel/processor/types.h"

// Intel 6 Series datasheet 324645, sections 10.1.20-34 and 13.1.16/18.
// Preserve firmware pin selection and device-to-PIRQ routing.
namespace IntelPirq {
inline uint16_t routeOffset(uint8_t device) {
  switch (device) {
    case 31:
      return 0x3140;
    case 29:
      return 0x3144;
    case 27:
      return 0x3148;
    case 26:
      return 0x314c;
    case 25:
      return 0x3150;
    case 22:
      return 0x315c;
    default:
      return 0;
  }
}
inline bool decode(uint16_t routes, uint8_t pin, uint8_t& pirq) {
  if (pin < 1 || pin > 4 || routes == 0xffff)
    return false;
  const uint8_t selector = (routes >> (4 * (pin - 1))) & 0xf;
  if (selector > 7)
    return false;
  pirq = selector;
  return true;
}
inline uint16_t pinOffset(uint8_t device) {
  switch (device) {
    case 31:
      return 0x3100;
    case 29:
      return 0x3108;
    case 27:
      return 0x3110;
    case 26:
      return 0x3114;
    case 25:
      return 0x3118;
    case 22:
      return 0x3124;
    default:
      return 0;
  }
}
inline bool nativePinMatches(uint32_t pins, uint8_t function, uint8_t pin) {
  return pins != 0xffffffff && function < 8 && pin >= 1 && pin <= 4 &&
         ((pins >> (4 * function)) & 0xf) == pin;
}
inline bool chipsetExclusions(uint8_t acpiControl, uint8_t tcoControl, uint16_t& excluded) {
  if (acpiControl == 0xff || tcoControl == 0xff || (acpiControl & 7) == 3 ||
      ((tcoControl & 0x80) && (tcoControl & 7) == 3))
    return false;
  excluded = 0;
  if ((acpiControl & 7) < 3)
    excluded |= uint16_t{1} << (9 + (acpiControl & 7));
  if ((tcoControl & 0x80) && (tcoControl & 7) < 3)
    excluded |= uint16_t{1} << (9 + (tcoControl & 7));
  return true;
}
inline uint16_t hpetExclusion(uint32_t config, uint32_t pending, unsigned timer,
                              bool legacyReplacement) {
  if (config == 0xffffffff || pending == 0xffffffff)
    return (1U << 10) | (1U << 11);
  if ((config & (1U << 14)) || (legacyReplacement && timer < 2))
    return 0;
  const unsigned irq = (config >> 9) & 31;
  const bool active = (config & 4) || ((config & 2) && (pending & (1U << timer)));
  return active && irq < 16 ? uint16_t{1} << irq : 0;
}
inline uint16_t configOffset(uint8_t pirq) {
  return pirq < 4 ? 0x60 + pirq : 0x68 + pirq - 4;
}
inline bool validIrq(uint8_t irq) {
  return irq < 16 && (0xdef8U & (1U << irq));
}

enum class Result { Ready, ReadFailed, Unsupported, Conflict, WriteFailed, RestoreFailed };

// Reserve PIC ownership before connecting a previously disabled PIRQ. An enabled
// firmware route is preserved unless the caller explicitly identifies its line
// as unavailable to PCI (for example, the fixed PS/2 mouse line).
template <class Config, class Reserve>
Result establish(Config& config, Reserve reserve, uint8_t pirq, uint16_t excluded, uint8_t& irq,
                 bool rerouteExcludedEnabled = false) {
  if (pirq >= 8)
    return Result::Unsupported;
  const uint16_t offset = configOffset(pirq);
  uint8_t original = 0;
  if (!config.read8(offset, original) || original == 0xff)
    return Result::ReadFailed;
  if (!(original & 0x80)) {
    const uint8_t selected = original & 0xf;
    if (validIrq(selected) && !(excluded & (1U << selected))) {
      if (!reserve(selected))
        return Result::Conflict;
      irq = selected;
      return Result::Ready;
    }
    if (!rerouteExcludedEnabled)
      return Result::Unsupported;
  }
  // Keep legacy timer/keyboard/serial/storage lines and the SCI out of new
  // assignments. Sharing a reserved PCI level line is supported by the PIC.
  const uint8_t choices[] = {static_cast<uint8_t>(10 + (pirq & 1)),
                             static_cast<uint8_t>(11 - (pirq & 1))};
  for (uint8_t selected : choices) {
    if ((excluded & (1U << selected)) || !reserve(selected))
      continue;
    const uint8_t desired = (original & 0x70) | selected;
    uint8_t actual = 0;
    if (config.write8(offset, desired) && config.read8(offset, actual) && actual == desired) {
      irq = selected;
      return Result::Ready;
    }
    // Keep the reservation even on failure: a posted/partial write must never
    // leave an electrically connected PIRQ on a line another driver can claim.
    if (!config.write8(offset, original) || !config.read8(offset, actual) || actual != original)
      return Result::RestoreFailed;
    return Result::WriteFailed;
  }
  return Result::Conflict;
}
}  // namespace IntelPirq
#endif
