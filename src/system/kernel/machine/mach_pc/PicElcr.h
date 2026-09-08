/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PIC_ELCR_H
#define PIC_ELCR_H
#include "pedigree/kernel/processor/types.h"

constexpr uint16_t PicElcrProgrammable = 0xdef8;

template <class Read, class Write>
bool updatePicElcr(uint8_t irq, bool level, Read read, Write write, uint8_t& previous) {
  if (irq >= 16 || !(PicElcrProgrammable & (uint16_t{1} << irq)))
    return false;
  const size_t bank = irq / 8;
  const uint8_t bit = 1U << (irq % 8);
  previous = read(bank);
  if (previous & ~static_cast<uint8_t>(PicElcrProgrammable >> (bank * 8)))
    return false;
  const uint8_t desired = level ? previous | bit : previous & ~bit;
  if (desired != previous)
    write(bank, desired);
  if (read(bank) == desired)
    return true;
  write(bank, previous);
  (void)read(bank);
  return false;
}
#endif
