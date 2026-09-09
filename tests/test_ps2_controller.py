"""Exercise the real PS/2 controller IRQ routing and shutdown lifecycle."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


STUBS = (
    "config.h",
    "pedigree/kernel/Atomic.h",
    "pedigree/kernel/compiler.h",
    "pedigree/kernel/Log.h",
    "pedigree/kernel/LockGuard.h",
    "pedigree/kernel/machine/Controller.h",
    "pedigree/kernel/machine/Device.h",
    "pedigree/kernel/machine/IrqHandler.h",
    "pedigree/kernel/machine/IrqManager.h",
    "pedigree/kernel/machine/Machine.h",
    "pedigree/kernel/machine/Trace.h",
    "pedigree/kernel/machine/types.h",
    "pedigree/kernel/process/Mutex.h",
    "pedigree/kernel/process/Scheduler.h",
    "pedigree/kernel/process/Thread.h",
    "pedigree/kernel/processor/state_forward.h",
    "pedigree/kernel/processor/types.h",
    "pedigree/kernel/processor/IoBase.h",
    "pedigree/kernel/processor/Processor.h",
    "pedigree/kernel/processor/ProcessorInformation.h",
    "pedigree/kernel/utilities/Buffer.h",
    "pedigree/kernel/utilities/String.h",
    "pedigree/kernel/utilities/Vector.h",
    "pedigree/kernel/utilities/assert.h",
)


class Ps2ControllerTests(unittest.TestCase):
    def test_threaded_irq_routing_and_shutdown(self):
        repository = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="pedigree-ps2-") as temporary:
            directory = Path(temporary)
            for name in STUBS:
                path = directory / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("")
            binary = directory / "controller-test"
            build = subprocess.run(
                [
                    *shlex.split(os.environ.get("CXX", "c++")),
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I",
                    str(directory),
                    "-I",
                    str(repository / "src/system/kernel/machine/mach_pc"),
                    "-I",
                    str(repository / "src/system/include"),
                    str(repository / "tests/fixtures/ps2-controller.cc"),
                    "-o",
                    str(binary),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            for scenario in (
                "routing",
                "controller-busy",
                "buffer-full",
                "shutdown",
                "partial-init",
                "first-init-failure",
                "first-port-only",
            ):
                with self.subTest(scenario=scenario):
                    result = subprocess.run(
                        [str(binary), scenario],
                        capture_output=True,
                        text=True,
                        timeout=10,
                    )
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
