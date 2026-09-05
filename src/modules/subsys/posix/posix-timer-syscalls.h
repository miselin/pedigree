/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef POSIX_TIMER_SYSCALLS_H
#define POSIX_TIMER_SYSCALLS_H

#include "pedigree/kernel/processor/types.h"

class Process;
class Thread;

int posix_timer_create(int clock, const void* event, int* timerId);
int posix_timer_settime(int timerId, int flags, const void* setting, void* previous);
int posix_timer_gettime(int timerId, void* setting);
int posix_timer_getoverrun(int timerId);
int posix_timer_delete(int timerId);

void posix_timer_process_exit(Process* process);
void posix_timer_thread_exit(Thread* thread);

#endif
