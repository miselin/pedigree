/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_QUOTA_SYSCALLS_H
#define POSIX_QUOTA_SYSCALLS_H

int posix_quotactl(int command, const char* special, int id, void* address);

#endif
