/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef MACHINE_FRAMEBUFFER_CONSOLE_H
#define MACHINE_FRAMEBUFFER_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

#include "ConsoleFont.h"

// Preserve the VGA cell interface used by BootIO, the debugger and TextIO.
class FramebufferConsole {
 public:
  static const size_t Columns = 80;
  static const size_t Rows = 25;
  static const size_t CellCount = Columns * Rows;

  bool initialise(void* pixels, size_t bytes, uint32_t width, uint32_t height, uint32_t pitch,
                  uint32_t format) {
    if (!pixels || (reinterpret_cast<uintptr_t>(pixels) & 3) || width < 640 || height < 400 ||
        static_cast<uint64_t>(width) * 4 > pitch || (pitch & 3) || format > 1 ||
        static_cast<uint64_t>(pitch) * height > bytes) {
      return false;
    }
    m_Pixels = static_cast<volatile uint32_t*>(pixels);
    m_Stride = pitch / 4;
    m_Format = format;
    m_Scale = width / 640 < height / 400 ? width / 640 : height / 400;
    m_Left = (width - 640 * m_Scale) / 2;
    m_Top = (height - 400 * m_Scale) / 2;
    m_FirstFrame = true;
    for (size_t y = 0; y < height; ++y) {
      for (size_t x = 0; x < width; ++x) {
        m_Pixels[y * m_Stride + x] = 0;
      }
    }
    return true;
  }

  uint16_t* cells() {
    return m_Cells;
  }

  const uint16_t* cells() const {
    return m_Cells;
  }

  void moveCursor(size_t x, size_t y) {
    m_Cursor = x < Columns && y < Rows ? y * Columns + x : CellCount;
  }

  void flush() {
    if (!m_Pixels) {
      return;
    }
    for (size_t i = 0; i < CellCount; ++i) {
      uint16_t cell = m_Cells[i];
      if (!m_FirstFrame && cell == m_Rendered[i] && i != m_Cursor && i != m_PreviousCursor) {
        continue;
      }
      const uint8_t* glyph = g_ConsoleFont[cell & 0xff];
      uint32_t foreground = colour((cell >> 8) & 15);
      uint32_t background = colour((cell >> 12) & 15);
      size_t left = m_Left + (i % Columns) * 8 * m_Scale;
      size_t top = m_Top + (i / Columns) * 16 * m_Scale;
      for (size_t y = 0; y < 16 * m_Scale; ++y) {
        uint8_t bits = glyph[y / (2 * m_Scale)];
        bool cursor = i == m_Cursor && y >= 14 * m_Scale;
        for (size_t x = 0; x < 8 * m_Scale; ++x) {
          bool set = (bits & (1U << (x / m_Scale))) != 0;
          m_Pixels[(top + y) * m_Stride + left + x] = (set != cursor) ? foreground : background;
        }
      }
      m_Rendered[i] = cell;
    }
    m_PreviousCursor = m_Cursor;
    m_FirstFrame = false;
  }

 private:
  uint32_t colour(size_t index) const {
    static const uint32_t palette[16] = {0x000000, 0x0000aa, 0x00aa00, 0x00aaaa, 0xaa0000, 0xaa00aa,
                                         0xaa5500, 0xaaaaaa, 0x555555, 0x5555ff, 0x55ff55, 0x55ffff,
                                         0xff5555, 0xff55ff, 0xffff55, 0xffffff};
    uint32_t value = palette[index];
    return m_Format == 1 ? value : ((value & 0xff) << 16) | (value & 0xff00) | (value >> 16);
  }

  volatile uint32_t* m_Pixels = nullptr;
  size_t m_Stride = 0;
  size_t m_Left = 0;
  size_t m_Top = 0;
  size_t m_Scale = 1;
  uint32_t m_Format = 0;
  uint16_t m_Cells[CellCount] = {};
  uint16_t m_Rendered[CellCount] = {};
  size_t m_Cursor = CellCount;
  size_t m_PreviousCursor = CellCount;
  bool m_FirstFrame = true;
};

#endif
