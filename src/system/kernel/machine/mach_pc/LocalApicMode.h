/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_KERNEL_MACHINE_MACH_PC_LOCALAPICMODE_H
#define PEDIGREE_KERNEL_MACHINE_MACH_PC_LOCALAPICMODE_H
#include <stdint.h>

class LocalApicMode {
 public:
  enum class Mode { Disabled, XApic, X2Apic, Invalid };
  static constexpr uint32_t BaseMsr = 0x1B;
  static constexpr uint64_t Enabled = 1ULL << 11;
  static constexpr uint64_t Extended = 1ULL << 10;

  static constexpr Mode decode(uint64_t apicBase) {
    return (apicBase & Enabled) ? ((apicBase & Extended) ? Mode::X2Apic : Mode::XApic)
                                : ((apicBase & Extended) ? Mode::Invalid : Mode::Disabled);
  }
};
#endif
