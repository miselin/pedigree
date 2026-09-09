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

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Bus.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/processor/IoPort.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Bar.h"
#include "ProbeBars.h"
#include "modules/Module.h"
#include "pci_list.h"

#define CONFIG_ADDRESS 0
#define CONFIG_DATA 4

#define MAX_BUS 4

static IoPort configSpace("PCI config space");

union ConfigAddress {
  struct {
    uint32_t always0 : 2;
    uint32_t offset : 6;
    uint32_t function : 3;
    uint32_t device : 5;
    uint32_t bus : 8;
    uint32_t reserved : 7;
    uint32_t enable : 1;
  } __attribute__((packed));
  uint32_t raw;
};

static void readConfigSpace(Device* pDev, PciBus::ConfigSpace* pCs) {
  uint32_t* pCs32 = reinterpret_cast<uint32_t*>(pCs);
  for (unsigned int i = 0; i < sizeof(PciBus::ConfigSpace) / 4; i++) {
    pCs32[i] = PciBus::instance().readConfigSpace(pDev, i);
  }
}

static const char* getVendor(uint16_t vendor) {
  for (unsigned int i = 0; i < PCI_VENTABLE_LEN; i++) {
    if (PciVenTable[i].VenId == vendor)
      return PciVenTable[i].VenShort;
  }
  return "";
}

static const char* getDevice(uint16_t vendor, uint16_t device) {
  for (unsigned int i = 0; i < PCI_DEVTABLE_LEN; i++) {
    if (PciDevTable[i].VenId == vendor && PciDevTable[i].DevId == device)
      return PciDevTable[i].ChipDesc;
  }
  return "";
}

static bool entry() {
  for (int iBus = 0; iBus < MAX_BUS; iBus++) {
    // Firstly add the ISA bus.
    char* str = new char[256];
    StringFormat(str, "PCI #%d", iBus);
    Bus* pBus = new Bus(str);
    pBus->setSpecificType(String("pci"));

    for (int iDevice = 0; iDevice < 32; iDevice++) {
      bool bIsMultifunc = false;
      for (int iFunc = 0; iFunc < 8; iFunc++) {
        if (iFunc > 0 && !bIsMultifunc)
          break;

        Device* pDevice = new Device();
        pDevice->setPciPosition(iBus, iDevice, iFunc);

        uint32_t vendorAndDeviceId = PciBus::instance().readConfigSpace(pDevice, 0);
        if ((vendorAndDeviceId & 0xFFFF) == 0xFFFF || (vendorAndDeviceId & 0xFFFF) == 0) {
          delete pDevice;
          /// \note In some cases there are gaps between functions,
          /// so it shouldn't just skip the rest of the functions
          /// left of the current device. eddyb
          continue;
          // break;
        }

        PciBus::ConfigSpace cs;
        readConfigSpace(pDevice, &cs);

        if (cs.header_type & 0x80)
          bIsMultifunc = true;

        NOTICE("PCI: " << Dec << iBus << ":" << iDevice << ":" << iFunc << "\t Vendor:" << Hex
                       << cs.vendor << " Device:" << cs.device);

        char c[256];
        StringFormat(c, "%s - %s", getDevice(cs.vendor, cs.device), getVendor(cs.vendor));
        pDevice->setSpecificType(String(c));
        NOTICE("PCI:     " << c);
        pDevice->setPciIdentifiers(cs.class_code, cs.subclass, cs.vendor, cs.device, cs.progif);
        NOTICE("PCI:     Class: " << cs.class_code << " Subclass: " << cs.subclass
                                  << " ProgIF: " << cs.progif);

        auto& pci = PciBus::instance();
        const PciBar::Probe bars = PciBar::probe(pci, pDevice, cs);
        if (bars.result == PciBar::ProbeResult::DecodeDisableFailed) {
          ERROR("PCI: cannot disable decoding for BAR sizing");
          delete pDevice;
          continue;
        }
        if (bars.result == PciBar::ProbeResult::RestoreFailed) {
          ERROR("PCI: BAR/command restoration failed; function left disabled");
          delete pDevice;
          continue;
        }
        const size_t barCount = bars.count;
        for (size_t l = 0; l < barCount; ++l) {
          const bool wide = !(cs.bar[l] & 1U) && (cs.bar[l] & 6U) == 4;
          if (wide && l + 1 == barCount)
            break;
          const uint32_t high = wide ? cs.bar[l + 1] : 0;
          const uint32_t maskHigh = wide ? bars.masks[l + 1] : 0;
          PciBar::Mapping mapping;
          if (PciBar::decode(cs.bar[l], high, bars.masks[l], maskHigh, mapping) &&
              mapping.base <= ~uintptr_t{0} && mapping.bytes <= ~size_t{0}) {
            StringFormat(c, "bar%u", static_cast<unsigned>(l));
            NOTICE("PCI:     BAR" << Dec << l << Hex << ": " << mapping.base << ".."
                                  << (mapping.base + mapping.bytes) << " (" << mapping.io << ")");
            pDevice->addresses().pushBack(
                new Device::Address(String(c), static_cast<uintptr_t>(mapping.base),
                                    static_cast<size_t>(mapping.bytes), mapping.io));
          }
          if (wide)
            ++l;
        }

        NOTICE("PCI:     IRQ: L" << cs.interrupt_line << " P" << cs.interrupt_pin);
        pDevice->setInterruptNumber(cs.interrupt_line);
        pBus->addChild(pDevice);
        pDevice->setParent(pBus);

        pDevice->setPciConfigHeader(cs);
      }
    }

    // If the bus was actually populated...
    if (pBus->getNumChildren() > 0) {
      Device::addToRoot(pBus);
    } else {
      delete pBus;
    }
  }

  return true;
}

static void exit() {}

// The enumerated nodes outlive module initialization and have no removal path.
MODULE_INFO_NON_UNLOADABLE("pci-enumeration", &entry, &exit);
