/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include <assert.h>
#include <stdio.h>
#include <vector>

#include "../../src/system/kernel/machine/mach_pc/FramebufferConsole.h"

int main() {
  FramebufferConsole console;
  assert(console.cells());
  console.cells()[0] = 0x04db;
  console.flush();  // Serial-only boots retain usable cells without pixel memory.

  const size_t width = 648, height = 402, stride = 656, guard = 8;
  const uint32_t sentinel = 0x12345678;
  std::vector<uint32_t> memory(stride * height + guard * 2, sentinel);
  uint32_t* pixels = memory.data() + guard;
  const size_t bytes = stride * height * 4;
  assert(!console.initialise(nullptr, bytes, width, height, stride * 4, 1));
  assert(!console.initialise(pixels, bytes, 639, height, stride * 4, 1));
  assert(!console.initialise(pixels, bytes, width, 399, stride * 4, 1));
  assert(!console.initialise(pixels, bytes - 1, width, height, stride * 4, 1));
  assert(!console.initialise(pixels, bytes, width, height, width * 4 - 1, 1));
  assert(!console.initialise(pixels, bytes, width, height, stride * 4, 3));
  assert(!console.initialise(pixels + 1, bytes, UINT32_MAX, height, stride * 4, 1));

  assert(console.initialise(pixels, bytes, width, height, stride * 4, 1));
  console.flush();
  const size_t top = stride, left = 4;
  assert(pixels[0] == 0);
  assert(pixels[top + left] == 0x00aa0000);  // Red, BGR byte order.
  assert(pixels[top + left + 7] == 0x00aa0000);
  assert(pixels[top + left + 8] == 0);
  console.cells()[0] = 0x1420;  // Blank red-on-blue cell.
  console.moveCursor(0, 0);
  console.flush();
  assert(pixels[top + left] == 0x000000aa);
  assert(pixels[top + left + 14 * stride] == 0x00aa0000);
  console.cells()[1] = 0x0f20;
  console.moveCursor(1, 0);
  console.flush();
  assert(pixels[top + left + 14 * stride] == 0x000000aa);
  assert(pixels[top + left + 8 + 14 * stride] == 0x00ffffff);
  console.moveCursor(80, 25);
  console.flush();
  assert(pixels[top + left + 8 + 14 * stride] == 0);

  for (size_t i = 0; i < guard; ++i) {
    assert(memory[i] == sentinel);
    assert(memory[memory.size() - 1 - i] == sentinel);
  }
  for (size_t y = 0; y < height; ++y) {
    for (size_t x = width; x < stride; ++x) {
      assert(pixels[y * stride + x] == sentinel);
    }
  }

  std::vector<uint32_t> large(1280 * 800);
  assert(console.initialise(large.data(), large.size() * 4, 1280, 800, 1280 * 4, 0));
  console.cells()[0] = 0x04db;
  console.cells()[FramebufferConsole::CellCount - 1] = 0x02db;
  console.flush();
  assert(large[0] == 0x000000aa);  // Red, RGB byte order, at integer scale 2.
  assert(large[15 + 31 * 1280] == 0x000000aa);
  assert(large[16] == 0);
  assert(large.back() == 0x0000aa00);
  console.cells()[0] = 0x0020;
  console.flush();
  assert(large[0] == 0);
  puts("Framebuffer console contracts passed");
}
