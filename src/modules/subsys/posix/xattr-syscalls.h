/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_XATTR_SYSCALLS_H
#define POSIX_XATTR_SYSCALLS_H

#include "pedigree/kernel/processor/types.h"

int posix_setxattr(const char* path, const char* name, const void* value, size_t size, int flags);
int posix_lsetxattr(const char* path, const char* name, const void* value, size_t size, int flags);
int posix_fsetxattr(int fd, const char* name, const void* value, size_t size, int flags);
ssize_t posix_getxattr(const char* path, const char* name, void* value, size_t size);
ssize_t posix_lgetxattr(const char* path, const char* name, void* value, size_t size);
ssize_t posix_fgetxattr(int fd, const char* name, void* value, size_t size);
ssize_t posix_listxattr(const char* path, char* list, size_t size);
ssize_t posix_llistxattr(const char* path, char* list, size_t size);
ssize_t posix_flistxattr(int fd, char* list, size_t size);
int posix_removexattr(const char* path, const char* name);
int posix_lremovexattr(const char* path, const char* name);
int posix_fremovexattr(int fd, const char* name);

#endif
