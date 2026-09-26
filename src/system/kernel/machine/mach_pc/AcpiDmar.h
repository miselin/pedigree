/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_PC_ACPI_DMAR_H
#define PEDIGREE_PC_ACPI_DMAR_H

#include <stddef.h>
#include <stdint.h>

namespace AcpiDmar {
struct Info {
  static constexpr size_t MaxDirectEndpoints = 128;

  uint8_t hostAddressWidth = 0;
  bool interruptRemapping = false;
  bool x2ApicOptOut = false;
  bool dmaControlPlatformOptIn = false;
  bool reservedMemoryRegions = false;
  size_t hardwareUnitCount = 0;
  size_t segmentZeroUnitCount = 0;
  size_t segmentZeroIncludeAllCount = 0;
  uint64_t firstSegmentZeroUnitAddress = 0;
  uint8_t firstSegmentZeroUnitRegisterPagesLog2 = 0;
  uint64_t firstSegmentZeroIncludeAllAddress = 0;
  uint8_t firstSegmentZeroIncludeAllRegisterPagesLog2 = 0;
  uint16_t directEndpoints[MaxDirectEndpoints] = {};
  size_t directEndpointCount = 0;

  bool includesDirectEndpoint(uint8_t bus, uint8_t device, uint8_t function) const {
    const uint16_t sourceId = (uint16_t(bus) << 8) | (uint16_t(device) << 3) | function;
    for (size_t index = 0; index < directEndpointCount; ++index) {
      if (directEndpoints[index] == sourceId) {
        return true;
      }
    }
    return false;
  }
};

inline uint16_t read16(const uint8_t* bytes) {
  return uint16_t(bytes[0]) | (uint16_t(bytes[1]) << 8);
}

inline uint32_t read32(const uint8_t* bytes) {
  return uint32_t(read16(bytes)) | (uint32_t(read16(bytes + 2)) << 16);
}

inline uint64_t read64(const uint8_t* bytes) {
  return uint64_t(read32(bytes)) | (uint64_t(read32(bytes + 4)) << 32);
}

/** Validate a complete ACPI DMAR table before publishing any discovery result. */
inline bool parse(const uint8_t* table, size_t available, Info& result) {
  constexpr size_t HeaderBytes = 48;
  if (!table || available < HeaderBytes || read32(table) != 0x52414d44U) {
    return false;
  }
  const size_t length = read32(table + 4);
  if (length < HeaderBytes || length > available || table[36] < 31 || table[36] > 63 ||
      (table[37] & ~7U)) {
    return false;
  }

  uint8_t checksum = 0;
  for (size_t i = 0; i < length; ++i) {
    checksum += table[i];
  }
  if (checksum) {
    return false;
  }

  Info found;
  found.hostAddressWidth = table[36] + 1;
  found.interruptRemapping = table[37] & 1U;
  found.x2ApicOptOut = table[37] & 2U;
  found.dmaControlPlatformOptIn = table[37] & 4U;
  for (size_t offset = HeaderBytes; offset < length;) {
    if (length - offset < 4) {
      return false;
    }
    const uint16_t type = read16(table + offset);
    const size_t bytes = read16(table + offset + 2);
    if (bytes < 4 || bytes > length - offset) {
      return false;
    }
    if (type == 0) {
      if (bytes < 16) {
        return false;
      }
      const uint8_t registerPagesLog2 = table[offset + 5] & 0x0fU;
      if ((table[offset + 4] & ~1U) || (table[offset + 5] & ~0x0fU) ||
          !read64(table + offset + 8) ||
          (read64(table + offset + 8) & ((uint64_t(1) << (12 + registerPagesLog2)) - 1))) {
        return false;
      }
      for (size_t scope = offset + 16; scope < offset + bytes;) {
        if (offset + bytes - scope < 8) {
          return false;
        }
        const size_t scopeBytes = table[scope + 1];
        if (!table[scope] || scopeBytes < 8 || (scopeBytes & 1U) ||
            scopeBytes > offset + bytes - scope) {
          return false;
        }
        if (!read16(table + offset + 6) && table[scope] == 1 && scopeBytes == 8 &&
            !table[scope + 2] && !table[scope + 3] && !table[scope + 4] && table[scope + 6] < 32 &&
            table[scope + 7] < 8) {
          if (found.directEndpointCount == Info::MaxDirectEndpoints) {
            return false;
          }
          found.directEndpoints[found.directEndpointCount++] = (uint16_t(table[scope + 5]) << 8) |
                                                               (uint16_t(table[scope + 6]) << 3) |
                                                               table[scope + 7];
        }
        scope += scopeBytes;
      }
      ++found.hardwareUnitCount;
      if (!read16(table + offset + 6)) {
        ++found.segmentZeroUnitCount;
        if (!found.firstSegmentZeroUnitAddress) {
          found.firstSegmentZeroUnitAddress = read64(table + offset + 8);
          found.firstSegmentZeroUnitRegisterPagesLog2 = registerPagesLog2;
        }
        if (table[offset + 4] & 1U) {
          ++found.segmentZeroIncludeAllCount;
          if (!found.firstSegmentZeroIncludeAllAddress) {
            found.firstSegmentZeroIncludeAllAddress = read64(table + offset + 8);
            found.firstSegmentZeroIncludeAllRegisterPagesLog2 = registerPagesLog2;
          }
        }
      }
    } else if (type == 1) {
      if (bytes < 24) {
        return false;
      }
      found.reservedMemoryRegions = true;
    }
    offset += bytes;
  }
  if (!found.hardwareUnitCount) {
    return false;
  }
  result = found;
  return true;
}
}  // namespace AcpiDmar

#endif
