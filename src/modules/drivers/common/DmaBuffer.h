/*
 * Copyright (c) 2026, Pedigree Developers
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

#ifndef PEDIGREE_MODULES_DRIVERS_COMMON_DMABUFFER_H
#define PEDIGREE_MODULES_DRIVERS_COMMON_DMABUFFER_H

#include "pedigree/kernel/TargetInfo.h"

namespace DriverDma {

constexpr size_t pageCountForBytes(size_t byteCount, size_t pageSize = TargetInfo::getPageSize()) {
  return byteCount ? ((byteCount - 1) / pageSize) + 1 : 0;
}

constexpr bool physicalRangeFits(physical_uintptr_t address, size_t byteCount,
                                 physical_uintptr_t highestAddress) {
  return !byteCount || (address <= highestAddress && (byteCount - 1) <= (highestAddress - address));
}

constexpr bool physicalEndpointsAreContiguous(physical_uintptr_t first, physical_uintptr_t last,
                                              size_t byteCount, physical_uintptr_t highestAddress) {
  return byteCount && physicalRangeFits(first, byteCount, highestAddress) &&
         last == first + (byteCount - 1);
}

/** Translate an exact virtual byte address rather than just its target-page base. */
template <size_t PageBytes = TargetInfo::getPageSize(), typename AddressSpace>
bool virtualToPhysical(AddressSpace& addressSpace, uintptr_t virtualAddress,
                       physical_uintptr_t& physicalAddress) {
  static_assert(PageBytes && !(PageBytes & (PageBytes - 1)),
                "DMA mapping pages must be a power of two");

  void* address = reinterpret_cast<void*>(virtualAddress);
  if (!addressSpace.isMapped(address))
    return false;

  physical_uintptr_t physicalPage = 0;
  size_t flags = 0;
  addressSpace.getMapping(address, physicalPage, flags);

  const size_t pageOffset = virtualAddress & (PageBytes - 1);
  const physical_uintptr_t highestPhysical = static_cast<physical_uintptr_t>(-1);
  if (physicalPage > highestPhysical - pageOffset)
    return false;

  physicalAddress = physicalPage + pageOffset;
  return true;
}

/**
 * Translate a virtual byte range which must be physically contiguous.
 *
 * Single-address DMA descriptors cannot describe a target-page transition to
 * unrelated physical storage, so every crossed mapping is checked explicitly.
 */
template <size_t PageBytes = TargetInfo::getPageSize(), typename AddressSpace>
bool contiguousVirtualRangeToPhysical(AddressSpace& addressSpace, uintptr_t virtualAddress,
                                      size_t byteCount, physical_uintptr_t& physicalAddress) {
  static_assert(PageBytes && !(PageBytes & (PageBytes - 1)),
                "DMA mapping pages must be a power of two");

  if (!byteCount)
    return false;

  const uintptr_t highestVirtual = static_cast<uintptr_t>(-1);
  if ((byteCount - 1) > highestVirtual - virtualAddress)
    return false;

  if (!virtualToPhysical<PageBytes>(addressSpace, virtualAddress, physicalAddress))
    return false;

  const physical_uintptr_t highestPhysical = static_cast<physical_uintptr_t>(-1);
  if ((byteCount - 1) > highestPhysical - physicalAddress)
    return false;

  size_t consumed = PageBytes - (virtualAddress & (PageBytes - 1));
  if (consumed >= byteCount)
    return true;

  while (consumed < byteCount) {
    physical_uintptr_t nextPhysical = 0;
    if (!virtualToPhysical<PageBytes>(addressSpace, virtualAddress + consumed, nextPhysical) ||
        nextPhysical != physicalAddress + consumed) {
      return false;
    }

    const size_t remaining = byteCount - consumed;
    consumed += remaining < PageBytes ? remaining : PageBytes;
  }

  return true;
}

static_assert(pageCountForBytes(0, 4096) == 0);
static_assert(pageCountForBytes(4096, 4096) == 1);
static_assert(pageCountForBytes(4097, 4096) == 2);
static_assert(pageCountForBytes(16384, 16384) == 1);
static_assert(pageCountForBytes(20480, 16384) == 2);
static_assert(physicalRangeFits(0xfffff000, 4096, 0xffffffff));
static_assert(!physicalRangeFits(0xfffff001, 4096, 0xffffffff));
static_assert(physicalEndpointsAreContiguous(0x1000, 0x1fff, 4096, 0xffffffff));
static_assert(!physicalEndpointsAreContiguous(0x1000, 0x2fff, 4096, 0xffffffff));
static_assert(!physicalEndpointsAreContiguous(0xfffff001, 0xffffffff, 4096, 0xffffffff));

}  // namespace DriverDma

#endif
