/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_POSIX_WAIT_SYSCALLS_H
#define PEDIGREE_POSIX_WAIT_SYSCALLS_H

#include <stdint.h>

struct LinuxRusage64;
int posix_waitpid(int pid, int* status, int options, LinuxRusage64* usage);
int posix_waitid(int which, int32_t id, void* information, int options, LinuxRusage64* usage);

#endif
