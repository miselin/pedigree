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

#ifndef FONT_H
#define FONT_H

#include "environment.h"
#include <stdint.h>
#include <stdlib.h>

#include <native/graphics/Graphics.h>

#include <map>

#include <cairo/cairo.h>

class Font
{
  public:
    Font(
        cairo_t *pCairo, size_t requestedSize, const char *pFilename,
        bool bCache, size_t nWidth);
    virtual ~Font();

    virtual size_t render(
        PedigreeGraphics::Framebuffer *pFb, uint32_t c, size_t x, size_t y,
        uint32_t f, uint32_t b, bool bBack = true, bool bBold = false,
        bool bItalic = false, bool bUnderline = false);

    virtual size_t render(
        const char *s, size_t x, size_t y, uint32_t f, uint32_t b,
        bool bBack = true, bool bBold = false, bool bItalic = false,
        bool bUnderline = false);

    size_t getWidth()
    {
        return m_CellWidth;
    }
    size_t getHeight()
    {
        return m_CellHeight;
    }
    size_t getBaseline()
    {
        return m_Baseline;
    }

    const char *precache(uint32_t c);

    void updateCairo(cairo_t *pCairo);

  private:
    Font(const Font &);
    Font &operator=(const Font &);

    size_t m_CellWidth;
    size_t m_CellHeight;
    size_t m_Baseline;

    std::map<uint32_t, char *> m_ConversionCache;

    // opaque pointer that allows the .cc files to refer to pango etc without
    // requiring clients to also see the headers
    struct FontLibraries *m_FontLibraries;
};

#endif
