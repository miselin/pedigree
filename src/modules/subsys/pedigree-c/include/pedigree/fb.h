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

#ifndef PEDIGREE_FB_H
#define PEDIGREE_FB_H

#include <stddef.h>
#include <stdint.h>

/// Commands that can be performed against the framebuffer device.
enum FbCommands {
  PEDIGREE_FB_CMD_MIN = 0x5000,
  PEDIGREE_FB_SETMODE = 0x5000,
  PEDIGREE_FB_GETMODE = 0x5001,
  PEDIGREE_FB_REDRAW = 0x5002,
  PEDIGREE_FB_CMD_MAX = 0x5002,
};

// Numeric values are part of the framebuffer ioctl ABI.
enum FbPixelFormat {
  PEDIGREE_FB_FORMAT_ARGB32 = 0,
  PEDIGREE_FB_FORMAT_RGBA32 = 1,
  PEDIGREE_FB_FORMAT_RGB32 = 2,
  PEDIGREE_FB_FORMAT_BGR32 = 3,
  PEDIGREE_FB_FORMAT_RGB24 = 4,
  PEDIGREE_FB_FORMAT_BGR24 = 5,
  PEDIGREE_FB_FORMAT_ARGB16 = 6,
  PEDIGREE_FB_FORMAT_RGB565 = 7,
  PEDIGREE_FB_FORMAT_RGB555 = 8,
  PEDIGREE_FB_FORMAT_INDEXED8 = 9,
  PEDIGREE_FB_FORMAT_RGB332 = 10,
};

/// All zeroes = 'revert to text mode'.
typedef struct {
  size_t width;
  size_t height;
  size_t depth;
} pedigree_fb_modeset;

typedef struct {
  size_t width;
  size_t height;
  size_t depth;
  size_t bytes_per_pixel;
  uint32_t format;
  // Bytes between the starts of consecutive scanlines. GOP framebuffers may
  // include padding after the visible pixels in each scanline.
  size_t bytes_per_line;
} pedigree_fb_mode;

typedef struct {
  size_t x;
  size_t y;
  size_t w;
  size_t h;
} pedigree_fb_rect;

#endif
