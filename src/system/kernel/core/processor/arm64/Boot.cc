#include "pedigree/kernel/BootstrapInfo.h"

#include "../../../machine/mach_virt/DeviceTree.h"

extern "C" void _main(BootstrapStruct_t& bootstrap);
extern "C" char kernel_physical_start, kernel_physical_end;

namespace {
constexpr size_t MaximumMemoryRegions = 128;
constexpr uint64_t DirectMapBase = 0xffff000000000000ULL;
BootstrapStruct_t::MemoryMapEntry memoryMap[MaximumMemoryRegions] = {};
BootstrapStruct_t::Module initrdModule = {};
char uefiCommandLine[4096] = {};

struct ReservedRegion {
  uint64_t start;
  uint64_t end;
};

uint64_t alignDown(uint64_t value) {
  return value & ~uint64_t(PAGE_SIZE - 1);
}

uint64_t alignUp(uint64_t value) {
  return (value + PAGE_SIZE - 1) & ~uint64_t(PAGE_SIZE - 1);
}

bool readHex(const char*& text, uint64_t& value) {
  value = 0;
  bool found = false;
  if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    text += 2;
  }
  for (;;) {
    const char c = *text;
    uint64_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    } else {
      break;
    }
    if (value > (UINT64_MAX - digit) / 16) {
      return false;
    }
    value = value * 16 + digit;
    ++text;
    found = true;
  }
  return found;
}

bool initrdFromBootargs(const char* bootargs, uint64_t& start, uint64_t& end) {
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
    for (size_t i = 0; i < sizeof(prefix) - 1 && cursor < bootargs; ++i) {
      if (*cursor++ != prefix[i]) {
        break;
      }
      if (i == sizeof(prefix) - 2) {
        return readHex(cursor, start) && cursor < bootargs && *cursor++ == ':' &&
               readHex(cursor, end) && cursor == bootargs && start < end;
      }
    }
  }
  return false;
}

bool initrdInMemory(uint64_t start, uint64_t end, const void* deviceTree,
                    const BootstrapStruct_t::MemoryMapEntry* uefiMap, size_t uefiCount) {
  if (!start || start >= end) {
    return false;
  }
  const uint64_t kernelStart = reinterpret_cast<uintptr_t>(&kernel_physical_start);
  const uint64_t kernelEnd = reinterpret_cast<uintptr_t>(&kernel_physical_end);
  const uint64_t dtbStart = reinterpret_cast<uintptr_t>(deviceTree);
  const uint64_t dtbEnd = dtbStart + VirtDeviceTree::blobSize();
  if (!(end <= kernelStart || start >= kernelEnd) ||
      (deviceTree && !(end <= dtbStart || start >= dtbEnd))) {
    return false;
  }
  if (uefiMap) {
    for (size_t i = 0; i < uefiCount; ++i) {
      const auto& entry = uefiMap[i];
      if (entry.type == 2 && entry.address <= start && entry.length &&
          entry.address <= UINT64_MAX - entry.length && end <= entry.address + entry.length) {
        return true;
      }
    }
    return false;
  }
  for (size_t i = 0; i < MaximumMemoryRegions; ++i) {
    uint64_t base = 0, size = 0;
    if (!virtGetMemoryRegion(i, &base, &size)) {
      break;
    }
    if (base <= start && size && base <= UINT64_MAX - size && end <= base + size) {
      return true;
    }
  }
  return false;
}

void addUsable(uint64_t start, uint64_t end, size_t& count) {
  start = alignUp(start);
  end = alignDown(end);
  if (start >= end || count == MaximumMemoryRegions) {
    return;
  }
  memoryMap[count++] = {sizeof(BootstrapStruct_t::MemoryMapEntry), start, end - start, 1};
}
}  // namespace

extern "C" void arm64BootMain(const void* deviceTree, uint64_t uefiInitrdStart,
                              uint64_t uefiInitrdEnd, uint64_t uefiCommandLineAddress,
                              uint64_t uefiMemoryMapAddress, uint64_t uefiMemoryMapBytes,
                              uint64_t uefiAcpiRsdp) {
  const bool uefi = uefiCommandLineAddress != 0;
  if (uefi && (!uefiMemoryMapAddress || !uefiMemoryMapBytes ||
               uefiMemoryMapBytes % sizeof(BootstrapStruct_t::MemoryMapEntry) ||
               uefiMemoryMapBytes > 16 * PAGE_SIZE)) {
    while (true) {
      asm volatile("wfe");
    }
  }
  const auto* uefiMemoryMap = uefi ? reinterpret_cast<const BootstrapStruct_t::MemoryMapEntry*>(
                                         DirectMapBase + uefiMemoryMapAddress)
                                   : nullptr;
  const size_t uefiCount =
      uefi ? uefiMemoryMapBytes / sizeof(BootstrapStruct_t::MemoryMapEntry) : 0;
  if (deviceTree) {
    virtSetDeviceTree(deviceTree);
  } else if (uefi && uefiAcpiRsdp) {
    VirtDeviceTree::initialiseAcpi(uefiAcpiRsdp, uefiMemoryMap, uefiCount);
  }
  if (!VirtDeviceTree::valid()) {
    while (true) {
      asm volatile("wfe");
    }
  }

  const char* bootargs = nullptr;
  virtGetBootargs(&bootargs);
  if (uefi) {
    const char* supplied = reinterpret_cast<const char*>(DirectMapBase + uefiCommandLineAddress);
    size_t length = 0;
    while (length < sizeof(uefiCommandLine) - 1 && supplied[length]) {
      uefiCommandLine[length] = supplied[length];
      ++length;
    }
    uefiCommandLine[length] = 0;
    bootargs = uefiCommandLine;
  }
  uint64_t initrdStart = 0, initrdEnd = 0;
  if (uefiInitrdStart && uefiInitrdEnd) {
    initrdStart = uefiInitrdStart;
    initrdEnd = uefiInitrdEnd;
  }
  const bool hasInitrd =
      ((initrdStart && initrdEnd) || virtGetInitrd(&initrdStart, &initrdEnd) ||
       initrdFromBootargs(bootargs, initrdStart, initrdEnd)) &&
      initrdInMemory(initrdStart, initrdEnd, deviceTree, uefiMemoryMap, uefiCount);
  ReservedRegion reserved[3] = {
      {alignDown(reinterpret_cast<uintptr_t>(&kernel_physical_start)),
       alignUp(reinterpret_cast<uintptr_t>(&kernel_physical_end))},
      {alignDown(reinterpret_cast<uintptr_t>(deviceTree)),
       alignUp(reinterpret_cast<uintptr_t>(deviceTree) + VirtDeviceTree::blobSize())},
      {hasInitrd ? alignDown(initrdStart) : 0, hasInitrd ? alignUp(initrdEnd) : 0}};
  for (size_t i = 1; i < 3; ++i) {
    for (size_t j = i; j && reserved[j].start < reserved[j - 1].start; --j) {
      const ReservedRegion previous = reserved[j - 1];
      reserved[j - 1] = reserved[j];
      reserved[j] = previous;
    }
  }

  size_t count = 0;
  const size_t sourceCount = uefi ? uefiCount : MaximumMemoryRegions / 4;
  for (size_t i = 0; i < sourceCount; ++i) {
    uint64_t base = 0, size = 0;
    if (uefi) {
      if (uefiMemoryMap[i].type != 1) {
        continue;
      }
      base = uefiMemoryMap[i].address;
      size = uefiMemoryMap[i].length;
    } else {
      if (!virtGetMemoryRegion(i, &base, &size)) {
        break;
      }
    }
    if (!size || base + size < base) {
      continue;
    }

    uint64_t cursor = base;
    const uint64_t end = base + size;
    for (size_t r = 0; r < 3; ++r) {
      if (!reserved[r].end || reserved[r].start >= end) {
        continue;
      }
      if (reserved[r].start > cursor) {
        addUsable(cursor, reserved[r].start < end ? reserved[r].start : end, count);
      }
      if (reserved[r].end > cursor) {
        cursor = reserved[r].end;
      }
    }
    addUsable(cursor, end, count);
  }

  BootstrapStruct_t bootstrap;
  bootstrap.setMemoryMap(memoryMap, count);
  if (uefi) {
    bootstrap.setUefi();
  }
  if (uefiAcpiRsdp) {
    bootstrap.setAcpiRsdp(DirectMapBase + uefiAcpiRsdp);
  }
  if (hasInitrd) {
    static const char name[] = "rootfs.img";
    initrdModule = {DirectMapBase + initrdStart, DirectMapBase + initrdEnd,
                    reinterpret_cast<uintptr_t>(name), 0};
    bootstrap.setModules(&initrdModule, 1);
  }
  if (bootargs) {
    bootstrap.setCommandLine(uefi ? bootargs
                                  : reinterpret_cast<const char*>(
                                        DirectMapBase + reinterpret_cast<uintptr_t>(bootargs)));
  }
  _main(bootstrap);
  while (true) {
    asm volatile("wfe");
  }
}
