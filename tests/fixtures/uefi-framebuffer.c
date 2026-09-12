/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/system/boot/uefi/framebuffer.h"

static efi_graphics_mode_info_t available[4], active;
static unsigned query_fail, short_info, set_fail, wrong_size, calls[4], allocations;

static uint64_t query_mode(efi_graphics_output_t* graphics, uint32_t number, uint64_t* size,
                           efi_graphics_mode_info_t** info) {
  assert(number < graphics->mode->max_mode);
  if (query_fail & (1U << number))
    return 1;
  *info = malloc(sizeof(**info));
  assert(*info);
  ++allocations;
  **info = available[number];
  *size = sizeof(**info) - !!(short_info & (1U << number));
  return 0;
}

static uint64_t free_pool(void* info) {
  assert(allocations);
  --allocations;
  free(info);
  return 0;
}

static uint64_t set_mode(efi_graphics_output_t* graphics, uint32_t number) {
  ++calls[number];
  if (set_fail & (1U << number))
    return 1;
  active = available[number];
  if (wrong_size & (1U << number))
    ++active.height;
  graphics->mode->mode = number;
  graphics->mode->info = &active;
  graphics->mode->info_size = sizeof(active);
  graphics->mode->framebuffer = 0xd0000000;
  graphics->mode->framebuffer_size = (uint64_t)active.pixels_per_scanline * active.height * 4;
  return 0;
}

static void checksum_edid(uint8_t* bytes) {
  bytes[127] = 0;
  for (unsigned i = 0; i < 127; ++i)
    bytes[127] -= bytes[i];
}

static void mode_selection_contracts(void) {
  uint8_t bytes[128] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
  bytes[18] = 1;
  bytes[19] = 3;
  bytes[24] = 2;
  bytes[54] = 1;
  bytes[56] = 1366 & 0xff;
  bytes[58] = (1366 >> 8) << 4;
  bytes[59] = 768 & 0xff;
  bytes[61] = (768 >> 8) << 4;
  checksum_edid(bytes);
  efi_edid_active_t edid = {sizeof(bytes), bytes};
  uint32_t width = 0, height = 0;
  assert(preferred_framebuffer_size(&edid, &width, &height));
  assert(width == 1366 && height == 768);
  edid.size = 127;
  assert(!preferred_framebuffer_size(&edid, &width, &height));
  edid.size = 128;
  ++bytes[127];
  assert(!preferred_framebuffer_size(&edid, &width, &height));
  checksum_edid(bytes);
  bytes[24] = 0;
  checksum_edid(bytes);
  assert(!preferred_framebuffer_size(&edid, &width, &height));
  bytes[24] = 2;
  bytes[71] = 0x80;
  checksum_edid(bytes);
  assert(!preferred_framebuffer_size(&edid, &width, &height));
  bytes[71] = 0;
  checksum_edid(bytes);

  available[0] =
      (efi_graphics_mode_info_t){.width = 800, .height = 600, .pixels_per_scanline = 832};
  available[1] =
      (efi_graphics_mode_info_t){.width = 1366, .height = 768, .pixels_per_scanline = 1408};
  available[2] =
      (efi_graphics_mode_info_t){.width = 1920, .height = 1080, .pixels_per_scanline = 1920};
  available[3] = (efi_graphics_mode_info_t){
      .width = 3840, .height = 2160, .pixel_format = 3, .pixels_per_scanline = 3840};
  efi_graphics_mode_t mode = {.max_mode = 4};
  efi_graphics_output_t graphics = {.query_mode = query_mode, .set_mode = set_mode, .mode = &mode};
  boot_framebuffer_t result;
  assert(select_framebuffer_mode(&graphics, free_pool, &edid, &result));
  assert(mode.mode == 1 && calls[1] == 1 && !calls[2] && !calls[3]);
  assert(result.width == 1366 && result.height == 768 && result.pitch == 1408 * 4 &&
         result.address == 0xd0000000 && !allocations);

  // Even the reported current mode must be explicitly applied before handoff.
  active = available[0];
  assert(select_framebuffer_mode(&graphics, free_pool, &edid, &result));
  assert(mode.mode == 1 && calls[1] == 2 && result.width == 1366);
  assert(select_framebuffer_mode(&graphics, free_pool, 0, &result));
  assert(mode.mode == 2 && result.width == 1920 && !calls[3]);
  ++bytes[127];
  assert(select_framebuffer_mode(&graphics, free_pool, &edid, &result) && mode.mode == 2);
  checksum_edid(bytes);

  set_fail = 1U << 2;
  assert(select_framebuffer_mode(&graphics, free_pool, 0, &result) && mode.mode == 1);
  set_fail = 0;
  wrong_size = 1U << 2;
  assert(select_framebuffer_mode(&graphics, free_pool, 0, &result) && mode.mode == 1);
  wrong_size = 0;
  query_fail = 1U << 2;
  short_info = 1U << 1;
  assert(select_framebuffer_mode(&graphics, free_pool, &edid, &result) && mode.mode == 0);
  query_fail = short_info = 0;
  available[1].width = 1280;
  assert(select_framebuffer_mode(&graphics, free_pool, &edid, &result) && mode.mode == 2);
  set_fail = 0xf;
  assert(!select_framebuffer_mode(&graphics, free_pool, &edid, &result));
  assert(!allocations);
  graphics.set_mode = 0;
  assert(!select_framebuffer_mode(&graphics, free_pool, &edid, &result));
}

int main(void) {
  mode_selection_contracts();
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
