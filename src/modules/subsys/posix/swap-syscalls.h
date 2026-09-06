/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_SWAP_SYSCALLS_H
#define POSIX_SWAP_SYSCALLS_H
int posix_swapon(const char* path, int flags);
int posix_swapoff(const char* path);
#endif
