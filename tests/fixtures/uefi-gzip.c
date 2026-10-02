#include "../../src/system/boot/uefi/gzip.h"

int gzip_detect(const uint8_t* source, uint64_t length) {
  return initrd_is_gzip(source, length);
}

unsigned int gzip_size(const uint8_t* source, uint64_t length) {
  return gzip_initrd_size(source, length);
}

int gzip_unpack(void* destination, unsigned int* length, const void* source,
                unsigned int source_length) {
  return tinf_gzip_uncompress(destination, length, source, source_length);
}
