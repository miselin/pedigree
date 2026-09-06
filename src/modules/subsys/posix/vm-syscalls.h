/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_VM_SYSCALLS_H
#define POSIX_VM_SYSCALLS_H

#include "pedigree/kernel/processor/types.h"

void* posix_mremap(void* oldAddress, size_t oldLength, size_t newLength, int flags,
                   void* newAddress);
int posix_mincore(void* address, size_t length, unsigned char* vector);
int posix_madvise(void* address, size_t length, int advice);

#endif
