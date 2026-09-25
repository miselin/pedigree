/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_PCI_MESSAGE_BAR_H
#define PEDIGREE_PCI_MESSAGE_BAR_H

#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/PciFunctionState.h"
#include "pedigree/kernel/processor/IoBase.h"

namespace PciFunctionState {
inline IoBase* msixTableIo(Device* device, const State& state, const MsixTable& table, bool map) {
  if (!device)
    return nullptr;
  const PciBus::ConfigSpace original = device->getPciConfigHeader();
  const auto findBar = [&](uint8_t index, uint64_t end) -> Device::Address* {
    if (index >= 6 || state.bars[index] != original.bar[index] || (state.bars[index] & 1U) ||
        ((state.bars[index] & 6U) != 0 && (state.bars[index] & 6U) != 4))
      return nullptr;
    for (uint8_t i = 0; i < index;) {
      i += !(state.bars[i] & 1U) && (state.bars[i] & 6U) == 4 ? 2 : 1;
      if (i > index)
        return nullptr;
    }
    if ((state.bars[index] & 6U) == 4 &&
        (index == 5 || state.bars[index + 1] != original.bar[index + 1]))
      return nullptr;
    char name[] = "bar0";
    name[3] = '0' + index;
    for (auto* address : device->addresses()) {
      if (address->m_Name == name && !address->m_IsIoSpace && end <= address->m_Size)
        return address;
    }
    return nullptr;
  };
  const uint64_t tableEnd = uint64_t(table.offset) + uint64_t(table.vectors) * 16;
  const uint64_t pbaEnd = uint64_t(table.pbaOffset) + uint64_t((table.vectors + 63) / 64) * 8;
  Device::Address* address = findBar(table.bar, tableEnd);
  if (!address || !findBar(table.pbaBar, pbaEnd))
    return nullptr;
  if (map)
    address->map();
  return address->m_Io && address->m_Io->size() >= tableEnd ? address->m_Io : nullptr;
}
}  // namespace PciFunctionState

#endif
