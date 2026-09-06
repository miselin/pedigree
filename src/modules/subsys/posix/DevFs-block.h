/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_DEVFS_BLOCK_H
#define POSIX_DEVFS_BLOCK_H
#include "pedigree/kernel/processor/types.h"
class DevFs;
class File;
namespace PosixBlock {
constexpr uint32_t MountedMajor = 240;
constexpr uint32_t PhysicalMajor = 241;
constexpr uint32_t MaximumMinor = 0xfffff;
constexpr uint64_t encode(uint32_t major, uint32_t minor) {
  return (minor & 0xffULL) | (static_cast<uint64_t>(major & 0xfff) << 8) |
         (static_cast<uint64_t>(minor & ~0xffU) << 12);
}
constexpr uint32_t major(uint64_t device) {
  return (device >> 8) & 0xfff;
}
constexpr uint32_t minor(uint64_t device) {
  return (device & 0xff) | ((device >> 12) & 0xffffff00);
}
constexpr bool valid(uint64_t device, uint32_t expectedMajor) {
  return major(device) == expectedMajor && minor(device) && minor(device) <= MaximumMinor &&
         encode(expectedMajor, minor(device)) == device;
}
}  // namespace PosixBlock
File* posix_make_block_directory(DevFs& filesystem, File* parent);
#endif
