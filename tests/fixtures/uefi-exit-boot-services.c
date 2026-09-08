/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/system/boot/uefi/exit_boot_services.h"

enum Scenario {
  Success,
  StaleThenSuccess,
  StaleLimit,
  PermanentExitError,
  InitialMapError,
  RetryMapError,
  ZeroStride,
  ShortStride,
  UnalignedStride,
  PartialDescriptor,
  OversizeMap,
  OutputTooSmall,
  PageCountOverflow,
  AddressOverflow,
  UnalignedAddress,
  WrongVersion,
  EmptyMap,
  OutputLengthOverflow
};
static enum Scenario scenario;
static unsigned maps, exits;
static uint64_t raw_capacity;
static char calls[32];
static size_t call_count;

static efi_status_t get_map(uint64_t* size, void* buffer, uint64_t* key, uint64_t* stride,
                            uint32_t* version) {
  assert(*size == raw_capacity);
  calls[call_count++] = 'M';
  ++maps;
  if (scenario == InitialMapError || (scenario == RetryMapError && maps == 2)) {
    *size = raw_capacity + 64;
    return EFI_BUFFER_TOO_SMALL;
  }
  *stride = maps == 1 ? 48 : 56;
  *size = *stride * (maps == 1 ? 1 : 2);
  *key = 0x100 + maps;
  *version = 1;
  memset(buffer, 0, 128);
  efi_memory_descriptor_t descriptor = {
      .type = maps == 1 ? 7 : 9, .physical_start = 0x1000 * maps, .number_of_pages = maps};
  memcpy(buffer, &descriptor, sizeof(descriptor));
  if (maps > 1) {
    descriptor.type = 10;
    descriptor.physical_start = 0x8000;
    memcpy((uint8_t*)buffer + *stride, &descriptor, sizeof(descriptor));
  }
  switch (scenario) {
    case ZeroStride:
      *stride = 0;
      break;
    case ShortStride:
      *stride = 32;
      *size = 32;
      break;
    case UnalignedStride:
      *stride = 41;
      *size = 41;
      break;
    case PartialDescriptor:
      ++*size;
      break;
    case OversizeMap:
      *size = raw_capacity + 48;
      break;
    case PageCountOverflow:
      ((efi_memory_descriptor_t*)buffer)->number_of_pages = UINT64_MAX / 4096U + 1;
      break;
    case AddressOverflow:
      ((efi_memory_descriptor_t*)buffer)->physical_start = UINT64_MAX & ~4095ULL;
      break;
    case UnalignedAddress:
      ((efi_memory_descriptor_t*)buffer)->physical_start = 1;
      break;
    case WrongVersion:
      *version = 2;
      break;
    case EmptyMap:
      *size = 0;
      break;
    case OutputLengthOverflow:
      *size = (UINT32_MAX / 32ULL + 1) * *stride;
      break;
    default:
      break;
  }
  return EFI_SUCCESS;
}

static efi_status_t exit_services(efi_handle_t image, uint64_t key) {
  assert(image == (void*)(uintptr_t)0x1234);
  assert(key == 0x100 + maps);
  calls[call_count++] = 'E';
  ++exits;
  if (scenario == PermanentExitError)
    return EFI_LOAD_ERROR;
  if (scenario == StaleLimit || scenario == RetryMapError ||
      (scenario == StaleThenSuccess && exits == 1))
    return EFI_INVALID_PARAMETER;
  return EFI_SUCCESS;
}

static void run_case(enum Scenario selected, efi_status_t expected, unsigned expected_maps,
                     unsigned expected_exits) {
  scenario = selected;
  maps = exits = 0;
  call_count = 0;
  memset(calls, 0, sizeof(calls));
  uint64_t raw[16];
  struct {
    uint64_t before;
    bootstrap_memory_map_entry_t map[2];
    uint64_t after;
  } output;
  memset(&output, 0x5a, sizeof(output));
  raw_capacity = selected == OutputLengthOverflow ? UINT64_MAX : sizeof(raw);
  const uint64_t output_capacity = selected == OutputTooSmall         ? 31
                                   : selected == OutputLengthOverflow ? UINT64_MAX
                                                                      : sizeof(output.map);
  uint32_t bytes = 99;
  int attempted = 99;
  assert(exit_boot_services_with_map(get_map, exit_services, (void*)(uintptr_t)0x1234, raw,
                                     raw_capacity, output.map, output_capacity, &bytes,
                                     &attempted) == expected);
  assert(maps == expected_maps && exits == expected_exits);
  assert(attempted == (expected_exits != 0));
  assert(output.before == 0x5a5a5a5a5a5a5a5aULL && output.after == output.before);
  if (expected == EFI_SUCCESS) {
    assert(bytes == (selected == StaleThenSuccess ? 64 : 32));
    assert(output.map[0].address == 0x1000 * maps);
    assert(output.map[0].length == 4096 * maps);
    assert(output.map[0].size == 28);
    assert(output.map[0].type == (maps == 1 ? 1 : 3));
    if (selected == StaleThenSuccess) {
      assert(!strcmp(calls, "MEME"));
      assert(output.map[1].address == 0x8000 && output.map[1].type == 4);
    }
  } else {
    assert(bytes == 0);
  }
  if (selected == StaleLimit)
    assert(!strcmp(calls, "MEMEMEME"));
  if (selected == RetryMapError)
    assert(!strcmp(calls, "MEM"));
}

int main(void) {
  run_case(Success, EFI_SUCCESS, 1, 1);
  run_case(StaleThenSuccess, EFI_SUCCESS, 2, 2);
  run_case(StaleLimit, EFI_INVALID_PARAMETER, EFI_EXIT_BOOT_SERVICES_ATTEMPTS,
           EFI_EXIT_BOOT_SERVICES_ATTEMPTS);
  run_case(PermanentExitError, EFI_LOAD_ERROR, 1, 1);
  run_case(InitialMapError, EFI_BUFFER_TOO_SMALL, 1, 0);
  run_case(RetryMapError, EFI_BUFFER_TOO_SMALL, 2, 1);
  for (enum Scenario invalid = ZeroStride; invalid <= OutputLengthOverflow; ++invalid)
    run_case(invalid, EFI_LOAD_ERROR, 1, 0);
  puts("UEFI exit contracts passed");
  return 0;
}
