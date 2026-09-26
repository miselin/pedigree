/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */

#include <config.h>

#if MULTIPROCESSOR

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/processor/InterruptManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

#include "LocalApic.h"
#include "Pc.h"
#include "PciMessageInterrupts.h"

PciMessageInterrupts::PciMessageInterrupts()
    : m_RegistrationLock(),
      m_Lock(false),
      m_Handlers(),
      m_Dispatcher(MakeConstantString("PCI message IRQ"), VectorCount, dispatchThreaded, this),
      m_Lines(),
      m_DestinationApicId(0),
      m_Initialised(false),
      m_ShuttingDown(false) {}

bool PciMessageInterrupts::initialise() {
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
  m_Initialised = true;
  return true;
}

bool PciMessageInterrupts::initialiseThreaded() {
  LockGuard<Mutex> registration(m_RegistrationLock);
  return m_Initialised && m_Dispatcher.initialise(true);
}

bool PciMessageInterrupts::shutdownThreaded() {
  if (!m_Dispatcher.canShutdown()) {
    return false;
  }
  LockGuard<Mutex> registration(m_RegistrationLock);
  if (!m_Initialised) {
    return true;
  }
  bool disabled = true;
  {
    LockGuard<Spinlock> guard(m_Lock);
    m_ShuttingDown = true;
    for (Line& line : m_Lines) {
      line.enabled = false;
      if (line.device && !(line.msix ? PciBus::instance().disableMsix(line.device)
                                     : PciBus::instance().disableMsi(line.device))) {
        disabled = false;
      }
    }
  }
  return disabled && (!m_Dispatcher.isInitialised() || m_Dispatcher.shutdown());
}

irq_id_t PciMessageInterrupts::registerThreaded(Device* device, IrqHandler* handler,
                                                bool& fallbackSafe) {
  return registerHandler(device, handler, Mode::Threaded, fallbackSafe);
}

bool PciMessageInterrupts::registerThreadedVectors(Device* device, IrqHandler* const* handlers,
                                                   size_t count, irq_id_t* ids, bool& fallbackSafe,
                                                   const size_t* processors) {
  fallbackSafe = true;
  if (!device || !handlers || !ids || !count || count > VectorCount || !m_Initialised ||
      !m_Dispatcher.isInitialised()) {
    return false;
  }
  if (Processor::executionContext() != ExecutionContext::WaitableThread ||
      m_Dispatcher.isCurrentWorker()) {
    return false;
  }
  LockGuard<Mutex> registration(m_RegistrationLock);
  for (size_t i = 0; i < count; ++i) {
    ids[i] = 0;
    if (!handlers[i] || (processors && !Processor::informationAt(processors[i]))) {
      return false;
    }
  }

  PciBus& pci = PciBus::instance();
  PciFunctionState::State state;
  if (!pci.inspectFunction(device, state, false) || !state.msix) {
    return false;
  }
  uint16_t control = 0;
  if (!pci.readConfig16(device, state.msix + 2, control) || count > (control & 0x7ffU) + 1) {
    return false;
  }

  uint8_t slots[VectorCount] = {};
  uint32_t data[VectorCount] = {};
  uint64_t addresses[VectorCount] = {};
  bool selected[VectorCount] = {};
  bool reused[VectorCount] = {};
  {
    LockGuard<Spinlock> guard(m_Lock);
    if (m_ShuttingDown) {
      return false;
    }
    for (const Line& line : m_Lines) {
      if (line.device == device) {
        fallbackSafe = false;
        return false;
      }
    }
    for (size_t i = 0; i < count; ++i) {
      const size_t cpu = processors ? processors[i] : 0;
      const bool spuriousSafe = handlers[i]->acceptsSpuriousInterrupts();
      size_t slot = 0;
      for (; slot < VectorCount; ++slot) {
        const Line& line = m_Lines[slot];
        if (!selected[slot] && !line.device && (!line.used || (line.reusable && spuriousSafe)) &&
            (!(line.used || line.workerPrepared) || line.processor == cpu)) {
          break;
        }
      }
      if (slot == VectorCount) {
        return false;
      }
      slots[i] = static_cast<uint8_t>(slot);
      selected[slot] = true;
      reused[i] = m_Lines[slot].used;
    }
  }
  // Worker creation may allocate and schedule; registration serialization keeps
  // these unpublished slots reserved without holding an interrupt spinlock.
  for (size_t i = 0; i < count; ++i) {
    const size_t cpu = processors ? processors[i] : 0;
    if (!m_Dispatcher.prepareLine(slots[i], cpu)) {
      return false;
    }
    LockGuard<Spinlock> guard(m_Lock);
    Line& line = m_Lines[slots[i]];
    line.workerPrepared = true;
    line.processor = cpu;
    addresses[i] = 0xFEE00000ULL | (uint64_t{Processor::informationAt(cpu)->localApicId()} << 12);
  }
  {
    LockGuard<Spinlock> guard(m_Lock);
    for (size_t i = 0; i < count; ++i) {
      const uint8_t vector = FirstVector + slots[i];
      if (!m_Handlers.registerThreadedHandler(vector, handlers[i], IrqPolicy::edgeThreaded())) {
        for (size_t j = 0; j < i; ++j) {
          const uint8_t previous = FirstVector + slots[j];
          if (m_Handlers.unregisterHandler(previous, handlers[j]) !=
              IrqHandlerRegistry::UnregisterResult::Completed) {
            FATAL("PCI message registration could not drain its unpublished handler");
          }
          Line& previousLine = m_Lines[slots[j]];
          previousLine.device = nullptr;
          previousLine.handler = nullptr;
          previousLine.mode = Mode::None;
          previousLine.enabled = false;
        }
        return false;
      }
      Line& line = m_Lines[slots[i]];
      line.device = device;
      line.handler = handlers[i];
      line.mode = Mode::Threaded;
      advanceCookie(line);
      line.spuriousSafe = handlers[i]->acceptsSpuriousInterrupts();
      line.removing = false;
      line.deferred = false;
      line.unhandled = 0;
      line.enabled = false;
      line.msix = true;
      line.msixIndex = static_cast<uint8_t>(i);
      data[i] = vector;
    }
    for (size_t i = 0; i < count; ++i) {
      m_Lines[slots[i]].enabled = true;
    }
  }

  bool touched = false;
  if (pci.enableMsixVectors(device, addresses[0], data, count, &touched, addresses)) {
    {
      LockGuard<Spinlock> guard(m_Lock);
      for (size_t i = 0; i < count; ++i) {
        m_Lines[slots[i]].used = true;
        m_Lines[slots[i]].reusable = false;
        ids[i] = static_cast<irq_id_t>(data[i]);
      }
    }
    for (size_t i = 0; i < count; ++i) {
      NOTICE("PCI MSI-X: vector " << Dec << data[i] << " CPU " << m_Lines[slots[i]].processor
                                  << (reused[i] ? " reused" : " allocated") << Hex);
    }
    return true;
  }

  uint16_t command = 0;
  bool disabled = pci.disableMessageInterrupts(device, state);
  if (touched) {
    for (size_t i = 0; i < count; ++i) {
      const bool masked = pci.setMsixVectorMask(device, i, true);
      disabled = masked && disabled;
    }
  }
  disabled = pci.updateCommand(device, 0x400U, state.command & 0x400U) && disabled;
  disabled =
      pci.readConfig16(device, 4, command) && ((command ^ state.command) & 0x400U) == 0 && disabled;
  size_t cookies[VectorCount] = {};
  {
    LockGuard<Spinlock> guard(m_Lock);
    for (size_t i = 0; i < count; ++i) {
      Line& line = m_Lines[slots[i]];
      line.enabled = false;
      line.removing = true;
      cookies[i] = advanceCookie(line);
    }
  }
  for (size_t i = 0; i < count; ++i) {
    const uint8_t vector = FirstVector + slots[i];
    m_Handlers.invalidateThreadedLine(vector, cookies[i]);
    if (m_Handlers.unregisterHandler(vector, handlers[i]) !=
        IrqHandlerRegistry::UnregisterResult::Completed) {
      FATAL("PCI message registration could not drain its unpublished handler");
    }
  }
  {
    LockGuard<Spinlock> guard(m_Lock);
    for (size_t i = 0; i < count; ++i) {
      Line& line = m_Lines[slots[i]];
      line.device = nullptr;
      line.handler = nullptr;
      line.mode = Mode::None;
      line.used = line.used || touched || !disabled;
      line.reusable = disabled;
    }
  }
  if (!disabled) {
    fallbackSafe = false;
    ERROR("PCI MSI-X setup failed without verified source shutdown");
  }
  return false;
}

irq_id_t PciMessageInterrupts::registerHard(Device* device, HardIrqHandler* handler,
                                            bool& fallbackSafe) {
  return registerHandler(device, handler, Mode::Hard, fallbackSafe);
}

irq_id_t PciMessageInterrupts::registerHandler(Device* device, IrqHandlerBase* handler, Mode mode,
                                               bool& fallbackSafe) {
  fallbackSafe = true;
  if (!device || !handler || !m_Initialised ||
      (mode == Mode::Threaded && !m_Dispatcher.isInitialised())) {
    return 0;
  }
  if (Processor::executionContext() != ExecutionContext::WaitableThread ||
      m_Dispatcher.isCurrentWorker()) {
    return 0;
  }
  LockGuard<Mutex> registration(m_RegistrationLock);
  const bool spuriousSafe =
      mode == Mode::Threaded && static_cast<IrqHandler*>(handler)->acceptsSpuriousInterrupts();
  PciBus& pci = PciBus::instance();
  PciFunctionState::State state;
  if (!pci.inspectFunction(device, state, false) || (!state.msi && !state.msix)) {
    return 0;
  }

  size_t slot = VectorCount;
  {
    LockGuard<Spinlock> guard(m_Lock);
    if (m_ShuttingDown) {
      return 0;
    }
    for (size_t i = 0; i < VectorCount; ++i) {
      if (m_Lines[i].device == device) {
        fallbackSafe = false;
        return 0;
      }
      const Line& candidate = m_Lines[i];
      if (slot == VectorCount && !candidate.device &&
          (!candidate.used || (candidate.reusable && spuriousSafe)) &&
          (!(candidate.used || candidate.workerPrepared) || !candidate.processor)) {
        slot = i;
      }
    }
    if (slot == VectorCount) {
      return 0;
    }
  }
  if (mode == Mode::Threaded && !m_Dispatcher.prepareLine(slot, 0)) {
    return 0;
  }
  {
    LockGuard<Spinlock> guard(m_Lock);
    m_Lines[slot].processor = 0;
    m_Lines[slot].workerPrepared |= mode == Mode::Threaded;
    const uint8_t vector = FirstVector + slot;
    const bool registered =
        mode == Mode::Threaded
            ? m_Handlers.registerThreadedHandler(vector, static_cast<IrqHandler*>(handler),
                                                 IrqPolicy::edgeThreaded())
            : m_Handlers.registerHardHandler(
                  vector, static_cast<HardIrqHandler*>(handler),
                  IrqPolicy(IrqTrigger::Edge, IrqControllerAck::AfterHardStage,
                            IrqLineRelease::AfterHardStage));
    if (!registered) {
      return 0;
    }
    Line& line = m_Lines[slot];
    line.device = device;
    line.handler = handler;
    line.mode = mode;
    advanceCookie(line);
    line.spuriousSafe = spuriousSafe;
    line.reusable = false;
    line.removing = false;
    line.unhandled = 0;
    line.used = true;
    line.enabled = true;
    line.deferred = false;
    line.msix = true;
    line.msixIndex = 0;
  }

  const uint8_t vector = FirstVector + slot;
  const uint64_t address = 0xFEE00000ULL | (uint64_t{m_DestinationApicId} << 12);
  if (state.msix && pci.enableMsix(device, address, vector)) {
    return vector;
  }
  {
    LockGuard<Spinlock> guard(m_Lock);
    m_Lines[slot].msix = false;
  }
  if (state.msi && pci.enableMsi(device, address, vector)) {
    return vector;
  }

  // A failed readback may have followed a partial device write. Do not let
  // the PIC claim INTx until both message sources and the command bit match
  // the pre-attempt state.
  uint16_t command = 0;
  fallbackSafe = pci.disableMessageInterrupts(device, state) &&
                 pci.updateCommand(device, 0x400U, state.command & 0x400U) &&
                 pci.readConfig16(device, 4, command) && ((command ^ state.command) & 0x400U) == 0;

  {
    LockGuard<Spinlock> guard(m_Lock);
    m_Lines[slot].enabled = false;
    m_Lines[slot].removing = true;
  }
  size_t cookie = 0;
  {
    LockGuard<Spinlock> guard(m_Lock);
    cookie = advanceCookie(m_Lines[slot]);
  }
  m_Handlers.invalidateThreadedLine(vector, cookie);
  if (m_Handlers.unregisterHandler(vector, handler) !=
      IrqHandlerRegistry::UnregisterResult::Completed) {
    FATAL("PCI message registration could not drain its unpublished handler");
  }
  {
    LockGuard<Spinlock> guard(m_Lock);
    m_Lines[slot].device = nullptr;
    m_Lines[slot].handler = nullptr;
    m_Lines[slot].mode = Mode::None;
    m_Lines[slot].reusable = fallbackSafe;
  }
  if (!fallbackSafe) {
    ERROR("PCI message setup failed without a safe INTx fallback");
  }
  return 0;
}

bool PciMessageInterrupts::disableSource(Device* device, bool msix, uint8_t msixIndex) {
  return msix ? PciBus::instance().setMsixVectorMask(device, msixIndex, true)
              : PciBus::instance().disableMsi(device);
}

bool PciMessageInterrupts::unregisterHandler(irq_id_t id, IrqHandlerBase* handler) {
  if (!contains(id) || !handler) {
    return false;
  }
  if (Processor::executionContext() != ExecutionContext::WaitableThread ||
      m_Dispatcher.isCurrentWorker()) {
    return false;
  }
  LockGuard<Mutex> registration(m_RegistrationLock);
  const uint8_t slot = id - FirstVector;
  {
    LockGuard<Spinlock> guard(m_Lock);
    Line& line = m_Lines[slot];
    if (line.handler != handler || !line.device) {
      return false;
    }
    line.removing = true;
    line.enabled = false;
    if (!disableSource(line.device, line.msix, line.msixIndex)) {
      return false;
    }
  }
  size_t cookie = 0;
  {
    LockGuard<Spinlock> guard(m_Lock);
    cookie = advanceCookie(m_Lines[slot]);
  }
  m_Handlers.invalidateThreadedLine(id, cookie);
  const IrqHandlerRegistry::UnregisterResult result = m_Handlers.unregisterHandler(id, handler);
  if (result == IrqHandlerRegistry::UnregisterResult::Deferred) {
    LockGuard<Spinlock> guard(m_Lock);
    m_Lines[slot].deferred = true;
    return false;
  }
  if (result == IrqHandlerRegistry::UnregisterResult::NotFound) {
    bool deferred = false;
    {
      LockGuard<Spinlock> guard(m_Lock);
      deferred = m_Lines[slot].deferred;
    }
    size_t generation = 0;
    uintptr_t identity = 0;
    if (!deferred || m_Handlers.handlerCount(id) || m_Handlers.hardDispatchState(id, generation) ||
        m_Handlers.threadedDispatchState(id, identity)) {
      return false;
    }
  } else if (result != IrqHandlerRegistry::UnregisterResult::Completed) {
    return false;
  }
  {
    LockGuard<Spinlock> guard(m_Lock);
    Line& line = m_Lines[slot];
    bool last = true;
    for (const Line& other : m_Lines) {
      if (&other != &line && other.device == line.device) {
        last = false;
        break;
      }
    }
    if (last && line.msix && !PciBus::instance().disableMsix(line.device)) {
      line.deferred = true;
      return false;
    }
    line.device = nullptr;
    line.handler = nullptr;
    line.mode = Mode::None;
    line.reusable = true;
  }
  return true;
}

void PciMessageInterrupts::enable(irq_id_t id, bool enabled) {
  if (!contains(id)) {
    return;
  }
  const uint8_t slot = id - FirstVector;
  bool success = false;
  {
    LockGuard<Spinlock> guard(m_Lock);
    Line& line = m_Lines[slot];
    if (!line.device || line.removing || m_ShuttingDown || line.enabled == enabled) {
      return;
    }
    const uint64_t address = 0xFEE00000ULL | (uint64_t{m_DestinationApicId} << 12);
    success =
        enabled
            ? (line.msix ? PciBus::instance().setMsixVectorMask(line.device, line.msixIndex, false)
                         : PciBus::instance().enableMsi(line.device, address, id))
            : disableSource(line.device, line.msix, line.msixIndex);
    line.enabled = enabled && success;
    if (line.enabled) {
      line.unhandled = 0;
    }
  }
  if (!success) {
    WARNING("PCI message IRQ " << Dec << id << " could not be "
                               << (enabled ? "enabled" : "disabled"));
  }
}

void PciMessageInterrupts::quarantine(uint8_t slot, size_t expectedCookie) {
  bool masked = true;
  {
    LockGuard<Spinlock> guard(m_Lock);
    Line& line = m_Lines[slot];
    if (!line.enabled || line.removing || (expectedCookie && line.cookie != expectedCookie)) {
      return;
    }
    line.enabled = false;
    masked = disableSource(line.device, line.msix, line.msixIndex);
  }
  if (!masked) {
    ERROR("PCI message IRQ " << Dec << FirstVector + slot << " could not be masked");
  }
}

void PciMessageInterrupts::interrupt(size_t interruptNumber, InterruptState& state) {
  if (interruptNumber < FirstVector || interruptNumber >= FirstVector + VectorCount) {
    return;
  }
  const uint8_t slot = interruptNumber - FirstVector;

  Mode mode = Mode::None;
  size_t cookie = 0;
  bool published = false;
  IrqHandlerRegistry::AdmissionCutoff cutoff = {};
  {
    LockGuard<Spinlock> guard(m_Lock);
    Line& line = m_Lines[slot];
    if (line.enabled && !line.removing && !m_ShuttingDown) {
      mode = line.mode;
      cookie = advanceCookie(line);
      const uint8_t vector = FirstVector + slot;
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
    }
  }

  if (mode == Mode::Hard && published) {
    HardIrqDisposition result = HardIrqDisposition::NotHandled;
    const bool admitted =
        m_Handlers.dispatchHard(FirstVector + slot, state, result, nullptr, cookie, cutoff);
    if (!admitted || result != HardIrqDisposition::Handled) {
      quarantine(slot, cookie);
    }
  } else if (mode != Mode::None && !published) {
    quarantine(slot, cookie);
  }
  Pc::instance().getLocalApic().ack();
}

void PciMessageInterrupts::dispatchThreaded(void* context, uint8_t slot, size_t cookie) {
  PciMessageInterrupts* self = static_cast<PciMessageInterrupts*>(context);
  if (slot >= VectorCount) {
    return;
  }
  {
    LockGuard<Spinlock> guard(self->m_Lock);
    const Line& line = self->m_Lines[slot];
    if (!line.enabled || line.removing || line.mode != Mode::Threaded || cookie != line.cookie) {
      return;
    }
  }
  IrqHandlerRegistry::ThreadedDispatchResult result = {};
  const bool admitted = self->m_Handlers.dispatchThreaded(FirstVector + slot, cookie, result);
  bool mask = !admitted;
  if (admitted) {
    LockGuard<Spinlock> guard(self->m_Lock);
    Line& line = self->m_Lines[slot];
    if (line.enabled && !line.removing && line.mode == Mode::Threaded && cookie == line.cookie) {
      if (result.allowRearm) {
        line.unhandled = 0;
      } else if (!line.spuriousSafe) {
        // Polling can consume a completion before its queued MSI worker runs.
        // Tolerate a few empty edges, but mask a persistently unclaimed source.
        if (line.unhandled < 8) {
          ++line.unhandled;
        }
        mask = line.unhandled == 8;
      }
    }
  }
  if (mask) {
    self->quarantine(slot, cookie);
  }
}

#endif
