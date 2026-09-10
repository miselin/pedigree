# SPDX-License-Identifier: ISC
"""Run the production AHCI batch loop against deterministic admission/reap seams.

This verifies ownership and progress decisions, not MMIO or DMA correctness.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class AhciReadBatchTests(unittest.TestCase):
    def test_saturation_partial_admission_and_failure_drain(self):
        compiler = shutil.which("clang++")
        if not compiler:
            self.skipTest("clang++ is required")
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/modules/drivers/common/ahci/AhciPort.cc").read_text()
        start = source.index("bool AhciPort::readBatch(")
        end = source.index("void AhciPort::shutdown()", start)
        with tempfile.TemporaryDirectory(prefix="ahci-read-batch-") as temporary:
            work = Path(temporary)
            (work / "ahci-read-batch.inc").write_text(source[start:end])
            binary = work / "batch-test"
            subprocess.run([
                compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(work), str(root / "tests/fixtures/ahci-read-batch.cc"),
                "-o", str(binary),
            ], check=True, timeout=60)
            environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
            subprocess.run([str(binary)], check=True, timeout=15, env=environment)

    def test_deferred_event_completion_and_dma_timeout(self):
        compiler = shutil.which("clang++")
        if not compiler:
            self.skipTest("clang++ is required")
        root = Path(__file__).resolve().parents[1]
        directory = root / "src/modules/drivers/common/ahci"
        source = (directory / "AhciPort.cc").read_text()
        spans = [
            ("void AhciPort::waitForProgress()", "bool AhciPort::initialise("),
            ("void AhciPort::observe(", "bool AhciPort::chooseSlot("),
            ("bool AhciPort::reapCommand(", "bool AhciPort::command("),
        ]
        body = "\n".join(source[source.index(a):source.index(b, source.index(a))]
                         for a, b in spans)
        registers = (directory / "Registers.h").read_text().replace(
            '#include "pedigree/kernel/processor/types.h"', '')
        with tempfile.TemporaryDirectory(prefix="ahci-completion-wait-") as temporary:
            work = Path(temporary)
            (work / "ahci-completion-wait.inc").write_text(body)
            (work / "ahci-registers.inc").write_text(registers)
            binary = work / "completion-test"
            subprocess.run([
                compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-parameter", "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer", "-I", str(work),
                str(root / "tests/fixtures/ahci-completion-wait.cc"),
                "-o", str(binary),
            ], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=15,
                           env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))


if __name__ == "__main__":
    unittest.main()
