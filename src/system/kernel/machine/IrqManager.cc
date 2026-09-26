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

#include "pedigree/kernel/machine/IrqManager.h"

IrqManager::IrqManager() = default;
IrqManager::~IrqManager() = default;

irq_id_t IrqManager::registerPciMessageIrqHandler(IrqHandler* handler, Device* device,
                                                  const IrqPolicy& intxFallbackPolicy) {
  return registerPciIrqHandler(handler, device, intxFallbackPolicy);
}

bool IrqManager::registerPciMsixIrqHandlers(Device*, IrqHandler* const*, size_t, irq_id_t*,
                                            bool& fallbackSafe, const size_t*) {
  fallbackSafe = true;
  return false;
}

irq_id_t IrqManager::registerHardPciMessageIrqHandler(HardIrqHandler* handler, Device* device,
                                                      const IrqPolicy& intxFallbackPolicy) {
  return registerHardPciIrqHandler(handler, device, intxFallbackPolicy);
}

irq_id_t IrqManager::registerSchedulerIrqHandler(uint8_t, SchedulerIrqHandler*, const IrqPolicy&) {
  return 0;
}

bool IrqManager::unregisterSchedulerIrqHandler(irq_id_t, SchedulerIrqHandler*) {
  return false;
}

size_t IrqManager::snapshotIrqLines(IrqLineDiagnosticSnapshot*, size_t) const {
  return 0;
}

void IrqManager::tick() {}

bool IrqManager::control(uint8_t irq, ControlCode code, size_t argument) {
  return true;
}
