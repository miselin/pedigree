/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef SYSTEM_SYSCALLS_H
#define SYSTEM_SYSCALLS_H

#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/state.h"

#include "logging.h"

#include <sys/types.h>

// Forward-declare types.
struct group;
struct passwd;
struct timespec;

uintptr_t posix_brk(uintptr_t theBreak);
long posix_sbrk(int delta);
long posix_clone(
    SyscallState &state, unsigned long flags, void *child_stack, int *ptid,
    int *ctid, unsigned long newtls);
int posix_fork(SyscallState &state);
int posix_execve(
    const char *name, const char **argv, const char **env, SyscallState &state);
int posix_waitpid(const int pid, int *status, int options);
int posix_exit(int code, bool allthreads = true) NORETURN;
int posix_getpid();
int posix_getppid();

int posix_gettimeofday(timeval *tv, struct timezone *tz);
int posix_settimeofday(const timeval *tv, const struct timezone *tz);
time_t posix_time(time_t *tval);
clock_t posix_times(struct tms *tm);
int posix_getrusage(int who, struct rusage *r);

int posix_getpwent(passwd *pw, int n, char *str);
int posix_getpwnam(passwd *pw, const char *name, char *str);
uid_t posix_getuid();
gid_t posix_getgid();
uid_t posix_geteuid();
gid_t posix_getegid();
int posix_setuid(uid_t uid);
int posix_setgid(gid_t gid);
int posix_seteuid(uid_t euid);
int posix_setegid(gid_t egid);

size_t posix_alarm(uint32_t seconds);
int posix_sleep(uint32_t seconds);
int posix_usleep(size_t useconds);
int posix_nanosleep(const struct timespec *rqtp, struct timespec *rmtp);
int posix_clock_gettime(clockid_t clock_id, struct timespec *tp);

int pedigree_sigret();

int posix_setsid();
int posix_setpgid(int pid, int pgid);
int posix_getpgrp();
int posix_getpgid(int pid);

mode_t posix_umask(mode_t mask);

int posix_getgrnam(const char *name, struct group *out);
int posix_getgrgid(gid_t id, struct group *out);

int posix_linux_syslog(int type, char *buf, int len);
int posix_syslog(const char *msg, int prio);

int pedigree_login(int uid, const char *password);

int pedigree_reboot();

int posix_uname(struct utsname *n);

int posix_arch_prctl(int code, unsigned long addr);

int posix_pause();

int posix_setgroups(size_t size, const gid_t *list);
int posix_getgroups(int size, gid_t *list);

int posix_getrlimit(int resource, struct rlimit *rlim);
int posix_setrlimit(int resource, const struct rlimit *rlim);
int posix_getpriority(int which, int who);
int posix_setpriority(int which, int who, int prio);

int posix_setreuid(uid_t ruid, uid_t euid);
int posix_setregid(gid_t rgid, gid_t egid);

int posix_setresuid(uid_t ruid, uid_t euid, uid_t suid);
int posix_setresgid(gid_t rgid, gid_t egid, gid_t sgid);
int posix_getresuid(uid_t *ruid, uid_t *euid, uid_t *suid);
int posix_getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid);

int posix_get_robust_list(
    int pid, struct robust_list_head **head_ptr, size_t *len_ptr);
int posix_set_robust_list(struct robust_list_head *head, size_t len);

int posix_ioperm(unsigned long from, unsigned long num, int turn_on);
int posix_iopl(int level);

int posix_getitimer(int which, struct itimerval *curr_value);
int posix_setitimer(int which, const struct itimerval *new_value, struct itimerval *old_value);

#endif
