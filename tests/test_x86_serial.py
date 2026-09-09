"""Run the real x86 serial driver against present and absent UART registers."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


STUBS = {
    "config.h": "",
    "pedigree/kernel/processor/types.h": "#pragma once\n#include <cstddef>\n#include <cstdint>\n",
    "pedigree/kernel/Log.h": "#define NOTICE(x) do {} while (0)\n",
    "pedigree/kernel/processor/Processor.h": "struct Processor { static void pause() {} };\n",
    "pedigree/kernel/machine/Serial.h": """
#pragma once
#include <cstddef>
#include <cstdint>
class Serial {
 public:
  virtual ~Serial() = default;
  virtual void setBase(uintptr_t) = 0;
  virtual bool hasData() = 0;
  virtual char read() = 0;
  virtual char readNonBlock() = 0;
  virtual void write(char) = 0;
};
""",
    "pedigree/kernel/processor/IoPort.h": """
#pragma once
#include <cstddef>
#include <cstdint>
struct IoPort {
  inline static bool present = true, allocates = true;
  inline static uint8_t registers[8] = {};
  inline static size_t reads = 0, dataWrites = 0;
  explicit IoPort(const char*) {}
  bool allocate(uintptr_t, size_t) { return allocates; }
  uint8_t read8(size_t offset) {
    ++reads;
    return present ? registers[offset] : 0xff;
  }
  void write8(uint8_t value, size_t offset) {
    if (!present) return;
    if (offset == 0 && !(registers[3] & 0x80)) ++dataWrites;
    registers[offset] = value;
  }
};
""",
}


class X86SerialTests(unittest.TestCase):
    def test_uart_presence_and_io(self):
        repository = Path(__file__).resolve().parents[1]
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        with tempfile.TemporaryDirectory(prefix="pedigree-serial-") as temporary:
            directory = Path(temporary)
            for name, contents in STUBS.items():
                path = directory / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(contents)
            binary = directory / "serial-test"
            subprocess.run(
                [
                    *compiler,
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(directory),
                    "-I",
                    str(repository / "src/system/kernel/machine/mach_pc"),
                    str(repository / "tests/fixtures/x86-serial.cc"),
                    str(repository / "src/system/kernel/machine/mach_pc/Serial.cc"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            for scenario in ("absent", "present", "allocation-failure"):
                with self.subTest(scenario=scenario):
                    subprocess.run(
                        [str(binary), scenario], check=True,
                        capture_output=True, text=True, timeout=10,
                    )


if __name__ == "__main__":
    unittest.main()
