/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef KERNEL_MACHINE_X86_COMMON_PCSCHEDULERTIMERSELECTION_H
#define KERNEL_MACHINE_X86_COMMON_PCSCHEDULERTIMERSELECTION_H
#include <config.h>

/**
 * Runtime scheduler-timer choice for PC machines.
 *
 * PIT is the safe default. The local APIC timer is selected only after its
 * complete bootstrap-processor initialisation has succeeded.
 */
class PcSchedulerTimerSelection {
 public:
  enum class Source {
    Pit,
    LocalApic,
  };

  PcSchedulerTimerSelection() : m_Source(Source::Pit) {}

  void recordLocalApicInitialisation(bool succeeded) {
    m_Source = succeeded ? Source::LocalApic : Source::Pit;
  }

  Source source() const {
    return m_Source;
  }

  bool usesPit() const {
    return m_Source == Source::Pit;
  }

  bool usesLocalApic() const {
    return m_Source == Source::LocalApic;
  }

 private:
  Source m_Source;
};

#endif
