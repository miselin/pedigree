/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "Font.h"
#include "pedigree/native/graphics/Graphics.h"

#include <cerrno>
#include <cmath>
#include <ft2build.h>
#include <iconv.h>
#include <setjmp.h>
#include <string.h>
#include <vector>

#include <pedigree/log.h>
#include FT_FREETYPE_H

#include <cairo/cairo-ft.h>
#include <cairo/cairo.h>

struct FontLibraries {
  iconv_t m_Iconv;
  cairo_t* m_Cairo;
  FT_Library m_FreeType;
  FT_Face m_Face;
};

Font::Font(cairo_t* pCairo, size_t requestedSize, const char* pFilename, bool bCache, size_t nWidth)
    : m_CellWidth(0), m_CellHeight(0), m_Baseline(requestedSize), m_ConversionCache() {
  m_FontLibraries = new FontLibraries();
  m_FontLibraries->m_Cairo = pCairo;

  // Fontconfig/Pango initialisation from a forked userspace process can inherit
  // a locked library mutex. Load the bundled face directly instead.
  const char* fontPath = (pFilename && strstr(pFilename, "Bold"))
                             ? "/usr/share/fonts/DejaVuSansMono-Bold.ttf"
                             : "/usr/share/fonts/DejaVuSansMono.ttf";
  m_FontLibraries->m_FreeType = nullptr;
  m_FontLibraries->m_Face = nullptr;
  if (FT_Init_FreeType(&m_FontLibraries->m_FreeType) != 0 ||
      FT_New_Face(m_FontLibraries->m_FreeType, fontPath, 0, &m_FontLibraries->m_Face) != 0 ||
      FT_Set_Pixel_Sizes(m_FontLibraries->m_Face, 0, requestedSize) != 0) {
    pedigree_log(LOG_ALERT, "TUI: could not load DejaVu Sans Mono");
    m_CellWidth = 8;
    m_CellHeight = requestedSize;
    m_Baseline = requestedSize;
  } else {
    m_CellWidth = (m_FontLibraries->m_Face->size->metrics.max_advance + 32) >> 6;
    m_CellHeight = (m_FontLibraries->m_Face->size->metrics.height + 32) >> 6;
    m_Baseline = (m_FontLibraries->m_Face->size->metrics.ascender + 32) >> 6;
  }

  pedigree_log(LOG_INFO, "metrics: %zux%zu", m_CellWidth, m_CellHeight);

  /// \todo UTF-32 endianness
  m_FontLibraries->m_Iconv = iconv_open("UTF-8", "UTF-32LE");
  if (m_FontLibraries->m_Iconv == (iconv_t)-1) {
    pedigree_log(LOG_WARNING, "TUI: Font instance couldn't create iconv (%s)", strerror(errno));
  }

  for (uint32_t c = 32; c < 127; ++c) {
    precache(c);
  }
}

Font::~Font() {
  iconv_close(m_FontLibraries->m_Iconv);

  // Destroy our precached glyphs.
  for (std::map<uint32_t, char*>::iterator it = m_ConversionCache.begin();
       it != m_ConversionCache.end(); ++it) {
    delete[] it->second;
  }

  if (m_FontLibraries->m_Face)
    FT_Done_Face(m_FontLibraries->m_Face);
  if (m_FontLibraries->m_FreeType)
    FT_Done_FreeType(m_FontLibraries->m_FreeType);
  delete m_FontLibraries;
}

size_t Font::render(PedigreeGraphics::Framebuffer* pFb, uint32_t c, size_t x, size_t y, uint32_t f,
                    uint32_t b, bool bBack, bool bBold, bool bItalic, bool bUnderline) {
  (void) pFb;
  // Cache the character, if not already.
  const char* convertOut = precache(c);
  if (!convertOut) {
    pedigree_log(LOG_WARNING, "TUI: Character '%x' was not able to be precached?", c);
    return 0;
  }

  // Perform the render.
  return render(convertOut, x, y, f, b, bBack, bBold, bItalic, bUnderline);
}

size_t Font::render(const char* s, size_t x, size_t y, uint32_t f, uint32_t b, bool bBack,
                    bool bBold, bool bItalic, bool bUnderline) {
  if (!m_FontLibraries->m_Face || !s || !*s)
    return 0;

  uint32_t codepoint = static_cast<unsigned char>(s[0]);
  size_t sequenceLength = 1;
  if ((codepoint & 0xE0) == 0xC0) {
    sequenceLength = 2;
    codepoint = ((codepoint & 0x1F) << 6) | (static_cast<unsigned char>(s[1]) & 0x3F);
  } else if ((codepoint & 0xF0) == 0xE0) {
    sequenceLength = 3;
    codepoint = ((codepoint & 0x0F) << 12) | ((static_cast<unsigned char>(s[1]) & 0x3F) << 6) |
                (static_cast<unsigned char>(s[2]) & 0x3F);
  } else if ((codepoint & 0xF8) == 0xF0) {
    sequenceLength = 4;
    codepoint = ((codepoint & 0x07) << 18) | ((static_cast<unsigned char>(s[1]) & 0x3F) << 12) |
                ((static_cast<unsigned char>(s[2]) & 0x3F) << 6) |
                (static_cast<unsigned char>(s[3]) & 0x3F);
  }

  if (s[sequenceLength]) {
    size_t rendered = 0;
    const char* p = s;
    while (*p) {
      uint32_t first = static_cast<unsigned char>(*p);
      size_t length = 1;
      if ((first & 0xE0) == 0xC0)
        length = 2;
      else if ((first & 0xF0) == 0xE0)
        length = 3;
      else if ((first & 0xF8) == 0xF0)
        length = 4;

      char glyph[5] = {0, 0, 0, 0, 0};
      for (size_t i = 0; i < length; ++i)
        glyph[i] = p[i];
      rendered += render(glyph, x + rendered, y, f, b, bBack, bBold, bItalic, bUnderline);
      p += length;
    }
    return rendered;
  }

  if (FT_Load_Char(m_FontLibraries->m_Face, codepoint, FT_LOAD_RENDER) != 0)
    return 0;

  FT_GlyphSlot glyph = m_FontLibraries->m_Face->glyph;
  size_t width = (glyph->advance.x + 32) >> 6;

  cairo_save(m_FontLibraries->m_Cairo);

  if (bBack) {
    cairo_set_operator(m_FontLibraries->m_Cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(m_FontLibraries->m_Cairo, ((b >> 16) & 0xFF) / 256.0,
                          ((b >> 8) & 0xFF) / 256.0, ((b) & 0xFF) / 256.0, 0.8);

    size_t fillW = m_CellWidth > width ? m_CellWidth : width;
    size_t fillH = m_CellHeight;
    cairo_rectangle(m_FontLibraries->m_Cairo, x, y, fillW, fillH);
    cairo_fill(m_FontLibraries->m_Cairo);
  }

  cairo_set_operator(m_FontLibraries->m_Cairo, CAIRO_OPERATOR_OVER);
  cairo_set_source_rgba(m_FontLibraries->m_Cairo, ((f >> 16) & 0xFF) / 256.0,
                        ((f >> 8) & 0xFF) / 256.0, ((f) & 0xFF) / 256.0, 1.0);

  int bitmapPitch = glyph->bitmap.pitch;
  size_t bitmapStride = static_cast<size_t>(bitmapPitch < 0 ? -bitmapPitch : bitmapPitch);
  if (glyph->bitmap.width && glyph->bitmap.rows) {
    size_t cairoStride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, glyph->bitmap.width);
    std::vector<unsigned char> bitmap(cairoStride * glyph->bitmap.rows, 0);
    for (unsigned int row = 0; row < glyph->bitmap.rows; ++row) {
      size_t sourceRow = bitmapPitch < 0 ? glyph->bitmap.rows - row - 1 : row;
      memcpy(&bitmap[row * cairoStride], glyph->bitmap.buffer + sourceRow * bitmapStride,
             glyph->bitmap.width);
    }

    cairo_surface_t* mask = cairo_image_surface_create_for_data(
        bitmap.data(), CAIRO_FORMAT_A8, glyph->bitmap.width, glyph->bitmap.rows, cairoStride);
    cairo_set_source_rgba(m_FontLibraries->m_Cairo, ((f >> 16) & 0xFF) / 256.0,
                          ((f >> 8) & 0xFF) / 256.0, (f & 0xFF) / 256.0, 1.0);
    cairo_mask_surface(m_FontLibraries->m_Cairo, mask,
                       x + glyph->bitmap_left, y + m_Baseline - glyph->bitmap_top);
    cairo_surface_destroy(mask);
  }

  if (bUnderline) {
    cairo_move_to(m_FontLibraries->m_Cairo, x, y + m_Baseline + 1);
    cairo_line_to(m_FontLibraries->m_Cairo, x + width, y + m_Baseline + 1);
    cairo_stroke(m_FontLibraries->m_Cairo);
  }

  cairo_restore(m_FontLibraries->m_Cairo);

  return width;
}

const char* Font::precache(uint32_t c) {
  if (m_FontLibraries->m_Iconv == (iconv_t)-1) {
    pedigree_log(LOG_WARNING, "TUI: Font instance with bad iconv.");
    return 0;
  }

  // Let's try and skip any actual conversion.
  std::map<uint32_t, char*>::iterator it = m_ConversionCache.find(c);
  if (it == m_ConversionCache.end()) {
    // Reset iconv conversion state.
    iconv(m_FontLibraries->m_Iconv, 0, 0, 0, 0);

    // Convert UTF-32 input character to UTF-8 for Cairo rendering.
    uint32_t utf32[] = {c, 0};
    char* utf32_c = (char*)utf32;
    char* out = new char[100];
    char* out_c = out;
    size_t utf32_len = 8;
    size_t out_len = 100;
    size_t res = iconv(m_FontLibraries->m_Iconv, &utf32_c, &utf32_len, &out_c, &out_len);

    if (res == ((size_t)-1)) {
      pedigree_log(LOG_WARNING, "TUI: Font::render couldn't convert input UTF-32 %x", c);
      delete[] out;
    } else {
      m_ConversionCache[c] = out;
      return out;
    }
  } else {
    return it->second;
  }

  return 0;
}

void Font::updateCairo(cairo_t* pCairo) {
  m_FontLibraries->m_Cairo = pCairo;
}
