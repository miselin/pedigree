/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_PC_PCI_MESSAGE_INTERRUPTS_H
#define PEDIGREE_PC_PCI_MESSAGE_INTERRUPTS_H

#include <config.h>

#if MULTIPROCESSOR

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/machine/IrqHandlerRegistry.h"
#include "pedigree/kernel/machine/ThreadedIrqDispatcher.h"
#include "pedigree/kernel/processor/InterruptHandler.h"

class Device;
class HardIrqHandler;
class IrqHandler;
class IrqHandlerBase;

/** PCI message vectors delivered to the bootstrap local APIC. */
class PciMessageInterrupts : private InterruptHandler {
 public:
  static constexpr uint8_t FirstVector = 0x40;
  static constexpr uint8_t VectorCount = 16;

  PciMessageInterrupts();

  bool initialise();
  bool initialiseThreaded();
  bool shutdownThreaded();
  irq_id_t registerThreaded(Device* device, IrqHandler* handler, bool& fallbackSafe);
  bool registerThreadedVectors(Device* device, IrqHandler* const* handlers, size_t count,
                               irq_id_t* ids, bool& fallbackSafe);
  irq_id_t registerHard(Device* device, HardIrqHandler* handler, bool& fallbackSafe);
  bool unregisterHandler(irq_id_t id, IrqHandlerBase* handler);
  void enable(irq_id_t id, bool enabled);

  static bool contains(irq_id_t id) {
    return id >= FirstVector && id < FirstVector + VectorCount;
  }

 private:
  enum class Mode : uint8_t { None, Threaded, Hard };
  struct Line {
    Device* device = nullptr;
    IrqHandlerBase* handler = nullptr;
    Mode mode = Mode::None;
    size_t cookie = 0;
    // A retired vector is not reused: a posted MSI may arrive after disable.
    bool used = false;
    bool enabled = false;
    bool removing = false;
    bool deferred = false;
    bool msix = false;
    uint8_t msixIndex = 0;
  };

  irq_id_t registerHandler(Device* device, IrqHandlerBase* handler, Mode mode, bool& fallbackSafe);
  bool disableSource(Device* device, bool msix, uint8_t msixIndex);
  void quarantine(uint8_t slot, size_t expectedCookie = 0);
  void interrupt(size_t interruptNumber, InterruptState& state) override;
  static void dispatchThreaded(void* context, uint8_t slot, size_t cookie);

  Spinlock m_Lock;
  IrqHandlerRegistry m_Handlers;
  ThreadedIrqDispatcher m_Dispatcher;
  Line m_Lines[VectorCount];
  uint8_t m_DestinationApicId;
  bool m_Initialised;
  bool m_ShuttingDown;
};

#endif

#endif
