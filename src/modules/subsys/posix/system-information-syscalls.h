/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_SYSTEM_INFORMATION_SYSCALLS_H
#define POSIX_SYSTEM_INFORMATION_SYSCALLS_H

int posix_sysinfo(void* information);
int posix_personality(unsigned long requested);

#endif
