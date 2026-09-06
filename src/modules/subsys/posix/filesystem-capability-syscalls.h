/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_FILESYSTEM_CAPABILITY_SYSCALLS_H
#define POSIX_FILESYSTEM_CAPABILITY_SYSCALLS_H

#include <sys/types.h>

int posix_fallocate(int fd, int mode, off_t offset, off_t length);
int posix_renameat2(int oldDirFd, const char* oldPath, int newDirFd, const char* newPath,
                    unsigned flags);

#endif
