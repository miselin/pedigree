/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef KERNEL_MACHINE_VIRT_KEYBOARD_H
#define KERNEL_MACHINE_VIRT_KEYBOARD_H

#include "pedigree/kernel/machine/Keyboard.h"

class VirtSerial;

/** Serial console input for the kernel debugger. */
class VirtKeyboard : public Keyboard {
 public:
  explicit VirtKeyboard(VirtSerial& serial);
  void initialise() override;
  void setDebugState(bool enabled) override;
  bool getDebugState() override;
  char getChar() override;
  char getCharNonBlock() override;

 private:
  VirtSerial& m_Serial;
  bool m_DebugState;
};

#endif
