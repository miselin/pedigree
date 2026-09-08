/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef EHCI_LEGACY_H
#define EHCI_LEGACY_H
#include <stdint.h>

namespace EhciLegacy {
// EHCI 1.0 section 2.1.8: status shadows are RO, the remaining reserved
// fields are zero, and only the three upper status bits are W1C.
constexpr uint32_t SmiEnables = 0x0000e03fU;
constexpr uint32_t AcknowledgeStatus = 0xe0000000U;

template <typename Read, typename Write>
bool disableSmis(uint16_t capability, Read read, Write write) {
  if (capability < 0x40 || capability > 0xf8 || (capability & 3))
    return false;
  const uint16_t control = capability + 4;
  uint32_t value = 0;
  return write(control, AcknowledgeStatus) && read(control, value) && !(value & SmiEnables);
}
}  // namespace EhciLegacy
#endif
