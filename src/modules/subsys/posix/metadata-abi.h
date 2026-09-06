/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_METADATA_ABI_H
#define POSIX_METADATA_ABI_H
#include "pedigree/kernel/processor/types.h"
namespace PosixMetadata {
constexpr int64_t TimeNow = 1073741823, TimeOmit = 1073741822;
struct Timespec {
  int64_t seconds;
  int64_t nanoseconds;
};
struct StatxTimestamp {
  int64_t seconds;
  uint32_t nanoseconds, reserved;
};
struct Statx {
  uint32_t mask, blockSize;
  uint64_t attributes;
  uint32_t links, uid, gid;
  uint16_t mode, reserved;
  uint64_t inode, size, blocks, attributesMask;
  StatxTimestamp accessed, born, changed, modified;
  uint32_t deviceMajor, deviceMinor, filesystemMajor, filesystemMinor;
  uint64_t mountId;
  uint32_t directMemoryAlignment, directOffsetAlignment;
  uint64_t subvolume;
  uint32_t atomicWriteMin, atomicWriteMax, atomicWriteSegments, reserved2;
  uint64_t reserved3[9];
};
static_assert(sizeof(Timespec) == 16);
static_assert(sizeof(StatxTimestamp) == 16);
static_assert(sizeof(Statx) == 256);
static_assert(offsetof(Statx, accessed) == 64);
static_assert(offsetof(Statx, mountId) == 144);
constexpr bool validNanoseconds(int64_t value) {
  return value == TimeNow || value == TimeOmit || (value >= 0 && value < 1000000000);
}
constexpr uint32_t deviceMajor(uint64_t value) {
  return ((value >> 8) & 0xfff) | ((value >> 32) & 0xfffff000);
}
constexpr uint32_t deviceMinor(uint64_t value) {
  return (value & 0xff) | ((value >> 12) & 0xffffff00);
}
}  // namespace PosixMetadata
#endif
