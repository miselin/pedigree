/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef DRIVER_INTERRUPT_PROBE_H
#define DRIVER_INTERRUPT_PROBE_H

namespace InterruptProbe {
template <typename Command, typename Counter>
bool run(Command command, Counter interruptCompletions) {
  const auto before = interruptCompletions();
  const bool completed = command();
  // Polling can recover a command while its required interrupt route is dead.
  return completed && interruptCompletions() != before;
}
}  // namespace InterruptProbe
#endif
