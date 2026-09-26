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

#ifndef PCI_COMMON_H
#define PCI_COMMON_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/PciExtendedCapabilities.h"
#include "pedigree/kernel/machine/PciFunctionState.h"
#include "pedigree/kernel/processor/types.h"

class Device;

/** Architecture-independent interface to a PCI bus */
class EXPORTED_PUBLIC PciBus {
 public:
  class DmaMapping {
   public:
    DmaMapping() = default;
    ~DmaMapping() {
      release();
    }
    DmaMapping(const DmaMapping&) = delete;
    DmaMapping& operator=(const DmaMapping&) = delete;

    uint32_t address() const {
      return m_Address;
    }
    void release();

   private:
    friend class PciBus;
    Device* m_Device = nullptr;
    uint32_t m_Address = 0;
    uint16_t m_Token = 0;
  };

  PciBus();
  virtual ~PciBus();

  static PciBus& instance() {
    return m_Instance;
  }

  /**
   * Initialises the object for use.
   */
  void initialise();

  /**
   * Reads from the configuration space
   * \param pDev the device to read configuration space for
   * \param offset the dword index into the configuration space to read
   */
  uint32_t readConfigSpace(Device* pDev, uint8_t offset);

  /**
   * Reads from the configuration space
   * \param bus bus number for the read address
   * \param device device number for the read address
   * \param function function number for the read address
   * \param offset the dword index into the configuration space to read
   */
  uint32_t readConfigSpace(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);

  /**
   * Writes to the configuration space
   * \param pDev the device to write configuration space for
   * \param offset the dword index into the configuration space to write
   * \param data the data to write
   */
  void writeConfigSpace(Device* pDev, uint8_t offset, uint32_t data);

  /**
   * Writes to the configuration space
   * \param bus bus number for the write address
   * \param device device number for the write address
   * \param function function number for the write address
   * \param offset the dword index into the configuration space to write
   * \param data the data to write
   */
  void writeConfigSpace(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                        uint32_t data);

  // Byte offsets, checked before I/O; subword writes never touch adjacent W1C fields.
  bool readConfig8(Device* device, uint16_t offset, uint8_t& value);
  bool readConfig16(Device* device, uint16_t offset, uint16_t& value);
  bool readConfig32(Device* device, uint16_t offset, uint32_t& value);
  bool writeConfig8(Device* device, uint16_t offset, uint8_t value);
  bool writeConfig16(Device* device, uint16_t offset, uint16_t value);
  bool writeConfig32(Device* device, uint16_t offset, uint32_t value);
  PciExtendedCapabilities::FindResult findExtendedCapability(
      Device* device, uint16_t id, PciExtendedCapabilities::Capability& result);
  // Reserve a level-triggered PIC line before programming a chipset route.
  bool reserveLegacyInterrupt(uint8_t irq);
  bool updateCommand(Device* device, uint16_t clearBits, uint16_t setBits);
  bool inspectFunction(Device* device, PciFunctionState::State& state,
                       bool requireLegacyInterrupt = true);
  bool disableMessageInterrupts(Device* device, const PciFunctionState::State& state);
  bool enableMsi(Device* device, uint64_t address, uint16_t data);
  bool disableMsi(Device* device);
  bool enableMsix(Device* device, uint64_t address, uint32_t data);
  bool enableMsixVectors(Device* device, uint64_t address, const uint32_t* data, size_t count,
                         bool* touched = nullptr);
  bool setMsixVectorMask(Device* device, size_t index, bool masked);
  bool disableMsix(Device* device);
  bool resourcesUnchanged(Device* device, const PciFunctionState::State& state);
  /** Translate a PCI BAR address into the CPU's physical address space. */
  bool translateAddress(uint64_t pciAddress, uint64_t bytes, bool io, uint64_t& cpuPhysical);
  /** The host bridge's enumerable bus range. */
  bool busRange(uint8_t& first, uint8_t& last);
  uint32_t interruptRoute(uint8_t bus, uint8_t device, uint8_t function, uint8_t pin);
  /** Assign an unconfigured BAR from a host bridge window. */
  bool assignBar(Device* device, uint8_t index, uint32_t low, uint32_t high, uint32_t maskLow,
                 uint32_t maskHigh);
  /** Attach a PCI function to a translated DMA domain before bus mastering. */
  bool attachDmaRemapping(Device* device);
  bool hasDmaRemapping(Device* device) const;
  /** Map one pinned physical page to a 32-bit device address for the mapping lifetime. */
  bool mapDmaPage(Device* device, physical_uintptr_t physical, size_t bytes, DmaMapping& mapping);
  void unmapDmaPage(Device* device, uint16_t token);

  struct ConfigSpace {
    uint16_t vendor;
    uint16_t device;
    uint16_t command;
    uint16_t status;
    uint8_t revision;
    uint8_t progif;
    uint8_t subclass;
    uint8_t class_code;
    uint8_t cache_line_size;
    uint8_t latency_timer;
    uint8_t header_type;
    uint8_t bist;
    uint32_t bar[6];
    uint32_t cardbus_pointer;
    uint16_t subsys_vendor;
    uint16_t subsys_id;
    uint32_t rom_base_address;
    uint32_t reserved0;
    uint32_t reserved1;
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    uint8_t min_grant;
    uint8_t max_latency;
  } __attribute__((packed));

 private:
  static PciBus m_Instance;
};

inline void PciBus::DmaMapping::release() {
  if (m_Token) {
    PciBus::instance().unmapDmaPage(m_Device, m_Token);
  }
  m_Device = nullptr;
  m_Address = 0;
  m_Token = 0;
}

inline PciExtendedCapabilities::FindResult PciBus::findExtendedCapability(
    Device* device, uint16_t id, PciExtendedCapabilities::Capability& result) {
  if (!device) {
    return PciExtendedCapabilities::FindResult::Unavailable;
  }
  struct Config {
    PciBus& bus;
    Device* device;
    bool read32(uint16_t offset, uint32_t& value) {
      return bus.readConfig32(device, offset, value);
    }
  } config{*this, device};
  return PciExtendedCapabilities::find(config, id, result);
}

#endif
