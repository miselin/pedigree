/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_FILE_SYNC_SYSCALLS_H
#define POSIX_FILE_SYNC_SYSCALLS_H

#include <sys/types.h>

int posix_fdatasync(int fd);
int posix_readahead(int fd, off_t offset, size_t count);
int posix_fadvise64(int fd, off_t offset, off_t length, int advice);
int posix_sync_file_range(int fd, off_t offset, off_t length, unsigned flags);

#endif
