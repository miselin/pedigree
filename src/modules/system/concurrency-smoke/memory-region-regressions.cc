/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"

bool runAnonymousMemoryRegionRegression() {
  NOTICE("QEMU-CONCURRENCY-TEST: BEGIN anonymous-memory-region-release");

  PhysicalMemoryManager& memory = PhysicalMemoryManager::instance();
  const physical_uintptr_t page = memory.allocatePage();
  if (!page) {
    ERROR("QEMU anonymous MemoryRegion regression could not allocate a page");
    return false;
  }

  MemoryRegion region("QEMU anonymous MemoryRegion regression");
  if (!memory.allocateRegion(
          region, 1,
          PhysicalMemoryManager::force | PhysicalMemoryManager::continuous |
              PhysicalMemoryManager::anonymous,
          VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write, page)) {
    memory.freePage(page);
    ERROR("QEMU anonymous MemoryRegion regression could not map its page");
    return false;
  }

  void* address = region.virtualAddress();
  VirtualAddressSpace& addressSpace = VirtualAddressSpace::getKernelAddressSpace();
  if (!addressSpace.isMapped(address)) {
    region.free();
    memory.freePage(page);
    ERROR("QEMU anonymous MemoryRegion regression was not mapped");
    return false;
  }

  region.free();
  if (addressSpace.isMapped(address)) {
    addressSpace.unmap(address);
    memory.freePage(page);
    ERROR("QEMU anonymous MemoryRegion release left a stale mapping");
    return false;
  }

  memory.freePage(page);
  NOTICE("QEMU-CONCURRENCY-TEST: PASS anonymous-memory-region-release");
  return true;
}
