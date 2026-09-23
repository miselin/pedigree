/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef KERNEL_MACHINE_VIRT_DEVICETREE_H
#define KERNEL_MACHINE_VIRT_DEVICETREE_H

#include "pedigree/kernel/BootstrapInfo.h"

#include <stddef.h>
#include <stdint.h>

struct VirtMemoryRegion {
  uint64_t base;
  uint64_t size;
};

struct VirtMmioDevice {
  uint64_t base;
  uint64_t size;
  uint32_t irq;
};

struct VirtPciHost {
  uint64_t base;
  uint64_t size;
  uint32_t firstBus;
  uint32_t lastBus;
};

struct VirtPciWindow {
  uint32_t space;
  uint64_t pciBase;
  uint64_t cpuBase;
  uint64_t size;
};

/** Early, allocation-free discovery of ARM virt platform hardware. */
class VirtDeviceTree {
 public:
  static bool initialise(const void* dtb);
  static bool initialiseAcpi(uint64_t rsdpPhysical,
                             const BootstrapStruct_t::MemoryMapEntry* memoryMap,
                             size_t memoryMapCount);
  static bool valid();
  static bool memoryRegion(size_t index, VirtMemoryRegion& region);
  static bool virtioMmio(size_t index, VirtMmioDevice& device);
  static bool pciHost(VirtPciHost& host);
  static bool pciWindow(size_t index, VirtPciWindow& window);
  static bool pciTranslate(uint64_t address, uint64_t size, bool io, uint64_t& physical);
  static uint32_t pciInterrupt(uint8_t bus, uint8_t device, uint8_t function, uint8_t pin);
  /** Kernel direct-map virtual addresses for platform MMIO. */
  static uintptr_t uartBase();
  static uint32_t uartIrq();
  static uintptr_t rtcBase();
  static uint32_t gicVersion();
  static uintptr_t gicDistributorBase();
  static uintptr_t gicCpuBase();
  static uintptr_t gicRedistributorBase();
  static uint32_t physicalTimerIrq();
  static uint32_t virtualTimerIrq();
  static bool psciAvailable();
  static bool psciUsesHvc();
  static bool initrd(uint64_t& start, uint64_t& end);
  static const char* bootargs();
  static size_t blobSize();
};

extern "C" void virtSetDeviceTree(const void* dtb);
extern "C" bool virtGetMemoryRegion(size_t index, uint64_t* base, uint64_t* size);
extern "C" bool virtGetVirtioMmio(size_t index, uint64_t* base, uint64_t* size, uint32_t* irq);
extern "C" bool virtGetInitrd(uint64_t* start, uint64_t* end);
extern "C" bool virtGetBootargs(const char** text);

#endif
