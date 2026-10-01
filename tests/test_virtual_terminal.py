# SPDX-License-Identifier: ISC
"""Exercise production VT switching and descriptor ioctl routing on the host."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class VirtualTerminalTests(unittest.TestCase):
    def test_ownership_switching_and_descriptor_routing(self):
        compiler = shutil.which("clang++")
        if not compiler:
            self.skipTest("clang++ is required")
        root = Path(__file__).resolve().parents[1]
        directory = root / "src/modules/subsys/posix"
        header = (directory / "VirtualTerminal.h").read_text()
        source = (directory / "VirtualTerminal.cc").read_text()
        console = (directory / "console-syscalls.h").read_text()
        ioctls = (directory / "file-syscalls.cc").read_text()
        routing_start = ioctls.index("  size_t terminalNumber = ~size_t(0);")
        routing_end = ioctls.index("  if (f->getFile()->supports(command))", routing_start)
        spans = [
            ("    // KDSETMODE", "    // KDGKBMODE"),
            ("    // VT_OPENQRY", "    // VT_WAITACTIVE"),
        ]
        cases = "\n".join(ioctls[ioctls.index(a):ioctls.index(b, ioctls.index(a))]
                          for a, b in spans)
        with tempfile.TemporaryDirectory(prefix="pedigree-virtual-terminal-") as temporary:
            work = Path(temporary)
            (work / "vt-abi.inc").write_text(
                console[console.index("struct vt_mode {"):console.index("struct kbentry {")]
                + console[console.index("// vt_mode modes"):console.index("int posix_tcgetattr(")]
            )
            (work / "virtual-terminal-header.inc").write_text(
                header[header.index("#define MAX_VT"):header.rindex("#endif")]
            )
            (work / "virtual-terminal.inc").write_text(source[source.index("extern DevFs*"):])
            (work / "vt-ioctl.inc").write_text(
                ioctls[routing_start:routing_end] + "switch (command) {\n" + cases
                + "}\nreturn -1;\n"
            )
            binary = work / "virtual-terminal"
            subprocess.run([
                compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-private-field",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(work), str(root / "tests/fixtures/virtual-terminal.cc"),
                "-o", str(binary),
            ], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
