/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_NAMESPACE_SYSCALLS_H
#define POSIX_NAMESPACE_SYSCALLS_H
#include "pedigree/kernel/processor/types.h"
struct utsname;
int posix_unshare(unsigned long flags);
int posix_setns(int fd, int type);
int posix_sethostname(const char* name, size_t length);
int posix_setdomainname(const char* name, size_t length);
int posix_uname(struct utsname* result);
#endif
