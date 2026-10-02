/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_LANDLOCK_H
#define POSIX_LANDLOCK_H
#include "pedigree/kernel/process/FilesystemAccess.h"
#include "pedigree/kernel/process/FilesystemContext.h"
#include "pedigree/kernel/utilities/Vector.h"

namespace LandlockAccess = FilesystemAccess;

struct LandlockRule {
  FilesystemPathRef anchor;
  uint64_t inode = 0;
  uint64_t access = 0;
};

// Enforced layers are snapshots: changing a ruleset fd cannot widen a domain.
class LandlockDomain {
 public:
  SharedPointer<LandlockDomain> previous;
  Vector<LandlockRule> rules;
  uint64_t handled = 0;
  size_t depth = 1;
  uint64_t layerAccess(const FilesystemPathRef* ancestry, size_t count) const;
};

int posix_landlock_create_ruleset(const void* attributes, size_t size, unsigned flags);
int posix_landlock_add_rule(int ruleset, int type, const void* attributes, unsigned flags);
int posix_landlock_restrict_self(int ruleset, unsigned flags);
bool posix_landlock_check(const FilesystemPathRef& path, uint64_t requested);
bool posix_landlock_open(const FilesystemPathRef& path, int flags, bool& allowTruncate);
#endif
