/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_PCI_BAR_H
#define PEDIGREE_PCI_BAR_H
#include <stdint.h>
namespace PciBar {
struct Mapping {
  uint64_t base, bytes;
  bool io;
};
inline bool decode(uint32_t low, uint32_t high, uint32_t maskLow, uint32_t maskHigh,
                   Mapping& mapping) {
  mapping.io = low & 1U;
  const unsigned type = (low >> 1) & 3U;
  if ((!mapping.io && type == 3) || (mapping.io && (low & 0xffff0000U)))
    return false;
  const uint64_t width = mapping.io                 ? 0xffffU
                         : !mapping.io && type == 2 ? ~uint64_t{0}
                         : !mapping.io && type == 1 ? 0xfffffU
                                                    : 0xffffffffU;
  const uint64_t addressMask = width & (mapping.io ? ~uint64_t{3} : ~uint64_t{15});
  const uint64_t upper = !mapping.io && type == 2 ? uint64_t{high} << 32 : 0;
  const uint64_t upperMask = !mapping.io && type == 2 ? uint64_t{maskHigh} << 32 : 0;
  mapping.base = (upper | low) & addressMask;
  const uint64_t mask = (upperMask | maskLow) & addressMask;
  mapping.bytes = ((~mask) & width) + 1;
  return mapping.base && mapping.bytes && !(mapping.bytes & (mapping.bytes - 1)) &&
         !(mapping.base & (mapping.bytes - 1)) && mapping.bytes <= width - mapping.base + 1 &&
         (!mapping.io || (mapping.base < 65536 && mapping.bytes <= 65536 - mapping.base));
}
}  // namespace PciBar
#endif
