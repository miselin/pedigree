/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef KERNEL_MACHINE_VIRT_ACPI_H
#define KERNEL_MACHINE_VIRT_ACPI_H

#include "DeviceTree.h"

struct VirtAcpiInfo {
  VirtPciHost pciHost;
  VirtPciWindow pciWindows[8];
  size_t pciWindowCount;
  uint32_t pciIrq[32][4];
  VirtMsiController msiController;
  uintptr_t uart;
  uintptr_t rtc;
  uintptr_t gicDistributor;
  uintptr_t gicCpu;
  uintptr_t gicRedistributor;
  uint32_t gicVersion;
  uint32_t msiItsId;
  uint32_t uartIrq;
  uint32_t physicalTimerIrq;
  uint32_t virtualTimerIrq;
  bool pciMsiIdentity;
  bool psciAvailable;
  bool psciHvc;
};

bool virtParseAcpi(uint64_t rsdpPhysical, const BootstrapStruct_t::MemoryMapEntry* memoryMap,
                   size_t memoryMapCount, VirtAcpiInfo& info);

#endif
