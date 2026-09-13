# SPDX-License-Identifier: ISC
"""Run production provider selection, startup ordering and console handover."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


class GraphicsHandoverTests(unittest.TestCase):
    def test_startup_and_console_contracts(self):
        root = Path(__file__).resolve().parents[1]
        kernel = root / "src/system/kernel"
        include = root / "src/system/include/pedigree/kernel"
        provider_header = (include / "graphics/GraphicsService.h").read_text()
        provider_start = provider_header.index("  struct GraphicsProvider {")
        provider_end = provider_header.index("  struct GraphicsParameters {", provider_start)
        graphics = (kernel / "graphics/GraphicsService.cc").read_text()
        graphics_start = graphics.index("GraphicsService::ProviderPair ")
        modules = (kernel / "linker/KernelElf.cc").read_text()
        modules_start = modules.index("bool KernelElf::moduleDependenciesSatisfiedLocked(")
        modules_end = modules.index("static int executeModuleThread(", modules_start)
        module_header = (include / "linker/KernelElf.h").read_text()
        status_start = module_header.index("  enum ModuleStatus ")
        status_end = module_header.index(" protected:", status_start)
        vga = (kernel / "machine/mach_pc/Vga.cc").read_text()
        vga_start = vga.index("void X86Vga::flush()")
        vga_end = vga.index("bool X86Vga::initialise()", vga_start)
        devfs = (root / "src/modules/subsys/posix/DevFs.cc").read_text()
        devfs_start = devfs.index("int FramebufferFile::command(")
        devfs_end = devfs.index("Tty0File::Tty0File(", devfs_start)
        with tempfile.TemporaryDirectory(prefix="pedigree-graphics-handover-") as temporary:
            work = Path(temporary)
            (work / "graphics-provider.inc").write_text(provider_header[provider_start:provider_end])
            (work / "module-status.inc").write_text(module_header[status_start:status_end])
            (work / "graphics-handover.inc").write_text(
                graphics[graphics_start:]
                + modules[modules_start:modules_end]
                + vga[vga_start:vga_end]
                + devfs[devfs_start:devfs_end]
            )
            binary = work / "handover"
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CXX", "c++")),
                    "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I", str(work),
                    str(root / "tests/fixtures/graphics-handover.cc"), "-o", str(binary),
                ], check=True, timeout=60,
            )
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
