/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_UEFI_EXIT_BOOT_SERVICES_H
#define PEDIGREE_UEFI_EXIT_BOOT_SERVICES_H
#include <stdint.h>

typedef uint64_t efi_status_t;
typedef void* efi_handle_t;
typedef efi_status_t (*efi_get_memory_map_t)(uint64_t*, void*, uint64_t*, uint64_t*, uint32_t*);
typedef efi_status_t (*efi_exit_boot_services_t)(efi_handle_t, uint64_t);

typedef struct efi_memory_descriptor {
  uint32_t type;
  uint32_t pad;
  uint64_t physical_start;
  uint64_t virtual_start;
  uint64_t number_of_pages;
  uint64_t attribute;
} efi_memory_descriptor_t;

typedef struct bootstrap_memory_map_entry {
  uint32_t size;
  uint64_t address;
  uint64_t length;
  uint32_t type;
} bootstrap_memory_map_entry_t;

_Static_assert(sizeof(bootstrap_memory_map_entry_t) == 32,
               "bootstrap memory-map entries must match the kernel ABI");

#define EFI_SUCCESS 0
#define EFI_LOAD_ERROR 0x8000000000000001ULL
#define EFI_INVALID_PARAMETER 0x8000000000000002ULL
#define EFI_BUFFER_TOO_SMALL 0x8000000000000005ULL
#define EFI_EXIT_BOOT_SERVICES_ATTEMPTS 4

static uint32_t normalized_type(uint32_t type) {
  if (type == 7)
    return 1;
  if (type == 9)
    return 3;
  if (type == 10)
    return 4;
  return 2;
}

static efi_status_t exit_boot_services_with_map(
    efi_get_memory_map_t get_map, efi_exit_boot_services_t exit_services, efi_handle_t image,
    void* raw_map, uint64_t raw_capacity, bootstrap_memory_map_entry_t* normalized_map,
    uint64_t normalized_capacity, uint32_t* normalized_bytes, int* exit_attempted) {
  *normalized_bytes = 0;
  *exit_attempted = 0;
  for (unsigned attempt = 0; attempt < EFI_EXIT_BOOT_SERVICES_ATTEMPTS; ++attempt) {
    uint64_t map_size = raw_capacity;
    uint64_t map_key = 0;
    uint64_t descriptor_size = 0;
    uint32_t descriptor_version = 0;
    efi_status_t status =
        get_map(&map_size, raw_map, &map_key, &descriptor_size, &descriptor_version);
    if (status != EFI_SUCCESS)
      return status;
    if (!map_size || map_size > raw_capacity || descriptor_version != 1 ||
        descriptor_size < sizeof(efi_memory_descriptor_t) || descriptor_size % 8 ||
        map_size % descriptor_size)
      return EFI_LOAD_ERROR;
    const uint64_t count = map_size / descriptor_size;
    if (count > normalized_capacity / sizeof(*normalized_map) ||
        count > UINT32_MAX / sizeof(*normalized_map))
      return EFI_LOAD_ERROR;
    for (uint64_t i = 0; i < count; ++i) {
      const efi_memory_descriptor_t* source =
          (const efi_memory_descriptor_t*)((const uint8_t*)raw_map + i * descriptor_size);
      if ((source->physical_start & 4095U) || source->number_of_pages > UINT64_MAX / 4096U ||
          source->number_of_pages * 4096U > UINT64_MAX - source->physical_start)
        return EFI_LOAD_ERROR;
      bootstrap_memory_map_entry_t* destination = &normalized_map[i];
      destination->size = sizeof(*destination) - sizeof(destination->size);
      destination->address = source->physical_start;
      destination->length = source->number_of_pages * 4096U;
      destination->type = normalized_type(source->type);
    }
    // Firmware may partially shut down even when the first exit returns an error.
    // The only callbacks reachable hereafter refresh the existing map or retry exit.
    *exit_attempted = 1;
    status = exit_services(image, map_key);
    if (status == EFI_SUCCESS) {
      *normalized_bytes = (uint32_t)(count * sizeof(*normalized_map));
      return EFI_SUCCESS;
    }
    if (status != EFI_INVALID_PARAMETER)
      return status;
  }
  return EFI_INVALID_PARAMETER;
}
#endif
