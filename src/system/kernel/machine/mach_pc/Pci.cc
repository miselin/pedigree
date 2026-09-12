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

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/machine/PciConfigAccess.h"
#include "pedigree/kernel/processor/IoPort.h"

#include "Pic.h"

namespace {
IoPort configSpace("PCI config space");
Spinlock configLock(false);
bool configAvailable = false;
PciConfigAccess<IoPort, Spinlock> config(configSpace, configLock);

struct FunctionConfig {
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
};

bool readFunction(Device* device, uint16_t offset, uint8_t width, uint32_t& value) {
  return configAvailable && device &&
         config.read(device->getPciBusPosition(), device->getPciDevicePosition(),
                     device->getPciFunctionNumber(), offset, width, value);
}
bool writeFunction(Device* device, uint16_t offset, uint8_t width, uint32_t value) {
  return configAvailable && device &&
         config.write(device->getPciBusPosition(), device->getPciDevicePosition(),
                      device->getPciFunctionNumber(), offset, width, value);
}
}  // namespace

PciBus PciBus::m_Instance;
PciBus::PciBus() {}
PciBus::~PciBus() {}

void PciBus::initialise() {
  if (!configSpace.allocate(0xCF8, 8)) {
    ERROR("PCI: Config space - unable to allocate IO port!");
    return;
  }
  {
    LockGuard<Spinlock> guard(configLock);
    configSpace.write32(0x80000000, 0);
    configAvailable = configSpace.read32(0) == 0x80000000;
  }
  if (!configAvailable)
    ERROR("PCI: Controller not detected.");
}

uint32_t PciBus::readConfigSpace(Device* device, uint8_t offset) {
  uint32_t value = 0xffffffffU;
  readConfig32(device, uint16_t{offset} * 4, value);
  return value;
}
uint32_t PciBus::readConfigSpace(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
  uint32_t value = 0xffffffffU;
  if (configAvailable)
    config.read(bus, device, function, uint16_t{offset} * 4, 4, value);
  return value;
}
void PciBus::writeConfigSpace(Device* device, uint8_t offset, uint32_t value) {
  writeConfig32(device, uint16_t{offset} * 4, value);
}
void PciBus::writeConfigSpace(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                              uint32_t value) {
  if (configAvailable)
    config.write(bus, device, function, uint16_t{offset} * 4, 4, value);
}
bool PciBus::readConfig8(Device* device, uint16_t offset, uint8_t& value) {
  uint32_t data = 0;
  if (!readFunction(device, offset, 1, data))
    return false;
  value = data;
  return true;
}
bool PciBus::readConfig16(Device* device, uint16_t offset, uint16_t& value) {
  uint32_t data = 0;
  if (!readFunction(device, offset, 2, data))
    return false;
  value = data;
  return true;
}
bool PciBus::readConfig32(Device* device, uint16_t offset, uint32_t& value) {
  return readFunction(device, offset, 4, value);
}
bool PciBus::writeConfig8(Device* device, uint16_t offset, uint8_t value) {
  return writeFunction(device, offset, 1, value);
}
bool PciBus::writeConfig16(Device* device, uint16_t offset, uint16_t value) {
  return writeFunction(device, offset, 2, value);
}
bool PciBus::writeConfig32(Device* device, uint16_t offset, uint32_t value) {
  return writeFunction(device, offset, 4, value);
}
bool PciBus::updateCommand(Device* device, uint16_t clearBits, uint16_t setBits) {
  return configAvailable && device &&
         config.updateCommand(device->getPciBusPosition(), device->getPciDevicePosition(),
                              device->getPciFunctionNumber(), clearBits, setBits);
}
bool PciBus::inspectFunction(Device* device, PciFunctionState::State& state) {
  FunctionConfig function{device};
  return PciFunctionState::inspect(function, state) &&
         state.interruptLine == device->getInterruptNumber();
}
bool PciBus::disableMessageInterrupts(Device* device, const PciFunctionState::State& state) {
  FunctionConfig function{device};
  return PciFunctionState::disableMessageInterrupts(function, state);
}
bool PciBus::resourcesUnchanged(Device* device, const PciFunctionState::State& state) {
  FunctionConfig function{device};
  return PciFunctionState::resourcesUnchanged(function, state);
}

bool PciBus::reserveLegacyInterrupt(uint8_t irq) {
  return Machine::instance().getIrqManager() == &Pic::instance() &&
         Pic::instance().reservePciRoute(irq);
}
