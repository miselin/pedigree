/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "PciePorts.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/machine/PciAer.h"
#include "pedigree/kernel/machine/PciExpress.h"
#include "pedigree/kernel/machine/PciFirmware.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/RequestQueue.h"

#include "Enumeration.h"

namespace {
constexpr size_t MaxPorts = 16;
constexpr uint32_t RootReporting = 7;
constexpr uint32_t RootStatusBits = 0x7f;
constexpr uint16_t SlotInterrupts = 0x103f;
constexpr uint16_t SlotEvents = 0x10f;

void waitFor(Time::Timestamp duration) {
  const auto deadline = Time::getTicks() + duration;
  for (auto now = Time::getTicks(); now < deadline; now = Time::getTicks()) {
    Time::delay(deadline - now);
  }
}

class Port;
bool slotWorkAvailable();
void scheduleSlot(Port* port);

struct Config {
  Device* device;
  bool read8(uint16_t offset, uint8_t& value) {
    return PciBus::instance().readConfig8(device, offset, value);
  }
  bool read16(uint16_t offset, uint16_t& value) {
    return PciBus::instance().readConfig16(device, offset, value);
  }
  bool read32(uint16_t offset, uint32_t& value) {
    return PciBus::instance().readConfig32(device, offset, value);
  }
  bool write16(uint16_t offset, uint16_t value) {
    return PciBus::instance().writeConfig16(device, offset, value);
  }
  bool write32(uint16_t offset, uint32_t value) {
    return PciBus::instance().writeConfig32(device, offset, value);
  }
};

uint16_t requester(Device* device) {
  return (device->getPciBusPosition() << 8) | (device->getPciDevicePosition() << 3) |
         device->getPciFunctionNumber();
}

bool pciFunction(Device* device) {
  return device->getPciVendorId() && device->getPciVendorId() != 0xffff &&
         device->getPciBusPosition() <= 255 && device->getPciDevicePosition() < 32 &&
         device->getPciFunctionNumber() < 8;
}

bool enableReporting(Device* device) {
  Config config{device};
  PciExpress::State express;
  const auto found = PciExpress::find(config, express);
  if (found == PciExpress::FindResult::Absent) {
    return true;
  }
  if (found != PciExpress::FindResult::Found ||
      !PciFunctionState::writeVerified16(config, express.offset + 8,
                                         express.deviceControl | 0xfU)) {
    return false;
  }
  uint8_t header = 0;
  if (!config.read8(0x0e, header)) {
    return false;
  }
  if ((header & 0x7fU) == 1) {
    uint16_t control = 0, actual = 0;
    // PCIe 2.1 table 7-6: SERR Enable forwards all three error message
    // classes across a bridge. Discard Timer Status is an unrelated W1C bit.
    if (!config.read16(0x3e, control) || !config.write16(0x3e, (control | 2U) & ~0x400U) ||
        !config.read16(0x3e, actual) || ((actual ^ (control | 2U)) & ~0x400U)) {
      return false;
    }
  }
  return true;
}

class Port : public IrqHandler {
 public:
  void initialize(Device* device) {
    Config config{device};
    PciFunctionState::State function;
    uint32_t status = 0, buses = 0, reporting = 0;
    uint16_t rootControl = 0;
    auto& pci = PciBus::instance();
    if (PciExpress::find(config, m_Express) != PciExpress::FindResult::Found ||
        m_Express.type != PciExpress::Type::RootPort) {
      return;
    }
    m_Device = device;
    const auto aer = pci.findExtendedCapability(device, PciAer::CapabilityId, m_Aer);
    if (aer != PciExtendedCapabilities::FindResult::Found &&
        aer != PciExtendedCapabilities::FindResult::Absent) {
      return;
    }
    bool aerUsable = aer == PciExtendedCapabilities::FindResult::Found;
    if (aerUsable && (m_Aer.offset > 0x1000 - 0x38 ||
                      (m_Aer.next >= m_Aer.offset && m_Aer.next < m_Aer.offset + 0x38) ||
                      !config.read32(m_Aer.offset + 0x30, status) ||
                      !config.read32(m_Aer.offset + 0x2c, reporting))) {
      return;
    }
    if (!config.read32(0x18, buses) || !config.read16(m_Express.offset + 0x1c, rootControl) ||
        !pci.inspectFunction(device, function, false) || (rootControl & 8U)) {
      WARNING("PCIe: root " << Hex << requester(device)
                            << " has unusable capabilities or PME users");
      return;
    }
    m_FirstBus = buses >> 8;
    m_LastBus = buses >> 16;
    const auto* root = PciFirmware::rootForBus(device->getPciBusPosition());
    if (!root || uint8_t(buses) != device->getPciBusPosition() ||
        m_FirstBus <= device->getPciBusPosition() || m_LastBus < m_FirstBus ||
        m_LastBus > root->lastBus || (!function.msi && !function.msix)) {
      return;
    }
    // Both services must use message zero; PME remains owned by firmware.
    aerUsable = aerUsable && !((status >> 27) & 0x1fU) &&
                PciFirmware::requestNativeControl(
                    device->getPciBusPosition(),
                    PciFirmware::NativeAer | PciFirmware::NativePcieCapability);
    bool slotUsable = m_Express.slotImplemented && !m_Express.interruptMessage &&
                      (m_Express.slotCapabilities & (1U << 6)) && slotWorkAvailable() &&
                      PciFirmware::requestNativeControl(
                          device->getPciBusPosition(),
                          PciFirmware::NativeHotplug | PciFirmware::NativePcieCapability);
    if ((!aerUsable && !slotUsable) || (!aerUsable && (reporting & RootReporting)) ||
        (!slotUsable && (m_Express.slotControl & SlotInterrupts))) {
      WARNING("PCIe: root " << Hex << requester(device) << " lacks exclusive native ownership");
      return;
    }
    if (aerUsable && !setReporting(0)) {
      panic("PCIe AER could not quiesce root reporting");
    }
    if (slotUsable && !slotCommand(SlotInterrupts, 0)) {
      return;
    }
    if (!pci.disableMessageInterrupts(device, function) || !pci.updateCommand(device, 0, 4U)) {
      return;
    }
    auto* irqManager = Machine::instance().getIrqManager();
    const irq_id_t id =
        irqManager->registerPciMessageIrqHandler(this, device, IrqPolicy::pciIntxThreaded());
    uint16_t msiControl = 0, msixControl = 0;
    const bool messageEnabled =
        (function.msi && config.read16(function.msi + 2, msiControl) && (msiControl & 1U)) ||
        (function.msix && config.read16(function.msix + 2, msixControl) && (msixControl & 0x8000U));
    const bool selectedMessage =
        !aerUsable || (config.read32(m_Aer.offset + 0x30, status) && !((status >> 27) & 0x1fU));
    if (id && messageEnabled && selectedMessage) {
      if (aerUsable && enableReporting(device)) {
        auto report = [&](Device* child) -> Device* {
          if (pciFunction(child) && !enableReporting(child)) {
            WARNING("PCIe AER: could not enable reporting at " << Hex << requester(child));
          }
          return child;
        };
        auto callback = pedigree_std::make_callable(report);
        Device::foreach (callback, device);
        LockGuard<Mutex> guard(m_Lock);
        m_AerReady = setReporting(RootReporting);
        if (!m_AerReady) {
          stopReporting();
        }
      }
      if (slotUsable) {
        uint16_t slotStatus = 0;
        if (!config.read16(m_Express.offset + 0x1a, slotStatus) ||
            !config.write16(m_Express.offset + 0x1a, slotStatus & SlotEvents)) {
          panic("PCIe: could not acknowledge inherited slot events");
        }
        uint16_t enables = (1U << 3) | (1U << 5);
        if (m_Express.slotCapabilities & 1U) {
          enables |= 1U;
        }
        if (m_Express.slotCapabilities & 2U) {
          enables |= 2U;
        }
        if (m_Express.slotCapabilities & 4U) {
          enables |= 4U;
        }
        if (m_Express.linkCapabilities & (1U << 20)) {
          enables |= 1U << 12;
        }
        // Some ports expose live link status without a link-change interrupt.
        // Presence notifications remain required; lifecycle operations also
        // check the link directly. Power-fault detection is optional too.
        if (slotCommand(SlotInterrupts, enables, enables & ((1U << 12) | 2U))) {
          uint16_t enabled = 0;
          if ((enables & (1U << 12)) && config.read16(m_Express.offset + 0x18, enabled) &&
              !(enabled & (1U << 12))) {
            WARNING("PCIe hotplug: root " << Hex << requester(device)
                                          << " link-change notifications unavailable");
          }
          LockGuard<Mutex> guard(m_Lock);
          m_SlotReady = true;
        } else if (!slotCommand(SlotInterrupts, 0)) {
          panic("PCIe: could not disable an unavailable slot service");
        }
      }
    }
    if (!m_AerReady && !m_SlotReady) {
      if (id && !irqManager->unregisterHandler(id, this)) {
        panic("PCIe: could not remove an inactive port interrupt handler");
      }
      return;
    }
    if (m_AerReady) {
      NOTICE("PCIe AER: root " << Hex << requester(device) << " native reporting on IRQ " << id);
    }
    if (m_SlotReady) {
      NOTICE("PCIe hotplug: root " << Hex << requester(device) << " native slot service on IRQ "
                                   << id);
    }
    irq(id);
  }

  IrqDisposition irq(irq_id_t) override {
    LockGuard<Mutex> guard(m_Lock);
    bool handled = slotInterrupt();
    if (!m_AerReady) {
      return handled ? IrqDisposition::Handled : IrqDisposition::NotHandled;
    }
    Config config{m_Device};
    for (unsigned pass = 0; pass < 8; ++pass) {
      uint32_t status = 0, source = 0;
      if (!config.read32(m_Aer.offset + 0x30, status) ||
          !config.read32(m_Aer.offset + 0x34, source)) {
        stopReporting();
        return IrqDisposition::Handled;
      }
      if (!(status & RootStatusBits)) {
        return handled ? IrqDisposition::Handled : IrqDisposition::NotHandled;
      }
      handled = true;
      if (status & 4U) {
        m_SlotFault = true;
        const uint16_t id = source >> 16;
        const bool contained = contain(id);
        ERROR("PCIe AER: " << ((status & 0x40U) ? "fatal" : "nonfatal") << " source " << Dec
                           << (id >> 8) << ":" << ((id >> 3) & 31U) << ":" << (id & 7U) << Hex
                           << " root status=" << status << " bus master stopped=" << contained
                           << "; driver recovery required");
        if (!contained || (status & 8U) || id == requester(m_Device)) {
          containBranch();
        }
        acknowledge(id, false);
        if (status & 8U) {
          acknowledgeBranch(false);
        }
      }
      if (status & 1U) {
        const uint16_t id = source;
        NOTICE("PCIe AER: correctable source " << Dec << (id >> 8) << ":" << ((id >> 3) & 31U)
                                               << ":" << (id & 7U) << Hex
                                               << " root status=" << status);
        acknowledge(id, true);
        if (status & 2U) {
          acknowledgeBranch(true);
        }
      }
      // Only the observed W1C status bits are written, never the selector.
      if (!config.write32(m_Aer.offset + 0x30, status & RootStatusBits)) {
        stopReporting();
        return IrqDisposition::Handled;
      }
    }
    WARNING("PCIe AER: root " << Hex << requester(m_Device) << " reporting did not drain");
    stopReporting();
    return IrqDisposition::Handled;
  }

  void serviceSlot() {
    for (;;) {
      uint16_t events = 0;
      {
        LockGuard<Mutex> guard(m_Lock);
        events = m_SlotEvents;
        m_SlotEvents = 0;
        if (!events || m_SlotFault) {
          m_SlotQueued = false;
          return;
        }
      }
      processSlot(events);
    }
  }

 private:
  bool slotInterrupt() {
    if (!m_SlotReady) {
      return false;
    }
    Config config{m_Device};
    uint16_t status = 0;
    if (!config.read16(m_Express.offset + 0x1a, status)) {
      panic("PCIe: slot status became unavailable");
    }
    const uint16_t events = status & SlotEvents;
    if (!events) {
      return false;
    }
    if (!config.write16(m_Express.offset + 0x1a, events)) {
      panic("PCIe: could not acknowledge slot events");
    }
    if (!m_SlotFault) {
      m_SlotEvents |= events;
      if (!m_SlotQueued) {
        m_SlotQueued = true;
        scheduleSlot(this);
      }
    }
    return true;
  }

  bool slotCommand(uint16_t clear, uint16_t set, uint16_t optional = 0) {
    Config config{m_Device};
    bool verified = false;
    {
      LockGuard<Mutex> guard(m_Lock);
      uint16_t control = 0, status = 0;
      if (!config.read16(m_Express.offset + 0x18, control) ||
          !config.read16(m_Express.offset + 0x1a, status)) {
        return false;
      }
      // Interlock Control is a write-one toggle, not a state to preserve.
      const uint16_t desired = (control & ~(clear | 0x800U)) | set;
      if (!(set & 0x800U) && desired == control) {
        return true;
      }
      if ((status & 0x10U) && !config.write16(m_Express.offset + 0x1a, 0x10U)) {
        return false;
      }
      const bool written = config.write16(m_Express.offset + 0x18, desired);
      uint16_t actual = 0;
      verified = config.read16(m_Express.offset + 0x18, actual) && written &&
                 !((actual ^ desired) & ~(0x800U | optional));
    }
    if (m_Express.slotCapabilities & (1U << 18)) {
      return verified;
    }
    // PCIe 2.1 6.7.3.2: serialize all Slot Control writes through completion.
    // CCIE stays disabled so this polling does not depend on the shared IRQ.
    const auto deadline = Time::getTicks() + Time::Multiplier::Second;
    do {
      {
        LockGuard<Mutex> guard(m_Lock);
        uint16_t status = 0;
        if (!config.read16(m_Express.offset + 0x1a, status)) {
          verified = false;
        } else if (status & 0x10U) {
          return config.write16(m_Express.offset + 0x1a, 0x10U) && verified;
        }
      }
      Time::delay(10 * Time::Multiplier::Millisecond);
    } while (Time::getTicks() < deadline);
    WARNING("PCIe hotplug: root " << Hex << requester(m_Device) << " command completion timeout");
    return false;
  }

  bool powerIndicator(uint16_t state) {
    return !(m_Express.slotCapabilities & (1U << 4)) || slotCommand(3U << 8, state << 8);
  }

  bool interlock(bool engaged) {
    if (!(m_Express.slotCapabilities & (1U << 17))) {
      return true;
    }
    uint16_t status = 0, link = 0;
    if (!slotState(status, link)) {
      return false;
    }
    if (bool(status & (1U << 7)) == engaged) {
      return true;
    }
    if (!slotCommand(0, 1U << 11)) {
      return false;
    }
    const auto deadline = Time::getTicks() + Time::Multiplier::Second;
    do {
      if (!slotState(status, link)) {
        return false;
      }
      if (bool(status & (1U << 7)) == engaged) {
        return true;
      }
      Time::delay(10 * Time::Multiplier::Millisecond);
    } while (Time::getTicks() < deadline);
    return false;
  }

  bool slotState(uint16_t& status, uint16_t& link) {
    Config config{m_Device};
    return config.read16(m_Express.offset + 0x1a, status) &&
           config.read16(m_Express.offset + 0x12, link);
  }

  bool occupied(uint16_t status) const {
    return (status & (1U << 6)) &&
           (!(m_Express.slotCapabilities & (1U << 2)) || !(status & (1U << 5)));
  }

  bool linkActive(uint16_t link) const {
    return (m_Express.linkCapabilities & (1U << 20)) ? bool(link & (1U << 13))
                                                     : !(link & (1U << 11)) && (link & 0x3f0U);
  }

  bool slotFault() {
    LockGuard<Mutex> guard(m_Lock);
    return m_SlotFault || (m_SlotEvents & 2U);
  }

  void retainSlot() {
    {
      LockGuard<Mutex> guard(m_Lock);
      m_SlotFault = true;
    }
    // No callback may access vanished MMIO, and no DMA mapping can be freed.
    if (!PciBus::instance().updateCommand(m_Device, 4U, 0)) {
      panic("PCIe: could not contain a lost slot");
    }
    ERROR("PCIe hotplug: root "
          << Hex << requester(m_Device)
          << " lost its device or power; retaining resources, recovery required");
  }

  bool settleSlot() {
    uint16_t status = 0, link = 0;
    const auto deadline = Time::getTicks() + Time::Multiplier::Second;
    do {
      if (!slotState(status, link) || !occupied(status) || slotFault()) {
        return false;
      }
      if (linkActive(link)) {
        break;
      }
      Time::delay(10 * Time::Multiplier::Millisecond);
    } while (Time::getTicks() < deadline);
    if (!linkActive(link)) {
      return false;
    }
    // Link-active is not permission to access configuration immediately.
    waitFor(100 * Time::Multiplier::Millisecond);
    Device endpoint;
    endpoint.setPciPosition(m_FirstBus, 0, 0);
    const auto configDeadline = Time::getTicks() + Time::Multiplier::Second;
    do {
      uint32_t identity = 0;
      if (!slotState(status, link) || !occupied(status) || !linkActive(link) || slotFault()) {
        return false;
      }
      if (PciBus::instance().readConfig32(&endpoint, 0, identity) && (identity & 0xffffU) > 1 &&
          (identity & 0xffffU) != 0xffffU) {
        return true;
      }
      Time::delay(10 * Time::Multiplier::Millisecond);
    } while (Time::getTicks() < configDeadline);
    return false;
  }

  bool attentionDelay() {
    if (!powerIndicator(2)) {
      return false;
    }
    const auto deadline = Time::getTicks() + 5 * Time::Multiplier::Second;
    do {
      {
        LockGuard<Mutex> guard(m_Lock);
        if (m_SlotEvents & 1U) {
          m_SlotEvents &= ~1U;
          NOTICE("PCIe hotplug: root " << Hex << requester(m_Device) << " eject cancelled");
          return false;
        }
        if (m_SlotFault || (m_SlotEvents & 2U)) {
          return false;
        }
      }
      uint16_t status = 0, link = 0;
      if (!slotState(status, link) || !occupied(status) || !linkActive(link)) {
        retainSlot();
        return false;
      }
      Time::delay(50 * Time::Multiplier::Millisecond);
    } while (Time::getTicks() < deadline);
    return true;
  }

  bool populated() const {
    Device::TreeLockGuard guard;
    for (size_t i = 0; i < m_Device->getNumChildren(); ++i) {
      Device* child = m_Device->getChild(i);
      if (child->getSpecificType() == "pci" && child->getNumChildren()) {
        return true;
      }
    }
    return false;
  }

  void processSlot(uint16_t events) {
    uint16_t status = 0, link = 0;
    if (!slotState(status, link)) {
      retainSlot();
      return;
    }
    if ((events & 2U) || (populated() && (!occupied(status) || !linkActive(link)))) {
      retainSlot();
      return;
    }
    if (m_Ejected) {
      if (!(status & (1U << 6))) {
        m_Ejected = false;
      }
      return;
    }
    if ((events & 1U) && populated()) {
      if (!(m_Express.slotCapabilities & 2U)) {
        WARNING("PCIe hotplug: root " << Hex << requester(m_Device) << " has no power controller");
        return;
      }
      if (!attentionDelay()) {
        if (!slotFault()) {
          (void)powerIndicator(1);
        }
        return;
      }
      if (!PciEnumeration::prepareRemoveSlot(m_Device)) {
        PciEnumeration::cancelRemoveSlot(m_Device);
        (void)powerIndicator(1);
        NOTICE("PCIe hotplug: root " << Hex << requester(m_Device) << " eject denied: device busy");
        return;
      }
      if (slotFault() || !slotState(status, link) || !occupied(status) || !linkActive(link)) {
        PciEnumeration::cancelRemoveSlot(m_Device);
        retainSlot();
        return;
      }
      if (!PciEnumeration::removeSlot(m_Device)) {
        PciEnumeration::cancelRemoveSlot(m_Device);
        (void)powerIndicator(1);
        WARNING("PCIe hotplug: root " << Hex << requester(m_Device) << " driver detach failed");
        return;
      }
      m_Ejected = true;
      if (!slotCommand(1U << 10, 1U << 10)) {
        retainSlot();
        return;
      }
      waitFor(Time::Multiplier::Second);
      if (!interlock(false)) {
        retainSlot();
        return;
      }
      (void)powerIndicator(3);
      NOTICE("PCIe hotplug: root " << Hex << requester(m_Device) << " orderly eject complete");
      return;
    }
    if (!occupied(status) || populated()) {
      return;
    }
    waitFor(100 * Time::Multiplier::Millisecond);
    if (!slotState(status, link) || !occupied(status) || slotFault()) {
      return;
    }
    if ((m_Express.slotCapabilities & 2U) && !slotCommand(1U << 10, 0)) {
      return;
    }
    if (!settleSlot() || !PciEnumeration::probeSlot(m_Device)) {
      WARNING("PCIe hotplug: root " << Hex << requester(m_Device) << " insertion unavailable");
      return;
    }
    if (slotFault()) {
      retainSlot();
      return;
    }
    bool aerReady = false;
    {
      LockGuard<Mutex> guard(m_Lock);
      aerReady = m_AerReady;
    }
    if (aerReady) {
      auto report = [&](Device* child) -> Device* {
        if (pciFunction(child) && !enableReporting(child)) {
          WARNING("PCIe AER: could not enable reporting at " << Hex << requester(child));
        }
        return child;
      };
      auto callback = pedigree_std::make_callable(report);
      Device::foreach (callback, m_Device);
    }
    (void)powerIndicator(1);
    NOTICE("PCIe hotplug: root " << Hex << requester(m_Device) << " insertion complete");
  }

  bool inBranch(uint16_t id) const {
    return id == requester(m_Device) || ((id >> 8) >= m_FirstBus && (id >> 8) <= m_LastBus);
  }

  bool setReporting(uint32_t bits) {
    Config config{m_Device};
    uint32_t command = 0;
    return config.read32(m_Aer.offset + 0x2c, command) &&
           PciFunctionState::writeVerified32(config, m_Aer.offset + 0x2c,
                                             (command & ~RootReporting) | bits);
  }

  void stopReporting() {
    if (!setReporting(0)) {
      panic("PCIe AER could not disable an unserviceable interrupt source");
    }
    m_AerReady = false;
  }

  bool contain(uint16_t id) {
    if (!inBranch(id)) {
      return false;
    }
    Device source;
    source.setPciPosition(id >> 8, (id >> 3) & 31U, id & 7U);
    Config config{&source};
    uint32_t identity = 0;
    return config.read32(0, identity) && (identity & 0xffffU) && (identity & 0xffffU) != 0xffffU &&
           PciBus::instance().updateCommand(&source, 4U, 0);
  }

  void containBranch() {
    uint16_t slotStatus = 0, link = 0;
    if (m_SlotReady && slotState(slotStatus, link) &&
        (!occupied(slotStatus) || !linkActive(link))) {
      if (!contain(requester(m_Device))) {
        panic("PCIe AER could not contain a lost slot");
      }
      ERROR("PCIe AER: root " << Hex << requester(m_Device)
                              << " lost its link; retaining downstream resources");
      return;
    }
    bool stopped = contain(requester(m_Device));
    auto containChild = [&](Device* child) -> Device* {
      if (pciFunction(child) && !contain(requester(child))) {
        stopped = false;
      }
      return child;
    };
    auto callback = pedigree_std::make_callable(containChild);
    Device::foreach (callback, m_Device);
    if (!stopped) {
      panic("PCIe AER could not contain an uncorrectable error");
    }
    ERROR("PCIe AER: root " << Hex << requester(m_Device)
                            << " stopped bus mastering throughout its branch");
  }

  void acknowledgeBranch(bool correctable) {
    acknowledge(requester(m_Device), correctable);
    auto clearChild = [&](Device* child) -> Device* {
      if (pciFunction(child)) {
        acknowledge(requester(child), correctable);
      }
      return child;
    };
    auto callback = pedigree_std::make_callable(clearChild);
    Device::foreach (callback, m_Device);
  }

  void acknowledge(uint16_t id, bool correctable) {
    if (!inBranch(id)) {
      WARNING("PCIe AER: source outside root bus range " << Hex << id);
      return;
    }
    Device source;
    source.setPciPosition(id >> 8, (id >> 3) & 31U, id & 7U);
    Config config{&source};
    PciExtendedCapabilities::Capability aer;
    PciAer::Status status;
    if (PciBus::instance().findExtendedCapability(&source, PciAer::CapabilityId, aer) ==
            PciExtendedCapabilities::FindResult::Found &&
        PciAer::read(config, aer.offset, aer.next, status)) {
      const uint32_t bits = correctable ? status.correctable : status.uncorrectable;
      if (bits) {
        NOTICE("PCIe AER: requester "
               << Hex << id << " status=" << bits
               << " mask=" << (correctable ? status.correctableMask : status.uncorrectableMask));
        if (!config.write32(aer.offset + (correctable ? 0x10 : 4), bits)) {
          WARNING("PCIe AER: requester status could not be acknowledged " << Hex << id);
        }
      }
    }
    PciExpress::State express;
    if (PciExpress::find(config, express) == PciExpress::FindResult::Found) {
      const uint16_t bits = express.deviceStatus & (correctable ? 1U : 0xeU);
      if (bits && !config.write16(express.offset + 0x0a, bits)) {
        WARNING("PCIe AER: device status could not be acknowledged " << Hex << id);
      }
    }
  }

  Device* m_Device = nullptr;
  PciExpress::State m_Express;
  PciExtendedCapabilities::Capability m_Aer;
  uint8_t m_FirstBus = 0, m_LastBus = 0;
  bool m_AerReady = false;
  bool m_SlotReady = false;
  bool m_SlotFault = false;
  bool m_SlotQueued = false;
  bool m_Ejected = false;
  uint16_t m_SlotEvents = 0;
  Mutex m_Lock;
};

class SlotQueue : public RequestQueue {
 public:
  SlotQueue() : RequestQueue(MakeConstantString("PCIe slots")) {}
  ~SlotQueue() override {
    destroy();
  }

 protected:
  uint64_t executeRequest(uint64_t port, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                          uint64_t) override {
    reinterpret_cast<Port*>(static_cast<uintptr_t>(port))->serviceSlot();
    return 0;
  }
};

SlotQueue slotWork;
bool slotWorkAvailable() {
  return slotWork.getLifecycleState() == RequestQueue::LifecycleState::Accepting;
}
void scheduleSlot(Port* port) {
  if (!slotWork.publishAsyncRequest(0, reinterpret_cast<uintptr_t>(port))) {
    panic("PCIe: slot work queue unavailable");
  }
}

Port ports[MaxPorts];
bool initialized = false;
}  // namespace

void PciePorts::initialize() {
  if (initialized) {
    return;
  }
  initialized = true;
  slotWork.initialise();
  Device* candidates[MaxPorts] = {};
  size_t count = 0;
  auto collect = [&](Device* device) -> Device* {
    if (pciFunction(device) && device->getPciClassCode() == 6 &&
        device->getPciSubclassCode() == 4) {
      Config config{device};
      PciExpress::State express;
      if (PciExpress::find(config, express) == PciExpress::FindResult::Found &&
          express.type == PciExpress::Type::RootPort) {
        if (count < MaxPorts) {
          candidates[count++] = device;
        } else {
          WARNING("PCIe: root service capacity exhausted");
        }
      }
    }
    return device;
  };
  auto callback = pedigree_std::make_callable(collect);
  Device::foreach (callback, nullptr);
  for (size_t i = 0; i < count; ++i) {
    ports[i].initialize(candidates[i]);
  }
}
