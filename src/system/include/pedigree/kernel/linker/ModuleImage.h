/* Copyright (c) 2026, Pedigree Developers. */
#ifndef KERNEL_LINKER_MODULEIMAGE_H
#define KERNEL_LINKER_MODULEIMAGE_H

#include "pedigree/kernel/linker/Elf.h"

/** Allocation-free plan over an immutable, kernel-owned native module image. */
class EXPORTED_PUBLIC ModuleImage {
 public:
  static constexpr size_t MaximumImageBytes = 1024 * 1024;
  static constexpr size_t MaximumMappedBytes = 1024 * 1024;
  static constexpr size_t MaximumSegments = 16;
  static constexpr size_t MaximumSections = 256;
  static constexpr size_t MaximumSymbols = 4096;
  static constexpr size_t MaximumRelocations = 16384;
  static constexpr size_t MaximumDependencies = 16;
  static constexpr size_t MaximumLifecycleFunctions = 128;
  static constexpr size_t NameBytes = 56;
  static constexpr size_t SymbolNameBytes = 256;

  enum class Result { Valid, Malformed, Unsupported, TooLarge };
  using Header = Elf::ElfHeader_t;
  using Segment = Elf::ElfProgramHeader_t;
  using Section = Elf::ElfSectionHeader_t;
  using Symbol = Elf::ElfSymbol_t;
  using Relocation = Elf::ElfRela_t;
  using Dynamic = Elf::ElfDyn_t;

  ModuleImage();
  Result preflight(const uint8_t* image, size_t length);

  bool symbol(size_t index, Symbol& result, bool dynamic = true) const;
  const char* symbolName(const Symbol& value, bool dynamic = true) const;
  bool findSymbol(const char* name, Symbol& result, bool dynamic = false) const;
  bool relocation(size_t index, Relocation& result) const;
  bool contains(uintptr_t address, size_t length, size_t flags = PF_R) const;
  bool fileOffset(uintptr_t address, size_t length, size_t& offset) const;
  bool localPointer(uintptr_t address, uintptr_t& target) const;
  bool validateMaterialized(uintptr_t base) const;
  bool exportedSymbol(size_t index, Symbol& result) const;

  const uint8_t* bytes;
  size_t byteCount;
  Segment segments[MaximumSegments];
  size_t segmentCount;
  size_t mappedBytes;
  size_t symbolCount;
  size_t relocationCount;
  char name[NameBytes];
  char dependencies[MaximumDependencies][NameBytes];
  size_t dependencyCount;
  char optionalDependencies[MaximumDependencies][NameBytes];
  size_t optionalDependencyCount;
  uintptr_t entry;
  uintptr_t exit;
  uintptr_t constructors[MaximumLifecycleFunctions];
  size_t constructorCount;
  uintptr_t destructors[MaximumLifecycleFunctions];
  size_t destructorCount;

 private:
  bool section(size_t index, Section& result) const;
  bool string(size_t tableOffset, size_t tableBytes, size_t offset, const char*& result,
              size_t maximum) const;
  bool mappedString(uintptr_t address, const char*& result, size_t maximum) const;
  bool namedObject(const char* name, size_t size, Symbol& result) const;
  bool dependenciesAt(const char* symbolName, char names[][NameBytes], size_t& count,
                      bool optional) const;
  bool lifecycle(const char* startName, const char* endName, uintptr_t* targets,
                 size_t& count) const;
  Result validateSections(const Header& header);
  Result validateDynamic();
  Result validateRelocations();
  Result validateMetadata();

  size_t sectionOffset;
  size_t sectionCount;
  size_t sectionStrings;
  size_t sectionStringBytes;
  size_t dynamicOffset;
  size_t dynamicBytes;
  size_t symbols;
  size_t symbolBytes;
  size_t strings;
  size_t stringBytes;
  size_t fullSymbols;
  size_t fullSymbolBytes;
  size_t fullStrings;
  size_t fullStringBytes;
  size_t relocations;
  size_t relocationBytes;
  uintptr_t symbolAddress;
  uintptr_t stringAddress;
  uintptr_t relocationAddress;
  uintptr_t constructorAddress;
  size_t constructorBytes;
  uintptr_t destructorAddress;
  size_t destructorBytes;
};

#endif
