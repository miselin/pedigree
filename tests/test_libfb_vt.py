# SPDX-License-Identifier: ISC
"""Run libfb virtual-terminal handover with native POSIX signals."""

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path


class LibfbVtTests(unittest.TestCase):
    def test_process_handover(self):
        root = Path(__file__).resolve().parents[1]
        library = root / "src/user/libraries/libfb"
        with tempfile.TemporaryDirectory(prefix="pedigree-libfb-vt-") as temporary:
            work = Path(temporary)
            (work / "cairo").mkdir()
            (work / "sys").mkdir()
            (work / "cairo/cairo.h").write_text(
                "#pragma once\n"
                "enum cairo_format_t { CAIRO_FORMAT_ARGB32, CAIRO_FORMAT_RGB16_565 };\n"
                "inline int cairo_format_stride_for_width(cairo_format_t, int width) {\n"
                "  return width * 4;\n}\n"
            )
            (work / "sys/vt.h").write_text(
                "#pragma once\n#define VT_AUTO 0\n#define VT_PROCESS 1\n"
                "#define VT_GETMODE 0x5601\n#define VT_SETMODE 0x5602\n"
                "#define VT_GETSTATE 0x5603\n"
                "#define VT_RELDISP 0x5605\n#define VT_ACKACQ 2\n"
                "struct vt_mode { char mode, waitv; short relsig, acqsig, frsig; };\n"
                "struct vt_stat { unsigned short v_active, v_signal, v_state; };\n"
            )
            (work / "sys/kd.h").write_text(
                "#pragma once\n#define KDGETMODE 0x4b3b\n#define KDSETMODE 0x4b3a\n"
                "#define KD_TEXT 0\n#define KD_GRAPHICS 1\n"
            )
            (work / "libfb-implementation.inc").write_text((library / "src/fb.cc").read_text())
            binary = work / "libfb-vt"
            subprocess.run(
                [
                    *shlex.split(os.environ.get("CXX", "c++")),
                    "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I", str(work), "-I", str(library / "include"),
                    "-I", str(root / "src/modules/subsys/pedigree-c/include"),
                    str(root / "tests/fixtures/libfb-vt.cc"), "-o", str(binary),
                ], check=True, timeout=60,
            )
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
