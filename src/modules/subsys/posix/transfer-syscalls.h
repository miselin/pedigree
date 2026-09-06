/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_TRANSFER_SYSCALLS_H
#define POSIX_TRANSFER_SYSCALLS_H

#include "pedigree/kernel/processor/types.h"

ssize_t posix_sendfile(int outputFd, int inputFd, int64_t* offset, size_t count);
ssize_t posix_copy_file_range(int inputFd, int64_t* inputOffset, int outputFd,
                              int64_t* outputOffset, size_t count, unsigned flags);

#endif
