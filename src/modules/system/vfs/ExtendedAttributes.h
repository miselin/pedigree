/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_VFS_EXTENDEDATTRIBUTES_H
#define PEDIGREE_VFS_EXTENDEDATTRIBUTES_H

#include "pedigree/kernel/processor/types.h"

enum class XattrStatus {
  Success,
  Missing,
  Exists,
  Range,
  NoSpace,
  NoMemory,
  Unsupported,
  Denied,
  IoError,
  Invalid,
  ReadOnly,
  Quota,
  Overflow,
};

namespace Xattr {
constexpr size_t MaximumNameLength = 255;
constexpr size_t MaximumValueLength = 65536;
constexpr size_t MaximumListLength = 65536;
constexpr unsigned Create = 1, Replace = 2;
}  // namespace Xattr

#endif
