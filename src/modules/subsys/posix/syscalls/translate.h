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

#ifndef _POSIX_SYSCALLS_TRANSLATE_H
#define _POSIX_SYSCALLS_TRANSLATE_H

// from musl
#include <bits/syscall.h>

#define TRANSLATION_ENTRY(FROM, TO) \
    case FROM: \
        pedigree_translation = TO; \
        break;

#include <posixSyscallNumbers.h>

inline long posix_translate_syscall(long which)
{
    long pedigree_translation = -1;

    switch (which)
    {
        TRANSLATION_ENTRY(SYS_read, POSIX_READ)
        TRANSLATION_ENTRY(SYS_write, POSIX_WRITE)
        TRANSLATION_ENTRY(SYS_open, POSIX_OPEN)
        TRANSLATION_ENTRY(SYS_close, POSIX_CLOSE)
        TRANSLATION_ENTRY(SYS_stat, POSIX_STAT)
        TRANSLATION_ENTRY(SYS_fstat, POSIX_FSTAT)
        TRANSLATION_ENTRY(SYS_lstat, POSIX_LSTAT)
        TRANSLATION_ENTRY(SYS_poll, POSIX_POLL)
        TRANSLATION_ENTRY(SYS_lseek, POSIX_LSEEK)
#ifdef SYS_mmap
        TRANSLATION_ENTRY(SYS_mmap, POSIX_MMAP)
#endif
        TRANSLATION_ENTRY(SYS_mprotect, POSIX_MPROTECT)
        TRANSLATION_ENTRY(SYS_munmap, POSIX_MUNMAP)
        TRANSLATION_ENTRY(SYS_brk, POSIX_BRK)
        TRANSLATION_ENTRY(SYS_rt_sigaction, POSIX_SIGACTION)
        TRANSLATION_ENTRY(SYS_rt_sigprocmask, POSIX_SIGPROCMASK)
        TRANSLATION_ENTRY(SYS_rt_sigreturn, PEDIGREE_SIGRET)
        TRANSLATION_ENTRY(SYS_ioctl, POSIX_IOCTL)
        // ...
        TRANSLATION_ENTRY(SYS_readv, POSIX_READV)
        TRANSLATION_ENTRY(SYS_writev, POSIX_WRITEV)
        TRANSLATION_ENTRY(SYS_access, POSIX_ACCESS)
        TRANSLATION_ENTRY(SYS_pipe, POSIX_PIPE)
#ifdef SYS_select
        TRANSLATION_ENTRY(SYS_select, POSIX_SELECT)
#endif
        TRANSLATION_ENTRY(SYS_sched_yield, POSIX_SCHED_YIELD)
        // ...
        TRANSLATION_ENTRY(SYS_msync, POSIX_MSYNC)
        // ...
        TRANSLATION_ENTRY(SYS_dup, POSIX_DUP)
        TRANSLATION_ENTRY(SYS_dup2, POSIX_DUP2)
        TRANSLATION_ENTRY(SYS_pause, POSIX_PAUSE)
        // ...
        TRANSLATION_ENTRY(SYS_nanosleep, POSIX_NANOSLEEP)
        TRANSLATION_ENTRY(SYS_getitimer, POSIX_GETITIMER)
#ifdef SYS_alarm
        TRANSLATION_ENTRY(SYS_alarm, POSIX_ALARM)
#endif
        TRANSLATION_ENTRY(SYS_setitimer, POSIX_SETITIMER)
        TRANSLATION_ENTRY(SYS_getpid, POSIX_GETPID)
        // ...
        TRANSLATION_ENTRY(SYS_socket, POSIX_SOCKET)
        TRANSLATION_ENTRY(SYS_connect, POSIX_CONNECT)
        TRANSLATION_ENTRY(SYS_accept, POSIX_ACCEPT)
        TRANSLATION_ENTRY(SYS_sendto, POSIX_SENDTO)
        TRANSLATION_ENTRY(SYS_recvfrom, POSIX_RECVFROM)
        TRANSLATION_ENTRY(SYS_sendmsg, POSIX_SENDMSG)
        TRANSLATION_ENTRY(SYS_recvmsg, POSIX_RECVMSG)
        TRANSLATION_ENTRY(SYS_shutdown, POSIX_SHUTDOWN)
        TRANSLATION_ENTRY(SYS_bind, POSIX_BIND)
        TRANSLATION_ENTRY(SYS_listen, POSIX_LISTEN)
        TRANSLATION_ENTRY(SYS_getsockname, POSIX_GETSOCKNAME)
        TRANSLATION_ENTRY(SYS_getpeername, POSIX_GETPEERNAME)
        TRANSLATION_ENTRY(SYS_socketpair, POSIX_SOCKETPAIR)
        TRANSLATION_ENTRY(SYS_setsockopt, POSIX_SETSOCKOPT)
        TRANSLATION_ENTRY(SYS_getsockopt, POSIX_GETSOCKOPT)
        TRANSLATION_ENTRY(SYS_clone, POSIX_CLONE)
        TRANSLATION_ENTRY(SYS_fork, POSIX_FORK)
        // ...
        TRANSLATION_ENTRY(SYS_execve, POSIX_EXECVE)
        TRANSLATION_ENTRY(SYS_exit, POSIX_EXIT)
        TRANSLATION_ENTRY(SYS_wait4, POSIX_WAITPID)
        TRANSLATION_ENTRY(SYS_kill, POSIX_KILL)
        TRANSLATION_ENTRY(SYS_uname, POSIX_UNAME)
        // ...
        TRANSLATION_ENTRY(SYS_fcntl, POSIX_FCNTL)
#ifdef SYS_flock
        TRANSLATION_ENTRY(SYS_flock, POSIX_FLOCK)
#endif
        TRANSLATION_ENTRY(SYS_fsync, POSIX_FSYNC)
        // ...
        TRANSLATION_ENTRY(SYS_ftruncate, POSIX_FTRUNCATE)
        TRANSLATION_ENTRY(SYS_getdents, POSIX_GETDENTS)
        TRANSLATION_ENTRY(SYS_getcwd, POSIX_GETCWD)
        TRANSLATION_ENTRY(SYS_chdir, POSIX_CHDIR)
        TRANSLATION_ENTRY(SYS_fchdir, POSIX_FCHDIR)
        TRANSLATION_ENTRY(SYS_rename, POSIX_RENAME)
        TRANSLATION_ENTRY(SYS_mkdir, POSIX_MKDIR)
        TRANSLATION_ENTRY(SYS_rmdir, POSIX_RMDIR)
        TRANSLATION_ENTRY(SYS_creat, POSIX_CREAT)
        TRANSLATION_ENTRY(SYS_link, POSIX_LINK)
        TRANSLATION_ENTRY(SYS_unlink, POSIX_UNLINK)
        TRANSLATION_ENTRY(SYS_symlink, POSIX_SYMLINK)
        TRANSLATION_ENTRY(SYS_readlink, POSIX_READLINK)
        TRANSLATION_ENTRY(SYS_chmod, POSIX_CHMOD)
        TRANSLATION_ENTRY(SYS_fchmod, POSIX_FCHMOD)
        TRANSLATION_ENTRY(SYS_chown, POSIX_CHOWN)
        TRANSLATION_ENTRY(SYS_fchown, POSIX_FCHOWN)
        // ...
        TRANSLATION_ENTRY(SYS_umask, POSIX_UMASK)
        TRANSLATION_ENTRY(SYS_gettimeofday, POSIX_GETTIMEOFDAY)
#ifdef SYS_getrlimit
        TRANSLATION_ENTRY(SYS_getrlimit, POSIX_GETRLIMIT)
#endif
        // ...
        TRANSLATION_ENTRY(SYS_times, POSIX_TIMES)
        // ...
        TRANSLATION_ENTRY(SYS_getuid, POSIX_GETUID)
        TRANSLATION_ENTRY(SYS_syslog, POSIX_L_SYSLOG)
        TRANSLATION_ENTRY(SYS_getgid, POSIX_GETGID)
        TRANSLATION_ENTRY(SYS_setuid, POSIX_SETUID)
        TRANSLATION_ENTRY(SYS_setgid, POSIX_SETGID)
        TRANSLATION_ENTRY(SYS_geteuid, POSIX_GETEUID)
        TRANSLATION_ENTRY(SYS_getegid, POSIX_GETEGID)
        TRANSLATION_ENTRY(SYS_setpgid, POSIX_SETPGID)
        TRANSLATION_ENTRY(SYS_getppid, POSIX_GETPPID)
        TRANSLATION_ENTRY(SYS_getpgrp, POSIX_GETPGRP)
        TRANSLATION_ENTRY(SYS_setsid, POSIX_SETSID)
        TRANSLATION_ENTRY(SYS_setreuid, POSIX_SETREUID)
        TRANSLATION_ENTRY(SYS_setregid, POSIX_SETREGID)
        TRANSLATION_ENTRY(SYS_getgroups, POSIX_GETGROUPS)
        TRANSLATION_ENTRY(SYS_setgroups, POSIX_SETGROUPS)
        TRANSLATION_ENTRY(SYS_setresuid, POSIX_SETRESUID)
        TRANSLATION_ENTRY(SYS_getresuid, POSIX_GETRESUID)
        TRANSLATION_ENTRY(SYS_setresgid, POSIX_SETRESGID)
        TRANSLATION_ENTRY(SYS_getresgid, POSIX_GETRESGID)
        TRANSLATION_ENTRY(SYS_getpgid, POSIX_GETPGID)
        // ...
        TRANSLATION_ENTRY(SYS_capget, POSIX_CAPGET)
        TRANSLATION_ENTRY(SYS_capset, POSIX_CAPSET)
        // ...
#ifdef SYS_utime
        TRANSLATION_ENTRY(SYS_utime, POSIX_UTIME)
#endif
        TRANSLATION_ENTRY(SYS_mknod, POSIX_MKNOD)
        // ...
        TRANSLATION_ENTRY(SYS_statfs, POSIX_STATFS)
        TRANSLATION_ENTRY(SYS_fstatfs, POSIX_FSTATFS)
        // ...
        TRANSLATION_ENTRY(SYS_getpriority, POSIX_GETPRIORITY)
        TRANSLATION_ENTRY(SYS_setpriority, POSIX_SETPRIORITY)
        // ...
        TRANSLATION_ENTRY(SYS_prctl, POSIX_PRCTL)
#ifdef SYS_arch_prctl
        TRANSLATION_ENTRY(SYS_arch_prctl, POSIX_ARCH_PRCTL)
#endif
        // ...
#ifdef SYS_setrlimit
        TRANSLATION_ENTRY(SYS_setrlimit, POSIX_SETRLIMIT)
#endif
        TRANSLATION_ENTRY(SYS_chroot, POSIX_CHROOT)
        // ...
        TRANSLATION_ENTRY(SYS_settimeofday, POSIX_SETTIMEOFDAY)
        TRANSLATION_ENTRY(SYS_mount, POSIX_MOUNT)
        // ...
        TRANSLATION_ENTRY(SYS_sethostname, POSIX_SETHOSTNAME)
        // ...
#ifdef SYS_iopl
        TRANSLATION_ENTRY(SYS_iopl, POSIX_IOPL)
#endif
#ifdef SYS_ioperm
        TRANSLATION_ENTRY(SYS_ioperm, POSIX_IOPERM)
#endif
        // ...
        TRANSLATION_ENTRY(SYS_gettid, POSIX_GETTID)
        // ...
        TRANSLATION_ENTRY(SYS_getxattr, POSIX_GETXATTR)
        TRANSLATION_ENTRY(SYS_lgetxattr, POSIX_LGETXATTR)
        TRANSLATION_ENTRY(SYS_fgetxattr, POSIX_FGETXATTR)
        // ...
#ifdef SYS_time
        TRANSLATION_ENTRY(SYS_time, POSIX_TIME)
#endif
        TRANSLATION_ENTRY(SYS_futex, POSIX_FUTEX)
        // ...
#ifdef SYS_set_thread_area
        TRANSLATION_ENTRY(SYS_set_thread_area, POSIX_SET_TLS_AREA)
#endif
        // ...
        TRANSLATION_ENTRY(SYS_getdents64, POSIX_GETDENTS64)
        /// \todo this is a hack.
        case SYS_set_tid_address:
            return 0;
        // ...
        TRANSLATION_ENTRY(SYS_clock_gettime, POSIX_CLOCK_GETTIME)
        // ...
        TRANSLATION_ENTRY(SYS_exit_group, POSIX_EXIT_GROUP)
        // ...
        TRANSLATION_ENTRY(SYS_utimes, POSIX_UTIMES)
        // ...
        TRANSLATION_ENTRY(SYS_openat, POSIX_OPENAT)
        TRANSLATION_ENTRY(SYS_mkdirat, POSIX_MKDIRAT)
        // ...
        TRANSLATION_ENTRY(SYS_fchownat, POSIX_FCHOWNAT)
        TRANSLATION_ENTRY(SYS_futimesat, POSIX_FUTIMESAT)
#ifdef SYS_newfstatat
        TRANSLATION_ENTRY(SYS_newfstatat, POSIX_FSTATAT)
#endif
        TRANSLATION_ENTRY(SYS_unlinkat, POSIX_UNLINKAT)
        TRANSLATION_ENTRY(SYS_renameat, POSIX_RENAMEAT)
        TRANSLATION_ENTRY(SYS_linkat, POSIX_LINKAT)
        TRANSLATION_ENTRY(SYS_symlinkat, POSIX_SYMLINKAT)
        TRANSLATION_ENTRY(SYS_readlinkat, POSIX_READLINKAT)
        TRANSLATION_ENTRY(SYS_fchmodat, POSIX_FCHMODAT)
        TRANSLATION_ENTRY(SYS_faccessat, POSIX_FACCESSAT)
        // ...
        TRANSLATION_ENTRY(SYS_set_robust_list, POSIX_SET_ROBUST_LIST)
        TRANSLATION_ENTRY(SYS_get_robust_list, POSIX_GET_ROBUST_LIST)

        // Pedigree pass-through syscalls.
        TRANSLATION_ENTRY(0x8000, POSIX_TTYNAME)
    }
    return pedigree_translation;
}

#endif  // _POSIX_SYSCALLS_TRANSLATE_H
