/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_PTRACE_SYSCALLS_H
#define POSIX_PTRACE_SYSCALLS_H

#include "pedigree/kernel/processor/types.h"

long posix_ptrace(unsigned long request, int32_t pid, uintptr_t address, uintptr_t data,
                  bool linuxAbi);

#endif
