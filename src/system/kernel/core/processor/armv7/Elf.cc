#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/linker/Elf.h"
#include "pedigree/kernel/linker/KernelElf.h"

namespace {
enum ArmRelocation {
  R_ARM_NONE = 0,
  R_ARM_ABS32 = 2,
  R_ARM_REL32 = 3,
  R_ARM_COPY = 20,
  R_ARM_GLOB_DAT = 21,
  R_ARM_JUMP_SLOT = 22,
  R_ARM_RELATIVE = 23,
};
}

bool Elf::applyRelocation(ElfRel_t rel, ElfSectionHeader_t* section, SymbolTable* symbols,
                          uintptr_t loadBase, SymbolTable::Policy policy,
                          uintptr_t destinationAddress, uintptr_t destinationEnd) {
  if ((section && !section->addr) || R_TYPE(rel.info) == R_ARM_NONE) {
    return true;
  }
  if (!loadBase && section) {
    loadBase = section->addr - section->offset;
  }
  if (!loadBase) {
    ERROR("ARM relocation has no load base");
    return false;
  }
  const uintptr_t place = loadBase + rel.offset;
  const uintptr_t destination = destinationAddress ? destinationAddress : place;
  if (destinationEnd && (destination > destinationEnd || destinationEnd - destination < 4)) {
    ERROR("ARM REL relocation crosses the demand-page staging boundary");
    return false;
  }
  const intptr_t addend = *reinterpret_cast<const int32_t*>(destination);
  return applyArmRelocation(R_TYPE(rel.info), R_SYM(rel.info), rel.offset, addend, section, symbols,
                            loadBase, policy, destinationAddress, destinationEnd);
}

bool Elf::applyRelocation(ElfRela_t rel, ElfSectionHeader_t* section, SymbolTable* symbols,
                          uintptr_t loadBase, SymbolTable::Policy policy,
                          uintptr_t destinationAddress, uintptr_t destinationEnd) {
  return applyArmRelocation(R_TYPE(rel.info), R_SYM(rel.info), rel.offset, rel.addend, section,
                            symbols, loadBase, policy, destinationAddress, destinationEnd);
}

bool Elf::applyArmRelocation(Elf_Word type, Elf_Word symbolIndex, uintptr_t offset, intptr_t addend,
                             ElfSectionHeader_t* section, SymbolTable* symbols, uintptr_t loadBase,
                             SymbolTable::Policy policy, uintptr_t destinationAddress,
                             uintptr_t destinationEnd) {
  if (section && !section->addr) {
    return true;
  }
  if (type == R_ARM_NONE) {
    return true;
  }
  if (!loadBase) {
    loadBase = section ? section->addr - section->offset : 0;
    if (!loadBase) {
      ERROR("ARM relocation has no load base");
      return false;
    }
  }

  const uintptr_t place = loadBase + offset;
  const uintptr_t destination = destinationAddress ? destinationAddress : place;
  ElfSymbol_t* elfSymbols = m_pDynamicSymbolTable ? m_pDynamicSymbolTable : m_pSymbolTable;
  const char* strings = m_pDynamicStringTable ? m_pDynamicStringTable : m_pStringTable;
  uintptr_t symbol = 0;
  size_t symbolSize = 0;

  if (type != R_ARM_RELATIVE) {
    if (!elfSymbols) {
      ERROR("ARM relocation has no symbol table");
      return false;
    }
    const ElfSymbol_t& entry = elfSymbols[symbolIndex];
    symbolSize = entry.size;
    if (ST_TYPE(entry.info) == 3) {
      if (!m_pSectionHeaders || entry.shndx >= m_nSectionHeaders) {
        return false;
      }
      symbol = m_pSectionHeaders[entry.shndx].addr;
    } else {
      if (!strings) {
        return false;
      }
      const char* name = strings + entry.name;
      if (type == R_ARM_COPY) {
        policy = SymbolTable::NotOriginatingElf;
      }
      if (!symbols) {
        symbols = &m_SymbolTable;
      }
      symbol = symbols->lookup(String(name), this, policy);
      if (!symbol) {
        symbol = KernelElf::instance().getSymbolTable()->lookup(String(name), this, policy);
      }
      if (!symbol && ST_BIND(entry.info) != 2) {
        WARNING("ARM relocation failed for symbol \"" << name << "\"");
        return false;
      }
    }
  }

  const size_t writeSize = type == R_ARM_COPY ? symbolSize : sizeof(uint32_t);
  if (destinationEnd &&
      (destination > destinationEnd || destinationEnd - destination < writeSize)) {
    ERROR("ARM relocation crosses the demand-page staging boundary");
    return false;
  }

  switch (type) {
    case R_ARM_ABS32:
      *reinterpret_cast<uint32_t*>(destination) = symbol + addend;
      return true;
    case R_ARM_REL32:
      *reinterpret_cast<uint32_t*>(destination) = symbol + addend - place;
      return true;
    case R_ARM_GLOB_DAT:
    case R_ARM_JUMP_SLOT:
      *reinterpret_cast<uint32_t*>(destination) = symbol;
      return true;
    case R_ARM_RELATIVE:
      *reinterpret_cast<uint32_t*>(destination) = loadBase + addend;
      return true;
    case R_ARM_COPY:
      if (!symbol) {
        ERROR("ARM COPY relocation has no source");
        return false;
      }
      for (size_t i = 0; i < symbolSize; ++i) {
        reinterpret_cast<uint8_t*>(destination)[i] = reinterpret_cast<const uint8_t*>(symbol)[i];
      }
      return true;
    default:
      ERROR("Unsupported ARM ELF relocation " << Dec << type);
      return false;
  }
}
