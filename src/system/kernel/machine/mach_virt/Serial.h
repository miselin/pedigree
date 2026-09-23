/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef KERNEL_MACHINE_VIRT_SERIAL_H
#define KERNEL_MACHINE_VIRT_SERIAL_H

#include "pedigree/kernel/machine/Serial.h"

/** Polled ARM PL011 UART supplied by the virt device tree. */
class VirtSerial : public Serial {
 public:
  VirtSerial();
  void setBase(uintptr_t base) override;
  bool hasData() override;
  char read() override;
  char readNonBlock() override;
  void write(char c) override;

  /** Direct console output before global constructors have run. */
  static void earlyWrite(uintptr_t base, char c);

 private:
  uintptr_t m_Base;
};

#endif
