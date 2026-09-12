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
  uint64_t (*query_mode)(struct efi_graphics_output*, uint32_t, uint64_t*,
                         efi_graphics_mode_info_t**);
  uint64_t (*set_mode)(struct efi_graphics_output*, uint32_t);
  void* blt;
  efi_graphics_mode_t* mode;
} efi_graphics_output_t;

typedef struct boot_framebuffer {
  uint64_t address;
  uint32_t width, height, pitch, bpp, format;
} boot_framebuffer_t;

typedef uint64_t (*efi_free_pool_t)(void*);

typedef struct efi_edid_active {
  uint32_t size;
  uint8_t* bytes;
} efi_edid_active_t;

static int framebuffer_format(const efi_graphics_mode_info_t* info, uint32_t* output) {
  if (!info || !info->width || !info->height || info->pixels_per_scanline < info->width ||
      info->pixels_per_scanline > UINT32_MAX / 4)
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
  *output = format;
  return 1;
}

static int decode_framebuffer(const efi_graphics_output_t* graphics, boot_framebuffer_t* output) {
  if (!graphics || !graphics->mode)
    return 0;
  const efi_graphics_mode_t* mode = graphics->mode;
  const efi_graphics_mode_info_t* info = mode->info;
  uint32_t format;
  if (!info || mode->info_size < sizeof(*info) || !mode->max_mode || mode->mode >= mode->max_mode ||
      !mode->framebuffer || !framebuffer_format(info, &format))
    return 0;

  const uint32_t pitch = info->pixels_per_scanline * 4;
  const uint64_t span = (uint64_t)pitch * info->height;
  if (span > mode->framebuffer_size || mode->framebuffer_size > UINT64_MAX - mode->framebuffer)
    return 0;
  *output = (boot_framebuffer_t){mode->framebuffer, info->width, info->height, pitch, 32, format};
  return 1;
}

static int preferred_framebuffer_size(const efi_edid_active_t* edid, uint32_t* width,
                                      uint32_t* height) {
  static const uint8_t header[] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
  if (!edid || !edid->bytes || edid->size < 128)
    return 0;
  const uint8_t* bytes = edid->bytes;
  uint8_t checksum = 0;
  for (unsigned i = 0; i < sizeof(header); ++i)
    if (bytes[i] != header[i])
      return 0;
  for (unsigned i = 0; i < 128; ++i)
    checksum += bytes[i];
  if (checksum || bytes[18] != 1 || !(bytes[24] & 2))
    return 0;
  const uint8_t* timing = bytes + 54;
  if (!(timing[0] | timing[1]) || (timing[17] & 0x80))
    return 0;
  const uint32_t x = timing[2] | ((uint32_t)(timing[4] & 0xf0) << 4);
  const uint32_t y = timing[5] | ((uint32_t)(timing[7] & 0xf0) << 4);
  if (!x || !y)
    return 0;
  *width = x;
  *height = y;
  return 1;
}

static int select_framebuffer_mode(efi_graphics_output_t* graphics, efi_free_pool_t free_pool,
                                   const efi_edid_active_t* edid, boot_framebuffer_t* output) {
  if (!graphics || !graphics->mode || !graphics->query_mode || !graphics->set_mode || !free_pool)
    return 0;
  uint32_t preferred_width = 0, preferred_height = 0;
  preferred_framebuffer_size(edid, &preferred_width, &preferred_height);
  const uint32_t count = graphics->mode->max_mode;
  int ceiling_preferred = 2;
  uint64_t ceiling_area = UINT64_MAX;
  uint32_t ceiling_mode = UINT32_MAX;
  for (uint32_t attempt = 0; attempt < count; ++attempt) {
    int best_preferred = -1;
    uint64_t best_area = 0;
    uint32_t best_mode = 0, best_width = 0, best_height = 0;
    for (uint32_t candidate = 0; candidate < count; ++candidate) {
      efi_graphics_mode_info_t* info = 0;
      uint64_t size = 0;
      uint32_t format;
      const uint64_t status = graphics->query_mode(graphics, candidate, &size, &info);
      if (!status && info && size >= sizeof(*info) && framebuffer_format(info, &format)) {
        const int preferred = info->width == preferred_width && info->height == preferred_height;
        const uint64_t area = (uint64_t)info->width * info->height;
        const int below_ceiling =
            preferred < ceiling_preferred ||
            (preferred == ceiling_preferred &&
             (area < ceiling_area || (area == ceiling_area && candidate < ceiling_mode)));
        if (below_ceiling &&
            (preferred > best_preferred ||
             (preferred == best_preferred &&
              (area > best_area || (area == best_area && candidate > best_mode))))) {
          best_preferred = preferred;
          best_area = area;
          best_mode = candidate;
          best_width = info->width;
          best_height = info->height;
        }
      }
      if (info)
        free_pool(info);
    }
    if (best_preferred < 0)
      return 0;
    // Apply even an apparently current mode: GRUB may have changed the hardware
    // through another interface while this GOP retained its previous metadata.
    if (!graphics->set_mode(graphics, best_mode) && graphics->mode &&
        graphics->mode->mode == best_mode && decode_framebuffer(graphics, output) &&
        output->width == best_width && output->height == best_height)
      return 1;
    // Retry lower-ranked candidates without retaining firmware-owned QueryMode buffers.
    ceiling_preferred = best_preferred;
    ceiling_area = best_area;
    ceiling_mode = best_mode;
  }
  return 0;
}

#endif
