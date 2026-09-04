/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_POSIX_LINUX_RESOURCE_ABI_H
#define PEDIGREE_POSIX_LINUX_RESOURCE_ABI_H

#include <stdint.h>

struct LinuxRlimit64 {
  uint64_t current;
  uint64_t maximum;
};

static_assert(sizeof(LinuxRlimit64) == 16, "Linux amd64 rlimit64 must remain 16 bytes");

#endif
