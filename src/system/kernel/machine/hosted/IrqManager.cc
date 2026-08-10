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

#include "IrqManager.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/SchedulerIrqHandler.h"
#include "pedigree/kernel/processor/InterruptManager.h"
#include "pedigree/kernel/processor/Processor.h"

#include <signal.h>

#include "system/kernel/core/processor/DeviceHardIrqContext.h"

namespace {
constexpr uint8_t TimerIrq = 0;
constexpr uint8_t SchedulerIrq = 1;

irq_id_t signalForIrq(uint8_t irq) {
  if (irq == TimerIrq) {
    return SIGUSR1;
  }
  if (irq == SchedulerIrq) {
    return SIGUSR2;
  }
  return 0;
}
}  // namespace

HostedIrqManager HostedIrqManager::m_Instance;

irq_id_t HostedIrqManager::registerIsaIrqHandler(uint8_t, IrqHandler*, const IrqPolicy&) {
  return 0;
}

irq_id_t HostedIrqManager::registerPciIrqHandler(IrqHandler*, Device*, const IrqPolicy&) {
  return 0;
}

irq_id_t HostedIrqManager::registerHardIsaIrqHandler(uint8_t irq, HardIrqHandler* handler,
                                                     const IrqPolicy& policy) {
  if (irq != TimerIrq || !handler || policy != IrqPolicy::syntheticHard()) {
    return 0;
  }

  HardIrqHandler* expected = nullptr;
  if (!__atomic_compare_exchange_n(&m_TimerHandler, &expected, handler, false, __ATOMIC_RELEASE,
                                   __ATOMIC_ACQUIRE)) {
    return 0;
  }
  return signalForIrq(irq);
}

irq_id_t HostedIrqManager::registerHardPciIrqHandler(HardIrqHandler*, Device*, const IrqPolicy&) {
  return 0;
}

irq_id_t HostedIrqManager::registerSchedulerIrqHandler(uint8_t irq, SchedulerIrqHandler* handler,
                                                       const IrqPolicy& policy) {
  if (irq != SchedulerIrq || !handler || policy != IrqPolicy::syntheticHard()) {
    return 0;
  }

  SchedulerIrqHandler* expected = nullptr;
  if (!__atomic_compare_exchange_n(&m_SchedulerHandler, &expected, handler, false, __ATOMIC_RELEASE,
                                   __ATOMIC_ACQUIRE)) {
    return 0;
  }
  return signalForIrq(irq);
}

bool HostedIrqManager::unregisterSchedulerIrqHandler(irq_id_t id, SchedulerIrqHandler* handler) {
  if (id != SIGUSR2 || !handler) {
    return false;
  }

  SchedulerIrqHandler* expected = handler;
  return __atomic_compare_exchange_n(&m_SchedulerHandler, &expected,
                                     static_cast<SchedulerIrqHandler*>(nullptr), false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

bool HostedIrqManager::unregisterHandler(irq_id_t id, IrqHandlerBase* handler) {
  if (id != SIGUSR1 || !handler) {
    return false;
  }

  HardIrqHandler* expected = __atomic_load_n(&m_TimerHandler, __ATOMIC_ACQUIRE);
  if (static_cast<IrqHandlerBase*>(expected) != handler) {
    return false;
  }
  return __atomic_compare_exchange_n(&m_TimerHandler, &expected,
                                     static_cast<HardIrqHandler*>(nullptr), false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE);
}

bool HostedIrqManager::initialise() {
  InterruptManager& manager = InterruptManager::instance();
  return manager.registerInterruptHandler(SIGUSR1, this) &&
         manager.registerInterruptHandler(SIGUSR2, this);
}

bool HostedIrqManager::control(uint8_t, ControlCode, size_t) {
  return false;
}

HostedIrqManager::HostedIrqManager() : m_TimerHandler(nullptr), m_SchedulerHandler(nullptr) {}

void HostedIrqManager::interrupt(size_t interruptNumber, InterruptState& state) {
  if (interruptNumber == SIGUSR1) {
    HardIrqHandler* handler = __atomic_load_n(&m_TimerHandler, __ATOMIC_ACQUIRE);
    if (!handler) {
      return;
    }

    size_t previousDepth = 0;
    bool restoreDepth = false;
    DeviceHardIrqContext context(previousDepth, restoreDepth);
    const HardIrqDisposition disposition = handler->irq(TimerIrq, state);
    if (disposition != HardIrqDisposition::Handled) {
      FATAL_NOLOCK("Hosted timer hard stage failed to publish its deferred work");
    }
    return;
  }

  if (interruptNumber == SIGUSR2) {
    SchedulerIrqHandler* handler = __atomic_load_n(&m_SchedulerHandler, __ATOMIC_ACQUIRE);
    if (handler) {
      // This callback may abandon the signal frame and must stay the
      // terminal action for the synthetic scheduler occurrence.
      handler->schedulerIrq(SchedulerIrq, state);
    }
  }
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
SchedulerIrqHandler* HostedIrqManager::schedulerIrqHandlerForTest(uint8_t irq) {
  if (irq != SchedulerIrq) {
    return nullptr;
  }
  return __atomic_load_n(&m_Instance.m_SchedulerHandler, __ATOMIC_ACQUIRE);
}
#endif
