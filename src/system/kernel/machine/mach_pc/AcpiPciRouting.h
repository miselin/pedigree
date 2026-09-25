/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_PC_ACPI_PCI_ROUTING_H
#define PEDIGREE_PC_ACPI_PCI_ROUTING_H

#include <stddef.h>
#include <stdint.h>

/** A bounded reader for static PCI interrupt routes in a DSDT. */
class AcpiPciRouting {
 public:
  struct Route {
    uint32_t gsi = 0;
    bool activeLow = false;
  };

  static bool parse(const uint8_t* aml, size_t size, Route routes[32][4]) {
    if (!aml || !size || !routes) {
      return false;
    }
    for (size_t slot = 0; slot < 32; ++slot) {
      for (size_t pin = 0; pin < 4; ++pin) {
        routes[slot][pin] = {};
      }
    }
    // Q35 exposes its APIC-mode table as PRTA, selected by a _PRT method.
    // Other firmware can expose a static _PRT package directly.
    return parsePackage(aml, size, "_PRT", routes) || parsePackage(aml, size, "PRTA", routes);
  }

 private:
  static bool equal(const uint8_t* bytes, const char* name) {
    for (size_t i = 0; i < 4; ++i) {
      if (bytes[i] != static_cast<uint8_t>(name[i])) {
        return false;
      }
    }
    return true;
  }

  static bool package(const uint8_t* bytes, size_t size, size_t& cursor, size_t& end) {
    if (cursor >= size) {
      return false;
    }
    const uint8_t lead = bytes[cursor];
    const size_t follow = lead >> 6;
    if (follow >= size - cursor) {
      return false;
    }
    uint32_t length = follow ? lead & 0x0f : lead & 0x3f;
    for (size_t i = 0; i < follow; ++i) {
      length |= uint32_t(bytes[cursor + 1 + i]) << (4 + i * 8);
    }
    if (length < follow + 1 || length > size - cursor) {
      return false;
    }
    end = cursor + length;
    cursor += follow + 1;
    return true;
  }

  static bool integer(const uint8_t* bytes, size_t end, size_t& cursor, uint64_t& value) {
    if (cursor >= end) {
      return false;
    }
    const uint8_t op = bytes[cursor++];
    if (op <= 1) {
      value = op;
      return true;
    }
    const size_t width = op == 0x0a ? 1 : op == 0x0b ? 2 : op == 0x0c ? 4 : op == 0x0e ? 8 : 0;
    if (!width || width > end - cursor) {
      return false;
    }
    value = 0;
    for (size_t i = 0; i < width; ++i) {
      value |= uint64_t(bytes[cursor++]) << (i * 8);
    }
    return true;
  }

  static bool link(const uint8_t* aml, size_t size, const char source[4], Route& route) {
    for (size_t i = 0; i + 8 < size; ++i) {
      if (aml[i] != 0x5b || aml[i + 1] != 0x82) {
        continue;
      }
      size_t body = i + 2, deviceEnd = 0;
      if (!package(aml, size, body, deviceEnd) || deviceEnd - body < 4 ||
          !equal(aml + body, source)) {
        continue;
      }
      body += 4;
      for (size_t j = body; j + 7 < deviceEnd; ++j) {
        if (aml[j] != 0x08 || !equal(aml + j + 1, "_CRS") || aml[j + 5] != 0x11) {
          continue;
        }
        size_t data = j + 6, bufferEnd = 0;
        uint64_t declared = 0;
        if (!package(aml, deviceEnd, data, bufferEnd) || !integer(aml, bufferEnd, data, declared) ||
            declared > bufferEnd - data) {
          continue;
        }
        const size_t end = data + static_cast<size_t>(declared);
        while (data < end) {
          const uint8_t tag = aml[data++];
          size_t length = 0;
          if (tag & 0x80) {
            if (end - data < 2) {
              return false;
            }
            length = size_t(aml[data]) | (size_t(aml[data + 1]) << 8);
            data += 2;
          } else {
            length = tag & 7;
          }
          if (length > end - data) {
            return false;
          }
          if (tag == 0x89 && length == 6 && aml[data + 1] == 1 && !(aml[data] & 2)) {
            route.gsi = uint32_t(aml[data + 2]) | (uint32_t(aml[data + 3]) << 8) |
                        (uint32_t(aml[data + 4]) << 16) | (uint32_t(aml[data + 5]) << 24);
            route.activeLow = (aml[data] & 4) != 0;
            return route.gsi >= 16;
          }
          data += length;
        }
      }
    }
    return false;
  }

  static bool parsePackage(const uint8_t* aml, size_t size, const char* name, Route routes[32][4]) {
    for (size_t i = 0; i + 7 < size; ++i) {
      if (aml[i] != 0x08 || !equal(aml + i + 1, name) || aml[i + 5] != 0x12) {
        continue;
      }
      size_t body = i + 6, end = 0;
      if (!package(aml, size, body, end) || body >= end) {
        continue;
      }
      const uint8_t count = aml[body++];
      Route parsed[32][4] = {};
      bool valid = true, found = false;
      for (size_t entry = 0; entry < count; ++entry) {
        if (body >= end || aml[body++] != 0x12) {
          valid = false;
          break;
        }
        size_t itemEnd = 0;
        if (!package(aml, end, body, itemEnd) || body >= itemEnd || aml[body++] != 4) {
          valid = false;
          break;
        }
        uint64_t address = 0, pin = 0, sourceIndex = 0;
        if (!integer(aml, itemEnd, body, address) || !integer(aml, itemEnd, body, pin) ||
            body >= itemEnd) {
          valid = false;
          break;
        }
        Route route = {};
        char source[4] = {};
        const bool direct = aml[body] == 0;
        if (direct) {
          ++body;
        } else if (itemEnd - body >= 4) {
          for (size_t c = 0; c < 4; ++c) {
            source[c] = aml[body++];
          }
        } else {
          valid = false;
          break;
        }
        if (!integer(aml, itemEnd, body, sourceIndex) || (address & 0xffff) != 0xffff || pin > 3 ||
            sourceIndex > UINT32_MAX) {
          valid = false;
          break;
        }
        if (direct) {
          route.gsi = static_cast<uint32_t>(sourceIndex);
          route.activeLow = true;
        } else if (sourceIndex == 0) {
          link(aml, size, source, route);
        }
        const uint32_t slot = (address >> 16) & 0xffff;
        if (slot < 32 && route.gsi >= 16) {
          if (parsed[slot][pin].gsi && (parsed[slot][pin].gsi != route.gsi ||
                                        parsed[slot][pin].activeLow != route.activeLow)) {
            valid = false;
            break;
          }
          parsed[slot][pin] = route;
          found = true;
        }
        body = itemEnd;
      }
      if (valid && found && body == end) {
        for (size_t slot = 0; slot < 32; ++slot) {
          for (size_t pin = 0; pin < 4; ++pin) {
            routes[slot][pin] = parsed[slot][pin];
          }
        }
        return true;
      }
    }
    return false;
  }
};

#endif
