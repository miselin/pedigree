/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/system/boot/uefi/load_options.h"

static uefi_load_options_t check_options(const void* raw, uint32_t size,
                                         enum uefi_boot_directory directory,
                                         const char* arguments) {
  struct {
    uint64_t before;
    uefi_load_options_t options;
    uint64_t after;
  } guarded;
  memset(&guarded, 0x5a, sizeof(guarded));
  assert(parse_load_options(raw, size, &guarded.options));
  assert(guarded.before == 0x5a5a5a5a5a5a5a5aULL && guarded.after == guarded.before);
  assert(guarded.options.directory == directory);
  assert(guarded.options.arguments_length == strlen(arguments));
  assert(!strcmp(guarded.options.text + guarded.options.arguments_offset, arguments));
  return guarded.options;
}

static uint32_t utf16(const char* text, uint8_t* output) {
  uint32_t size = 0;
  do {
    output[size++] = (uint8_t)*text;
    output[size++] = 0;
  } while (*text++);
  return size;
}

int main(void) {
  uefi_load_options_t options;
  check_options(NULL, 0, UEFI_BOOT_DIRECTORY_DEFAULT, "");
  assert(!parse_load_options(NULL, 1, &options));
  check_options("current", 7, UEFI_BOOT_DIRECTORY_CURRENT, "");
  check_options("current", 8, UEFI_BOOT_DIRECTORY_CURRENT, "");
  check_options("known-good", 11, UEFI_BOOT_DIRECTORY_KNOWN_GOOD, "");
  check_options("debug", 6, UEFI_BOOT_DIRECTORY_DEBUG, "");
  const char debug[] = "debug intelgfx=off";
  check_options(debug, sizeof(debug), UEFI_BOOT_DIRECTORY_DEBUG, "intelgfx=off");
  check_options(" \t ", 4, UEFI_BOOT_DIRECTORY_DEFAULT, "");
  const char off[] = "current intelgfx=off";
  const char on[] = "known-good intelgfx=on";
  const char current_on[] = "current intelgfx=on";
  options = check_options(off, sizeof(off), UEFI_BOOT_DIRECTORY_CURRENT, "intelgfx=off");
  check_options(off, sizeof(off) - 1, UEFI_BOOT_DIRECTORY_CURRENT, "intelgfx=off");
  check_options(on, sizeof(on), UEFI_BOOT_DIRECTORY_KNOWN_GOOD, "intelgfx=on");
  check_options(current_on, sizeof(current_on), UEFI_BOOT_DIRECTORY_CURRENT, "intelgfx=on");
  const char tabbed[] = "\tcurrent\tintelgfx=off\tsplash=log\t";
  check_options(tabbed, sizeof(tabbed), UEFI_BOOT_DIRECTORY_CURRENT, "intelgfx=off splash=log");
  const char unknown[] = "currently intelgfx=off";
  check_options(unknown, sizeof(unknown), UEFI_BOOT_DIRECTORY_DEFAULT, unknown);
  const char near_good[] = "known-good-extra intelgfx=on";
  check_options(near_good, sizeof(near_good), UEFI_BOOT_DIRECTORY_DEFAULT, near_good);
  check_options("intelgfx=off", 12, UEFI_BOOT_DIRECTORY_DEFAULT, "intelgfx=off");

  uint8_t unaligned[128];
  uint8_t* wide = unaligned + 1;
  uint32_t size = utf16(off, wide);
  uint32_t debug_size = utf16(debug, wide);
  check_options(wide, debug_size, UEFI_BOOT_DIRECTORY_DEBUG, "intelgfx=off");
  size = utf16(off, wide);
  check_options(wide, size, UEFI_BOOT_DIRECTORY_CURRENT, "intelgfx=off");
  check_options(wide, size - 2, UEFI_BOOT_DIRECTORY_CURRENT, "intelgfx=off");
  assert(!parse_load_options(wide, size - 1, &options));
  wide[3] = 1;
  assert(!parse_load_options(wide, size, &options));
  size = utf16(on, wide);
  check_options(wide, size, UEFI_BOOT_DIRECTORY_KNOWN_GOOD, "intelgfx=on");
  wide[size] = 'x';
  wide[size + 1] = 0;
  assert(!parse_load_options(wide, size + 2, &options));
  size = utf16(current_on, wide);
  check_options(wide, size, UEFI_BOOT_DIRECTORY_CURRENT, "intelgfx=on");
  size = utf16("current", wide);
  check_options(wide, size, UEFI_BOOT_DIRECTORY_CURRENT, "");
  const char embedded[] = "current\0intelgfx=off";
  assert(!parse_load_options(embedded, sizeof(embedded), &options));
  const char newline[] = "current\nintelgfx=off";
  assert(!parse_load_options(newline, sizeof(newline), &options));
  const uint8_t non_ascii[] = {'c', 0x80};
  assert(!parse_load_options(non_ascii, sizeof(non_ascii), &options));
  assert(!parse_load_options(off, UINT32_MAX, &options));

  char maximum[UEFI_LOAD_OPTIONS_CAPACITY + 1];
  memset(maximum, 'x', sizeof(maximum));
  maximum[UEFI_LOAD_OPTIONS_CAPACITY - 1] = 0;
  check_options(maximum, UEFI_LOAD_OPTIONS_CAPACITY, UEFI_BOOT_DIRECTORY_DEFAULT, maximum);
  maximum[UEFI_LOAD_OPTIONS_CAPACITY - 1] = 'x';
  assert(!parse_load_options(maximum, UEFI_LOAD_OPTIONS_CAPACITY, &options));
  maximum[UEFI_LOAD_OPTIONS_CAPACITY - 1] = 0;
  uint8_t wide_maximum[UEFI_LOAD_OPTIONS_CAPACITY * 2];
  size = utf16(maximum, wide_maximum);
  assert(size == sizeof(wide_maximum));
  check_options(wide_maximum, size, UEFI_BOOT_DIRECTORY_DEFAULT, maximum);
  wide_maximum[size - 2] = 'x';
  assert(!parse_load_options(wide_maximum, size, &options));

  options = check_options(off, sizeof(off), UEFI_BOOT_DIRECTORY_CURRENT, "intelgfx=off");
  const uint8_t base[] = "root=UUID=1234-5678 splash=log\r\n\0\0";
  const char expected[] = "root=UUID=1234-5678 splash=log intelgfx=off";
  char combined[sizeof(expected) + 1];
  memset(combined, 0x5a, sizeof(combined));
  assert(append_load_options(base, sizeof(base), &options, combined, sizeof(expected)));
  assert(!strcmp(combined, expected));
  assert(combined[sizeof(expected)] == 0x5a);
  memset(combined, 0x5a, sizeof(combined));
  assert(!append_load_options(base, sizeof(base), &options, combined, sizeof(expected) - 1));
  for (uint32_t i = 0; i < sizeof(combined); ++i)
    assert(combined[i] == 0x5a);
  assert(append_load_options(NULL, 0, &options, combined, sizeof(combined)));
  assert(!strcmp(combined, "intelgfx=off"));
  const uint8_t bad_base[] = "root=UUID=1234\0splash=log";
  assert(!append_load_options(bad_base, sizeof(bad_base), &options, combined, sizeof(combined)));
  assert(!append_load_options(base, UINT64_MAX, &options, combined, sizeof(combined)));
  assert(!append_load_options(base, sizeof(base), &options, combined, 0));
  options = check_options("current", 8, UEFI_BOOT_DIRECTORY_CURRENT, "");
  const uint8_t unchanged[] = "root=UUID=1234-5678\n";
  assert(
      append_load_options(unchanged, sizeof(unchanged) - 1, &options, combined, sizeof(combined)));
  assert(!strcmp(combined, (const char*)unchanged));
  puts("UEFI load option contracts passed");
  return 0;
}
