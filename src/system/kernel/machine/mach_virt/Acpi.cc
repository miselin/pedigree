/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "Acpi.h"

namespace {
constexpr uint64_t DirectMapBase = 0xffff000000000000ULL;
constexpr uint64_t DirectMapLimit = 64ULL << 30;
constexpr uint32_t MaxTableSize = 1024 * 1024;
constexpr uint32_t XsdtSignature = 0x54445358;
constexpr uint32_t MadtSignature = 0x43495041;
constexpr uint32_t GtdtSignature = 0x54445447;
constexpr uint32_t SpcrSignature = 0x52435053;
constexpr uint32_t McfgSignature = 0x4746434d;
constexpr uint32_t FadtSignature = 0x50434146;
constexpr uint32_t HeaderSize = 36;

const BootstrapStruct_t::MemoryMapEntry* g_MemoryMap = nullptr;
size_t g_MemoryMapCount = 0;

uint16_t read16(const uint8_t* p) {
  return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

uint32_t read32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint64_t read64(const uint8_t* p) {
  return uint64_t(read32(p)) | (uint64_t(read32(p + 4)) << 32);
}

bool checksum(const uint8_t* bytes, size_t size) {
  uint8_t sum = 0;
  for (size_t i = 0; i < size; ++i) {
    sum += bytes[i];
  }
  return sum == 0;
}

const uint8_t* physical(uint64_t address, size_t bytes) {
  if (!address || !bytes || address >= DirectMapLimit || bytes > DirectMapLimit - address) {
    return nullptr;
  }
  const uint64_t end = address + bytes;
  for (uint64_t cursor = address; cursor < end;) {
    uint64_t next = cursor;
    for (size_t i = 0; i < g_MemoryMapCount; ++i) {
      const auto& region = g_MemoryMap[i];
      if ((region.type != 3 && region.type != 4) || region.address > cursor || !region.length ||
          region.address > UINT64_MAX - region.length || cursor >= region.address + region.length) {
        continue;
      }
      const uint64_t regionEnd = region.address + region.length;
      if (regionEnd > next) {
        next = regionEnd < end ? regionEnd : end;
      }
    }
    if (next == cursor) {
      return nullptr;
    }
    cursor = next;
  }
  return reinterpret_cast<const uint8_t*>(DirectMapBase + address);
}

const uint8_t* tableAt(uint64_t address) {
  const uint8_t* table = physical(address, HeaderSize);
  if (!table) {
    return nullptr;
  }
  const uint32_t length = read32(table + 4);
  if (length < HeaderSize || length > MaxTableSize || !physical(address, length) ||
      !checksum(table, length)) {
    return nullptr;
  }
  return table;
}

bool package(const uint8_t* bytes, size_t size, size_t& cursor, size_t& end) {
  if (cursor >= size) {
    return false;
  }
  const uint8_t lead = bytes[cursor];
  const size_t follow = lead >> 6;
  if (cursor + follow >= size) {
    return false;
  }
  uint32_t length = follow ? lead & 0x0f : lead & 0x3f;
  for (size_t i = 0; i < follow; ++i) {
    length |= uint32_t(bytes[cursor + 1 + i]) << (4 + 8 * i);
  }
  if (length < follow + 1 || length > size - cursor) {
    return false;
  }
  end = cursor + length;
  cursor += follow + 1;
  return true;
}

bool integer(const uint8_t* bytes, size_t end, size_t& cursor, uint64_t& result) {
  if (cursor >= end) {
    return false;
  }
  const uint8_t op = bytes[cursor++];
  if (op <= 1) {
    result = op;
    return true;
  }
  const size_t width = op == 0x0a ? 1 : op == 0x0b ? 2 : op == 0x0c ? 4 : op == 0x0e ? 8 : 0;
  if (!width || width > end - cursor) {
    return false;
  }
  result = 0;
  for (size_t i = 0; i < width; ++i) {
    result |= uint64_t(bytes[cursor++]) << (i * 8);
  }
  return true;
}

bool name(const uint8_t* p, const char* wanted) {
  for (size_t i = 0; i < 4; ++i) {
    if (p[i] != static_cast<uint8_t>(wanted[i])) {
      return false;
    }
  }
  return true;
}

// Only static AML objects are consumed. Methods and computed resources require
// an interpreter and are deliberately left unsupported.
bool device(const uint8_t* aml, size_t size, const char* wanted, size_t& start, size_t& end) {
  for (size_t i = 0; i + 8 < size; ++i) {
    if (aml[i] != 0x5b || aml[i + 1] != 0x82) {
      continue;
    }
    size_t body = i + 2;
    size_t limit = 0;
    if (package(aml, size, body, limit) && body + 4 <= limit && name(aml + body, wanted)) {
      start = body + 4;
      end = limit;
      return true;
    }
  }
  return false;
}

bool namedObject(const uint8_t* aml, size_t start, size_t end, const char* wanted, uint8_t op,
                 size_t& body, size_t& limit) {
  for (size_t i = start; i + 7 < end; ++i) {
    if (aml[i] != 0x08 || !name(aml + i + 1, wanted) || aml[i + 5] != op) {
      continue;
    }
    body = i + 6;
    return package(aml, end, body, limit);
  }
  return false;
}

bool resources(const uint8_t* aml, size_t start, size_t end, const uint8_t*& data, size_t& size) {
  size_t body = 0, limit = 0;
  if (!namedObject(aml, start, end, "_CRS", 0x11, body, limit)) {
    return false;
  }
  uint64_t declared = 0;
  if (!integer(aml, limit, body, declared) || declared > limit - body) {
    return false;
  }
  data = aml + body;
  size = static_cast<size_t>(declared);
  return true;
}

bool resource(const uint8_t* data, size_t size, size_t& cursor, uint8_t& tag,
              const uint8_t*& payload, size_t& length) {
  if (cursor >= size) {
    return false;
  }
  const uint8_t lead = data[cursor++];
  if (lead & 0x80) {
    if (size - cursor < 2) {
      return false;
    }
    tag = lead;
    length = read16(data + cursor);
    cursor += 2;
  } else {
    tag = lead & 0xf8;
    length = lead & 7;
  }
  if (length > size - cursor) {
    return false;
  }
  payload = data + cursor;
  cursor += length;
  return true;
}

bool addPciWindow(VirtAcpiInfo& out, uint8_t tag, const uint8_t* payload, size_t length) {
  const size_t width = tag == 0x88 ? 2 : tag == 0x87 ? 4 : tag == 0x8a ? 8 : 0;
  if (!width || length < 3 + width * 5 || out.pciWindowCount == 8) {
    return false;
  }
  const uint8_t type = payload[0];
  if (type > 1) {
    return false;
  }
  const uint8_t* fields = payload + 3;
  uint64_t minimum = 0, maximum = 0, translation = 0, bytes = 0;
  for (size_t i = 0; i < width; ++i) {
    minimum |= uint64_t(fields[width + i]) << (i * 8);
    maximum |= uint64_t(fields[2 * width + i]) << (i * 8);
    translation |= uint64_t(fields[3 * width + i]) << (i * 8);
    bytes |= uint64_t(fields[4 * width + i]) << (i * 8);
  }
  if (!bytes || maximum < minimum || bytes - 1 > maximum - minimum ||
      minimum > UINT64_MAX - translation || minimum + translation > UINT64_MAX - bytes) {
    return false;
  }
  out.pciWindows[out.pciWindowCount++] = {type == 1     ? 0x01000000U
                                          : tag == 0x8a ? 0x03000000U
                                                        : 0x02000000U,
                                          minimum, minimum + translation, bytes};
  return true;
}

uint32_t linkIrq(const uint8_t* aml, size_t size, const char* link) {
  size_t start = 0, end = 0;
  if (!device(aml, size, link, start, end)) {
    return 0;
  }
  const uint8_t* data = nullptr;
  size_t length = 0;
  if (!resources(aml, start, end, data, length)) {
    return 0;
  }
  size_t cursor = 0;
  uint8_t tag = 0;
  const uint8_t* payload = nullptr;
  size_t bytes = 0;
  while (resource(data, length, cursor, tag, payload, bytes)) {
    if (tag == 0x89 && bytes >= 6 && (payload[0] == 1 || payload[0] == 9) && payload[1] == 1) {
      const uint32_t irq = read32(payload + 2);
      return irq >= 32 && irq < 256 ? irq : 0;
    }
  }
  return 0;
}

bool pciResources(const uint8_t* aml, size_t size, VirtAcpiInfo& out) {
  size_t start = 0, end = 0;
  if (!device(aml, size, "PCI0", start, end)) {
    return false;
  }
  const uint8_t* data = nullptr;
  size_t length = 0;
  if (!resources(aml, start, end, data, length)) {
    return false;
  }
  size_t cursor = 0;
  uint8_t tag = 0;
  const uint8_t* payload = nullptr;
  size_t bytes = 0;
  while (resource(data, length, cursor, tag, payload, bytes)) {
    addPciWindow(out, tag, payload, bytes);
  }
  if (!out.pciWindowCount) {
    return false;
  }

  size_t body = 0, limit = 0;
  if (!namedObject(aml, start, end, "_PRT", 0x13, body, limit)) {
    return false;
  }
  uint64_t entries = 0;
  if (!integer(aml, limit, body, entries) || entries > 1024) {
    return false;
  }
  for (uint64_t i = 0; i < entries; ++i) {
    if (body >= limit || aml[body++] != 0x12) {
      return false;
    }
    size_t itemEnd = 0;
    if (!package(aml, limit, body, itemEnd) || body >= itemEnd || aml[body++] != 4) {
      return false;
    }
    uint64_t address = 0, pin = 0, sourceIndex = 0;
    if (!integer(aml, itemEnd, body, address) || !integer(aml, itemEnd, body, pin) ||
        itemEnd - body < 4) {
      return false;
    }
    const char* link = reinterpret_cast<const char*>(aml + body);
    body += 4;
    if (!integer(aml, itemEnd, body, sourceIndex) || sourceIndex || (address & 0xffff) != 0xffff ||
        pin >= 4) {
      return false;
    }
    const uint32_t slot = (address >> 16) & 0xffff;
    if (slot < 32) {
      out.pciIrq[slot][pin] = linkIrq(aml, size, link);
    }
    body = itemEnd;
  }
  for (size_t slot = 0; slot < 32; ++slot) {
    for (size_t pin = 0; pin < 4; ++pin) {
      if (out.pciIrq[slot][pin]) {
        return true;
      }
    }
  }
  return false;
}

void rtcResource(const uint8_t* aml, size_t size, VirtAcpiInfo& out) {
  size_t start = 0, end = 0;
  if (!device(aml, size, "RTC0", start, end)) {
    return;
  }
  const uint8_t* data = nullptr;
  size_t length = 0;
  if (!resources(aml, start, end, data, length)) {
    return;
  }
  size_t cursor = 0;
  uint8_t tag = 0;
  const uint8_t* payload = nullptr;
  size_t bytes = 0;
  while (resource(data, length, cursor, tag, payload, bytes)) {
    if (tag == 0x86 && bytes >= 9 && read32(payload + 5) >= 4) {
      out.rtc = read32(payload + 1);
      return;
    }
  }
}

void parseMadt(const uint8_t* table, VirtAcpiInfo& out) {
  const size_t length = read32(table + 4);
  if (length < 44) {
    return;
  }
  size_t cursor = 44;
  while (cursor + 2 <= length) {
    const uint8_t type = table[cursor];
    const uint8_t bytes = table[cursor + 1];
    if (bytes < 2 || bytes > length - cursor) {
      return;
    }
    const uint8_t* entry = table + cursor;
    if (type == 12 && bytes >= 24) {
      out.gicDistributor = read64(entry + 8);
      out.gicVersion = entry[20];
    } else if (type == 14 && bytes >= 16) {
      out.gicRedistributor = read64(entry + 4);
    } else if (type == 11 && bytes >= 40 && (read32(entry + 12) & 1)) {
      out.gicCpu = read64(entry + 32);
    }
    cursor += bytes;
  }
}

void parseGtdt(const uint8_t* table, VirtAcpiInfo& out) {
  if (read32(table + 4) >= 68) {
    out.physicalTimerIrq = read32(table + 56);
    out.virtualTimerIrq = read32(table + 64);
  }
}

void parseSpcr(const uint8_t* table, VirtAcpiInfo& out) {
  if (read32(table + 4) >= 58 && table[36] == 3 && table[40] == 0) {
    out.uart = read64(table + 44);
    out.uartIrq = read32(table + 54);
  }
}

void parseMcfg(const uint8_t* table, VirtAcpiInfo& out) {
  const size_t length = read32(table + 4);
  if (length < 60 || (length - 44) % 16) {
    return;
  }
  for (size_t cursor = 44; cursor + 16 <= length; cursor += 16) {
    if (read16(table + cursor + 8) != 0) {
      continue;
    }
    const uint8_t first = table[cursor + 10];
    const uint8_t last = table[cursor + 11];
    const uint64_t base = read64(table + cursor);
    if (last >= first && !(base & 0xfffff)) {
      out.pciHost = {base, uint64_t(last - first + 1) << 20, first, last};
      return;
    }
  }
}

uint64_t parseFadt(const uint8_t* table, VirtAcpiInfo& out) {
  const size_t length = read32(table + 4);
  if (length >= 132) {
    const uint16_t flags = read16(table + 129);
    out.psciAvailable = flags & 1;
    out.psciHvc = flags & 2;
  }
  if (length >= 148 && read64(table + 140)) {
    return read64(table + 140);
  }
  return length >= 44 ? read32(table + 40) : 0;
}
}  // namespace

bool virtParseAcpi(uint64_t rsdpPhysical, const BootstrapStruct_t::MemoryMapEntry* memoryMap,
                   size_t memoryMapCount, VirtAcpiInfo& info) {
  g_MemoryMap = memoryMap;
  g_MemoryMapCount = memoryMapCount;
  const uint8_t* rsdp = physical(rsdpPhysical, 36);
  if (!rsdp || read64(rsdp) != 0x2052545020445352ULL || !checksum(rsdp, 20) || rsdp[15] < 2 ||
      read32(rsdp + 20) < 36 || read32(rsdp + 20) > 4096 ||
      !physical(rsdpPhysical, read32(rsdp + 20)) || !checksum(rsdp, read32(rsdp + 20))) {
    return false;
  }
  const uint8_t* xsdt = tableAt(read64(rsdp + 24));
  if (!xsdt || read32(xsdt) != XsdtSignature || (read32(xsdt + 4) - HeaderSize) % 8) {
    return false;
  }
  info = {};
  uint64_t dsdtPhysical = 0;
  const size_t count = (read32(xsdt + 4) - HeaderSize) / 8;
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* table = tableAt(read64(xsdt + HeaderSize + i * 8));
    if (!table) {
      continue;
    }
    switch (read32(table)) {
      case MadtSignature:
        parseMadt(table, info);
        break;
      case GtdtSignature:
        parseGtdt(table, info);
        break;
      case SpcrSignature:
        parseSpcr(table, info);
        break;
      case McfgSignature:
        parseMcfg(table, info);
        break;
      case FadtSignature:
        dsdtPhysical = parseFadt(table, info);
        break;
      default:
        break;
    }
  }
  if (info.pciHost.size) {
    const uint8_t* dsdt = tableAt(dsdtPhysical);
    if (!dsdt || read32(dsdt) != 0x54445344 ||
        !pciResources(dsdt + HeaderSize, read32(dsdt + 4) - HeaderSize, info)) {
      info.pciHost = {};
      info.pciWindowCount = 0;
    } else {
      rtcResource(dsdt + HeaderSize, read32(dsdt + 4) - HeaderSize, info);
    }
  }
  return info.uart && info.gicDistributor &&
         ((info.gicVersion == 2 && info.gicCpu) ||
          (info.gicVersion == 3 && info.gicRedistributor)) &&
         info.physicalTimerIrq && info.virtualTimerIrq;
}
