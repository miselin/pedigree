/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_SCHEDULING_SYSCALLS_H
#define POSIX_SCHEDULING_SYSCALLS_H

int posix_sched_setparam(int pid, const void* parameter);
int posix_sched_getparam(int pid, void* parameter);
int posix_sched_setscheduler(int pid, int policy, const void* parameter);
int posix_sched_getscheduler(int pid);
int posix_sched_get_priority_max(int policy);
int posix_sched_get_priority_min(int policy);
int posix_sched_rr_get_interval(int pid, void* interval);
int posix_sched_setaffinity(int pid, unsigned int length, const void* mask);
int posix_sched_getaffinity(int pid, unsigned int length, void* mask);
int posix_getcpu(unsigned int* cpu, unsigned int* node);

#endif
