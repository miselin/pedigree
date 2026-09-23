/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/machine/Trace.h"

#include "DeviceTree.h"
#include "Serial.h"

namespace pedigree_trace {
void trace(const char* message) {
  const uintptr_t base = VirtDeviceTree::uartBase();
  if (!base) {
    return;
  }

  for (; *message; ++message) {
    VirtSerial::earlyWrite(base, *message);
  }
  VirtSerial::earlyWrite(base, '\n');
}
}  // namespace pedigree_trace
