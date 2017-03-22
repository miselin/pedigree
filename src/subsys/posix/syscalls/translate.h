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

#include <posixSyscallNumbers.h>

inline long posix_translate_syscall(long which)
{
    long pedigree_translation = -1;

    switch (which)
    {
        case SYS_read: pedigree_translation = POSIX_READ; break;
        case SYS_write: pedigree_translation = POSIX_WRITE; break;
        case SYS_open: pedigree_translation = POSIX_OPEN; break;
        case SYS_close: pedigree_translation = POSIX_CLOSE; break;
        case SYS_stat: pedigree_translation = POSIX_STAT; break;
        case SYS_fstat: pedigree_translation = POSIX_FSTAT; break;
        case SYS_lstat: pedigree_translation = POSIX_LSTAT; break;
        case SYS_poll: pedigree_translation = POSIX_POLL; break;
        case SYS_lseek: pedigree_translation = POSIX_LSEEK; break;
        case SYS_mmap: pedigree_translation = POSIX_MMAP; break;
        case SYS_mprotect: pedigree_translation = POSIX_MPROTECT; break;
        case SYS_munmap: pedigree_translation = POSIX_MUNMAP; break;
        case SYS_brk: pedigree_translation = POSIX_BRK; break;
        case SYS_rt_sigaction: pedigree_translation = POSIX_SIGACTION; break;
        case SYS_rt_sigprocmask: pedigree_translation = POSIX_SIGPROCMASK; break;
        case SYS_rt_sigreturn: pedigree_translation = PEDIGREE_SIGRET; break;
        case SYS_ioctl: pedigree_translation = POSIX_IOCTL; break;
        // ...
        case SYS_readv: pedigree_translation = POSIX_READV; break;
        case SYS_writev: pedigree_translation = POSIX_WRITEV; break;
        case SYS_access: pedigree_translation = POSIX_ACCESS; break;
        case SYS_pipe: pedigree_translation = POSIX_PIPE; break;
        case SYS_select: pedigree_translation = POSIX_SELECT; break;
        case SYS_sched_yield: pedigree_translation = POSIX_SCHED_YIELD; break;
        // ...
        case SYS_msync: pedigree_translation = POSIX_MSYNC; break;
        // ...
        case SYS_dup: pedigree_translation = POSIX_DUP; break;
        case SYS_dup2: pedigree_translation = POSIX_DUP2; break;
        case SYS_pause: pedigree_translation = POSIX_PAUSE; break;
        // ...
        case SYS_nanosleep: pedigree_translation = POSIX_NANOSLEEP; break;
        // ...
        case SYS_alarm: pedigree_translation = POSIX_ALARM; break;
        // ...
        case SYS_getpid: pedigree_translation = POSIX_GETPID; break;
        // ...
        case SYS_socket: pedigree_translation = POSIX_SOCKET; break;
        case SYS_connect: pedigree_translation = POSIX_CONNECT; break;
        case SYS_accept: pedigree_translation = POSIX_ACCEPT; break;
        case SYS_sendto: pedigree_translation = POSIX_SENDTO; break;
        case SYS_recvfrom: pedigree_translation = POSIX_RECVFROM; break;
        case SYS_shutdown: pedigree_translation = POSIX_SHUTDOWN; break;
        case SYS_bind: pedigree_translation = POSIX_BIND; break;
        case SYS_listen: pedigree_translation = POSIX_LISTEN; break;
        case SYS_getsockname: pedigree_translation = POSIX_GETSOCKNAME; break;
        case SYS_getpeername: pedigree_translation = POSIX_GETPEERNAME; break;
        // ...
        case SYS_getsockopt: pedigree_translation = POSIX_GETSOCKOPT; break;
        case SYS_clone: pedigree_translation = POSIX_CLONE; break;
        case SYS_fork: pedigree_translation = POSIX_FORK; break;
        // ...
        case SYS_execve: pedigree_translation = POSIX_EXECVE; break;
        case SYS_exit: pedigree_translation = POSIX_EXIT; break;
        case SYS_wait4: pedigree_translation = POSIX_WAITPID; break;
        case SYS_kill: pedigree_translation = POSIX_KILL; break;
        case SYS_uname: pedigree_translation = POSIX_UNAME; break;
        // ...
        case SYS_fcntl: pedigree_translation = POSIX_FCNTL; break;
        case SYS_flock: pedigree_translation = POSIX_FLOCK; break;
        case SYS_fsync: pedigree_translation = POSIX_FSYNC; break;
        // ...
        case SYS_ftruncate: pedigree_translation = POSIX_FTRUNCATE; break;
        case SYS_getdents: pedigree_translation = POSIX_GETDENTS; break;
        case SYS_getcwd: pedigree_translation = POSIX_GETCWD; break;
        case SYS_chdir: pedigree_translation = POSIX_CHDIR; break;
        case SYS_fchdir: pedigree_translation = POSIX_FCHDIR; break;
        case SYS_rename: pedigree_translation = POSIX_RENAME; break;
        case SYS_mkdir: pedigree_translation = POSIX_MKDIR; break;
        case SYS_rmdir: pedigree_translation = POSIX_RMDIR; break;
        // ...
        case SYS_link: pedigree_translation = POSIX_LINK; break;
        case SYS_unlink: pedigree_translation = POSIX_UNLINK; break;
        case SYS_symlink: pedigree_translation = POSIX_SYMLINK; break;
        case SYS_readlink: pedigree_translation = POSIX_READLINK; break;
        case SYS_chmod: pedigree_translation = POSIX_CHMOD; break;
        case SYS_fchmod: pedigree_translation = POSIX_FCHMOD; break;
        case SYS_chown: pedigree_translation = POSIX_CHOWN; break;
        case SYS_fchown: pedigree_translation = POSIX_FCHOWN; break;
        // ...
        case SYS_umask: pedigree_translation = POSIX_UMASK; break;
        case SYS_gettimeofday: pedigree_translation = POSIX_GETTIMEOFDAY; break;
        case SYS_getrlimit: pedigree_translation = POSIX_GETRLIMIT; break;
        // ...
        case SYS_times: pedigree_translation = POSIX_TIMES; break;
        // ...
        case SYS_getuid: pedigree_translation = POSIX_GETUID; break;
        case SYS_syslog: pedigree_translation = POSIX_L_SYSLOG; break;
        case SYS_getgid: pedigree_translation = POSIX_GETGID; break;
        case SYS_setuid: pedigree_translation = POSIX_SETUID; break;
        case SYS_setgid: pedigree_translation = POSIX_SETGID; break;
        case SYS_geteuid: pedigree_translation = POSIX_GETEUID; break;
        case SYS_getegid: pedigree_translation = POSIX_GETEGID; break;
        case SYS_setpgid: pedigree_translation = POSIX_SETPGID; break;
        case SYS_getppid: pedigree_translation = POSIX_GETPPID; break;
        case SYS_getpgrp: pedigree_translation = POSIX_GETPGRP; break;
        case SYS_setsid: pedigree_translation = POSIX_SETSID; break;
        // ...
        case SYS_setgroups: pedigree_translation = POSIX_SETGROUPS; break;
        // ...
        case SYS_utime: pedigree_translation = POSIX_UTIME; break;
        // ...
        case SYS_getpriority: pedigree_translation = POSIX_GETPRIORITY; break;
        case SYS_setpriority: pedigree_translation = POSIX_SETPRIORITY; break;
        // ...
        case SYS_arch_prctl: pedigree_translation = POSIX_ARCH_PRCTL; break;
        // ...
        case SYS_chroot: pedigree_translation = POSIX_CHROOT; break;
        // ...
        case SYS_gettid: pedigree_translation = POSIX_GETTID; break;
        // ...
        case SYS_getxattr: pedigree_translation = POSIX_GETXATTR; break;
        case SYS_lgetxattr: pedigree_translation = POSIX_LGETXATTR; break;
        case SYS_fgetxattr: pedigree_translation = POSIX_FGETXATTR; break;
        // ...
        case SYS_futex: pedigree_translation = POSIX_FUTEX; break;
        // ...
        case SYS_set_thread_area: pedigree_translation = POSIX_SET_TLS_AREA; break;
        // ...
        case SYS_getdents64: pedigree_translation = POSIX_GETDENTS64; break;
        /// \todo this is a hack.
        case SYS_set_tid_address: return 0;
        // ...
        case SYS_clock_gettime: pedigree_translation = POSIX_CLOCK_GETTIME; break;
        // ...
        case SYS_exit_group: pedigree_translation = POSIX_EXIT_GROUP; break;
        // ...
        case SYS_utimes: pedigree_translation = POSIX_UTIMES; break;
        // ...
        case SYS_openat: pedigree_translation = POSIX_OPENAT; break;
        case SYS_mkdirat: pedigree_translation = POSIX_MKDIRAT; break;
        // ...
        case SYS_fchownat: pedigree_translation = POSIX_FCHOWNAT; break;
        case SYS_futimesat: pedigree_translation = POSIX_FUTIMESAT; break;
        case SYS_newfstatat: pedigree_translation = POSIX_FSTATAT; break;
        case SYS_unlinkat: pedigree_translation = POSIX_UNLINKAT; break;
        case SYS_renameat: pedigree_translation = POSIX_RENAMEAT; break;
        case SYS_linkat: pedigree_translation = POSIX_LINKAT; break;
        case SYS_symlinkat: pedigree_translation = POSIX_SYMLINKAT; break;
        case SYS_readlinkat: pedigree_translation = POSIX_READLINKAT; break;
        case SYS_fchmodat: pedigree_translation = POSIX_FCHMODAT; break;
        case SYS_faccessat: pedigree_translation = POSIX_FACCESSAT; break;

        // Pedigree pass-through syscalls.
        case 0x8000: pedigree_translation = POSIX_TTYNAME;
    }

    return pedigree_translation;
}

#endif  // _POSIX_SYSCALLS_TRANSLATE_H
