/* Copyright (c) 2026, Pedigree Developers. See LICENSE for licensing details. */
#ifndef SYSV_MESSAGE_SYSCALLS_H
#define SYSV_MESSAGE_SYSCALLS_H

#include "pedigree/kernel/processor/types.h"

int posix_msgget(int32_t key, int flags);
int posix_msgsnd(int id, const void* message, size_t size, int flags);
ssize_t posix_msgrcv(int id, void* message, size_t size, int64_t type, int flags);
int posix_msgctl(int id, int command, void* buffer);

#endif
