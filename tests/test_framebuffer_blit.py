"""Run the production software blit with packed and padded pixel buffers."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


class FramebufferBlitTests(unittest.TestCase):
    def test_stride_contracts(self):
        repository = Path(__file__).resolve().parents[1]
        source = (repository / "src/system/kernel/machine/Framebuffer.cc").read_text()
        # Isolate the real method from unrelated kernel allocation/graphics services.
        method = source.split("void Framebuffer::swBlit(", 1)[1].split(
            "\nvoid Framebuffer::swRect(", 1
        )[0]
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#define UNLIKELY(x) (x)
#define MemoryCopy std::memcpy
void *adjust_pointer(void *p, size_t n) { return static_cast<char *>(p) + n; }
namespace Graphics {
struct Buffer { uintptr_t base; size_t width, height, bytesPerPixel; };
}
struct Framebuffer {
  uintptr_t m_FramebufferBase;
  size_t m_nWidth, m_nHeight, m_nBytesPerLine, m_nBytesPerPixel;
  void swBlit(Graphics::Buffer *, size_t, size_t, size_t, size_t, size_t, size_t);
};
'''
        harness += "void Framebuffer::swBlit(" + method
        harness += r'''
int main() {
  // T420 native padding, scaled-mode padding, packed fast path, wider source,
  // and partial-width rows. Offsets exercise both nonzero source/destination Y.
  for (unsigned kind = 0; kind < 5; ++kind) {
    const size_t width = kind == 0 ? 1366 : 1024;
    const size_t sourceWidth = kind == 3 ? width + 16 : width;
    const size_t pitch = kind < 2 ? 5504 : width * 4;
    const size_t x = kind == 4 ? 3 : 0;
    const size_t copiedWidth = width - x;
    std::vector<uint32_t> src(sourceWidth * 4);
    for (size_t i = 0; i < src.size(); ++i) src[i] = 0x10000000 + i;
    std::vector<uint32_t> dst(pitch / 4 * 5, 0xdeadbeef);
    auto expected = dst;
    for (size_t y = 0; y < 3; ++y)
      for (size_t col = 0; col < copiedWidth; ++col)
        expected[(y + 2) * (pitch / 4) + x + col] = src[(y + 1) * sourceWidth + x + col];
    Graphics::Buffer buffer{reinterpret_cast<uintptr_t>(src.data()), sourceWidth, 4, 4};
    Framebuffer fb{reinterpret_cast<uintptr_t>(dst.data()), width, 5, pitch, 4};
    fb.swBlit(&buffer, x, 1, x, 2, copiedWidth, 3);
    assert(dst == expected);
  }
}
'''
        with tempfile.TemporaryDirectory(prefix="pedigree-blit-") as temporary:
            fixture = Path(temporary) / "blit.cc"
            binary = Path(temporary) / "blit"
            fixture.write_text(harness)
            compiler = shlex.split(os.environ.get("CXX", "c++"))
            subprocess.run(
                [*compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", str(fixture), "-o", str(binary)],
                check=True, capture_output=True, text=True,
            )
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
