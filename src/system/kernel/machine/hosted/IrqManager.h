/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef KERNEL_MACHINE_HOSTED_IRQMANAGER_H
#define KERNEL_MACHINE_HOSTED_IRQMANAGER_H
#include <config.h>

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/processor/InterruptHandler.h"

/**
 * Routes the two synthetic signals used by the hosted machine.
 *
 * Hosted is a single-processor kernel validation shim, not an interrupt
 * controller model. Line 0 belongs exclusively to the hosted wall-clock timer
 * and line 1 belongs exclusively to the scheduler timer. Device, PCI, shared,
 * and threaded controller registrations are deliberately unsupported.
 */
class HostedIrqManager : public IrqManager, private InterruptHandler {
 public:
  inline static HostedIrqManager& instance() {
    return m_Instance;
  }

  irq_id_t registerIsaIrqHandler(uint8_t irq, IrqHandler* handler,
                                 const IrqPolicy& policy) override;
  irq_id_t registerPciIrqHandler(IrqHandler* handler, class Device* device,
                                 const IrqPolicy& policy) override;
  irq_id_t registerHardIsaIrqHandler(uint8_t irq, HardIrqHandler* handler,
                                     const IrqPolicy& policy) override;
  irq_id_t registerHardPciIrqHandler(HardIrqHandler* handler, class Device* device,
                                     const IrqPolicy& policy) override;
  irq_id_t registerSchedulerIrqHandler(uint8_t irq, SchedulerIrqHandler* handler,
                                       const IrqPolicy& policy) override;
  bool unregisterSchedulerIrqHandler(irq_id_t id, SchedulerIrqHandler* handler) override;
  bool unregisterHandler(irq_id_t id, IrqHandlerBase* handler) override;

  bool initialise() INITIALISATION_ONLY;

  bool control(uint8_t irq, ControlCode code, size_t argument) override;

  void enable(uint8_t irq, bool enable) override {
    // Hosted has no per-line interrupt controller. Processor::setInterrupts
    // owns the only meaningful signal mask.
  }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  static EXPORTED_PUBLIC SchedulerIrqHandler* schedulerIrqHandlerForTest(uint8_t irq);
#endif

 private:
  HostedIrqManager() INITIALISATION_ONLY;
  inline ~HostedIrqManager() override {}

  HostedIrqManager(const HostedIrqManager&) = delete;
  HostedIrqManager& operator=(const HostedIrqManager&) = delete;

  void interrupt(size_t interruptNumber, InterruptState& state) override;

  HardIrqHandler* m_TimerHandler;
  SchedulerIrqHandler* m_SchedulerHandler;

  static HostedIrqManager m_Instance;
};

#endif
