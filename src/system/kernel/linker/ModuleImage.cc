/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/linker/ModuleImage.h"
#include "pedigree/kernel/utilities/utility.h"

namespace {
bool range(size_t offset, size_t length, size_t limit) {
  return offset <= limit && length <= limit - offset;
}

bool overlap(uintptr_t first, size_t firstBytes, uintptr_t second, size_t secondBytes) {
  return first <= second ? second - first < firstBytes : first - second < secondBytes;
}

constexpr uint32_t Relative = 8;
constexpr uint32_t Absolute = 1;
constexpr uint32_t GlobalData = 6;
constexpr uint32_t JumpSlot = 7;
}  // namespace

ModuleImage::ModuleImage() {
  ByteSet(this, 0, sizeof(*this));
}

bool ModuleImage::section(size_t index, Section& result) const {
  if (index >= sectionCount) {
    return false;
  }
  MemoryCopy(&result, bytes + sectionOffset + index * sizeof(result), sizeof(result));
  return true;
}

bool ModuleImage::contains(uintptr_t address, size_t length, size_t flags) const {
  for (size_t i = 0; i < segmentCount; ++i) {
    const auto& segment = segments[i];
    if ((segment.flags & flags) == flags && address >= segment.vaddr &&
        range(address - segment.vaddr, length, segment.memsz)) {
      return true;
    }
  }
  return false;
}

bool ModuleImage::fileOffset(uintptr_t address, size_t length, size_t& offset) const {
  for (size_t i = 0; i < segmentCount; ++i) {
    const auto& segment = segments[i];
    if (address >= segment.vaddr && range(address - segment.vaddr, length, segment.filesz)) {
      offset = segment.offset + address - segment.vaddr;
      return range(offset, length, byteCount);
    }
  }
  return false;
}

bool ModuleImage::string(size_t tableOffset, size_t tableBytes, size_t offset, const char*& result,
                         size_t maximum) const {
  if (offset >= tableBytes || !range(tableOffset, tableBytes, byteCount)) {
    return false;
  }
  result = reinterpret_cast<const char*>(bytes + tableOffset + offset);
  const size_t available = pedigree_std::min(tableBytes - offset, maximum);
  return BoundedStringLength(result, available) < available;
}

bool ModuleImage::symbol(size_t index, Symbol& result, bool dynamic) const {
  const size_t offset = dynamic ? symbols : fullSymbols;
  const size_t length = dynamic ? symbolBytes : fullSymbolBytes;
  if (index >= length / sizeof(result)) {
    return false;
  }
  MemoryCopy(&result, bytes + offset + index * sizeof(result), sizeof(result));
  return true;
}

const char* ModuleImage::symbolName(const Symbol& value, bool dynamic) const {
  const char* result = nullptr;
  return string(dynamic ? strings : fullStrings, dynamic ? stringBytes : fullStringBytes,
                value.name, result, SymbolNameBytes)
             ? result
             : nullptr;
}

bool ModuleImage::findSymbol(const char* wanted, Symbol& result, bool dynamic) const {
  bool found = false;
  const size_t count = (dynamic ? symbolBytes : fullSymbolBytes) / sizeof(Symbol);
  for (size_t i = 0; i < count; ++i) {
    Symbol candidate;
    if (!symbol(i, candidate, dynamic)) {
      return false;
    }
    const char* name = symbolName(candidate, dynamic);
    if (candidate.shndx && name && !StringCompare(name, wanted)) {
      if (found) {
        return false;
      }
      result = candidate;
      found = true;
    }
  }
  return found;
}

bool ModuleImage::relocation(size_t index, Relocation& result) const {
  if (index >= relocationCount) {
    return false;
  }
  MemoryCopy(&result, bytes + relocations + index * sizeof(result), sizeof(result));
  return true;
}

bool ModuleImage::localPointer(uintptr_t address, uintptr_t& target) const {
  size_t offset = 0;
  if (!fileOffset(address, sizeof(uintptr_t), offset)) {
    return false;
  }
  MemoryCopy(&target, bytes + offset, sizeof(target));
  for (size_t i = 0; i < relocationCount; ++i) {
    Relocation value;
    relocation(i, value);
    if (value.offset != address) {
      continue;
    }
    if (R_TYPE(value.info) == Relative) {
      target = value.addend;
      return true;
    }
    Symbol local;
    if (R_TYPE(value.info) != Absolute || !symbol(R_SYM(value.info), local) || !local.shndx ||
        value.addend < 0 || !range(local.value, value.addend, MaximumMappedBytes)) {
      return false;
    }
    target = local.value + value.addend;
    return true;
  }
  // Unrelocated metadata may only express a sentinel, never an address.
  return target == 0 || target == ~uintptr_t{0};
}

ModuleImage::Result ModuleImage::preflight(const uint8_t* image, size_t length) {
  ByteSet(this, 0, sizeof(*this));
#if !BITS_64 || (!X64 && !HOSTED)
  return Result::Unsupported;
#endif
  if (length > MaximumImageBytes) {
    return Result::TooLarge;
  }
  Elf::ExecutableMetadata metadata;
  const auto headerResult = Elf::validateExecutableHeader(image, length, length, metadata);
  if (headerResult != Elf::ExecutableValidationResult::Valid) {
    return headerResult == Elf::ExecutableValidationResult::Malformed ? Result::Malformed
                                                                      : Result::Unsupported;
  }
  Header header;
  MemoryCopy(&header, image, sizeof(header));
  if (header.type != ET_DYN || header.entry || header.flags || header.phnum > MaximumSegments ||
      !header.shnum || header.shnum > MaximumSections || header.shentsize != sizeof(Section) ||
      !header.shstrndx || header.shstrndx >= header.shnum) {
    return Result::Unsupported;
  }
  if (!range(header.shoff, header.shnum * sizeof(Section), length)) {
    return Result::Malformed;
  }
  bytes = image;
  byteCount = length;
  sectionOffset = header.shoff;
  sectionCount = header.shnum;
  bool executable = false;
  for (size_t i = 0; i < header.phnum; ++i) {
    Segment segment;
    MemoryCopy(&segment, image + header.phoff + i * sizeof(segment), sizeof(segment));
    if (segment.type == PT_LOAD) {
      if (!(segment.flags & PF_R) || (segment.flags & ~(PF_R | PF_W | PF_X)) ||
          ((segment.flags & PF_W) && (segment.flags & PF_X))) {
        return Result::Unsupported;
      }
      segments[segmentCount++] = segment;
      if ((segment.flags & PF_X) && segment.memsz && !executable) {
        // The executable validator's entry coverage becomes module code coverage.
        // The original module e_entry remains required to be zero above.
        metadata.entryPoint = segment.vaddr;
        executable = true;
      }
    } else if (segment.type == PT_DYNAMIC) {
      if (dynamicBytes || !segment.filesz || segment.filesz != segment.memsz ||
          segment.filesz % sizeof(Dynamic)) {
        return Result::Malformed;
      }
      dynamicOffset = segment.offset;
      dynamicBytes = segment.filesz;
    } else if (segment.type != PT_NULL && segment.type != PT_NOTE && segment.type != PT_PHDR &&
               !(segment.type == 0x6474e551 && !(segment.flags & PF_X))) {
      return Result::Unsupported;
    }
  }
  const auto segmentsResult = Elf::validateExecutableProgramHeaders(
      image + header.phoff, metadata.programHeaderSize, length, metadata);
  if (!executable || segmentsResult != Elf::ExecutableValidationResult::Valid || !dynamicBytes ||
      metadata.hasInterpreter) {
    return Result::Malformed;
  }
  if (metadata.loadEnd > MaximumMappedBytes) {
    return Result::TooLarge;
  }
  mappedBytes = metadata.loadEnd;
  Result result = validateSections(header);
  if (result == Result::Valid)
    result = validateDynamic();
  if (result == Result::Valid)
    result = validateRelocations();
  if (result == Result::Valid)
    result = validateMetadata();
  return result;
}

ModuleImage::Result ModuleImage::validateSections(const Header& header) {
  Section names;
  section(header.shstrndx, names);
  if (names.type != SHT_STRTAB || !names.size || !range(names.offset, names.size, byteCount)) {
    return Result::Malformed;
  }
  sectionStrings = names.offset;
  sectionStringBytes = names.size;
  size_t dynamicSectionCount = 0;
  size_t dynamicSymbolIndex = 0;
  for (size_t i = 0; i < sectionCount; ++i) {
    Section value;
    section(i, value);
    const char* sectionName = nullptr;
    if (!string(sectionStrings, sectionStringBytes, value.name, sectionName, SymbolNameBytes) ||
        (value.type != SHT_NOBITS && !range(value.offset, value.size, byteCount)) ||
        (value.addralign && (value.addralign & (value.addralign - 1)))) {
      return Result::Malformed;
    }
    if ((value.flags & (0x400 | 0x800)) || value.type == SHT_REL ||
        value.type == SHT_PREINIT_ARRAY) {
      return Result::Unsupported;
    }
    if ((value.flags & SHF_ALLOC) && value.size) {
      size_t offset = 0;
      if (!contains(value.addr, value.size, PF_R | ((value.flags & SHF_EXECINSTR) ? PF_X : 0)) ||
          (value.type != SHT_NOBITS &&
           (!fileOffset(value.addr, value.size, offset) || offset != value.offset))) {
        return Result::Malformed;
      }
    }
    if (value.type == SHT_DYNAMIC) {
      if (++dynamicSectionCount != 1 || value.offset != dynamicOffset ||
          value.size != dynamicBytes || value.entsize != sizeof(Dynamic) ||
          !(value.flags & SHF_ALLOC)) {
        return Result::Malformed;
      }
    } else if (value.type == SHT_SYMTAB || value.type == SHT_DYNSYM) {
      Section stringsSection;
      if (!value.size || value.size % sizeof(Symbol) || value.entsize != sizeof(Symbol) ||
          value.size / sizeof(Symbol) > MaximumSymbols || !section(value.link, stringsSection) ||
          stringsSection.type != SHT_STRTAB || !stringsSection.size ||
          !range(stringsSection.offset, stringsSection.size, byteCount)) {
        return Result::Malformed;
      }
      if (value.type == SHT_DYNSYM) {
        if (symbolBytes || !(value.flags & SHF_ALLOC))
          return Result::Unsupported;
        symbols = value.offset;
        symbolBytes = value.size;
        strings = stringsSection.offset;
        stringBytes = stringsSection.size;
        symbolAddress = value.addr;
        stringAddress = stringsSection.addr;
        dynamicSymbolIndex = i;
      } else {
        if (fullSymbolBytes)
          return Result::Unsupported;
        fullSymbols = value.offset;
        fullSymbolBytes = value.size;
        fullStrings = stringsSection.offset;
        fullStringBytes = stringsSection.size;
      }
    } else if (value.type == SHT_RELA && (value.flags & SHF_ALLOC)) {
      if (relocationBytes || !value.size || value.entsize != sizeof(Relocation) ||
          value.size % sizeof(Relocation) || value.size / sizeof(Relocation) > MaximumRelocations) {
        return Result::Unsupported;
      }
      relocations = value.offset;
      relocationBytes = value.size;
      relocationAddress = value.addr;
    }
  }
  if (dynamicSectionCount != 1 || !symbolBytes || !fullSymbolBytes) {
    return Result::Malformed;
  }
  symbolCount = symbolBytes / sizeof(Symbol);
  relocationCount = relocationBytes / sizeof(Relocation);
  for (size_t i = 0; i < sectionCount; ++i) {
    Section value;
    section(i, value);
    if (value.type == SHT_RELA && (value.flags & SHF_ALLOC) &&
        (value.link != dynamicSymbolIndex || value.info)) {
      return Result::Unsupported;
    }
  }
  for (size_t table = 0; table < 2; ++table) {
    const bool dynamic = table == 0;
    const size_t count = (dynamic ? symbolBytes : fullSymbolBytes) / sizeof(Symbol);
    for (size_t i = 0; i < count; ++i) {
      Symbol value;
      symbol(i, value, dynamic);
      if (!i &&
          (value.name || value.info || value.other || value.shndx || value.value || value.size)) {
        return Result::Malformed;
      }
      if (!symbolName(value, dynamic) || ST_BIND(value.info) > STB_WEAK ||
          ST_TYPE(value.info) > STT_FILE || (value.other & ~3U)) {
        return Result::Unsupported;
      }
      if (!value.shndx) {
        if (value.value || (!i && (value.name || value.info || value.other || value.size))) {
          return Result::Malformed;
        }
      } else if (value.shndx != 0xfff1) {
        Section owner;
        if (!section(value.shndx, owner))
          return Result::Malformed;
        if (dynamic && !(owner.flags & SHF_ALLOC))
          return Result::Unsupported;
        if ((owner.flags & SHF_ALLOC) &&
            (!contains(value.value, value.size) || value.value < owner.addr ||
             !range(value.value - owner.addr, value.size, owner.size))) {
          return Result::Malformed;
        }
        if (ST_TYPE(value.info) == STT_FUNC && (owner.flags & SHF_ALLOC) &&
            !contains(value.value, value.size ? value.size : 1, PF_R | PF_X)) {
          return Result::Malformed;
        }
      } else if (dynamic || ST_TYPE(value.info) != STT_FILE) {
        return Result::Unsupported;
      }
    }
  }
  return Result::Valid;
}

ModuleImage::Result ModuleImage::validateDynamic() {
  uintptr_t rela = 0, jump = 0, sym = 0, str = 0;
  size_t relaBytes = 0, jumpBytes = 0, stringsBytes = 0;
  size_t relaEntry = 0, symbolEntry = 0, pltType = 0;
  uint64_t seen = 0;
  bool terminated = false;
  for (size_t i = 0; i < dynamicBytes / sizeof(Dynamic); ++i) {
    Dynamic value;
    MemoryCopy(&value, bytes + dynamicOffset + i * sizeof(value), sizeof(value));
    if (terminated) {
      if (value.tag || value.un.val)
        return Result::Malformed;
      continue;
    }
    if (value.tag == DT_NULL) {
      terminated = true;
      continue;
    }
    if (value.tag >= 0 && value.tag < 64 && value.tag != DT_NEEDED) {
      const uint64_t bit = uint64_t{1} << value.tag;
      if (seen & bit)
        return Result::Malformed;
      seen |= bit;
    }
    switch (value.tag) {
      case DT_NEEDED: {
        const char* needed = nullptr;
        if (!string(strings, stringBytes, value.un.val, needed, SymbolNameBytes) ||
            StringCompare(needed, "libkernel_shared.so"))
          return Result::Unsupported;
        break;
      }
      case DT_SYMTAB:
        sym = value.un.ptr;
        break;
      case DT_STRTAB:
        str = value.un.ptr;
        break;
      case DT_STRSZ:
        stringsBytes = value.un.val;
        break;
      case DT_SYMENT:
        symbolEntry = value.un.val;
        break;
      case DT_RELA:
        rela = value.un.ptr;
        break;
      case DT_RELASZ:
        relaBytes = value.un.val;
        break;
      case DT_RELAENT:
        relaEntry = value.un.val;
        break;
      case DT_JMPREL:
        jump = value.un.ptr;
        break;
      case DT_PLTRELSZ:
        jumpBytes = value.un.val;
        break;
      case DT_PLTREL:
        pltType = value.un.val;
        break;
      case DT_INIT_ARRAY:
        constructorAddress = value.un.ptr;
        break;
      case DT_INIT_ARRAYSZ:
        constructorBytes = value.un.val;
        break;
      case DT_FINI_ARRAY:
        destructorAddress = value.un.ptr;
        break;
      case DT_FINI_ARRAYSZ:
        destructorBytes = value.un.val;
        break;
      case DT_PLTGOT:
      case DT_HASH:
      case 0x6ffffef5: {
        size_t unused = 0;
        if (!fileOffset(value.un.ptr, sizeof(uintptr_t), unused))
          return Result::Malformed;
        break;
      }
      case DT_BIND_NOW:
        if (value.un.val)
          return Result::Unsupported;
        break;
      case DT_FLAGS:
        if (value.un.val & ~uint64_t{8})
          return Result::Unsupported;
        break;
      case 0x6ffffffb:
        if (value.un.val & ~uint64_t{1})
          return Result::Unsupported;
        break;
      case 0x6ffffff9:
        if (value.un.val > relocationCount)
          return Result::Malformed;
        break;
      default:
        return Result::Unsupported;
    }
  }
  if (!terminated || sym != symbolAddress || str != stringAddress || stringsBytes != stringBytes ||
      symbolEntry != sizeof(Symbol) || relaBytes > relocationBytes ||
      jumpBytes != relocationBytes - relaBytes ||
      (relocationBytes && relaEntry != sizeof(Relocation)) ||
      (relaBytes && rela != relocationAddress) ||
      (jumpBytes && (jump != relocationAddress + relaBytes || pltType != DT_RELA))) {
    return Result::Malformed;
  }
  return Result::Valid;
}

ModuleImage::Result ModuleImage::validateRelocations() {
  uintptr_t previous = 0;
  for (size_t i = 0; i < relocationCount; ++i) {
    Relocation value;
    relocation(i, value);
    const size_t kind = R_TYPE(value.info);
    if ((kind != Relative && kind != Absolute && kind != GlobalData && kind != JumpSlot) ||
        !contains(value.offset, sizeof(uintptr_t)) ||
        contains(value.offset, sizeof(uintptr_t), PF_X) ||
        (i && (value.offset < previous || value.offset - previous < sizeof(uintptr_t)))) {
      return Result::Unsupported;
    }
    previous = value.offset;
    if (overlap(value.offset, sizeof(uintptr_t), symbolAddress, symbolBytes) ||
        overlap(value.offset, sizeof(uintptr_t), stringAddress, stringBytes) ||
        overlap(value.offset, sizeof(uintptr_t), relocationAddress, relocationBytes)) {
      return Result::Malformed;
    }
    if (kind == Relative) {
      if (R_SYM(value.info) || value.addend < 0 || !contains(value.addend, 1)) {
        return Result::Malformed;
      }
    } else {
      Symbol target;
      if (!symbol(R_SYM(value.info), target) || !R_SYM(value.info) ||
          ST_TYPE(target.info) > STT_FUNC || (!target.shndx && ST_BIND(target.info) == STB_LOCAL) ||
          ((kind == JumpSlot || kind == GlobalData) && value.addend)) {
        return Result::Malformed;
      }
      if (target.shndx &&
          (value.addend < 0 || !range(target.value, value.addend, MaximumMappedBytes) ||
           !contains(target.value + value.addend, 1))) {
        return Result::Malformed;
      }
    }
  }
  return Result::Valid;
}

bool ModuleImage::namedObject(const char* wanted, size_t size, Symbol& result) const {
  Section owner;
  Symbol exported;
  if (!findSymbol(wanted, exported, true) || !findSymbol(wanted, result) ||
      exported.value != result.value || exported.size != result.size ||
      exported.shndx != result.shndx || exported.info != result.info || result.size != size ||
      ST_TYPE(result.info) != STT_OBJECT || !section(result.shndx, owner))
    return false;
  const char* ownerName = nullptr;
  return string(sectionStrings, sectionStringBytes, owner.name, ownerName, SymbolNameBytes) &&
         !StringCompare(ownerName, ".modinfo") && contains(result.value, size);
}

bool ModuleImage::dependenciesAt(const char* wanted, char names[][NameBytes], size_t& count,
                                 bool optional) const {
  Symbol list;
  if (!findSymbol(wanted, list))
    return optional;
  if (list.size < sizeof(uintptr_t) || list.size % sizeof(uintptr_t) ||
      list.size / sizeof(uintptr_t) > MaximumDependencies + 1 ||
      !namedObject(wanted, list.size, list))
    return false;
  for (size_t i = 0; i < list.size / sizeof(uintptr_t); ++i) {
    uintptr_t target = 0;
    if (!localPointer(list.value + i * sizeof(uintptr_t), target))
      return false;
    if (!target)
      return i + 1 == list.size / sizeof(uintptr_t);
    if (count == MaximumDependencies)
      return false;
    size_t offset = 0;
    if (!fileOffset(target, 1, offset))
      return false;
    const char* dependency = nullptr;
    if (!mappedString(target, dependency, NameBytes) || !*dependency)
      return false;
    const size_t length = StringLength(dependency);
    for (size_t j = 0; j < count; ++j) {
      if (!StringCompare(names[j], dependency))
        return false;
    }
    MemoryCopy(names[count++], dependency, length + 1);
  }
  return false;
}

bool ModuleImage::lifecycle(const char* startName, const char* endName, uintptr_t* targets,
                            size_t& count) const {
  Symbol first, last;
  if (!findSymbol(startName, first) || !findSymbol(endName, last) || last.value < first.value ||
      (last.value - first.value) % sizeof(uintptr_t) ||
      (last.value - first.value) / sizeof(uintptr_t) > MaximumLifecycleFunctions ||
      !contains(first.value, last.value - first.value))
    return false;
  const bool constructor = !StringCompare(startName, "start_ctors");
  const uintptr_t expected = constructor ? constructorAddress : destructorAddress;
  const size_t expectedBytes = constructor ? constructorBytes : destructorBytes;
  if (expectedBytes && (expected != first.value || expectedBytes != last.value - first.value)) {
    return false;
  }
  for (uintptr_t at = first.value; at < last.value; at += sizeof(uintptr_t)) {
    uintptr_t target = 0;
    if (!localPointer(at, target))
      return false;
    if (!target)
      break;
    if (target == ~uintptr_t{0})
      continue;
    if (!contains(target, 1, PF_R | PF_X))
      return false;
    targets[count++] = target;
  }
  return true;
}

ModuleImage::Result ModuleImage::validateMetadata() {
  Symbol nameObject, entryObject, exitObject, unloadable, runtimeUnloadable, optionalFunction;
  if (!namedObject("g_pModuleName", sizeof(uintptr_t), nameObject) ||
      !namedObject("g_pModuleEntry", sizeof(uintptr_t), entryObject) ||
      !namedObject("g_pModuleExit", sizeof(uintptr_t), exitObject) ||
      !namedObject("g_bModuleUnloadable", 1, unloadable) ||
      !namedObject("g_bModuleRuntimeUnloadable", 1, runtimeUnloadable) ||
      findSymbol("__add_optional_deps", optionalFunction))
    return Result::Unsupported;
  size_t offset = 0;
  if (!fileOffset(unloadable.value, 1, offset) || bytes[offset] != 1 ||
      !fileOffset(runtimeUnloadable.value, 1, offset) || bytes[offset] != 1) {
    return Result::Unsupported;
  }
  uintptr_t nameAddress = 0;
  const char* moduleName = nullptr;
  if (!localPointer(nameObject.value, nameAddress) || !fileOffset(nameAddress, 1, offset) ||
      !mappedString(nameAddress, moduleName, NameBytes) || !*moduleName ||
      !StringCompare(moduleName, "init") || !localPointer(entryObject.value, entry) || !entry ||
      !contains(entry, 1, PF_R | PF_X) || !localPointer(exitObject.value, exit) || !exit ||
      !contains(exit, 1, PF_R | PF_X)) {
    return Result::Malformed;
  }
  const size_t length = StringLength(moduleName);
  for (size_t i = 0; i < length; ++i) {
    const char c = moduleName[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
          c == '_'))
      return Result::Unsupported;
  }
  MemoryCopy(name, moduleName, length + 1);
  if (!dependenciesAt("g_pDepends", dependencies, dependencyCount, false) ||
      !dependenciesAt("g_pOptionalDepends", optionalDependencies, optionalDependencyCount, true) ||
      !lifecycle("start_ctors", "end_ctors", constructors, constructorCount) ||
      !lifecycle("start_dtors", "end_dtors", destructors, destructorCount)) {
    return Result::Malformed;
  }
  return Result::Valid;
}

bool ModuleImage::mappedString(uintptr_t address, const char*& result, size_t maximum) const {
  for (size_t i = 0; i < segmentCount; ++i) {
    const auto& segment = segments[i];
    if (address >= segment.vaddr && address - segment.vaddr < segment.filesz) {
      return string(segment.offset, segment.filesz, address - segment.vaddr, result, maximum);
    }
  }
  return false;
}

bool ModuleImage::exportedSymbol(size_t index, Symbol& result) const {
  Section owner;
  return symbol(index, result) && result.shndx && section(result.shndx, owner) &&
         (owner.flags & SHF_ALLOC) &&
         (ST_BIND(result.info) == STB_GLOBAL || ST_BIND(result.info) == STB_WEAK) &&
         ST_TYPE(result.info) <= STT_FUNC && (result.other == 0 || result.other == 3) &&
         contains(result.value, result.size);
}

bool ModuleImage::validateMaterialized(uintptr_t base) const {
  // Metadata is frozen in the plan. Verify relocation did not change the ABI
  // which the module itself observes before calling any of its code.
  const char* objects[] = {"g_pModuleName", "g_pModuleEntry", "g_pModuleExit", "g_pDepends",
                           "g_pOptionalDepends"};
  for (size_t i = 0; i < sizeof(objects) / sizeof(objects[0]); ++i) {
    Symbol value;
    if (!findSymbol(objects[i], value)) {
      if (i == 4)
        continue;
      return false;
    }
    for (size_t offset = 0; offset < value.size; offset += sizeof(uintptr_t)) {
      uintptr_t expected = 0, actual = 0;
      if (!localPointer(value.value + offset, expected))
        return false;
      MemoryCopy(&actual, reinterpret_cast<void*>(base + value.value + offset), sizeof(actual));
      if (actual != (expected ? base + expected : 0))
        return false;
      if (i == 0 || (i >= 3 && expected)) {
        const char* original = nullptr;
        if (!mappedString(expected, original, NameBytes) ||
            MemoryCompare(original, reinterpret_cast<void*>(base + expected),
                          StringLength(original) + 1))
          return false;
      }
    }
  }
  const char* starts[] = {"start_ctors", "start_dtors"};
  const char* ends[] = {"end_ctors", "end_dtors"};
  for (size_t i = 0; i < 2; ++i) {
    Symbol first, last;
    if (!findSymbol(starts[i], first) || !findSymbol(ends[i], last))
      return false;
    for (uintptr_t at = first.value; at < last.value; at += sizeof(uintptr_t)) {
      uintptr_t expected = 0, actual = 0;
      if (!localPointer(at, expected))
        return false;
      MemoryCopy(&actual, reinterpret_cast<void*>(base + at), sizeof(actual));
      if (actual != ((!expected || expected == ~uintptr_t{0}) ? expected : base + expected))
        return false;
    }
  }
  const char* flags[] = {"g_bModuleUnloadable", "g_bModuleRuntimeUnloadable"};
  for (const char* flag : flags) {
    Symbol value;
    if (!findSymbol(flag, value) || *reinterpret_cast<const uint8_t*>(base + value.value) != 1)
      return false;
  }
  return true;
}
