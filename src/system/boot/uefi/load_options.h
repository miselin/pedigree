/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_UEFI_LOAD_OPTIONS_H
#define PEDIGREE_UEFI_LOAD_OPTIONS_H

#include <stdint.h>

#define UEFI_LOAD_OPTIONS_CAPACITY 1024U
#define UEFI_COMMAND_LINE_CAPACITY 4096U

enum uefi_boot_directory {
  UEFI_BOOT_DIRECTORY_DEFAULT,
  UEFI_BOOT_DIRECTORY_CURRENT,
  UEFI_BOOT_DIRECTORY_KNOWN_GOOD
};

typedef struct uefi_load_options {
  enum uefi_boot_directory directory;
  uint32_t arguments_offset;
  uint32_t arguments_length;
  char text[UEFI_LOAD_OPTIONS_CAPACITY];
} uefi_load_options_t;

static int parse_load_options(const void* raw, uint32_t size, uefi_load_options_t* options) {
  if (!options)
    return 0;
  options->directory = UEFI_BOOT_DIRECTORY_DEFAULT;
  options->arguments_offset = options->arguments_length = 0;
  options->text[0] = 0;
  if (!size)
    return 1;
  if (!raw || size > UEFI_LOAD_OPTIONS_CAPACITY * 2)
    return 0;

  const uint8_t* bytes = (const uint8_t*)raw;
  const uint32_t stride = size >= 2 && !bytes[1] ? 2 : 1;
  if (size % stride)
    return 0;
  uint32_t length = 0;
  int terminated = 0;
  for (uint32_t offset = 0; offset < size; offset += stride) {
    if (stride == 2 && bytes[offset + 1])
      return 0;
    const uint8_t value = bytes[offset];
    if (!value) {
      terminated = 1;
      continue;
    }
    if (terminated || (value != '\t' && (value < ' ' || value > '~')) ||
        length + 1 >= UEFI_LOAD_OPTIONS_CAPACITY)
      return 0;
    // Kernel consumers split on spaces, while EFI launchers may supply tabs.
    options->text[length++] = value == '\t' ? ' ' : (char)value;
  }
  while (length && options->text[length - 1] == ' ')
    --length;
  options->text[length] = 0;
  uint32_t start = 0;
  while (start < length && options->text[start] == ' ')
    ++start;
  uint32_t end = start;
  while (end < length && options->text[end] != ' ')
    ++end;
  static const char* selectors[] = {"current", "known-good"};
  for (uint32_t selector = 0; selector < 2; ++selector) {
    uint32_t matched = 0;
    while (start + matched < end && selectors[selector][matched] &&
           options->text[start + matched] == selectors[selector][matched])
      ++matched;
    if (start + matched == end && !selectors[selector][matched]) {
      options->directory = selector ? UEFI_BOOT_DIRECTORY_KNOWN_GOOD : UEFI_BOOT_DIRECTORY_CURRENT;
      start = end;
      while (start < length && options->text[start] == ' ')
        ++start;
      break;
    }
  }
  options->arguments_offset = start;
  options->arguments_length = length - start;
  return 1;
}

static int append_load_options(const uint8_t* command_line, uint64_t length,
                               const uefi_load_options_t* options, char* output,
                               uint64_t capacity) {
  if (!options || !output || (!command_line && length) || !capacity ||
      length > UEFI_COMMAND_LINE_CAPACITY)
    return 0;
  if (options->arguments_length) {
    while (length && (!command_line[length - 1] || command_line[length - 1] == '\r' ||
                      command_line[length - 1] == '\n'))
      --length;
    for (uint64_t i = 0; i < length; ++i) {
      if (!command_line[i])
        return 0;
    }
  }
  const uint64_t separator = length && options->arguments_length ? 1 : 0;
  if (length >= capacity || options->arguments_length > capacity - length - 1 ||
      separator > capacity - length - options->arguments_length - 1)
    return 0;
  for (uint64_t i = 0; i < length; ++i)
    output[i] = (char)command_line[i];
  if (separator)
    output[length++] = ' ';
  for (uint32_t i = 0; i < options->arguments_length; ++i)
    output[length++] = options->text[options->arguments_offset + i];
  output[length] = 0;
  return 1;
}

#endif
