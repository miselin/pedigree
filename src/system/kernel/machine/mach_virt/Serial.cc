/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "Serial.h"

namespace {
constexpr uintptr_t Data = 0x000;
constexpr uintptr_t Flags = 0x018;
constexpr uintptr_t IntegerBaud = 0x024;
constexpr uintptr_t FractionalBaud = 0x028;
constexpr uintptr_t LineControl = 0x02c;
constexpr uintptr_t Control = 0x030;
constexpr uintptr_t InterruptMask = 0x038;
constexpr uintptr_t InterruptClear = 0x044;
constexpr uint32_t TxFull = 1U << 5;
constexpr uint32_t RxEmpty = 1U << 4;
constexpr size_t PollLimit = 100000;

volatile uint32_t& registerAt(uintptr_t base, uintptr_t offset) {
  return *reinterpret_cast<volatile uint32_t*>(base + offset);
}
}  // namespace

VirtSerial::VirtSerial() : m_Base(0) {}

void VirtSerial::setBase(uintptr_t base) {
  m_Base = base;
  if (!base) {
    return;
  }

  registerAt(base, Control) = 0;
  registerAt(base, InterruptMask) = 0;
  registerAt(base, InterruptClear) = 0x7ff;
  // QEMU virt exposes a 24 MHz PL011 clock. 13 + 1/64 gives 115200 baud.
  registerAt(base, IntegerBaud) = 13;
  registerAt(base, FractionalBaud) = 1;
  registerAt(base, LineControl) = (3U << 5) | (1U << 4);
  registerAt(base, Control) = (1U << 9) | (1U << 8) | 1U;
}

void VirtSerial::earlyWrite(uintptr_t base, char c) {
  if (!base) {
    return;
  }
  if (!(registerAt(base, Control) & 1U)) {
    registerAt(base, Control) = 0;
    registerAt(base, InterruptMask) = 0;
    registerAt(base, IntegerBaud) = 13;
    registerAt(base, FractionalBaud) = 1;
    registerAt(base, LineControl) = (3U << 5) | (1U << 4);
    registerAt(base, Control) = (1U << 9) | (1U << 8) | 1U;
  }
  for (size_t i = 0; i < PollLimit; ++i) {
    if (!(registerAt(base, Flags) & TxFull)) {
      registerAt(base, Data) = static_cast<uint8_t>(c);
      return;
    }
    asm volatile("yield");
  }
}

bool VirtSerial::hasData() {
  return m_Base && !(registerAt(m_Base, Flags) & RxEmpty);
}

char VirtSerial::read() {
  if (!m_Base) {
    return 0;
  }
  while (!hasData()) {
    asm volatile("yield");
  }
  return static_cast<char>(registerAt(m_Base, Data));
}

char VirtSerial::readNonBlock() {
  return hasData() ? static_cast<char>(registerAt(m_Base, Data)) : 0;
}

void VirtSerial::write(char c) {
  earlyWrite(m_Base, c);
}
