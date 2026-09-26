/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_PCI_FIRMWARE_H
#define PEDIGREE_PCI_FIRMWARE_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"

namespace PciFirmware {
constexpr size_t MaxWindows = 16;

struct Window {
  uint64_t pciBase = 0;
  uint64_t cpuBase = 0;
  uint64_t size = 0;
  bool io = false;
  bool prefetchable = false;
};

struct Root {
  uint16_t segment = 0;
  uint8_t firstBus = 0;
  uint8_t lastBus = 0;
  Window windows[MaxWindows] = {};
  size_t windowCount = 0;
};

constexpr uint32_t NativeHotplug = 1U << 0;
constexpr uint32_t NativeAer = 1U << 3;
constexpr uint32_t NativePcieCapability = 1U << 4;

EXPORTED_PUBLIC bool discover();
EXPORTED_PUBLIC const Root* rootForBus(uint8_t bus);
EXPORTED_PUBLIC bool requestNativeControl(uint8_t bus, uint32_t bits);
EXPORTED_PUBLIC uint32_t nativeControl(uint8_t bus);
}  // namespace PciFirmware

#endif
