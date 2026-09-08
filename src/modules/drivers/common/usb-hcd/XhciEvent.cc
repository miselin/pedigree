/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Xhci.h"
using namespace XhciHw;
bool Xhci::collectEventsLocked(bool fromInterrupt) {
  const bool credit = fromInterrupt && m_PolledInterrupt;
  if (fromInterrupt)
    m_PolledInterrupt = false;
  if (!m_Online)
    return credit;
  const uint32_t status = read(m_Op + 4);
  const uint32_t iman = read(m_Runtime + 0x20);
  const bool cause = (status & 0x1cU) || (iman & 1U);
  if (status & ((1U << 12) | 4U)) {
    failLocked();
    return true;
  }
  if (status & 0x18U)
    write(m_Op + 4, status & 0x18U);
  if (iman & 1U)
    write(m_Runtime + 0x20, 3);
  auto* entries = static_cast<volatile Trb*>(m_Events.virtualAddress());
  bool consumed = false;
  for (size_t count = 0; count < RingEntries; ++count) {
    volatile auto& source = entries[m_EventHead];
    const uint32_t control = source.control;
    if (bool(control & Cycle) != m_EventCycle)
      break;
    FENCE();
    Trb event{source.parameter, source.status, control};
    if (++m_EventHead == RingEntries) {
      m_EventHead = 0;
      m_EventCycle = !m_EventCycle;
      if (!m_EventWraps++) {
        NOTICE("xHCI: event ring wrapped");
#if PEDIGREE_USB_SMOKE_TESTS
        NOTICE("XHCI-SMOKE: event-ring-wrap");
#endif
      }
    }
    consumed = true;
    const uint32_t type = (control >> 10) & 63U;
    if (type == 33) {
      if (!m_CommandAddress || event.parameter != m_CommandAddress) {
        failLocked();
        break;
      }
      m_Commands.retire(1);
      m_CommandAddress = 0;
      m_CommandCode = event.status >> 24;
      m_CommandSlot = control >> 24;
      m_CommandDone.release();
    } else if (type == 32) {
      const bool valid = transferEventLocked(event);
#if PEDIGREE_USB_SMOKE_TESTS
      if (valid && fromInterrupt && m_Online && !m_ObservedInterruptCompletion) {
        m_ObservedInterruptCompletion = true;
        NOTICE("XHCI-SMOKE: interrupt-completion");
      }
#else
      (void)valid;
#endif
    } else if (type == 34) {
      const size_t port = (event.parameter >> 24) & 255U;
      if (!port || port > m_PortCount) {
        failLocked();
        break;
      }
      const size_t reg = m_Op + 0x400 + (port - 1) * 16;
      const uint32_t portStatus = read(reg);
      m_Ports[port - 1].resetChanges |= portStatus & ((1U << 19) | (1U << 21));
      if (portStatus & (1U << 17))
        m_Ports[port - 1].changePending = true;
      write(reg, (portStatus & 0x0e00c200U) | (portStatus & PortChanges));
      (void)read(reg);
      if ((portStatus & (1U << 17)) && !m_PortsClosing)
        notifyPortLocked(port - 1);
    } else if (type == 37) {
      failLocked();
      break;
    }
    if (!m_Online)
      break;
  }
  if (m_Online && (consumed || cause)) {
    if (!fromInterrupt)
      m_PolledInterrupt = true;
    FENCE();
    write64(m_Runtime + 0x38, m_Events.physicalAddress() + m_EventHead * sizeof(Trb) + 8);
    (void)read(m_Runtime + 0x38);
  }
  return cause || consumed || credit;
}
IrqDisposition Xhci::irq(irq_id_t) {
  LockGuard<Mutex> lock(m_Lock);
  if (m_Stopping)
    return IrqDisposition::Quiesced;
  return collectEventsLocked(true) ? IrqDisposition::Handled : IrqDisposition::NotHandled;
}
void Xhci::notifyPortLocked(size_t port) {
  if (m_PortsClosing)
    return;
  m_Ports[port].changePending = true;
  if (deferConnectionChangeIfSuppressed(port))
    return;
  const auto observation = m_PortChanges[port].observe();
  if (!UsbHcd::PortChangeRequest::canAcknowledge(observation.result)) {
    ERROR("xHCI: root-port publication failed");
    failLocked();
    return;
  }
  m_PortChanges[port].acknowledge(observation.generation);
  m_Ports[port].changePending = false;
}
size_t Xhci::currentRootPortGeneration(size_t port) const {
  return port < m_PortCount ? m_PortChanges[port].observedGeneration() : 0;
}
void Xhci::replaySuppressedConnectionChange(size_t port) {
  LockGuard<Mutex> lock(m_Lock);
  if (port < m_PortCount && !m_PortsClosing)
    notifyPortLocked(port);
}
bool Xhci::portReset(uint8_t port, bool errorResponse) {
  if (port >= m_PortCount || !m_Online)
    return false;
  const size_t reg = m_Op + 0x400 + port * 16;
  const bool warm = errorResponse && m_Ports[port].major == 3;
  const uint32_t completion = warm ? 1U << 19 : 1U << 21;
  {
    LockGuard<Mutex> lock(m_Lock);
    const uint32_t status = read(reg);
    if (!(status & 1U))
      return false;
    m_Ports[port].resetChanges = 0;
    write(reg, (status & 0x0e00c200U) | (status & completion) | (warm ? 1U << 31 : 1U << 4));
    (void)read(reg);
  }
  const auto deadline = Time::getTicks() + Time::Multiplier::Second;
  do {
    {
      LockGuard<Mutex> lock(m_Lock);
      const uint32_t ready = read(reg);
      if (!m_Online || !(ready & 1U))
        return false;
      // WPR reads as zero. Both reset forms drive PR until completion.
      if (!(ready & (1U << 4)) && ((ready | m_Ports[port].resetChanges) & completion)) {
        const uint8_t speed = (ready >> 10) & 15U;
        // CSC belongs to the event path, including disconnects during reset.
        write(reg, (ready & 0x0e00c200U) | (ready & ((1U << 19) | (1U << 21))));
        (void)read(reg);
        return (ready & 3U) == 3U && m_Ports[port].validSpeed[speed];
      }
    }
    Time::delay(Time::Multiplier::Millisecond);
  } while (Time::getTicks() < deadline);
  return false;
}
uint64_t Xhci::executeRequest(uint64_t port, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t generation) {
  if (port >= m_PortCount)
    return 0;
  UsbHcd::PortChangeRequest::Completion completion(m_PortChanges[port], generation);
  if (!completion || m_PortsClosing || !m_Online)
    return 0;
  uint32_t status = read(m_Op + 0x400 + port * 16);
  if (!(status & 1U))
    deviceDisconnected(port);
  else {
    // USB 2 port speed may be undefined until its first completed reset.
    if (!m_Ports[port].validSpeed[(status >> 10) & 15U]) {
      if (!portReset(port))
        return 0;
      status = read(m_Op + 0x400 + port * 16);
    }
    const uint8_t speed = (status >> 10) & 15U;
    if (m_Ports[port].validSpeed[speed])
      deviceConnected(port, m_Ports[port].speeds[speed]);
    else
      WARNING("xHCI: unsupported root-port speed ID " << speed);
  }
  return 0;
}
void Xhci::cancelRequest(const Request& request) {
  if (request.p1 < m_PortCount)
    m_PortChanges[request.p1].cancel(request.p8);
}
