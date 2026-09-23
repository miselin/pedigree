/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef KERNEL_MACHINE_VIRT_IRQMANAGER_H
#define KERNEL_MACHINE_VIRT_IRQMANAGER_H

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/machine/IrqHandlerRegistry.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/ThreadedIrqDispatcher.h"
#include "pedigree/kernel/processor/state_forward.h"

class VirtIrqManager : public IrqManager {
 public:
  static VirtIrqManager& instance();

  bool initialise();
  bool initialiseThreaded();
  bool shutdownThreaded();
  void handle(InterruptState& state);

  irq_id_t registerIsaIrqHandler(uint8_t irq, IrqHandler* handler,
                                 const IrqPolicy& policy) override;
  irq_id_t registerPciIrqHandler(IrqHandler* handler, Device* device,
                                 const IrqPolicy& policy) override;
  irq_id_t registerHardIsaIrqHandler(uint8_t irq, HardIrqHandler* handler,
                                     const IrqPolicy& policy) override;
  irq_id_t registerHardPciIrqHandler(HardIrqHandler* handler, Device* device,
                                     const IrqPolicy& policy) override;
  irq_id_t registerSchedulerIrqHandler(uint8_t irq, SchedulerIrqHandler* handler,
                                       const IrqPolicy& policy) override;
  bool unregisterSchedulerIrqHandler(irq_id_t id, SchedulerIrqHandler* handler) override;
  bool unregisterHandler(irq_id_t id, IrqHandlerBase* handler) override;
  void enable(uint8_t irq, bool enabled) override;

 private:
  static constexpr size_t MaxIrqs = 256;
  static constexpr size_t MaxPciLines = 4;

  struct PciLine {
    uint32_t irq;
    size_t cookie;
    size_t handlers;
    bool inFlight;
    bool removing;
    bool quarantined;
    bool hard;
  };

  VirtIrqManager();
  static void dispatchPciLine(void* context, uint8_t line, size_t cookie);
  irq_id_t registerPciHandler(IrqHandlerBase* handler, Device* device, const IrqPolicy& policy,
                              bool hard);
  size_t pciLine(uint32_t irq) const;
  uint32_t acknowledge();
  void complete(uint32_t acknowledgeValue);
  void setEnabled(uint32_t irq, bool enabled);
  void setLevel(uint32_t irq);
  bool initialiseV2();
  bool initialiseV3();

  HardIrqHandler* m_Hard[MaxIrqs];
  SchedulerIrqHandler* m_Scheduler[MaxIrqs];
  IrqHandlerRegistry m_PciHandlers;
  ThreadedIrqDispatcher m_PciDispatcher;
  Spinlock m_PciLock;
  PciLine m_PciLines[MaxPciLines];
  uint32_t m_Version;
  bool m_Initialised;

  static VirtIrqManager m_Instance;
};

extern "C" void virtHandleIrq(InterruptState& state);

#endif
