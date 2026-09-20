"""Exercise production CPU lookups with substituted GS and LAPIC inputs."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/system/kernel/core/processor/x86_common/Processor.cc"
HEADERS = ROOT / "src/system/include/pedigree/kernel/processor"
CXX = shlex.split(os.environ.get("CXX", "c++"))

HARNESS = r"""
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>
#define ALWAYS_INLINE
#define EXPORTED_PUBLIC
#define NEVER_INLINE
using ProcessorId = size_t;
template <class T> struct Vector : std::vector<T> {
  using std::vector<T>::vector;
  size_t count() const { return this->size(); }
};
struct ProcessorInformation {
  ProcessorId m_ProcessorId;
  uint8_t m_LocalApicId;
  ProcessorId processorId() const { return m_ProcessorId; }
  uint8_t localApicId() const { return m_LocalApicId; }
};
static size_t testGsReads, testApicReads;
#if X64
static ProcessorInformation* testGsInformation;
static size_t testGsIndex;
#endif
struct LocalApic {
  uint8_t id = 30;
  uint8_t getId() { ++testApicReads; return id; }
};
struct Pc {
  bool available = true;
  LocalApic apic;
  static Pc& instance() { static Pc pc; return pc; }
  bool localApicAvailable() const { return available; }
  LocalApic& getLocalApic() { return apic; }
};
struct ProcessorBase {
  inline static size_t m_Initialised = 2;
  inline static Vector<ProcessorInformation*> m_ProcessorInformation;
  inline static ProcessorInformation m_SafeBspProcessorInformation{0, 0};
  static ProcessorId id();
  static size_t index();
  static ProcessorInformation& information();
};

#include "lookup-header.inc"
#include "lookups.inc"

static void expect(size_t index, ProcessorInformation* info, size_t gsReads,
                   size_t apicReads) {
  testGsReads = testApicReads = 0;
  assert(ProcessorBase::index() == index);
  assert(ProcessorBase::id() == info->m_ProcessorId);
  assert(&ProcessorBase::information() == info);
  assert(testGsReads == gsReads);
  assert(testApicReads == apicReads);
}

int main() {
  // Logical IDs, APIC IDs, and vector positions deliberately differ.
  ProcessorInformation cpus[] = {{77, 21}, {4, 3}, {901, 30}, {18, 11}};
  auto& topology = ProcessorBase::m_ProcessorInformation;
  for (auto& cpu : cpus) topology.push_back(&cpu);
  auto* safe = &ProcessorBase::m_SafeBspProcessorInformation;
#if X64
  // The installed anchor is authoritative even before topology publication.
  for (size_t initialised : {0U, 1U, 2U}) {
    ProcessorBase::m_Initialised = initialised;
    for (size_t i = 0; i < 4; ++i) {
      testGsInformation = &cpus[i];
      testGsIndex = i;
      expect(i, &cpus[i], 3, 0);
    }
  }
  topology.clear();
  Pc::instance().available = false;
  testGsInformation = safe;
  testGsIndex = 0;
  expect(0, safe, 3, 0);
#else
  for (unsigned apic = 0; apic <= UINT8_MAX; ++apic) {
    Pc::instance().apic.id = apic;
#if MULTIPROCESSOR
    size_t expected = topology.count();
    ProcessorInformation* info = safe;
    for (size_t i = 0; i < topology.count(); ++i) {
      if (apic == cpus[i].m_LocalApicId) {
        expected = i;
        info = &cpus[i];
      }
    }
    expect(expected, info, 0, 3);
#else
    expect(0, safe, 0, 0);
#endif
  }
  ProcessorBase::m_Initialised = 1;
  expect(0, safe, 0, 0);
  ProcessorBase::m_Initialised = 2;
  Pc::instance().available = false;
  expect(0, safe, 0, 0);
  Pc::instance().available = true;
#if MULTIPROCESSOR
  // A sole processor needs no APIC read but retains its nonzero logical ID.
  topology.resize(1);
  expect(0, &cpus[0], 0, 0);
  topology.clear();
  expect(0, safe, 0, 3);
#endif
#endif
  return 0;
}
"""


class X86ProcessorIdentityTests(unittest.TestCase):
    @unittest.skipUnless(CXX and shutil.which(CXX[0]), "requires a native C++ compiler")
    def test_gs_lookup_and_32bit_hardware_fallbacks(self):
        source = SOURCE.read_text()
        start = source.index("#if MULTIPROCESSOR && !X64\nNEVER_INLINE")
        end = source.index("\nsize_t ProcessorBase::getCount()", start)
        lookups = source[start:end]
        common = (HEADERS / "x86_common/Processor.h").read_text()
        common = common[common.index("#if X86_COMMON && !X64") : common.rindex("#endif")]
        x64 = (HEADERS / "x64/Processor.h").read_text()
        x64 = x64[x64.index("#if X64\nALWAYS_INLINE") : x64.rindex("#endif")]
        for slot, output, substitute in (
            (16, "information", "testGsInformation"),
            (24, "index", "testGsIndex"),
        ):
            instruction = (
                f'asm volatile("movq %%gs:{slot}, %0" : "=r"({output}) : : "memory");'
            )
            self.assertEqual(x64.count(instruction), 1)
            x64 = x64.replace(instruction, f"{output} = {substitute}; ++testGsReads;")

        with tempfile.TemporaryDirectory(prefix="pedigree-cpu-identity-") as tmp:
            directory = Path(tmp)
            (directory / "lookup-header.inc").write_text(common + x64)
            (directory / "lookups.inc").write_text(lookups)
            harness = directory / "identity.cc"
            harness.write_text(HARNESS)
            for x64, smp in ((1, 1), (0, 1), (1, 0), (0, 0)):
                with self.subTest(x64=x64, smp=smp):
                    binary = directory / f"identity-{x64}-{smp}"
                    command = [
                        *CXX, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-DX86_COMMON=1", f"-DX64={x64}", f"-DMULTIPROCESSOR={smp}",
                        str(harness), "-o", str(binary),
                    ]
                    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
                    self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)
                    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
