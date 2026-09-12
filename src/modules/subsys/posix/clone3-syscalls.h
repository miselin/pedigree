/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_POSIX_CLONE3_SYSCALLS_H
#define PEDIGREE_POSIX_CLONE3_SYSCALLS_H

#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/processor/types.h"

struct LinuxCloneArgs;
long posix_clone3(SyscallState& state, const LinuxCloneArgs* userArgs, size_t size);

#endif
