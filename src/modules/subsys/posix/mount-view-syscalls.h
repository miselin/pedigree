/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_POSIX_MOUNT_VIEW_SYSCALLS_H
#define PEDIGREE_POSIX_MOUNT_VIEW_SYSCALLS_H
#include "pedigree/kernel/processor/types.h"
int posix_mount(const char* source, const char* target, const char* type, size_t flags,
                const void* data);
int posix_umount2(const char* target, int flags);
int posix_pivot_root(const char* newRoot, const char* putOld);
#endif
