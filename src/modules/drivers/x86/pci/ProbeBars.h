/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_PCI_PROBE_BARS_H
#define PEDIGREE_PCI_PROBE_BARS_H
#include <stdint.h>

namespace PciBar {
enum class ProbeResult { Success, DecodeDisableFailed, RestoreFailed };
struct Probe {
  ProbeResult result = ProbeResult::Success;
  unsigned count = 0;
  uint32_t masks[6] = {};
};

template <class Bus, class Device, class Header>
Probe probe(Bus& pci, Device* device, const Header& header) {
  Probe result;
  const uint8_t headerType = header.header_type & 0x7f;
  // Intel 324645-006, table 13-1 and section 13.1.3: QM67 LPC has no
  // standard BARs, and its IO/MEM/BME command bits are read-only ones.
  if (header.vendor == 0x8086 && header.device == 0x1c4f && header.class_code == 6 &&
      header.subclass == 1 && headerType == 0)
    return result;

  result.count = headerType == 0 ? 6 : headerType == 1 ? 2 : headerType == 2 ? 1 : 0;
  // Firmware DMA may still be active until the owning driver's handoff.
  if (!pci.updateCommand(device, 3, 0)) {
    result.result = ProbeResult::DecodeDisableFailed;
    return result;
  }
  bool restored = true;
  for (unsigned i = 0; i < result.count; ++i) {
    const uint8_t offset = 4 + i;
    pci.writeConfigSpace(device, offset, 0xffffffffU);
    result.masks[i] = pci.readConfigSpace(device, offset);
    pci.writeConfigSpace(device, offset, header.bar[i]);
    if (pci.readConfigSpace(device, offset) != header.bar[i]) {
      restored = false;
      break;
    }
  }
  if (!restored || !pci.updateCommand(device, 3, header.command & 3)) {
    (void)pci.updateCommand(device, 7, 0);
    result.result = ProbeResult::RestoreFailed;
  }
  return result;
}
}  // namespace PciBar
#endif
