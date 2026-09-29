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

#include <config.h>

#if MULTIPROCESSOR

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"

#include "IoApic.h"

namespace {
// Delivery Status and Remote IRR are read-only and can change during a
// redirection-table write, especially when unmasking a pending interrupt.
constexpr uint32_t RedirectionStatus = (1U << 12) | (1U << 14);
}  // namespace

IoApic::IoApic() : m_IoSpace("I/O APIC"), m_Lock(false), m_GsiBase(0), m_Count(0) {}

IoApic::~IoApic() {}

bool IoApic::initialise(physical_uintptr_t address, uint32_t gsiBase) {
  if (!address || (address & (PhysicalMemoryManager::getPageSize() - 1))) {
    return false;
  }
  if (!PhysicalMemoryManager::instance().allocateRegion(
          m_IoSpace, 1,
          PhysicalMemoryManager::continuous | PhysicalMemoryManager::nonRamMemory |
              PhysicalMemoryManager::force,
          VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write |
              VirtualAddressSpace::CacheDisable,
          address)) {
    return false;
  }
  m_IoSpace.write32(1, 0);
  const uint32_t version = m_IoSpace.read32(0x10);
  const uint32_t count = ((version >> 16) & 0xff) + 1;
  if (count > UINT32_MAX - gsiBase) {
    return false;
  }
  m_GsiBase = gsiBase;
  m_Count = count;
  return true;
}

bool IoApic::contains(uint32_t gsi) const {
  return m_Count && gsi >= m_GsiBase && gsi - m_GsiBase < m_Count;
}

bool IoApic::route(uint32_t gsi, uint8_t vector, uint8_t destination, bool activeLow) {
  if (!contains(gsi) || vector < 0x20) {
    return false;
  }
  const uint32_t registerNumber = 0x10 + 2 * (gsi - m_GsiBase);
  const uint32_t low = vector | (1U << 15) | (1U << 16) | (activeLow ? 1U << 13 : 0);
  const uint32_t high = uint32_t(destination) << 24;
  LockGuard<Spinlock> guard(m_Lock);
  m_IoSpace.write32(registerNumber, 0);
  const uint32_t current = m_IoSpace.read32(0x10);
  m_IoSpace.write32(current | (1U << 16), 0x10);
  m_IoSpace.write32(registerNumber + 1, 0);
  m_IoSpace.write32(high, 0x10);
  const bool highWritten = m_IoSpace.read32(0x10) == high;
  m_IoSpace.write32(registerNumber, 0);
  m_IoSpace.write32(low, 0x10);
  return highWritten && (m_IoSpace.read32(0x10) & ~RedirectionStatus) == low;
}

bool IoApic::mask(uint32_t gsi, bool masked) {
  if (!contains(gsi)) {
    return false;
  }
  const uint32_t registerNumber = 0x10 + 2 * (gsi - m_GsiBase);
  LockGuard<Spinlock> guard(m_Lock);
  m_IoSpace.write32(registerNumber, 0);
  uint32_t low = m_IoSpace.read32(0x10);
  low = masked ? low | (1U << 16) : low & ~(1U << 16);
  m_IoSpace.write32(low, 0x10);
  return (m_IoSpace.read32(0x10) & ~RedirectionStatus) == (low & ~RedirectionStatus);
}

#endif
