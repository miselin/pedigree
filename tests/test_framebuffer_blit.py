"""Run production framebuffer blits and splash scrolling over packed/padded buffers."""

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path


class FramebufferBlitTests(unittest.TestCase):
    def test_stride_contracts(self):
        repository = Path(__file__).resolve().parents[1]
        source = (repository / "src/system/kernel/machine/Framebuffer.cc").read_text()
        # Isolate the real method from unrelated kernel allocation/graphics services.
        method = source.split("void Framebuffer::swBlit(", 1)[1].split(
            "\nvoid Framebuffer::swRect(", 1
        )[0]
        operations = ""
        for name, following in [("redraw", "blit"), ("blit", "draw"),
                                ("copy", "line"), ("swCopy", "swLine")]:
            start = source.index(f"void Framebuffer::{name}(")
            end = source.index(f"\nvoid Framebuffer::{following}(", start)
            operations += source[start:end]
        splash = (repository / "src/modules/system/splash/main.cc").read_text()
        start = splash.index("static bool printChar(char c)")
        end = splash.index("static void printStringAt(", start)
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#define UNLIKELY(x) (x)
#define StringLength std::strlen
#define ERROR(...)
#define FONT_WIDTH 8
#define FONT_HEIGHT 16
uintptr_t scanoutBegin = 0, scanoutEnd = 0;
unsigned scanoutReads = 0;
void *MemoryCopy(void *dest, const void *src, size_t size) {
  uintptr_t address = reinterpret_cast<uintptr_t>(src);
  if (address >= scanoutBegin && address < scanoutEnd) ++scanoutReads;
  return std::memmove(dest, src, size);
}
void *adjust_pointer(void *p, size_t n) { return static_cast<char *>(p) + n; }
namespace Graphics {
enum PixelFormat { Bits32_Rgb };
struct Buffer {
  uintptr_t base; size_t width, height, bytesPerPixel;
  PixelFormat format = Bits32_Rgb;
};
}
struct Framebuffer {
  uintptr_t m_FramebufferBase;
  size_t m_nWidth, m_nHeight, m_nBytesPerLine, m_nBytesPerPixel;
  Graphics::PixelFormat m_PixelFormat = Graphics::Bits32_Rgb;
  Framebuffer *m_pParent = nullptr;
  size_t m_XPos = 0, m_YPos = 0;
  unsigned presents = 0;
  size_t redrawY = 0, redrawHeight = 0;
  void swBlit(Graphics::Buffer *, size_t, size_t, size_t, size_t, size_t, size_t);
  void swCopy(size_t, size_t, size_t, size_t, size_t, size_t);
  void blit(Graphics::Buffer *, size_t, size_t, size_t, size_t, size_t, size_t, bool = true);
  void copy(size_t, size_t, size_t, size_t, size_t, size_t, bool = true);
  void redraw(size_t, size_t, size_t, size_t, bool = false);
  Graphics::PixelFormat getFormat() const { return m_PixelFormat; }
  Framebuffer *getParent() const { return m_pParent; }
  void setParent(Framebuffer *parent) { m_pParent = parent; }
  Graphics::Buffer bufferFromSelf() {
    return {m_FramebufferBase, m_nWidth, m_nHeight, m_nBytesPerPixel};
  }
  void draw(Graphics::Buffer *buffer, size_t sx, size_t sy, size_t dx, size_t dy,
            size_t w, size_t h, bool lowest) {
    blit(buffer, sx, sy, dx, dy, w, h, lowest);
  }
  void rect(size_t x, size_t y, size_t w, size_t h, uint32_t colour,
            Graphics::PixelFormat format) {
    if (m_pParent) m_pParent->rect(x, y, w, h, colour, format);
    for (size_t row = y; row < y + h; ++row)
      for (size_t col = x; col < x + w; ++col)
        reinterpret_cast<uint32_t *>(m_FramebufferBase)[row * m_nBytesPerLine / 4 + col] = colour;
  }
  void hwRedraw(size_t, size_t y, size_t, size_t h) {
    ++presents;
    redrawY = y;
    redrawHeight = h;
  }
};
struct HugeStaticString : std::string {
  void truncate(size_t size) { resize(size); }
};
namespace BootIO {
enum Colour { LightGrey, Orange, Red, DarkGrey, Black };
}
struct { void write(const HugeStaticString &, BootIO::Colour, BootIO::Colour) {} } bootIO;
struct Mutex {
  bool held = false;
  bool tryAcquire() { if (held) return false; held = true; return true; }
  void release() { assert(held); held = false; }
};
struct TerminationDeferral { TerminationDeferral() {} ~TerminationDeferral() {} };
struct LogCord {
  std::vector<std::string> parts;
  struct Iterator {
    std::vector<std::string>::const_iterator it;
    Iterator &operator++() { ++it; return *this; }
    bool operator!=(Iterator other) const { return it != other.it; }
    const char *ptr() const { return it->c_str(); }
    size_t length() const { return it->size(); }
  };
  Iterator segbegin() const { return {parts.begin()}; }
  Iterator segend() const { return {parts.end()}; }
};
static Framebuffer *g_pFramebuffer;
static Graphics::Buffer *g_pFont;
static size_t g_LogBoxX, g_LogBoxY, g_LogX, g_LogY, g_LogW, g_LogH;
static uint32_t g_BackgroundColour = 0;
static Graphics::PixelFormat g_ColorFormat = Graphics::Bits32_Rgb;
static bool g_NoGraphics = false;
static Mutex g_PrintLock;
'''
        harness += "void Framebuffer::swBlit(" + method + operations + splash[start:end]
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

  std::vector<uint32_t> font(FONT_WIDTH * FONT_HEIGHT * 256);
  for (size_t i = 0; i < font.size(); ++i) font[i] = i / (FONT_WIDTH * FONT_HEIGHT);
  Graphics::Buffer fontBuffer{reinterpret_cast<uintptr_t>(font.data()), FONT_WIDTH,
                              FONT_HEIGHT * 256, 4};
  g_pFont = &fontBuffer;
  // Full-width and inset log areas, each with packed and padded scanout rows.
  for (size_t inset : {0, 8}) for (size_t padding : {0, 4}) {
    const size_t width = 24, height = 64, pitch = width + padding;
    const uint32_t sentinel = 0xdeadbeef;
    std::vector<uint32_t> scanout(pitch * height, sentinel), backing(width * height, sentinel);
    Framebuffer parent{reinterpret_cast<uintptr_t>(scanout.data()), width, height, pitch * 4, 4};
    Framebuffer child{reinterpret_cast<uintptr_t>(backing.data()), width, height, width * 4, 4};
    child.setParent(&parent);
    g_pFramebuffer = &child;
    g_LogBoxX = inset;
    g_LogBoxY = inset ? FONT_HEIGHT : 0;
    g_LogW = width - g_LogBoxX;
    g_LogH = height - g_LogBoxY;
    g_LogX = g_LogY = 0;
    child.rect(g_LogBoxX, g_LogBoxY, g_LogW, g_LogH, 0, g_ColorFormat);
    scanoutBegin = reinterpret_cast<uintptr_t>(scanout.data());
    scanoutEnd = scanoutBegin + scanout.size() * sizeof(uint32_t);
    scanoutReads = 0;
    printString(LogCord{{"A\nB\n", "C\nD\n"}});
    assert(child.getParent() == &parent);
    assert(!g_PrintLock.held);
    assert(parent.presents == 1);
    assert(scanoutReads == 0);
    assert(parent.redrawY == g_LogBoxY && parent.redrawHeight == g_LogH);
    const size_t rows = g_LogH / FONT_HEIGHT;
    for (size_t y = 0; y < height; ++y) for (size_t x = 0; x < pitch; ++x) {
      uint32_t expected = sentinel;
      if (x >= g_LogBoxX && x < g_LogBoxX + g_LogW && y >= g_LogBoxY) {
        const size_t row = (y - g_LogBoxY) / FONT_HEIGHT;
        expected = row + 1 == rows || x >= g_LogBoxX + FONT_WIDTH ? 0 : 'F' - rows + row;
      }
      assert(scanout[y * pitch + x] == expected);
    }
    // An unterminated segment must become visible without publishing other rows.
    printString(LogCord{{"E"}});
    assert(parent.presents == 2);
    assert(parent.redrawY == g_LogBoxY + (rows - 1) * FONT_HEIGHT);
    assert(parent.redrawHeight == FONT_HEIGHT);
    assert(scanout[(g_LogBoxY + (rows - 1) * FONT_HEIGHT) * pitch + g_LogBoxX] == 'E');
    // Wrapped characters across cord segments still publish only once.
    printString(LogCord{{std::string(g_LogW / FONT_WIDTH - 1, 'F'), "G"}});
    assert(parent.presents == 3);
    assert(parent.redrawHeight == g_LogH);
    assert(scanout[(g_LogBoxY + (rows - 1) * FONT_HEIGHT) * pitch + g_LogBoxX] == 'G');
    assert(scanoutReads == 0);
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
            result = subprocess.run([str(binary)], check=False, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
