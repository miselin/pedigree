/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_EXECVEAT_SYSCALLS_H
#define POSIX_EXECVEAT_SYSCALLS_H

#include "pedigree/kernel/processor/state.h"

int posix_execveat(int dirfd, const char* path, const char** argv, const char** env, int flags,
                   SyscallState& state);

#endif
