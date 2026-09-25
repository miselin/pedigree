/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */

#include <config.h>

#if MULTIPROCESSOR && ACPI

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/processor/InterruptManager.h"

#include "Acpi.h"
#include "LocalApic.h"
#include "Pc.h"
#include "PciIoApicInterrupts.h"

PciIoApicInterrupts::PciIoApicInterrupts()
    : m_Lock(false),
      m_Handlers(),
      m_Dispatcher(MakeConstantString("PCI INTx"), VectorCount, dispatchThreaded, this),
      m_Controllers(),
      m_ControllerCount(0),
      m_Lines(),
      m_DestinationApicId(0),
      m_Ready(false),
      m_ShuttingDown(false) {}

bool PciIoApicInterrupts::initialise() {
  const auto& controllers = Acpi::instance().getIoApicList();
  for (size_t i = 0; i < controllers.size() && m_ControllerCount < 4; ++i) {
    const auto* info = controllers[i];
    if (info && m_Controllers[m_ControllerCount].initialise(info->address, info->gsiBase)) {
      ++m_ControllerCount;
    }
  }
  if (!m_ControllerCount) {
    return false;
  }
  InterruptManager& manager = InterruptManager::instance();
  size_t installed = 0;
  for (; installed < VectorCount; ++installed) {
    if (!manager.registerInterruptHandler(FirstVector + installed, this)) {
      for (size_t i = 0; i < installed; ++i) {
        manager.registerInterruptHandler(FirstVector + i, nullptr);
      }
      return false;
    }
  }
  m_DestinationApicId = Pc::instance().getLocalApic().getId();
  m_Ready = true;
  return true;
}

bool PciIoApicInterrupts::initialiseThreaded() {
  return m_Ready && m_Dispatcher.initialise();
}

bool PciIoApicInterrupts::shutdownThreaded() {
  if (!m_Ready) {
    return true;
  }
  bool masked = true;
  {
    LockGuard<Spinlock> guard(m_Lock);
    m_ShuttingDown = true;
    for (Line& line : m_Lines) {
      if (line.used && line.controller && !line.controller->mask(line.gsi, true)) {
        masked = false;
      }
      line.enabled = false;
    }
  }
  return masked && (!m_Dispatcher.isInitialised() || m_Dispatcher.shutdown());
}

bool PciIoApicInterrupts::findRoute(Device* device, uint32_t& gsi, bool& activeLow) {
  if (!device) {
    return false;
  }
  uint8_t pin = 0;
  if (!PciBus::instance().readConfig8(device, 0x3d, pin) || pin < 1 || pin > 4) {
    return false;
  }
  Device* current = device;
  for (size_t depth = 0; current->getPciBusPosition(); ++depth) {
    if (depth == 8) {
      return false;
    }
    Device* bus = current->getParent();
    Device* bridge = bus ? bus->getParent() : nullptr;
    if (!bridge || bridge->getPciClassCode() != 6 || bridge->getPciSubclassCode() != 4) {
      return false;
    }
    pin = static_cast<uint8_t>(((pin - 1 + current->getPciDevicePosition()) & 3) + 1);
    current = bridge;
  }
  AcpiPciRouting::Route route = {};
  if (!Acpi::instance().pciInterruptRoute(current->getPciDevicePosition(), pin, route)) {
    return false;
  }
  gsi = route.gsi;
  activeLow = route.activeLow;
  return true;
}

irq_id_t PciIoApicInterrupts::registerThreaded(Device* device, IrqHandler* handler,
                                               const IrqPolicy& policy, bool& routed) {
  return registerHandler(device, handler, policy, Mode::Threaded, routed);
}

irq_id_t PciIoApicInterrupts::registerHard(Device* device, HardIrqHandler* handler,
                                           const IrqPolicy& policy, bool& routed) {
  return registerHandler(device, handler, policy, Mode::Hard, routed);
}

irq_id_t PciIoApicInterrupts::registerHandler(Device* device, IrqHandlerBase* handler,
                                              const IrqPolicy& policy, Mode mode, bool& routed) {
  routed = false;
  if (!m_Ready || !device || !handler || policy.trigger() != IrqTrigger::Level ||
      (mode == Mode::Threaded ? (!policy.validForThreaded() || !m_Dispatcher.isInitialised())
                              : !policy.validForHard())) {
    return 0;
  }
  uint32_t gsi = 0;
  bool activeLow = false;
  if (!findRoute(device, gsi, activeLow)) {
    return 0;
  }
  IoApic* controller = nullptr;
  for (size_t i = 0; i < m_ControllerCount; ++i) {
    if (m_Controllers[i].contains(gsi)) {
      controller = &m_Controllers[i];
      break;
    }
  }
  if (!controller) {
    return 0;
  }
  routed = true;

  LockGuard<Spinlock> guard(m_Lock);
  if (m_ShuttingDown) {
    return 0;
  }
  size_t slot = VectorCount;
  for (size_t i = 0; i < VectorCount; ++i) {
    if (m_Lines[i].used && m_Lines[i].gsi == gsi) {
      slot = i;
      break;
    }
  }
  if (slot == VectorCount) {
    for (size_t i = 0; i < VectorCount; ++i) {
      if (!m_Lines[i].used) {
        slot = i;
        break;
      }
    }
  }
  if (slot == VectorCount) {
    return 0;
  }
  Line& line = m_Lines[slot];
  if (line.removing || line.quarantined ||
      (line.used && (line.controller != controller || line.activeLow != activeLow ||
                     (line.handlers && line.mode != mode)))) {
    return 0;
  }
  const uint8_t vector = FirstVector + slot;
  if (!line.used && !controller->route(gsi, vector, m_DestinationApicId, activeLow)) {
    return 0;
  }
  const bool registered =
      mode == Mode::Threaded
          ? m_Handlers.registerThreadedHandler(vector, static_cast<IrqHandler*>(handler), policy)
          : m_Handlers.registerHardHandler(vector, static_cast<HardIrqHandler*>(handler), policy);
  if (!registered) {
    return 0;
  }
  line.controller = controller;
  line.gsi = gsi;
  line.activeLow = activeLow;
  line.mode = mode;
  line.used = true;
  ++line.handlers;
  line.enabled = true;
  if (!line.inFlight && !controller->mask(gsi, false)) {
    line.quarantined = true;
    ERROR("PCI INTx: could not enable IOAPIC GSI " << Dec << gsi);
    const auto result = m_Handlers.unregisterHandler(vector, handler);
    if (result == IrqHandlerRegistry::UnregisterResult::Completed) {
      --line.handlers;
      if (!line.handlers) {
        line.mode = Mode::None;
        line.enabled = false;
      }
    }
    return 0;
  }
  NOTICE("PCI INTx: IOAPIC GSI " << Dec << gsi << " -> vector " << Hex << vector);
  return vector;
}

bool PciIoApicInterrupts::unregisterHandler(irq_id_t id, IrqHandlerBase* handler) {
  if (!contains(id) || !handler) {
    return false;
  }
  const uint8_t slot = id - FirstVector;
  size_t cookie = 0;
  bool last = false;
  {
    LockGuard<Spinlock> guard(m_Lock);
    Line& line = m_Lines[slot];
    if (!line.handlers || line.removing || !line.controller ||
        !m_Handlers.containsHandler(id, handler) || !line.controller->mask(line.gsi, true)) {
      return false;
    }
    line.removing = true;
    last = line.handlers == 1;
    if (last) {
      cookie = ++line.cookie;
    }
  }
  if (last) {
    m_Handlers.invalidateThreadedLine(id, cookie);
  }
  const auto result = m_Handlers.unregisterHandler(id, handler);
  {
    LockGuard<Spinlock> guard(m_Lock);
    Line& line = m_Lines[slot];
    if (last) {
      line.inFlight = false;
    }
    if ((result == IrqHandlerRegistry::UnregisterResult::Completed ||
         result == IrqHandlerRegistry::UnregisterResult::Deferred) &&
        line.handlers) {
      --line.handlers;
      if (!line.handlers) {
        line.mode = Mode::None;
        line.enabled = false;
        // A delivered vector can remain pending in the LAPIC after the GSI is masked.
        // Keep its vector bound to this GSI so a late delivery cannot reach a new device.
      }
    } else if (result != IrqHandlerRegistry::UnregisterResult::Completed) {
      line.quarantined = true;
    }
    line.removing = false;
    if (line.handlers && line.enabled && !line.inFlight && !line.quarantined &&
        !line.controller->mask(line.gsi, false)) {
      line.quarantined = true;
    }
  }
  return result == IrqHandlerRegistry::UnregisterResult::Completed;
}

void PciIoApicInterrupts::enable(irq_id_t id, bool enabled) {
  if (!contains(id)) {
    return;
  }
  LockGuard<Spinlock> guard(m_Lock);
  Line& line = m_Lines[id - FirstVector];
  if (!line.handlers || line.removing || line.quarantined || m_ShuttingDown) {
    return;
  }
  line.enabled = enabled;
  if (!line.controller->mask(line.gsi, !enabled || line.inFlight)) {
    line.quarantined = true;
  }
}

void PciIoApicInterrupts::interrupt(size_t vector, InterruptState& state) {
  if (!contains(vector)) {
    return;
  }
  const uint8_t slot = vector - FirstVector;
  Mode mode = Mode::None;
  size_t cookie = 0;
  bool published = false;
  IrqHandlerRegistry::AdmissionCutoff cutoff = {};
  {
    LockGuard<Spinlock> guard(m_Lock);
    Line& line = m_Lines[slot];
    if (line.controller) {
      if (!line.controller->mask(line.gsi, true)) {
        line.quarantined = true;
      }
      if (line.handlers && line.enabled && !line.removing && !line.inFlight && !line.quarantined &&
          !m_ShuttingDown) {
        mode = line.mode;
        line.inFlight = true;
        cookie = ++line.cookie;
        if (!cookie) {
          cookie = ++line.cookie;
        }
        if (m_Handlers.captureAdmissionCutoff(vector, cutoff)) {
          if (mode == Mode::Threaded) {
            published = m_Handlers.publishThreadedDispatch(vector, cookie, cutoff);
            if (published && !m_Dispatcher.publishFromInterrupt(slot, cookie)) {
              m_Handlers.invalidateThreadedGenerationFromInterrupt(vector, cookie);
              published = false;
            }
          } else {
            published = true;
          }
        }
        if (!published) {
          line.quarantined = true;
        }
      }
    }
  }
  if (mode != Mode::Hard || !published) {
    Pc::instance().getLocalApic().ack();
    return;
  }
  HardIrqDisposition disposition = HardIrqDisposition::NotHandled;
  const bool admitted =
      m_Handlers.dispatchHard(vector, state, disposition, nullptr, cookie, cutoff);
  Pc::instance().getLocalApic().ack();
  LockGuard<Spinlock> guard(m_Lock);
  Line& line = m_Lines[slot];
  if (line.cookie != cookie) {
    return;
  }
  line.inFlight = false;
  if (!admitted || disposition != HardIrqDisposition::Handled) {
    line.quarantined = true;
  }
  if (line.enabled && !line.removing && !line.quarantined &&
      !line.controller->mask(line.gsi, false)) {
    line.quarantined = true;
  }
}

void PciIoApicInterrupts::dispatchThreaded(void* context, uint8_t slot, size_t cookie) {
  auto& self = *static_cast<PciIoApicInterrupts*>(context);
  if (slot >= VectorCount) {
    return;
  }
  const uint8_t vector = FirstVector + slot;
  IrqHandlerRegistry::ThreadedDispatchResult result = {};
  const bool admitted = self.m_Handlers.dispatchThreaded(vector, cookie, result);
  LockGuard<Spinlock> guard(self.m_Lock);
  Line& line = self.m_Lines[slot];
  if (line.cookie != cookie) {
    return;
  }
  line.inFlight = false;
  if (!admitted || !result.allowRearm) {
    line.quarantined = true;
  }
  if (line.enabled && !line.removing && !line.quarantined &&
      !line.controller->mask(line.gsi, false)) {
    line.quarantined = true;
  }
}

#endif
