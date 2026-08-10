/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_MACHINE_MACH_PC_LOCALAPICLINT0POLICY_H
#define PEDIGREE_KERNEL_MACHINE_MACH_PC_LOCALAPICLINT0POLICY_H

#include "pedigree/kernel/processor/types.h"

/** Executable virtual-wire routing policy for the legacy dual 8259 PIC. */
class LocalApicLint0Policy {
 public:
  static constexpr uint32_t ApicBaseMsr = 0x1B;
  static constexpr uint64_t BootstrapProcessor = 1ULL << 8;
  static constexpr uint32_t ProgrammableFields = 0x0001A7FF;
  static constexpr uint32_t ExtInt = 7U << 8;
  static constexpr uint32_t Masked = 1U << 16;
  static constexpr uint32_t BootstrapValue = ExtInt;
  static constexpr uint32_t ApplicationProcessorValue = Masked | 0xFF;

  static bool isBootstrapProcessor(uint64_t apicBase) {
    return (apicBase & BootstrapProcessor) != 0;
  }

  static uint32_t configuredValue(uint32_t current, uint64_t apicBase) {
    const uint32_t desired =
        isBootstrapProcessor(apicBase) ? BootstrapValue : ApplicationProcessorValue;
    return (current & ~ProgrammableFields) | desired;
  }

  static bool routesLegacyPic(uint32_t value) {
    return (value & (Masked | 0x700)) == ExtInt;
  }

  static bool matchesRole(uint32_t value, uint64_t apicBase) {
    const uint32_t expected =
        isBootstrapProcessor(apicBase) ? BootstrapValue : ApplicationProcessorValue;
    return (value & ProgrammableFields) == expected;
  }
};

#endif
