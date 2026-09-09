"""Exercise production CPU lookups with substituted TR and LAPIC hardware inputs."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/system/kernel/core/processor/x86_common/Processor.cc"
CXX = shlex.split(os.environ.get("CXX", "c++"))

HARNESS = r"""
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

using ProcessorId = size_t;
template <class T> struct Vector : std::vector<T> {
  using std::vector<T>::vector;
  size_t count() const { return this->size(); }
};
struct ProcessorInformation {
  ProcessorId m_ProcessorId;
  uint8_t m_LocalApicId;
  uint16_t selector;
  uint16_t getTssSelector() const { return selector; }
};
static uint16_t testTaskRegister;
static size_t testTaskReads, testApicReads;
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
  inline static ProcessorInformation m_SafeBspProcessorInformation{0, 0, 0};
  static ProcessorId id();
  static size_t index();
  static ProcessorInformation& information();
};

#include "lookups.inc"

static void expect(size_t index, ProcessorInformation* info, size_t taskReads,
                   size_t apicReads) {
  testTaskReads = testApicReads = 0;
  assert(ProcessorBase::index() == index);
  assert(ProcessorBase::id() == info->m_ProcessorId);
  assert(&ProcessorBase::information() == info);
  assert(testTaskReads == taskReads);
  assert(testApicReads == apicReads);
}

int main() {
  // Logical IDs, APIC IDs, and vector positions deliberately differ.
  ProcessorInformation cpus[] = {{77, 21, 0x38}, {4, 3, 0x48},
                                 {901, 30, 0x58}, {18, 11, 0x68}};
  auto& topology = ProcessorBase::m_ProcessorInformation;
  for (auto& cpu : cpus) topology.push_back(&cpu);
  auto* safe = &ProcessorBase::m_SafeBspProcessorInformation;

  for (uint32_t selector = 0; selector <= UINT16_MAX; ++selector) {
    testTaskRegister = selector;
#if MULTIPROCESSOR
    size_t expected = 2;
    size_t taskReads = 0, apicReads = 3;
#if X64
    taskReads = 3;
    for (size_t i = 0; i < 4; ++i) {
      if (selector == cpus[i].selector) {
        expected = i;
        apicReads = 0;
      }
    }
#endif
    expect(expected, &cpus[expected], taskReads, apicReads);
#else
    expect(0, safe, 0, 0);
#endif
  }

  testTaskRegister = 0x38;
  ProcessorBase::m_Initialised = 1;
  expect(0, safe, 0, 0);
  ProcessorBase::m_Initialised = 2;
  Pc::instance().available = false;
  expect(0, safe, 0, 0);
  Pc::instance().available = true;

#if MULTIPROCESSOR
  constexpr size_t taskReads = X64 ? 3 : 0;
  // A selector-shaped value cannot bypass agreement with the stored TSS.
  cpus[0].selector = 0x78;
  expect(2, &cpus[2], taskReads, 3);
  cpus[0].selector = 0x38;

  Pc::instance().apic.id = 255;
  testTaskRegister = 0;
  expect(topology.count(), safe, taskReads, 3);

  // The one-CPU case still returns its logical ID, rather than assuming zero.
  topology.resize(1);
  Pc::instance().apic.id = cpus[0].m_LocalApicId;
  testTaskRegister = 0x38;
  expect(0, &cpus[0], taskReads, X64 ? 0 : 3);

  // Discovery can leave the vector empty, even in an SMP-enabled build.
  topology.clear();
  expect(0, safe, taskReads, 3);
#endif
  return 0;
}
"""


class X86ProcessorIdentityTests(unittest.TestCase):
    @unittest.skipUnless(
        CXX and shutil.which(CXX[0]), "requires a native C++ compiler"
    )
    def test_tss_lookup_and_hardware_fallbacks(self):
        source = SOURCE.read_text()
        start = source.index("#if MULTIPROCESSOR && X64\nnamespace {")
        end = source.index("\nsize_t ProcessorBase::getCount()", start)
        lookups = source[start:end]
        instruction = 'asm volatile("str %0" : "=r"(selector));'
        self.assertEqual(lookups.count(instruction), 1)
        lookups = lookups.replace(
            instruction, "selector = testTaskRegister; ++testTaskReads;"
        )

        with tempfile.TemporaryDirectory(prefix="pedigree-cpu-identity-") as tmp:
            directory = Path(tmp)
            (directory / "lookups.inc").write_text(lookups)
            harness = directory / "identity.cc"
            harness.write_text(HARNESS)
            for x64, smp in ((1, 1), (0, 1), (1, 0)):
                with self.subTest(x64=x64, smp=smp):
                    binary = directory / f"identity-{x64}-{smp}"
                    command = [
                        *CXX,
                        "-std=c++17",
                        "-O2",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        f"-DX64={x64}",
                        f"-DMULTIPROCESSOR={smp}",
                        str(harness),
                        "-o",
                        str(binary),
                    ]
                    result = subprocess.run(
                        command, capture_output=True, text=True, timeout=60
                    )
                    self.assertEqual(
                        result.returncode, 0, msg=result.stdout + result.stderr
                    )
                    result = subprocess.run(
                        [str(binary)], capture_output=True, text=True, timeout=10
                    )
                    self.assertEqual(
                        result.returncode, 0, msg=result.stdout + result.stderr
                    )


if __name__ == "__main__":
    unittest.main()
