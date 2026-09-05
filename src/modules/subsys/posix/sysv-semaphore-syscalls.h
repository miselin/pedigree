/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef POSIX_SYSV_SEMAPHORE_SYSCALLS_H
#define POSIX_SYSV_SEMAPHORE_SYSCALLS_H

#include "pedigree/kernel/processor/types.h"

class Thread;

int posix_semget(int key, int count, int flags);
int posix_semop(int id, const void* operations, size_t count);
int posix_semtimedop(int id, const void* operations, size_t count, const void* timeout);
int posix_semctl(int id, int number, int command, uintptr_t argument);

bool posix_sem_clone(Thread* parent, Thread* child, bool shareUndo);
void posix_sem_thread_exit(Thread* thread);

#endif
