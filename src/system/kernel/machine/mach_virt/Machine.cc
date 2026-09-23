/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "Machine.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/panic.h"

#include "DeviceTree.h"
#include "IrqManager.h"
#include "SchedulerTimer.h"
#include "Timer.h"

namespace {
void psci(uint64_t function, bool hvc) {
  register uint64_t x0 asm("x0") = function;
  if (hvc) {
    asm volatile("hvc #0" : "+r"(x0) : : "memory");
  } else {
    asm volatile("smc #0" : "+r"(x0) : : "memory");
  }
}
}  // namespace

VirtMachine VirtMachine::m_Instance;

Machine& Machine::instance() {
  return VirtMachine::instance();
}

VirtMachine& VirtMachine::instance() {
  return m_Instance;
}

VirtMachine::VirtMachine()
    : m_Serial(), m_SerialKeyboard(m_Serial), m_Keyboard(&m_SerialKeyboard) {}

VirtMachine::~VirtMachine() {
  deinitialise();
}

void VirtMachine::initialiseDeviceTree() {
  PciBus::instance().initialise();
}

void VirtMachine::initialise() {
  if (!VirtDeviceTree::valid()) {
    panic("Virt: invalid device tree");
  }
  m_Serial.setBase(VirtDeviceTree::uartBase());
  m_SerialKeyboard.initialise();
  if (!VirtIrqManager::instance().initialise()) {
    panic("Virt: GIC initialisation failed");
  }
  if (!VirtTimer::instance().initialise1()) {
    panic("Virt: architectural clock initialisation failed");
  }
  if (!VirtSchedulerTimer::instance().initialise()) {
    panic("Virt: scheduler timer initialisation failed");
  }
  m_bInitialised = true;
}

void VirtMachine::initialise3() {
  if (!VirtTimer::instance().initialise3()) {
    panic("Virt: timer worker initialisation failed");
  }
  if (!VirtIrqManager::instance().initialiseThreaded()) {
    panic("Virt: PCI interrupt worker initialisation failed");
  }
}

void VirtMachine::deinitialise() {
  if (!m_bInitialised) {
    return;
  }
  if (!VirtIrqManager::instance().shutdownThreaded()) {
    panic("Shutdown aborted: PCI interrupt workers did not stop");
  }
  VirtTimer::instance().uninitialise();
  VirtSchedulerTimer::instance().uninitialise();
  Machine::deinitialise();
}

bool VirtMachine::supportsPowerOff() const {
  return VirtDeviceTree::psciAvailable();
}

void VirtMachine::finalShutdown(ShutdownType type) {
  if (!VirtDeviceTree::psciAvailable()) {
    return;
  }
  if (type == ShutdownType::Restart) {
    psci(0x84000009, VirtDeviceTree::psciUsesHvc());
  } else if (type == ShutdownType::PowerOff) {
    psci(0x84000008, VirtDeviceTree::psciUsesHvc());
  }
}

Serial* VirtMachine::getSerial(size_t n) {
  return n == 0 ? &m_Serial : nullptr;
}

size_t VirtMachine::getNumSerial() {
  return 1;
}

Vga* VirtMachine::getVga(size_t) {
  return nullptr;
}

size_t VirtMachine::getNumVga() {
  return 0;
}

IrqManager* VirtMachine::getIrqManager() {
  return &VirtIrqManager::instance();
}

SchedulerTimer* VirtMachine::getSchedulerTimer() {
  return &VirtSchedulerTimer::instance();
}

Timer* VirtMachine::getTimer() {
  return &VirtTimer::instance();
}

Keyboard* VirtMachine::getKeyboard() {
  return m_Keyboard;
}

void VirtMachine::setKeyboard(Keyboard* keyboard) {
  m_Keyboard = keyboard ? keyboard : &m_SerialKeyboard;
}
