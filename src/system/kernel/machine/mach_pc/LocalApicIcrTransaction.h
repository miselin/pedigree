/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef KERNEL_MACHINE_MACH_PC_LOCALAPICICRTRANSACTION_H
#define KERNEL_MACHINE_MACH_PC_LOCALAPICICRTRANSACTION_H

/**
 * Models the local interrupt boundary around one LAPIC ICR transaction.
 *
 * The ICR is per processor. Masking local maskable interrupts before its two
 * register writes prevents a same-core hard interrupt from preempting an
 * in-progress submission; no cross-CPU software ownership exists to acquire.
 */
class LocalApicIcrTransaction {
 public:
  explicit constexpr LocalApicIcrTransaction(bool interruptsWereEnabled)
      : m_InterruptsWereEnabled(interruptsWereEnabled) {}

  constexpr bool masksMaskableInterrupts() const {
    return true;
  }

  constexpr bool restoreInterrupts() const {
    return m_InterruptsWereEnabled;
  }

  constexpr bool allowsMaskableHardIrqPreemption() const {
    return false;
  }

 private:
  bool m_InterruptsWereEnabled;
};

#endif
