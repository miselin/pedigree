/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_MEMFD_SYSCALLS_H
#define POSIX_MEMFD_SYSCALLS_H

#include "pedigree/kernel/processor/types.h"

class DescriptorLease;

int posix_memfd_create(const char* name, unsigned int flags);
int posix_memfd_fcntl(const DescriptorLease& descriptor, int command, uintptr_t argument);

#endif
