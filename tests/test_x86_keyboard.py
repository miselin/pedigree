"""Exercise the real PS/2 keyboard reader with interleaved LED replies."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


STUBS = (
    "config.h",
    "pedigree/kernel/compiler.h",
    "pedigree/kernel/Log.h",
    "pedigree/kernel/LockGuard.h",
    "pedigree/kernel/machine/Keyboard.h",
    "pedigree/kernel/machine/KeymapManager.h",
    "pedigree/kernel/machine/HidInputManager.h",
    "pedigree/kernel/machine/InputManager.h",
    "pedigree/kernel/machine/types.h",
    "pedigree/kernel/process/Mutex.h",
    "pedigree/kernel/process/OwnedThread.h",
    "pedigree/kernel/process/Thread.h",
    "pedigree/kernel/processor/types.h",
    "pedigree/kernel/processor/Processor.h",
    "pedigree/kernel/processor/ProcessorInformation.h",
    "pedigree/kernel/utilities/new",
)


def build_keyboard_fixture(directory, source=None):
    repository = Path(__file__).resolve().parents[1]
    machine = repository / "src/system/kernel/machine/mach_pc"
    for name in STUBS:
        path = directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("")
    binary = directory / "keyboard-test"
    subprocess.run(
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
            str(machine),
            f'-DKEYBOARD_SOURCE="{source or machine / "Keyboard.cc"}"',
            str(repository / "tests/fixtures/x86-keyboard.cc"),
            "-o",
            str(binary),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    return binary


class X86KeyboardTests(unittest.TestCase):
    def test_led_responses_preserve_keyboard_input(self):
        with tempfile.TemporaryDirectory(prefix="pedigree-keyboard-") as temporary:
            binary = build_keyboard_fixture(Path(temporary))
            for scenario in (
                "release-interleaving",
                "ack-sequencing",
                "resend",
                "command-exhaustion",
                "data-exhaustion",
                "coalescing",
                "missing-ack",
                "lock-toggles",
            ):
                with self.subTest(scenario=scenario):
                    subprocess.run(
                        [str(binary), scenario],
                        check=True,
                        capture_output=True,
                        text=True,
                        timeout=10,
                    )


if __name__ == "__main__":
    unittest.main()
