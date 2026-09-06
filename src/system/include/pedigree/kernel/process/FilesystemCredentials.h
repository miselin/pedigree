/* Filesystem authority is numeric and independent of account-file records. */
#ifndef PEDIGREE_FILESYSTEM_CREDENTIALS_H
#define PEDIGREE_FILESYSTEM_CREDENTIALS_H
#include "pedigree/kernel/processor/types.h"

struct FilesystemCredentials {
  static constexpr size_t MaximumGroups = 32;
  uint32_t uid = 0, gid = 0;
  uint32_t groups[MaximumGroups] = {};
  size_t groupCount = 0;
  bool valid = false;
  bool inGroup(uint32_t id) const {
    if (gid == id)
      return true;
    for (size_t i = 0; i < groupCount; ++i)
      if (groups[i] == id)
        return true;
    return false;
  }
};
#endif
