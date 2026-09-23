#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/linker/Elf.h"
#include "pedigree/kernel/linker/KernelElf.h"

namespace {
enum AArch64Relocation {
  R_AARCH64_NONE = 0,
  R_AARCH64_ABS64 = 257,
  R_AARCH64_ABS32 = 258,
  R_AARCH64_PREL64 = 260,
  R_AARCH64_PREL32 = 261,
  R_AARCH64_COPY = 1024,
  R_AARCH64_GLOB_DAT = 1025,
  R_AARCH64_JUMP_SLOT = 1026,
  R_AARCH64_RELATIVE = 1027,
};
}

bool Elf::applyRelocation(ElfRel_t, ElfSectionHeader_t*, SymbolTable*, uintptr_t,
                          SymbolTable::Policy, uintptr_t, uintptr_t) {
  ERROR("AArch64 ELF REL relocation without an explicit addend");
  return false;
}

bool Elf::applyRelocation(ElfRela_t rel, ElfSectionHeader_t* section, SymbolTable* symbols,
                          uintptr_t loadBase, SymbolTable::Policy policy,
                          uintptr_t destinationAddress, uintptr_t destinationEnd) {
  if (section && !section->addr) {
    return true;
  }

  const auto type = static_cast<AArch64Relocation>(R_TYPE(rel.info));
  if (type == R_AARCH64_NONE) {
    return true;
  }
  if (!loadBase) {
    loadBase = section ? section->addr - section->offset : 0;
    if (!loadBase) {
      ERROR("AArch64 relocation has no load base");
      return false;
    }
  }

  const uintptr_t place = loadBase + rel.offset;
  const uintptr_t destination = destinationAddress ? destinationAddress : place;
  const size_t symbolIndex = R_SYM(rel.info);
  ElfSymbol_t* elfSymbols = m_pDynamicSymbolTable ? m_pDynamicSymbolTable : m_pSymbolTable;
  const char* strings = m_pDynamicStringTable ? m_pDynamicStringTable : m_pStringTable;
  uintptr_t symbol = 0;
  size_t symbolSize = 0;

  if (type != R_AARCH64_RELATIVE) {
    if (!elfSymbols) {
      ERROR("AArch64 relocation has no symbol table");
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
      if (type == R_AARCH64_COPY) {
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
        WARNING("AArch64 relocation failed for symbol \"" << name << "\"");
        return false;
      }
    }
  }

  size_t writeSize = sizeof(uint64_t);
  if (type == R_AARCH64_ABS32 || type == R_AARCH64_PREL32) {
    writeSize = sizeof(uint32_t);
  } else if (type == R_AARCH64_COPY) {
    writeSize = symbolSize;
  }
  if (destinationEnd &&
      (destination > destinationEnd || destinationEnd - destination < writeSize)) {
    ERROR("AArch64 relocation crosses the demand-page staging boundary");
    return false;
  }

  const uint64_t addend = static_cast<uint64_t>(rel.addend);
  uint64_t value = 0;
  switch (type) {
    case R_AARCH64_ABS64:
      value = symbol + addend;
      break;
    case R_AARCH64_ABS32:
      value = symbol + addend;
      if (value >> 32) {
        ERROR("AArch64 ABS32 relocation overflows");
        return false;
      }
      break;
    case R_AARCH64_PREL64:
      value = symbol + addend - place;
      break;
    case R_AARCH64_PREL32:
      value = symbol + addend - place;
      if (static_cast<int64_t>(value) < INT32_MIN || static_cast<int64_t>(value) > INT32_MAX) {
        ERROR("AArch64 PREL32 relocation overflows");
        return false;
      }
      break;
    case R_AARCH64_GLOB_DAT:
    case R_AARCH64_JUMP_SLOT:
      value = symbol + addend;
      break;
    case R_AARCH64_RELATIVE:
      value = loadBase + addend;
      break;
    case R_AARCH64_COPY:
      if (!symbol) {
        ERROR("AArch64 COPY relocation has no source");
        return false;
      }
      for (size_t i = 0; i < symbolSize; ++i) {
        reinterpret_cast<uint8_t*>(destination)[i] = reinterpret_cast<const uint8_t*>(symbol)[i];
      }
      return true;
    default:
      ERROR("Unsupported AArch64 ELF relocation " << Dec << R_TYPE(rel.info));
      return false;
  }

  if (writeSize == sizeof(uint32_t)) {
    *reinterpret_cast<uint32_t*>(destination) = static_cast<uint32_t>(value);
  } else {
    *reinterpret_cast<uint64_t*>(destination) = value;
  }
  return true;
}
