/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#ifndef LINUX_WAIT_ABI_H
#define LINUX_WAIT_ABI_H

#include <stdint.h>

struct LinuxKernelTimespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

static_assert(sizeof(LinuxKernelTimespec) == 16,
              "Linux amd64 kernel timespec must remain 16 bytes.");

#endif
