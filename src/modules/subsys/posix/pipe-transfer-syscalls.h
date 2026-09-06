/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_PIPE_TRANSFER_SYSCALLS_H
#define POSIX_PIPE_TRANSFER_SYSCALLS_H

#include "pedigree/kernel/processor/types.h"

struct iovec;

ssize_t posix_splice(int input, int64_t* inputOffset, int output, int64_t* outputOffset,
                     size_t count, unsigned flags);
ssize_t posix_tee(int input, int output, size_t count, unsigned flags);
ssize_t posix_vmsplice(int fd, const struct iovec* vectors, size_t vectorCount, unsigned flags);

#endif
