/* Early QEMU virt discovery runs before the machine layer is available. */
#include "pedigree/kernel/BootstrapInfo.h"
#include "pedigree/kernel/processor/armv7/UefiHandoff.h"

#include <stddef.h>
#include <stdint.h>

extern "C" char kernel_physical_start, kernel_physical_end;
extern "C" void _main(BootstrapStruct_t& bootstrap);
#ifndef ARMV7_BOOTSTRAP
extern "C" void virtSetDeviceTree(const void* tree);
#endif

namespace {
enum { FdtBeginNode = 1, FdtEndNode = 2, FdtProperty = 3, FdtNop = 4, FdtEnd = 9 };

struct EarlyPlatform {
  uintptr_t serial;
  uint32_t memoryBase;
  uint32_t memorySize;
  uint32_t dtbSize;
  uint32_t initrdStart;
  uint32_t initrdEnd;
  const char* bootargs;
};

uint32_t readBe32(const unsigned char* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

bool matches(const unsigned char* p, size_t length, const char* wanted) {
  size_t i = 0;
  while (i < length && wanted[i] && p[i] == (unsigned char)wanted[i]) {
    ++i;
  }
  return i < length && !p[i] && !wanted[i];
}

bool hasCompatible(const unsigned char* p, size_t length) {
  size_t offset = 0;
  while (offset < length) {
    size_t end = offset;
    while (end < length && p[end]) {
      ++end;
    }
    if (matches(p + offset, end - offset + (end < length), "arm,pl011")) {
      return true;
    }
    offset = end + 1;
  }
  return false;
}

bool readPhysical(const unsigned char* p, unsigned int cells, uint32_t& value) {
  if (cells == 1) {
    value = readBe32(p);
    return true;
  }
  if (cells == 2 && readBe32(p) == 0) {
    value = readBe32(p + 4);
    return true;
  }
  return false;
}

bool readHex(const char*& text, const char* end, uint32_t& value) {
  value = 0;
  bool found = false;
  if (end - text >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    text += 2;
  }
  while (text < end) {
    const char c = *text;
    uint32_t digit;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    } else {
      break;
    }
    if (value > (UINT32_MAX - digit) / 16) {
      return false;
    }
    value = value * 16 + digit;
    ++text;
    found = true;
  }
  return found;
}

bool initrdFromBootargs(const char* bootargs, uint32_t& start, uint32_t& end) {
  static const char prefix[] = "pedigree.initrd=";
  if (!bootargs) {
    return false;
  }
  while (*bootargs) {
    while (*bootargs == ' ') {
      ++bootargs;
    }
    const char* token = bootargs;
    while (*bootargs && *bootargs != ' ') {
      ++bootargs;
    }
    const char* cursor = token;
    if (size_t(bootargs - token) < sizeof(prefix) - 1) {
      continue;
    }
    size_t prefixLength = 0;
    while (prefixLength < sizeof(prefix) - 1 && cursor[prefixLength] == prefix[prefixLength]) {
      ++prefixLength;
    }
    if (prefixLength == sizeof(prefix) - 1) {
      cursor += prefixLength;
      return readHex(cursor, bootargs, start) && cursor < bootargs && *cursor++ == ':' &&
             readHex(cursor, bootargs, end) && cursor == bootargs && start < end;
    }
  }
  return false;
}

bool inspectTree(const void* tree, EarlyPlatform& found) {
  const auto* blob = static_cast<const unsigned char*>(tree);
  if (readBe32(blob) != 0xd00dfeed) {
    return false;
  }
  const uint32_t total = readBe32(blob + 4);
  const uint32_t structure = readBe32(blob + 8);
  const uint32_t strings = readBe32(blob + 12);
  if (total < 40 || total > 2 * 1024 * 1024 || structure >= total || strings >= total) {
    return false;
  }
  found.dtbSize = total;

  struct Node {
    uint32_t address;
    uint32_t size;
    unsigned int addressCells;
    unsigned int sizeCells;
    bool serial;
    bool memory;
    bool chosen;
  } nodes[16];
  unsigned int depth = 0;
  const unsigned char* p = blob + structure;
  const unsigned char* end = blob + total;
  while ((size_t)(end - p) >= 4) {
    uint32_t token = readBe32(p);
    p += 4;
    if (token == FdtBeginNode) {
      if (depth == 16) {
        return false;
      }
      const unsigned char* name = p;
      while (p < end && *p) {
        ++p;
      }
      if (p == end) {
        return false;
      }
      auto& node = nodes[depth];
      node.address = 0;
      node.size = 0;
      node.serial = false;
      node.memory = false;
      node.chosen = matches(name, size_t(p - name) + 1, "chosen");
      node.addressCells = depth ? nodes[depth - 1].addressCells : 2;
      node.sizeCells = depth ? nodes[depth - 1].sizeCells : 2;
      ++depth;
      uintptr_t next = (uintptr_t(p) + 4) & ~uintptr_t(3);
      if (next > uintptr_t(end)) {
        return false;
      }
      p = reinterpret_cast<const unsigned char*>(next);
    } else if (token == FdtEndNode) {
      if (!depth) {
        return false;
      }
      const auto& node = nodes[--depth];
      if (node.serial && node.address) {
        found.serial = node.address;
      }
      if (node.memory && node.address && node.size && !found.memorySize) {
        found.memoryBase = node.address;
        found.memorySize = node.size;
      }
    } else if (token == FdtProperty) {
      if (!depth || (size_t)(end - p) < 8) {
        return false;
      }
      uint32_t length = readBe32(p);
      uint32_t nameOffset = readBe32(p + 4);
      p += 8;
      size_t remaining = (size_t)(end - p);
      if (length > remaining || nameOffset >= total - strings) {
        return false;
      }
      size_t padded = ((size_t)length + 3u) & ~(size_t)3;
      if (padded > remaining) {
        return false;
      }
      const unsigned char* name = blob + strings + nameOffset;
      const unsigned char* value = p;
      p += padded;
      auto& node = nodes[depth - 1];
      size_t nameLength = size_t(end - name);
      if (matches(name, nameLength, "#address-cells") && length >= 4) {
        node.addressCells = readBe32(value);
      } else if (matches(name, nameLength, "#size-cells") && length >= 4) {
        node.sizeCells = readBe32(value);
      } else if (matches(name, nameLength, "compatible")) {
        node.serial = hasCompatible(value, length);
      } else if (matches(name, nameLength, "device_type")) {
        node.memory = matches(value, length, "memory");
      } else if (matches(name, nameLength, "bootargs") && node.chosen && length &&
                 value[length - 1] == 0) {
        found.bootargs = reinterpret_cast<const char*>(value);
      } else if (matches(name, nameLength, "linux,initrd-start") && node.chosen &&
                 (length == 4 || length == 8)) {
        readPhysical(value, length / 4, found.initrdStart);
      } else if (matches(name, nameLength, "linux,initrd-end") && node.chosen &&
                 (length == 4 || length == 8)) {
        readPhysical(value, length / 4, found.initrdEnd);
      } else if (matches(name, nameLength, "reg") && depth > 1) {
        unsigned int addressCells = nodes[depth - 2].addressCells;
        unsigned int sizeCells = nodes[depth - 2].sizeCells;
        if (addressCells >= 1 && addressCells <= 2 && sizeCells >= 1 && sizeCells <= 2 &&
            length >= (addressCells + sizeCells) * 4) {
          readPhysical(value, addressCells, node.address);
          readPhysical(value + addressCells * 4, sizeCells, node.size);
        }
      }
    } else if (token == FdtEnd) {
      return found.serial && found.memorySize;
    } else if (token != FdtNop) {
      return false;
    }
  }
  return false;
}

void writeSerial(uintptr_t base, const char* message) {
  auto* data = reinterpret_cast<volatile uint32_t*>(base);
  auto* flags = reinterpret_cast<volatile uint32_t*>(base + 0x18);
  while (*message) {
    while (*flags & (1u << 5)) {
    }
    *data = static_cast<unsigned char>(*message++);
  }
}

void writeHex(uintptr_t base, uint32_t value) {
  char digits[9];
  for (unsigned int i = 0; i < 8; ++i) {
    const uint32_t nibble = (value >> ((7 - i) * 4)) & 15;
    digits[i] = nibble < 10 ? char('0' + nibble) : char('a' + nibble - 10);
  }
  digits[8] = 0;
  writeSerial(base, digits);
}
}  // namespace

extern "C" void armv7BootMain(const void* tree, const armv7_uefi_handoff_t* uefi) {
  if (uefi) {
    if (uefi->magic != ARMV7_UEFI_HANDOFF_MAGIC || uefi->fdt < 0x40000000U ||
        uefi->fdt >= 0x80000000U) {
      for (;;) {
        asm volatile("wfi");
      }
    }
    tree = reinterpret_cast<const void*>(uefi->fdt + 0x80000000U);
  }
  EarlyPlatform platform = {0, 0, 0, 0, 0, 0, nullptr};
  if (!inspectTree(tree, platform)) {
    for (;;) {
      asm volatile("wfi");
    }
  }
  if (platform.serial >= 0x10000000) {
    for (;;) {
      asm volatile("wfi");
    }
  }
  platform.serial += 0x80000000;
  if (uefi) {
    platform.initrdStart = uefi->initrd_start;
    platform.initrdEnd = uefi->initrd_end;
    if (uefi->command_line >= 0x40000000U && uefi->command_line < 0x80000000U) {
      platform.bootargs = reinterpret_cast<const char*>(uefi->command_line + 0x80000000U);
    }
  }
  if (!platform.initrdStart && !platform.initrdEnd) {
    initrdFromBootargs(platform.bootargs, platform.initrdStart, platform.initrdEnd);
  }

  const uint64_t declaredEnd = uint64_t(platform.memoryBase) + platform.memorySize;
  const uint32_t ramEnd = declaredEnd > 0x80000000ULL ? 0x80000000U : uint32_t(declaredEnd);
  const uint32_t dtbStart = uefi ? uefi->fdt : 0x40000000U;
  const uint32_t dtbEnd = (dtbStart + platform.dtbSize + 4095) & ~uint32_t(4095);
  const uint32_t kernelStart = uintptr_t(&kernel_physical_start);
  const uint32_t kernelEnd = (uintptr_t(&kernel_physical_end) + 4095) & ~uint32_t(4095);
  if (platform.memoryBase != 0x40000000 || kernelEnd > ramEnd || dtbEnd < dtbStart ||
      (kernelStart < dtbEnd && kernelEnd > dtbStart)) {
    writeSerial(platform.serial, "Pedigree ARMv7: invalid memory map\r\n");
    for (;;) {
      asm volatile("wfi");
    }
  }

  const bool hasInitrd = platform.initrdStart && platform.initrdEnd > platform.initrdStart;
  const uint32_t initrdStart = platform.initrdStart & ~uint32_t(PAGE_SIZE - 1);
  const uint32_t initrdEnd = (platform.initrdEnd + PAGE_SIZE - 1) & ~uint32_t(PAGE_SIZE - 1);
  if (hasInitrd && (platform.initrdStart < platform.memoryBase || platform.initrdEnd >= ramEnd ||
                    platform.initrdEnd > UINT32_MAX - (PAGE_SIZE - 1) ||
                    (initrdStart < dtbEnd && initrdEnd > dtbStart) ||
                    (initrdStart < kernelEnd && initrdEnd > kernelStart))) {
    writeSerial(platform.serial, "Pedigree ARMv7: invalid initrd region\r\n");
    for (;;) {
      asm volatile("wfi");
    }
  }

  struct ReservedRegion {
    uint32_t start, end;
  } reserved[3] = {{dtbStart, dtbEnd},
                   {kernelStart, kernelEnd},
                   {hasInitrd ? initrdStart : 0, hasInitrd ? initrdEnd : 0}};
  for (size_t i = 1; i < 3; ++i) {
    for (size_t j = i; j && reserved[j].start < reserved[j - 1].start; --j) {
      ReservedRegion previous = reserved[j - 1];
      reserved[j - 1] = reserved[j];
      reserved[j] = previous;
    }
  }

  BootstrapStruct_t::MemoryMapEntry memoryMap[4];
  size_t memoryCount = 0;
  uint32_t cursor = platform.memoryBase;
  for (const ReservedRegion& region : reserved) {
    if (!region.end || region.start >= ramEnd) {
      continue;
    }
    if (region.start > cursor) {
      memoryMap[memoryCount++] = {sizeof(memoryMap[0]), cursor, region.start - cursor, 1};
    }
    if (region.end > cursor) {
      cursor = region.end;
    }
  }
  if (cursor < ramEnd) {
    memoryMap[memoryCount++] = {sizeof(memoryMap[0]), cursor, ramEnd - cursor, 1};
  }
  if (!memoryCount && !uefi) {
    writeSerial(platform.serial, "Pedigree ARMv7: no usable RAM\r\n");
    for (;;) {
      asm volatile("wfi");
    }
  }
  BootstrapStruct_t bootstrap;
  if (uefi) {
    if (uefi->memory_map < 0x40000000U || uefi->memory_map >= 0x80000000U ||
        !uefi->memory_map_bytes || uefi->memory_map_bytes > 16 * PAGE_SIZE ||
        uefi->memory_map_bytes % sizeof(BootstrapStruct_t::MemoryMapEntry)) {
      writeSerial(platform.serial, "Pedigree ARMv7: invalid UEFI memory map\r\n");
      for (;;) {
        asm volatile("wfi");
      }
    }
    bootstrap.setMemoryMap(
        reinterpret_cast<const BootstrapStruct_t::MemoryMapEntry*>(uefi->memory_map + 0x80000000U),
        uefi->memory_map_bytes / sizeof(BootstrapStruct_t::MemoryMapEntry));
    bootstrap.setUefi();
  } else {
    bootstrap.setMemoryMap(memoryMap, memoryCount);
  }
  BootstrapStruct_t::Module module;
  if (hasInitrd) {
    static const char name[] = "rootfs.img";
    module = {uint64_t(platform.initrdStart) + 0x80000000ULL,
              uint64_t(platform.initrdEnd) + 0x80000000ULL, reinterpret_cast<uintptr_t>(name), 0};
    bootstrap.setModules(&module, 1);
    writeSerial(platform.serial, "initrd 0x");
    writeHex(platform.serial, platform.initrdStart);
    writeSerial(platform.serial, " + 0x");
    writeHex(platform.serial, bootstrap.getInitrdSize());
    writeSerial(platform.serial, "\r\n");
  }
  if (platform.bootargs) {
    bootstrap.setCommandLine(platform.bootargs);
  }
#ifndef ARMV7_BOOTSTRAP
  if (uefi) {
    virtSetDeviceTree(tree);
  }
#endif

  writeSerial(platform.serial, "Pedigree ARMv7 virt bootstrap: MMU and FDT serial ready\r\n");
  for (void* entry = bootstrap.getMemoryMap(); entry; entry = bootstrap.nextMemoryMapEntry(entry)) {
    writeSerial(platform.serial, "usable RAM 0x");
    writeHex(platform.serial, uint32_t(bootstrap.getMemoryMapEntryAddress(entry)));
    writeSerial(platform.serial, " + 0x");
    writeHex(platform.serial, uint32_t(bootstrap.getMemoryMapEntryLength(entry)));
    writeSerial(platform.serial, "\r\n");
  }
  if (bootstrap.getCommandLine()) {
    writeSerial(platform.serial, "bootargs: ");
    writeSerial(platform.serial, bootstrap.getCommandLine());
    writeSerial(platform.serial, "\r\n");
  }
  _main(bootstrap);
  for (;;) {
    asm volatile("wfi");
  }
}
