// SPDX-License-Identifier: ISC
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#define ST_TYPE(info) ((info) & 15)
#define ST_BIND(info) ((info) >> 4)
constexpr unsigned STT_NOTYPE = 0, STT_OBJECT = 1, STT_FUNC = 2;
constexpr unsigned STB_LOCAL = 0, STB_GLOBAL = 1, STB_WEAK = 2;
constexpr unsigned PF_R = 4, PF_X = 1;

size_t BoundedStringLength(const char* value, size_t length) {
  return strnlen(value, length);
}

struct Symbol {
  uint32_t name;
  uint8_t info;
  uint8_t other;
  uint16_t shndx;
  uintptr_t value;
  size_t size;
};

class Elf {
 public:
  template <class T>
  const char* lookupSymbol(uintptr_t addr, uintptr_t* startAddr, T* symbolTable);
  char* m_pStringTable = nullptr;
  size_t m_nStringTableSize = 0;
  size_t m_nSymbolTableSize = 0;
  uintptr_t m_LoadBase = 0;
};

struct ModuleImage {
  using Symbol = ::Symbol;
  std::vector<Symbol> full;
  const char* names;
  bool symbol(size_t index, Symbol& result, bool dynamic = true) const {
    assert(!dynamic);
    if (index >= full.size())
      return false;
    result = full[index];
    return true;
  }
  const char* symbolName(const Symbol& value, bool dynamic = true) const {
    assert(!dynamic);
    return names + value.name;
  }
  bool contains(uintptr_t address, size_t size, unsigned flags) const {
    assert(flags == (PF_R | PF_X));
    return address >= 0x8000 && address < 0x9000 && size <= 0x9000 - address;
  }
};

struct Module {
  bool active = true;
  bool executing = false;
  bool isActive() const { return active; }
  bool isExecuting() const { return executing; }
};
struct Prepared {
  Module module;
  ModuleImage plan;
  uintptr_t base;
};
std::vector<Prepared*> slots;
class KernelElf {
 public:
  const char* runtimeLookupSymbolLocked(uintptr_t addr, uintptr_t* startAddr) const;
};

#include "debugger-symbols.inc"

int main() {
  char names[] = "\0local\0weak\0label\0object\0undefined\0hidden\0last\0";
  constexpr uintptr_t base = 0xffffffff9060b000ULL;
  std::vector<Symbol> symbols = {
      {},
      {1, STB_LOCAL * 16 + STT_FUNC, 0, 1, 0x866a, 180},
      {7, STB_WEAK * 16 + STT_FUNC, 0, 1, 0x8800, 12},
      {12, STB_LOCAL * 16 + STT_NOTYPE, 0, 1, 0x8820, 0},
      {18, STB_GLOBAL * 16 + STT_OBJECT, 0, 1, 0x8840, 16},
      {25, STB_GLOBAL * 16 + STT_FUNC, 0, 0, 0x8860, 16},
      {35, STB_LOCAL * 16 + STT_FUNC, 2, 1, 0x8880, 16},
      {42, STB_GLOBAL * 16 + STT_FUNC, 0, 1, 0x88a0, 16},
  };
  Elf elf;
  elf.m_pStringTable = names;
  elf.m_nStringTableSize = sizeof(names);
  elf.m_nSymbolTableSize = symbols.size() * sizeof(Symbol);
  elf.m_LoadBase = base;
  uintptr_t start = 0;
  auto lookup = [&](uintptr_t offset) {
    return elf.lookupSymbol(base + offset, &start, symbols.data());
  };
  assert(std::strcmp(lookup(0x8713), "local") == 0);
  assert(start == base + 0x866a);
  assert(!lookup(0x871e));
  assert(std::strcmp(lookup(0x880b), "weak") == 0);
  assert(!lookup(0x880c));
  assert(std::strcmp(lookup(0x8820), "label") == 0);
  assert(!lookup(0x8821));
  assert(!lookup(0x8840));
  assert(!lookup(0x8860));
  assert(std::strcmp(lookup(0x888f), "hidden") == 0);
  assert(std::strcmp(lookup(0x88af), "last") == 0);
  assert(!lookup(0x88b0));
  symbols.back().value = ~uintptr_t{0} - base + 1;
  assert(!elf.lookupSymbol(uintptr_t{0}, &start, symbols.data()));
  symbols.back().value = 0x88a0;
  symbols.back().name = sizeof(names);
  assert(!lookup(0x88a0));
  symbols.back().name = 42;
  elf.m_nStringTableSize = sizeof(names) - 2;
  assert(!lookup(0x88a0));
  elf.m_nStringTableSize = sizeof(names);

  Prepared prepared{{}, {symbols, names}, base};
  slots = {nullptr, &prepared};
  KernelElf kernel;
  auto runtime = [&](uintptr_t offset) {
    return kernel.runtimeLookupSymbolLocked(base + offset, &start);
  };
  assert(std::strcmp(runtime(0x8713), "local") == 0);
  assert(start == base + 0x866a);
  assert(std::strcmp(runtime(0x880b), "weak") == 0);
  assert(std::strcmp(runtime(0x8820), "label") == 0);
  assert(!runtime(0x8821));
  assert(!runtime(0x8840));
  assert(!runtime(0x8860));
  assert(std::strcmp(runtime(0x888f), "hidden") == 0);
  assert(std::strcmp(runtime(0x88af), "last") == 0);
  assert(!runtime(0x88b0));
  prepared.module.active = false;
  assert(!runtime(0x8713));
  prepared.module.executing = true;
  assert(std::strcmp(runtime(0x8713), "local") == 0);
  prepared.plan.full.back().value = 0x9000;
  assert(!runtime(0x9000));
  prepared.plan.full.back().value = ~uintptr_t{0} - base + 1;
  assert(!kernel.runtimeLookupSymbolLocked(0, &start));
}
