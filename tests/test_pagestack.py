"""Compile the real PageStack against a guarded native mapping backend."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(
    os.environ.get("PEDIGREE_PAGESTACK_SOURCE_ROOT", Path(__file__).resolve().parents[1])
)
CXX = shlex.split(os.environ.get("CXX", "c++"))
PMM = ROOT / "src/system/kernel/core/processor/x86_common/PhysicalMemoryManager"

PRELUDE = r"""
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <sanitizer/asan_interface.h>

#define INITIALISATION_ONLY
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#define EMIT_IF(x) if constexpr (x)
#define assertx(x) assert(x)
using physical_uintptr_t = uint64_t;
template <typename T> using Atomic = std::atomic<T>;
static size_t g_FreePages = 0, g_AllocedPages = 0;
static void* adjust_pointer(void* p, size_t n) {
  return static_cast<unsigned char*>(p) + n;
}
[[noreturn]] static void panic(const char* message) {
  throw std::runtime_error(message);
}
struct Processor {
  static void pause() { throw std::runtime_error("unexpected readiness wait"); }
};
struct PhysicalMemoryManager {
  static constexpr size_t getPageSize() { return 4096; }
};

struct VirtualAddressSpace {
  static constexpr size_t KernelMode = 1, Write = 2;
  static constexpr size_t bytes = 2 * 1024 * 1024;
  unsigned char* stacks[3] = {};
  size_t leaves[3] = {};
  size_t intermediate = HOSTED ? 0 : 3;
  std::set<void*> mapped;
  std::set<uint64_t> consumed;
  VirtualAddressSpace() {
    for (auto& stack : stacks) {
      stack = static_cast<unsigned char*>(std::calloc(1, bytes));
      if (!stack) throw std::bad_alloc();
      // ASan guards each target-sized page even on a 16 KiB host.
      __asan_poison_memory_region(stack, bytes);
    }
  }
  ~VirtualAddressSpace() {
    for (auto stack : stacks) std::free(stack);
  }
  static VirtualAddressSpace& getKernelAddressSpace();
  uintptr_t getKernelVirtualPagestack() { return reinterpret_cast<uintptr_t>(stacks[0]); }
  uintptr_t getKernelVirtualPagestackAdd1() { return reinterpret_cast<uintptr_t>(stacks[1]); }
  uintptr_t getKernelVirtualPagestackAdd2() { return reinterpret_cast<uintptr_t>(stacks[2]); }
  bool isMapped(void* address) { return mapped.count(address); }
  bool map(uint64_t physical, void* address, size_t) {
    if (isMapped(address)) return false;
    const uintptr_t target = reinterpret_cast<uintptr_t>(address);
    for (size_t i = 0; i < 3; ++i) {
      const uintptr_t base = reinterpret_cast<uintptr_t>(stacks[i]);
      if (target >= base && target - base < bytes) {
        assert((target - base) % 4096 == 0);
        assert(consumed.insert(physical).second);
        mapped.insert(address);
        ++leaves[i];
        __asan_unpoison_memory_region(address, 4096);
        return true;
      }
    }
    throw std::runtime_error("mapping outside reserved stack space");
  }
};
struct X64VirtualAddressSpace : VirtualAddressSpace {
  bool mapPageStructures(uint64_t physical, void* address, size_t flags) {
    if (intermediate) {
      --intermediate;
      assert(consumed.insert(physical).second);
      return true;
    }
    return map(physical, address, flags);
  }
  bool mapPageStructuresAbove4GB(uint64_t physical, void* address, size_t flags) {
    return map(physical, address, flags);
  }
};
VirtualAddressSpace& VirtualAddressSpace::getKernelAddressSpace() {
  static X64VirtualAddressSpace space;
  return space;
}
class X86CommonPhysicalMemoryManager : public PhysicalMemoryManager {
 public:
  static constexpr size_t below4GB = 1, below64GB = 2;
"""

ACCESSORS = r"""
  static void ready(PageStack& stack) {
    stack.markBelow4GReady();
    stack.markAbove4GReady();
  }
  static void seed(PageStack& stack, uint64_t base, size_t count) {
#if LEGACY_CAPACITY
    // Preserve the original callers when checking an unmodified snapshot.
    stack.increaseCapacity(count + (base < 0x100000000ULL ? 1 : 0));
#endif
    stack.free(base, count * getPageSize(), true);
  }
  static void makeFull(PageStack& stack, size_t index, uint64_t base) {
    // A full stack is a legal allocation input, regardless of boot headroom.
    const size_t entries = stack.m_StackMax[index] / (index ? 8 : 4);
    for (size_t i = 0; i < entries; ++i) {
      if (index) static_cast<uint64_t*>(stack.m_Stack[index])[i] = base + i * 4096;
      else static_cast<uint32_t*>(stack.m_Stack[index])[i] = base + i * 4096;
    }
    stack.m_StackSize[index] = stack.m_StackMax[index];
    stack.m_FreePages = entries;
  }
};
"""

CONTRACTS = r"""
using Manager = X86CommonPhysicalMemoryManager;
using Stack = Manager::PageStack;
#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
  return false; } } while (false)

static bool drain(Stack& stack, size_t expected) {
  auto& space = VirtualAddressSpace::getKernelAddressSpace();
  std::set<uint64_t> seen;
  while (uint64_t page = stack.allocate(0, false)) {
    CHECK(!space.consumed.count(page));
    CHECK(seen.insert(page).second);
  }
  CHECK(seen.size() == expected);
  CHECK(stack.freePages() == 0);
  return true;
}

static bool high_range() {
  static Stack stack;
  auto& space = VirtualAddressSpace::getKernelAddressSpace();
  Manager::seed(stack, 0x1000000, 511);
  Manager::seed(stack, 0x100000000ULL, 0x1e600000 / 4096);
  Manager::ready(stack);
  CHECK(space.leaves[0] == 1);
  CHECK(space.leaves[1] == 243);
  CHECK(space.leaves[2] == 0);
  const size_t expected = 511 + 124416 - space.consumed.size();
  CHECK(stack.totalPages() == expected);
  CHECK(stack.freePages() == expected);
  return drain(stack, expected);
}

static bool independent() {
  static Stack stack;
  auto& space = VirtualAddressSpace::getKernelAddressSpace();
  Manager::seed(stack, 0x1000000, 511);
  Manager::seed(stack, 0x100000000ULL, 700);
  Manager::seed(stack, 0x1000000000ULL, 1100);
  Manager::seed(stack, 0x101000000ULL, 600);
  Manager::ready(stack);
  CHECK(space.leaves[0] == 1);
  CHECK(space.leaves[1] == 3);
  CHECK(space.leaves[2] == 3);
  const size_t expected = 511 + 700 + 1100 + 600 - space.consumed.size();
  CHECK(stack.freePages() == expected);
  CHECK(stack.totalPages() == expected);
  return drain(stack, expected);
}

static bool consumed_pages() {
  static Stack stack;
  auto& space = VirtualAddressSpace::getKernelAddressSpace();
  Manager::seed(stack, 0x1000000, 100);
  Manager::ready(stack);
  CHECK(space.leaves[0] == 1);
  CHECK(space.consumed.size() == (HOSTED ? 1 : 4));
  const size_t expected = 100 - space.consumed.size();
  CHECK(stack.freePages() == expected);
  return drain(stack, expected);
}

static bool empty_high() {
  static Stack stack;
  Manager::seed(stack, 0x1000000, 100);
  const size_t lowPages = stack.freePages();
  Manager::seed(stack, 0x100000000ULL, 100);
  const size_t highPages = stack.freePages() - lowPages;
  Manager::ready(stack);
  for (size_t i = 0; i < highPages; ++i)
    CHECK(stack.allocate(Manager::below64GB, false) >= 0x100000000ULL);
  CHECK(stack.allocate(0, false) != 0);
  CHECK(stack.allocate(Manager::below64GB, false) != 0);
  CHECK(stack.allocate(Manager::below4GB, false) != 0);
  return true;
}

static bool full_stack(size_t index) {
  static Stack stack;
  const uint64_t base = index == 0 ? 0x1000000ULL :
                        index == 1 ? 0x100000000ULL : 0x1000000000ULL;
  Manager::seed(stack, base, 100);
  Manager::ready(stack);
  Manager::makeFull(stack, index, base + 0x1000000);
  const size_t count = stack.freePages();
  const size_t constraint = index == 0 ? Manager::below4GB :
                            index == 1 ? Manager::below64GB : 0;
  CHECK(stack.allocate(constraint, false) == base + 0x1000000 + (count - 1) * 4096);
  CHECK(stack.freePages() == count - 1);
  return true;
}

static bool refill() {
  static Stack stack;
  auto& space = VirtualAddressSpace::getKernelAddressSpace();
  Manager::seed(stack, 0x1000000, 511);
  Manager::seed(stack, 0x100000000ULL, 700);
  Manager::ready(stack);
  const size_t count = stack.freePages();
  const size_t total = stack.totalPages();
  const size_t mappings = space.mapped.size();
  std::vector<uint64_t> pages;
  while (uint64_t page = stack.allocate(0, false)) pages.push_back(page);
  CHECK(pages.size() == count);
  for (uint64_t page : pages) stack.free(page, 4096);
  CHECK(stack.freePages() == count);
  CHECK(stack.totalPages() == total);
  CHECK(space.mapped.size() == mappings);
  return drain(stack, count);
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const std::string test = argv[1];
  bool passed = false;
  if (test == "high-range") passed = high_range();
  else if (test == "independent") passed = independent();
  else if (test == "consumed-pages") passed = consumed_pages();
  else if (test == "empty-high") passed = empty_high();
  else if (test == "full-low") passed = full_stack(0);
  else if (test == "full-high") passed = full_stack(1);
  else if (test == "full-above64") passed = full_stack(2);
  else if (test == "refill") passed = refill();
  if (passed) std::puts("PageStack contract passed");
  return passed ? 0 : 1;
}
"""


class PageStackTests(unittest.TestCase):
    @unittest.skipUnless(CXX and shutil.which(CXX[0]), "requires a native C++ compiler")
    def test_stack_contracts(self):
        header = PMM.with_suffix(".h").read_text()
        implementation = PMM.with_suffix(".cc").read_text()
        declaration = header.split("  class PageStack {", 1)[1].split("\n  };", 1)[0]
        declaration = "  class PageStack {" + declaration + "\n  };\n"
        start = "physical_uintptr_t X86CommonPhysicalMemoryManager::PageStack::allocate"
        implementation = start + implementation.split(start, 1)[1]
        legacy = int("void increaseCapacity(" in declaration)
        cases = (
            "high-range",
            "independent",
            "consumed-pages",
            "empty-high",
            "full-low",
            "full-high",
            "full-above64",
            "refill",
        )
        with tempfile.TemporaryDirectory(prefix="pedigree-pagestack-") as temporary:
            directory = Path(temporary)
            source = directory / "pagestack.cc"
            source.write_text(PRELUDE + declaration + ACCESSORS + implementation + CONTRACTS)
            for hosted in (0, 1):
                binary = directory / f"pagestack-{hosted}"
                command = [
                    *CXX,
                    "-std=c++17",
                    "-O1",
                    "-g",
                    "-fsanitize=address,undefined",
                    f"-DHOSTED={hosted}",
                    f"-DLEGACY_CAPACITY={legacy}",
                    str(source),
                    "-o",
                    str(binary),
                ]
                compiled = subprocess.run(
                    command, capture_output=True, text=True, timeout=60
                )
                self.assertEqual(
                    compiled.returncode, 0, shlex.join(command) + "\n" + compiled.stderr
                )
                for case in cases:
                    with self.subTest(hosted=hosted, contract=case):
                        result = subprocess.run(
                            [str(binary), case], capture_output=True, text=True, timeout=15
                        )
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertEqual(result.stdout.strip(), "PageStack contract passed")


if __name__ == "__main__":
    unittest.main()
