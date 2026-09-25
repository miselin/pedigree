/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_PC_PCI_IOAPIC_INTERRUPTS_H
#define PEDIGREE_PC_PCI_IOAPIC_INTERRUPTS_H

#include <config.h>

#if MULTIPROCESSOR && ACPI

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/machine/IrqHandlerRegistry.h"
#include "pedigree/kernel/machine/ThreadedIrqDispatcher.h"
#include "pedigree/kernel/processor/InterruptHandler.h"

#include "IoApic.h"

class Device;
class HardIrqHandler;
class IrqHandler;
class IrqHandlerBase;

/** PCI INTx on ACPI-described I/O APIC GSIs; legacy ISA remains on the PIC. */
class PciIoApicInterrupts : private InterruptHandler {
 public:
  static constexpr uint8_t FirstVector = 0x80;
  static constexpr size_t VectorCount = 16;

  PciIoApicInterrupts();
  bool initialise();
  bool initialiseThreaded();
  bool shutdownThreaded();
  irq_id_t registerThreaded(Device* device, IrqHandler* handler, const IrqPolicy& policy,
                            bool& routed);
  irq_id_t registerHard(Device* device, HardIrqHandler* handler, const IrqPolicy& policy,
                        bool& routed);
  bool unregisterHandler(irq_id_t id, IrqHandlerBase* handler);
  void enable(irq_id_t id, bool enabled);

  static bool contains(irq_id_t id) {
    return id >= FirstVector && id < FirstVector + VectorCount;
  }

 private:
  enum class Mode : uint8_t { None, Threaded, Hard };
  struct Line {
    IoApic* controller = nullptr;
    uint32_t gsi = 0;
    size_t handlers = 0;
    size_t cookie = 0;
    Mode mode = Mode::None;
    bool activeLow = false;
    bool used = false;
    bool enabled = false;
    bool removing = false;
    bool inFlight = false;
    bool quarantined = false;
  };

  irq_id_t registerHandler(Device* device, IrqHandlerBase* handler, const IrqPolicy& policy,
                           Mode mode, bool& routed);
  static bool findRoute(Device* device, uint32_t& gsi, bool& activeLow);
  void interrupt(size_t interruptNumber, InterruptState& state) override;
  static void dispatchThreaded(void* context, uint8_t slot, size_t cookie);

  Spinlock m_Lock;
  IrqHandlerRegistry m_Handlers;
  ThreadedIrqDispatcher m_Dispatcher;
  IoApic m_Controllers[4];
  size_t m_ControllerCount;
  Line m_Lines[VectorCount];
  uint8_t m_DestinationApicId;
  bool m_Ready;
  bool m_ShuttingDown;
};

#endif

#endif
