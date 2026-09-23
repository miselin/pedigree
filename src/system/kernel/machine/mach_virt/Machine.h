/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef KERNEL_MACHINE_VIRT_MACHINE_H
#define KERNEL_MACHINE_VIRT_MACHINE_H

#include "pedigree/kernel/machine/Machine.h"

#include "Keyboard.h"
#include "Serial.h"

class VirtMachine : public Machine {
 public:
  static VirtMachine& instance();

  void initialise() override;
  void initialiseDeviceTree() override;
  void initialise3() override;
  void deinitialise() override;
  bool supportsPowerOff() const override;
  void finalShutdown(ShutdownType type) override;

  Serial* getSerial(size_t n) override;
  size_t getNumSerial() override;
  Vga* getVga(size_t n) override;
  size_t getNumVga() override;
  IrqManager* getIrqManager() override;
  SchedulerTimer* getSchedulerTimer() override;
  Timer* getTimer() override;
  Keyboard* getKeyboard() override;
  void setKeyboard(Keyboard* keyboard) override;

 private:
  VirtMachine();
  ~VirtMachine() override;

  VirtSerial m_Serial;
  VirtKeyboard m_SerialKeyboard;
  Keyboard* m_Keyboard;

  static VirtMachine m_Instance;
};

#endif
