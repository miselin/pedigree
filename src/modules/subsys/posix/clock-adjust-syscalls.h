/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_CLOCK_ADJUST_SYSCALLS_H
#define POSIX_CLOCK_ADJUST_SYSCALLS_H

int posix_adjtimex(void* value);
int posix_clock_adjtime(int clockId, void* value);

#endif
