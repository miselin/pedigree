/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_PCI_CONFIG_ACCESS_H
#define PEDIGREE_PCI_CONFIG_ACCESS_H
#include "pedigree/kernel/processor/types.h"

// Mechanism 1 exposes only the first 256 bytes. Keep validation ahead of any
// port access, including accesses made by the legacy dword-index interface.
template <class Io, class Lock>
class PciConfigAccess {
 public:
  PciConfigAccess(Io& io, Lock& lock) : m_Io(io), m_Lock(lock) {}
  static bool valid(uint8_t device, uint8_t function, uint16_t offset, uint8_t width) {
    return device < 32 && function < 8 && (width == 1 || width == 2 || width == 4) &&
           offset <= 256 - width && !(offset & (width - 1));
  }
  bool read(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint8_t width,
            uint32_t& value) {
    if (!valid(device, function, offset, width))
      return false;
    Guard guard(m_Lock);
    select(bus, device, function, offset);
    value = readData(offset, width);
    return true;
  }
  bool write(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint8_t width,
             uint32_t value) {
    if (!valid(device, function, offset, width))
      return false;
    Guard guard(m_Lock);
    select(bus, device, function, offset);
    writeData(offset, width, value);
    return true;
  }
  bool updateCommand(uint8_t bus, uint8_t device, uint8_t function, uint16_t clearBits,
                     uint16_t setBits) {
    if (!valid(device, function, 4, 2))
      return false;
    Guard guard(m_Lock);
    select(bus, device, function, 4);
    const uint16_t desired = (m_Io.read16(4) & ~clearBits) | setBits;
    m_Io.write16(desired, 4);
    return m_Io.read16(4) == desired;
  }

 private:
  struct Guard {
    Lock& lock;
    explicit Guard(Lock& value) : lock(value) {
      lock.acquire();
    }
    ~Guard() {
      lock.release();
    }
  };
  void select(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset) {
    m_Io.write32(0x80000000U | (uint32_t{bus} << 16) | (uint32_t{device} << 11) |
                     (uint32_t{function} << 8) | (offset & ~3U),
                 0);
  }
  uint32_t readData(uint16_t offset, uint8_t width) {
    const uint8_t port = 4 + (offset & 3);
    if (width == 1)
      return m_Io.read8(port);
    if (width == 2)
      return m_Io.read16(port);
    return m_Io.read32(port);
  }
  void writeData(uint16_t offset, uint8_t width, uint32_t value) {
    const uint8_t port = 4 + (offset & 3);
    if (width == 1)
      m_Io.write8(value, port);
    else if (width == 2)
      m_Io.write16(value, port);
    else
      m_Io.write32(value, port);
  }
  Io& m_Io;
  Lock& m_Lock;
};
#endif
