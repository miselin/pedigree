/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_UEFI_FRAMEBUFFER_H
#define PEDIGREE_UEFI_FRAMEBUFFER_H

#include <stdint.h>

typedef struct efi_graphics_mode_info {
  uint32_t version;
  uint32_t width;
  uint32_t height;
  uint32_t pixel_format;
  uint32_t red_mask;
  uint32_t green_mask;
  uint32_t blue_mask;
  uint32_t reserved_mask;
  uint32_t pixels_per_scanline;
} efi_graphics_mode_info_t;

typedef struct efi_graphics_mode {
  uint32_t max_mode;
  uint32_t mode;
  efi_graphics_mode_info_t* info;
  uint64_t info_size;
  uint64_t framebuffer;
  uint64_t framebuffer_size;
} efi_graphics_mode_t;

typedef struct efi_graphics_output {
  void* query_mode;
  void* set_mode;
  void* blt;
  efi_graphics_mode_t* mode;
} efi_graphics_output_t;

typedef struct boot_framebuffer {
  uint64_t address;
  uint32_t width, height, pitch, bpp, format;
} boot_framebuffer_t;

static int decode_framebuffer(const efi_graphics_output_t* graphics, boot_framebuffer_t* output) {
  if (!graphics || !graphics->mode)
    return 0;
  const efi_graphics_mode_t* mode = graphics->mode;
  const efi_graphics_mode_info_t* info = mode->info;
  if (!info || mode->info_size < sizeof(*info) || !mode->max_mode || mode->mode >= mode->max_mode ||
      !mode->framebuffer || !info->width || !info->height ||
      info->pixels_per_scanline < info->width || info->pixels_per_scanline > UINT32_MAX / 4)
    return 0;

  uint32_t format = info->pixel_format;
  if (format == 2) {
    // Only layouts that can use the same 32-bit renderer as the standard modes.
    if (info->green_mask != 0x0000ff00 ||
        (info->reserved_mask && info->reserved_mask != 0xff000000))
      return 0;
    if (info->red_mask == 0x000000ff && info->blue_mask == 0x00ff0000)
      format = 0;
    else if (info->red_mask == 0x00ff0000 && info->blue_mask == 0x000000ff)
      format = 1;
    else
      return 0;
  }
  if (format > 1)
    return 0;

  const uint32_t pitch = info->pixels_per_scanline * 4;
  const uint64_t span = (uint64_t)pitch * info->height;
  if (span > mode->framebuffer_size || mode->framebuffer_size > UINT64_MAX - mode->framebuffer)
    return 0;
  *output = (boot_framebuffer_t){mode->framebuffer, info->width, info->height, pitch, 32, format};
  return 1;
}

#endif
