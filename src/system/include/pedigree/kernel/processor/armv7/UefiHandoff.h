#ifndef PEDIGREE_ARMV7_UEFI_HANDOFF_H
#define PEDIGREE_ARMV7_UEFI_HANDOFF_H

#include <stdint.h>

#define ARMV7_UEFI_HANDOFF_MAGIC 0x50445637U

typedef struct {
  uint32_t magic;
  uint32_t fdt;
  uint32_t initrd_start;
  uint32_t initrd_end;
  uint32_t command_line;
  uint32_t memory_map;
  uint32_t memory_map_bytes;
} armv7_uefi_handoff_t;

#endif
