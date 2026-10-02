#ifndef PEDIGREE_UEFI_GZIP_H
#define PEDIGREE_UEFI_GZIP_H

#include <stdint.h>

// The PE wrapper consumes one COFF object; tinf needs no C runtime here.
#define assert(condition) ((condition) ? (void)0 : __builtin_trap())
#define read_le16 tinf_deflate_read_le16
#include "../../../../external/tinf/tinflate.c"
#undef read_le16
#undef assert
#include "../../../../external/tinf/crc32.c"
#include "../../../../external/tinf/tinfgzip.c"

#define MAX_INITRD_SIZE (256U * 1024 * 1024)

static int initrd_is_gzip(const uint8_t* data, uint64_t length) {
  return length >= 2 && data[0] == 0x1f && data[1] == 0x8b;
}

static unsigned int gzip_initrd_size(const uint8_t* data, uint64_t length) {
  if (!initrd_is_gzip(data, length) || length < 18 || length > MAX_INITRD_SIZE) {
    return 0;
  }
  // Gzip stores ISIZE modulo 4 GiB. Initrd allocations are bounded below that.
  unsigned int size = read_le32(data + length - 4);
  return size <= MAX_INITRD_SIZE ? size : 0;
}

#endif
