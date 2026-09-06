/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_METADATA_SYSCALLS_H
#define POSIX_METADATA_SYSCALLS_H
#include <sys/types.h>
int posix_truncate(const char* path, off_t length);
int posix_lchown(const char* path, uid_t owner, gid_t group);
int posix_utimensat(int dirfd, const char* path, const void* times, int flags);
int posix_statx(int dirfd, const char* path, int flags, unsigned mask, void* output);
int posix_fchmodat2(int dirfd, const char* path, mode_t mode, int flags);
int posix_mknodat(int dirfd, const char* path, mode_t mode, dev_t device);
#endif
