/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_SYSV_SHM_SYSCALLS_H
#define POSIX_SYSV_SHM_SYSCALLS_H

#include "pedigree/kernel/processor/types.h"

int posix_shmget(int32_t key, size_t size, int flags);
void* posix_shmat(int id, const void* address, int flags);
int posix_shmdt(const void* address);
int posix_shmctl(int id, int command, void* buffer);

#endif
