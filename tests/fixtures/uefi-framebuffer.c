/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include <assert.h>
#include <stdio.h>

#include "../../src/system/boot/uefi/framebuffer.h"

int main(void) {
  efi_graphics_mode_info_t info = {.width = 800, .height = 600, .pixels_per_scanline = 832};
  efi_graphics_mode_t mode = {.max_mode = 1,
                              .info = &info,
                              .info_size = sizeof(info),
                              .framebuffer = 0xe0000000,
                              .framebuffer_size = 832 * 4 * 600};
  efi_graphics_output_t graphics = {.mode = &mode};
  boot_framebuffer_t result = {0};
  assert(decode_framebuffer(&graphics, &result));
  assert(result.address == mode.framebuffer && result.width == 800 && result.height == 600 &&
         result.pitch == 3328 && result.bpp == 32 && result.format == 0);
  info.pixel_format = 1;
  assert(decode_framebuffer(&graphics, &result) && result.format == 1);
  info.pixel_format = 2;
  info.red_mask = 0xff;
  info.green_mask = 0xff00;
  info.blue_mask = 0xff0000;
  info.reserved_mask = 0xff000000;
  assert(decode_framebuffer(&graphics, &result) && result.format == 0);
  info.red_mask = 0xff0000;
  info.blue_mask = 0xff;
  info.reserved_mask = 0;
  assert(decode_framebuffer(&graphics, &result) && result.format == 1);
  info.reserved_mask = 0x1;
  assert(!decode_framebuffer(&graphics, &result));
  info.reserved_mask = 0;
  info.red_mask = 0xf800;
  assert(!decode_framebuffer(&graphics, &result));
  info.pixel_format = 3;
  assert(!decode_framebuffer(&graphics, &result));
  info.pixel_format = 99;
  assert(!decode_framebuffer(&graphics, &result));
  info.pixel_format = 0;
  --mode.framebuffer_size;
  assert(!decode_framebuffer(&graphics, &result));
  ++mode.framebuffer_size;
  mode.framebuffer = UINT64_MAX - mode.framebuffer_size + 1;
  assert(!decode_framebuffer(&graphics, &result));
  mode.framebuffer = 0;
  assert(!decode_framebuffer(&graphics, &result));
  mode.framebuffer = 0xe0000000;
  info.pixels_per_scanline = UINT32_MAX / 4 + 1;
  assert(!decode_framebuffer(&graphics, &result));
  info.pixels_per_scanline = 799;
  assert(!decode_framebuffer(&graphics, &result));
  info.pixels_per_scanline = 832;
  info.height = 0;
  assert(!decode_framebuffer(&graphics, &result));
  info.height = 600;
  mode.info_size = sizeof(info) - 1;
  assert(!decode_framebuffer(&graphics, &result));
  mode.info_size = sizeof(info);
  mode.mode = mode.max_mode;
  assert(!decode_framebuffer(&graphics, &result));
  mode.mode = 0;
  mode.info = 0;
  assert(!decode_framebuffer(&graphics, &result));
  graphics.mode = 0;
  assert(!decode_framebuffer(&graphics, &result));
  assert(!decode_framebuffer(0, &result));
  puts("UEFI framebuffer contracts passed");
}
