/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/time/Time.h"

#include "Ehci.h"

namespace {
struct FirmwareConfig {
  Device* device;
  bool read32(uint16_t offset, uint32_t& value) {
    return PciBus::instance().readConfig32(device, offset, value);
  }
  bool write32(uint16_t offset, uint32_t value) {
    return PciBus::instance().writeConfig32(device, offset, value);
  }
  bool write8(uint16_t offset, uint8_t value) {
    return PciBus::instance().writeConfig8(device, offset, value);
  }
  void delay() {
    Time::delay(Time::Multiplier::Millisecond);
  }
};
}  // namespace

bool Ehci::acquireFirmware(uint16_t capability) {
  FirmwareConfig config{this};
  const auto result = m_Handoff.acquire(capability, config);
  if (result == EhciHandoff::AcquireResult::Acquired ||
      result == EhciHandoff::AcquireResult::AlreadyUnowned)
    return true;
  const char* reason = "configuration access failed";
  switch (result) {
    case EhciHandoff::AcquireResult::InvalidCapability:
      reason = "invalid legacy capability";
      break;
    case EhciHandoff::AcquireResult::OwnershipTimeout:
      reason = "BIOS ownership did not clear within 1 second";
      break;
    case EhciHandoff::AcquireResult::OwnershipMismatch:
      reason = "OS ownership was not retained";
      break;
    case EhciHandoff::AcquireResult::SmiDisableFailure:
      reason = "legacy SMI controls did not disable";
      break;
    default:
      break;
  }
  ERROR("EHCI: firmware handoff failed: " << reason);
  return false;
}

void Ehci::returnToFirmware() {
  if (!m_Handoff.wasBiosOwned())
    return;
  FirmwareConfig config{this};
  if (m_HardwareTouched) {
    // shutdownController has stopped DMA and detached every OS schedule. Give
    // firmware its decoder/DMA permissions back, never its stale queue pointers.
    constexpr uint16_t CommandMask = 2 | 4 | 0x400;
    if (!PciBus::instance().updateCommand(this, CommandMask, m_FirmwarePciCommand & CommandMask)) {
      ERROR("EHCI: firmware recovery could not restore PCI access; controller remains stopped");
      return;
    }
  }
  const auto result = m_Handoff.acquired() ? m_Handoff.release(config, m_ControllerStopped)
                                           : m_Handoff.withdrawRequest(config);
  if (result == EhciHandoff::ReleaseResult::FirmwareConfirmed)
    NOTICE("EHCI: firmware reclaimed controller; legacy input recovery requested");
  else if (result == EhciHandoff::ReleaseResult::Released)
    WARNING("EHCI: OS ownership released; firmware did not confirm reclaim within 1 second");
  else if (result != EhciHandoff::ReleaseResult::NotOwned)
    ERROR("EHCI: firmware recovery failed; legacy USB input may remain unavailable");
}

bool Ehci::beginStartupActivity() {
  LockGuard<Spinlock> guard(m_StartupLock);
  return m_Startup.begin();
}

void Ehci::endStartupActivity(UsbStartup::Outcome outcome) {
  LockGuard<Spinlock> guard(m_StartupLock);
  if (m_Startup.finish(outcome))
    m_RecoveryWake.release();
}

void Ehci::completeInitialPort(size_t port) {
  LockGuard<Spinlock> guard(m_StartupLock);
  const uint16_t bit = uint16_t{1} << port;
  if (!(m_InitialPortMask & bit))
    return;
  m_InitialPortMask &= ~bit;
  if (!m_InitialPortMask && m_Startup.finish(UsbStartup::Outcome::Neutral))
    m_RecoveryWake.release();
}

void Ehci::startFirmwareRecovery() {
#if THREADS
  if (!m_Handoff.wasBiosOwned())
    return;
  m_RecoveryThread.adopt(
      new Thread(Processor::information().getCurrentThread()->getParent(), recoverFirmware, this));
  m_RecoveryThread->setName("EHCI firmware recovery");
#endif
}

int Ehci::recoverFirmware(void* parameter) {
  TerminationDeferral lifetime;
  auto* controller = static_cast<Ehci*>(parameter);
  const bool woken = controller->m_RecoveryWake.acquireForCompletion();
  (void)woken;
  if (controller->m_RecoveryStopping)
    return 0;
  WARNING("EHCI: startup failed before a device driver became usable; returning to firmware");
  controller->shutdownController();
  controller->returnToFirmware();
  return 0;
}
