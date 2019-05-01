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

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/processor/PageFaultHandler.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/SyscallManager.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/time/Time.h"

#include "PosixSyscallManager.h"
#include "console-syscalls.h"
#include "file-syscalls.h"
#include "logging.h"
#include "net-syscalls.h"
#include "pipe-syscalls.h"
#include "poll-syscalls.h"
#include "posixSyscallNumbers.h"
#include "pthread-syscalls.h"
#include "select-syscalls.h"
#include "signal-syscalls.h"
#include "syscalls/translate.h"
#include "system-syscalls.h"

#include <fcntl.h>
#include <time.h>

#include "pedigree/kernel/debugger/Backtrace.h"

PosixSyscallManager::PosixSyscallManager()
{
}

PosixSyscallManager::~PosixSyscallManager()
{
}

void PosixSyscallManager::initialise()
{
    SyscallManager::instance().registerSyscallHandler(linuxCompat, this);
    SyscallManager::instance().registerSyscallHandler(posix, this);
}

uintptr_t PosixSyscallManager::call(
    uintptr_t function, uintptr_t p1, uintptr_t p2, uintptr_t p3, uintptr_t p4,
    uintptr_t p5)
{
    if (function >= serviceEnd)
    {
        ERROR(
            "PosixSyscallManager: invalid function called: "
            << Dec << static_cast<int>(function));
        return 0;
    }

    uintptr_t ret =
        SyscallManager::instance().syscall(posix, function, p1, p2, p3, p4, p5);
    return ret;
}

uintptr_t PosixSyscallManager::syscall(SyscallState &state)
{
    uint64_t syscallNumber = state.getSyscallNumber();

    uintptr_t base = 0;
    if (state.getSyscallService() == linuxCompat)
    {
        // Switch ABI now that we've seen a Linux syscall come in.
        Process *pProcess =
            Processor::information().getCurrentThread()->getParent();
        PosixSubsystem *pSubsystem =
            static_cast<PosixSubsystem *>(pProcess->getSubsystem());
        pSubsystem->setAbi(PosixSubsystem::LinuxAbi);

        base = 6;  // use Linux syscall ABI

        // Translate the syscall.
        long which = posix_translate_syscall(syscallNumber);
        if (which < 0)
        {
            uint64_t key = (static_cast<uint64_t>(pProcess->getId()) << 32ULL) | syscallNumber;
            if (!m_SeenUnknownSyscalls.lookup(key))
            {
                ERROR(
                    "POSIX: unknown Linux syscall "
                    << syscallNumber << " by pid=" << pProcess->getId()
                    << ", translation failed!");
                m_SeenUnknownSyscalls.insert(key, true);
            }
            SYSCALL_ERROR(Unimplemented);
            return -1;
        }
#ifdef POSIX_VERBOSE_SYSCALLS
        else
        {
            NOTICE(
                "TRANSLATED syscall: Linux #" << syscallNumber
                                              << " -> Pedigree #" << which);
        }
#endif

        syscallNumber = which;
    }

    uintptr_t p1 = state.getSyscallParameter(base + 0);
    uintptr_t p2 = state.getSyscallParameter(base + 1);
    uintptr_t p3 = state.getSyscallParameter(base + 2);
    uintptr_t p4 = state.getSyscallParameter(base + 3);
    uintptr_t p5 = state.getSyscallParameter(base + 4);
    uintptr_t p6 = state.getSyscallParameter(base + 5);

#ifdef POSIX_VERBOSE_SYSCALLS
    NOTICE(
        "[" << Processor::information().getCurrentThread()->getParent()->getId()
            << "] : " << Dec << syscallNumber << Hex);
#endif

    // We're interruptible.
    Processor::setInterrupts(true);

    switch (syscallNumber)
    {
        // POSIX system calls
        case POSIX_OPEN:
            return posix_open(reinterpret_cast<const char *>(p1), p2, p3);
        case POSIX_WRITE:
            return posix_write(p1, reinterpret_cast<char *>(p2), p3);
        case POSIX_READ:
            return posix_read(p1, reinterpret_cast<char *>(p2), p3);
        case POSIX_CLOSE:
            return posix_close(p1);
        case POSIX_SBRK:
            return posix_sbrk(p1);
        case POSIX_FORK:
            return posix_fork(state);
        case POSIX_EXECVE:
            return posix_execve(
                reinterpret_cast<const char *>(p1),
                reinterpret_cast<const char **>(p2),
                reinterpret_cast<const char **>(p3), state);
        case POSIX_WAITPID:
            return posix_waitpid(p1, reinterpret_cast<int *>(p2), p3);
        case POSIX_EXIT:
            // If not Linux mode, we exit the entire process. If Linux, just
            // the current thread (as glibc uses exit_group for "all process").
            posix_exit(p1, state.getSyscallService() != linuxCompat);
        case POSIX_EXIT_GROUP:
            posix_exit(p1, true);
        case POSIX_TCGETATTR:
            return posix_tcgetattr(p1, reinterpret_cast<struct termios *>(p2));
        case POSIX_TCSETATTR:
            return posix_tcsetattr(
                p1, p2, reinterpret_cast<struct termios *>(p3));
        case POSIX_IOCTL:
            return posix_ioctl(p1, p2, reinterpret_cast<void *>(p3));
        case POSIX_STAT:
            return posix_stat(
                reinterpret_cast<const char *>(p1),
                reinterpret_cast<struct stat *>(p2));
        case POSIX_FSTAT:
            return posix_fstat(p1, reinterpret_cast<struct stat *>(p2));
        case POSIX_GETPID:
            return posix_getpid();
        case POSIX_CHDIR:
            return posix_chdir(reinterpret_cast<const char *>(p1));
        case POSIX_SELECT:
            return posix_select(
                static_cast<int>(p1), reinterpret_cast<fd_set *>(p2),
                reinterpret_cast<fd_set *>(p3), reinterpret_cast<fd_set *>(p4),
                reinterpret_cast<struct timeval *>(p5));
        case POSIX_LSEEK:
            return posix_lseek(
                static_cast<int>(p1), static_cast<off_t>(p2),
                static_cast<int>(p3));
        case POSIX_SOCKET:
            return posix_socket(
                static_cast<int>(p1), static_cast<int>(p2),
                static_cast<int>(p3));
        case POSIX_CONNECT:
            return posix_connect(
                static_cast<int>(p1), reinterpret_cast<struct sockaddr_storage *>(p2), p3);
        case POSIX_SEND:
            return posix_send(
                static_cast<int>(p1), reinterpret_cast<void *>(p2), p3,
                static_cast<int>(p4));
        case POSIX_RECV:
            return posix_recv(
                static_cast<int>(p1), reinterpret_cast<void *>(p2), p3,
                static_cast<int>(p4));
        case POSIX_BIND:
            return posix_bind(
                static_cast<int>(p1), reinterpret_cast<struct sockaddr_storage *>(p2), p3);
        case POSIX_LISTEN:
            return posix_listen(static_cast<int>(p1), static_cast<int>(p2));
        case POSIX_ACCEPT:
            return posix_accept(
                static_cast<int>(p1), reinterpret_cast<struct sockaddr_storage *>(p2),
                reinterpret_cast<socklen_t *>(p3));
        case POSIX_RECVFROM:
            return posix_recvfrom(
                static_cast<int>(p1), reinterpret_cast<void *>(p2), p3,
                static_cast<int>(p4), reinterpret_cast<struct sockaddr_storage *>(p5),
                reinterpret_cast<socklen_t *>(p6));
        case POSIX_SENDTO:
            return posix_sendto(
                static_cast<int>(p1), reinterpret_cast<void *>(p2), p3,
                static_cast<int>(p4), reinterpret_cast<struct sockaddr_storage *>(p5),
                static_cast<socklen_t>(p6));
        case POSIX_GETTIMEOFDAY:
            return posix_gettimeofday(
                reinterpret_cast<struct timeval *>(p1),
                reinterpret_cast<struct timezone *>(p2));
        case POSIX_DUP:
            return posix_dup(static_cast<int>(p1));
        case POSIX_DUP2:
            return posix_dup2(static_cast<int>(p1), static_cast<int>(p2));
        case POSIX_LSTAT:
            return posix_lstat(
                reinterpret_cast<char *>(p1),
                reinterpret_cast<struct stat *>(p2));
        case POSIX_UNLINK:
            return posix_unlink(reinterpret_cast<char *>(p1));
        case POSIX_SYMLINK:
            return posix_symlink(
                reinterpret_cast<char *>(p1), reinterpret_cast<char *>(p2));
        case POSIX_FCNTL:
            return posix_fcntl(
                static_cast<int>(p1), static_cast<int>(p2),
                reinterpret_cast<void *>(p3));
        case POSIX_PIPE:
            return posix_pipe(reinterpret_cast<int *>(p1));
        case POSIX_MKDIR:
            return posix_mkdir(
                reinterpret_cast<const char *>(p1), static_cast<int>(p2));
        case POSIX_RMDIR:
            return posix_rmdir(reinterpret_cast<const char *>(p1));
        case POSIX_GETPWENT:
            return posix_getpwent(
                reinterpret_cast<passwd *>(p1), static_cast<int>(p2),
                reinterpret_cast<char *>(p3));
        case POSIX_GETPWNAM:
            return posix_getpwnam(
                reinterpret_cast<passwd *>(p1),
                reinterpret_cast<const char *>(p2),
                reinterpret_cast<char *>(p3));
        case POSIX_GETUID:
            return posix_getuid();
        case POSIX_GETGID:
            return posix_getgid();
        case POSIX_SIGACTION:
            return posix_sigaction(
                static_cast<int>(p1),
                reinterpret_cast<const struct sigaction *>(p2),
                reinterpret_cast<struct sigaction *>(p3));
        case POSIX_SIGNAL:
            return posix_signal(
                static_cast<int>(p1), reinterpret_cast<void *>(p2));
        case POSIX_RAISE:
            return posix_raise(static_cast<int>(p1), state);
        case POSIX_KILL:
            return posix_kill(static_cast<int>(p1), static_cast<int>(p2));
        case POSIX_SIGPROCMASK:
            return posix_sigprocmask(
                static_cast<int>(p1), reinterpret_cast<const uint32_t *>(p2),
                reinterpret_cast<uint32_t *>(p3));
        case POSIX_ALARM:
            return posix_alarm(p1);
        case POSIX_SLEEP:
            return posix_sleep(p1);
        case POSIX_POLL:
            return posix_poll(
                reinterpret_cast<pollfd *>(p1), static_cast<unsigned int>(p2),
                static_cast<int>(p3));
        case POSIX_RENAME:
            return posix_rename(
                reinterpret_cast<const char *>(p1),
                reinterpret_cast<const char *>(p2));
        case POSIX_GETCWD:
            return posix_getcwd(reinterpret_cast<char *>(p1), p2);
        case POSIX_READLINK:
            return posix_readlink(
                reinterpret_cast<const char *>(p1),
                reinterpret_cast<char *>(p2), static_cast<unsigned int>(p3));
        case POSIX_LINK:
            return posix_link(
                reinterpret_cast<char *>(p1), reinterpret_cast<char *>(p2));
        case POSIX_ISATTY:
            return posix_isatty(static_cast<int>(p1));
        case POSIX_MMAP:
            return reinterpret_cast<uintptr_t>(posix_mmap(
                reinterpret_cast<void *>(p1), p2, static_cast<int>(p3),
                static_cast<int>(p4), static_cast<int>(p5),
                static_cast<off_t>(p6)));
        case POSIX_MUNMAP:
            return posix_munmap(reinterpret_cast<void *>(p1), p2);
        case POSIX_SHUTDOWN:
            return posix_shutdown(static_cast<int>(p1), static_cast<int>(p2));
        case POSIX_ACCESS:
            return posix_access(
                reinterpret_cast<const char *>(p1), static_cast<int>(p2));
        case POSIX_SETSID:
            return posix_setsid();
        case POSIX_SETPGID:
            return posix_setpgid(static_cast<int>(p1), static_cast<int>(p2));
        case POSIX_GETPGID:
            return posix_getpgid(static_cast<int>(p1));
        case POSIX_GETPGRP:
            return posix_getpgrp();
        case POSIX_SIGALTSTACK:
            return posix_sigaltstack(
                reinterpret_cast<const stack_t *>(p1),
                reinterpret_cast<stack_t *>(p2));

        case POSIX_SYSLOG:
            return posix_syslog(
                reinterpret_cast<const char *>(p1), static_cast<int>(p2));

        case POSIX_FTRUNCATE:
            return posix_ftruncate(
                static_cast<int>(p1), static_cast<off_t>(p2));

        // Stub warning
        case POSIX_STUBBED:
            // This is the solution to a bug - if the address in p1 traps
            // (because of demand loading), it MUST trap before we get the log
            // spinlock, else other things will want to write to it and
            // deadlock.
            static char buf[128];
            StringCopyN(buf, reinterpret_cast<char *>(p1), 128);
            WARNING("Using stubbed function '" << buf << "'");
            return 0;

        // POSIX-specific Pedigree system calls
        case PEDIGREE_SIGRET:
            return pedigree_sigret();
        case PEDIGREE_INIT_SIGRET:
            WARNING("POSIX: The 'init sigret' system call is no longer valid.");
            // pedigree_init_sigret();
            return 0;
        case POSIX_SCHED_YIELD:
            Scheduler::instance().yield();
            return 0;

        case POSIX_NANOSLEEP:
            return posix_nanosleep(
                reinterpret_cast<struct timespec *>(p1),
                reinterpret_cast<struct timespec *>(p2));
        case POSIX_CLOCK_GETTIME:
            return posix_clock_gettime(
                p1, reinterpret_cast<struct timespec *>(p2));

        case POSIX_GETEUID:
            return posix_geteuid();
        case POSIX_GETEGID:
            return posix_getegid();
        case POSIX_SETEUID:
            return posix_seteuid(static_cast<uid_t>(p1));
        case POSIX_SETEGID:
            return posix_setegid(static_cast<gid_t>(p1));
        case POSIX_SETUID:
            return posix_setuid(static_cast<uid_t>(p1));
        case POSIX_SETGID:
            return posix_setgid(static_cast<gid_t>(p1));

        case POSIX_CHOWN:
            return posix_chown(
                reinterpret_cast<const char *>(p1), static_cast<uid_t>(p2),
                static_cast<gid_t>(p3));
        case POSIX_CHMOD:
            return posix_chmod(
                reinterpret_cast<const char *>(p1), static_cast<mode_t>(p2));
        case POSIX_FCHOWN:
            return posix_fchown(
                static_cast<int>(p1), static_cast<uid_t>(p2),
                static_cast<gid_t>(p3));
        case POSIX_FCHMOD:
            return posix_fchmod(static_cast<int>(p1), static_cast<mode_t>(p2));
        case POSIX_FCHDIR:
            return posix_fchdir(static_cast<int>(p1));

        case POSIX_STATVFS:
            return posix_statvfs(
                reinterpret_cast<const char *>(p1),
                reinterpret_cast<struct statvfs *>(p2));
        case POSIX_FSTATVFS:
            return posix_fstatvfs(
                static_cast<int>(p1), reinterpret_cast<struct statvfs *>(p2));

        case PEDIGREE_UNWIND_SIGNAL:
            pedigree_unwind_signal();
            return 0;

        case POSIX_MSYNC:
            return posix_msync(
                reinterpret_cast<void *>(p1), p2, static_cast<int>(p3));
        case POSIX_GETPEERNAME:
            return posix_getpeername(
                static_cast<int>(p1), reinterpret_cast<struct sockaddr_storage *>(p2),
                reinterpret_cast<socklen_t *>(p3));
        case POSIX_GETSOCKNAME:
            return posix_getsockname(
                static_cast<int>(p1), reinterpret_cast<struct sockaddr_storage *>(p2),
                reinterpret_cast<socklen_t *>(p3));
        case POSIX_FSYNC:
            return posix_fsync(static_cast<int>(p1));

        case POSIX_PTSNAME:
            return console_ptsname(
                static_cast<int>(p1), reinterpret_cast<char *>(p2));
        case POSIX_TTYNAME:
            return console_ttyname(
                static_cast<int>(p1), reinterpret_cast<char *>(p2));
        case POSIX_TCGETPGRP:
            return posix_tcgetpgrp(static_cast<int>(p1));
        case POSIX_TCSETPGRP:
            return posix_tcsetpgrp(
                static_cast<int>(p1), static_cast<pid_t>(p2));

        case POSIX_USLEEP:
            return posix_usleep(p1);

        case POSIX_MPROTECT:
            return posix_mprotect(
                reinterpret_cast<void *>(p1), p2, static_cast<int>(p3));

        case POSIX_REALPATH:
            return posix_realpath(
                reinterpret_cast<const char *>(p1),
                reinterpret_cast<char *>(p2), p3);
        case POSIX_TIMES:
            return posix_times(reinterpret_cast<struct tms *>(p1));
        case POSIX_GETRUSAGE:
            return posix_getrusage(p1, reinterpret_cast<struct rusage *>(p2));
        case POSIX_SETSOCKOPT:
            return posix_setsockopt(
                p1, p2, p3, reinterpret_cast<const void *>(p4), p5);
        case POSIX_GETSOCKOPT:
            return posix_getsockopt(
                p1, p2, p3, reinterpret_cast<void *>(p4),
                reinterpret_cast<socklen_t *>(p5));
        case POSIX_GETPPID:
            return posix_getppid();
        case POSIX_UTIME:
            return posix_utime(
                reinterpret_cast<const char *>(p1),
                reinterpret_cast<const struct utimbuf *>(p2));
        case POSIX_UTIMES:
            return posix_utimes(
                reinterpret_cast<const char *>(p1),
                reinterpret_cast<const struct timeval *>(p2));
        case POSIX_CHROOT:
            return posix_chroot(reinterpret_cast<const char *>(p1));

        case POSIX_GETGRNAM:
            return posix_getgrnam(
                reinterpret_cast<const char *>(p1),
                reinterpret_cast<struct group *>(p2));
        case POSIX_GETGRGID:
            return posix_getgrgid(
                static_cast<gid_t>(p1), reinterpret_cast<struct group *>(p2));
        case POSIX_UMASK:
            return posix_umask(static_cast<mode_t>(p1));
        case POSIX_WRITEV:
            return posix_writev(
                static_cast<int>(p1),
                reinterpret_cast<const struct iovec *>(p2), p3);
        case POSIX_READV:
            return posix_readv(
                static_cast<int>(p1),
                reinterpret_cast<const struct iovec *>(p2), p3);
        case POSIX_GETDENTS:
            return posix_getdents(
                static_cast<int>(p1),
                reinterpret_cast<struct linux_dirent *>(p2),
                static_cast<int>(p3));
        case POSIX_GETTID:
            return posix_gettid();
        case POSIX_BRK:
            return posix_brk(p1);

        case POSIX_PEDIGREE_CREATE_WAITER:
            return reinterpret_cast<uintptr_t>(posix_pedigree_create_waiter());
        case POSIX_PEDIGREE_DESTROY_WAITER:
            posix_pedigree_destroy_waiter(reinterpret_cast<void *>(p1));
            break;
        case POSIX_PEDIGREE_THREAD_WAIT_FOR:
            return posix_pedigree_thread_wait_for(reinterpret_cast<void *>(p1));
        case POSIX_PEDIGREE_THREAD_TRIGGER:
            return posix_pedigree_thread_trigger(reinterpret_cast<void *>(p1));

        case POSIX_PEDIGREE_GET_INFO_BLOCK:
            return VirtualAddressSpace::getKernelAddressSpace()
                .getGlobalInfoBlock();

        case POSIX_SET_TLS_AREA:
            Processor::information().getCurrentThread()->setTlsBase(p1);
            return 0;

        case POSIX_FUTEX:
            return posix_futex(
                reinterpret_cast<int *>(p1), static_cast<int>(p2),
                static_cast<int>(p3),
                reinterpret_cast<const struct timespec *>(p4));
        case POSIX_UNAME:
            return posix_uname(reinterpret_cast<struct utsname *>(p1));
        case POSIX_ARCH_PRCTL:
            return posix_arch_prctl(p1, p2);
        case POSIX_CLONE:
            return posix_clone(
                state, p1, reinterpret_cast<void *>(p2),
                reinterpret_cast<int *>(p3), reinterpret_cast<int *>(p4), p5);
        case POSIX_PAUSE:
            return posix_pause();
        case POSIX_GETDENTS64:
            return posix_getdents64(
                static_cast<int>(p1), reinterpret_cast<struct dirent *>(p2),
                static_cast<int>(p3));
        case POSIX_L_SYSLOG:
            return posix_linux_syslog(p1, reinterpret_cast<char *>(p2), p3);
        case POSIX_FLOCK:
            return posix_flock(p1, p2);
        case POSIX_OPENAT:
            return posix_openat(p1, reinterpret_cast<const char *>(p2), p3, p4);
        case POSIX_MKDIRAT:
            return posix_mkdirat(p1, reinterpret_cast<const char *>(p2), p3);
        case POSIX_FCHOWNAT:
            return posix_fchownat(
                p1, reinterpret_cast<const char *>(p2), p3, p4, p5);
        case POSIX_FUTIMESAT:
            return posix_futimesat(
                p1, reinterpret_cast<const char *>(p2),
                reinterpret_cast<struct timeval *>(p3));
        case POSIX_UNLINKAT:
            return posix_unlinkat(p1, reinterpret_cast<const char *>(p2), p3);
        case POSIX_RENAMEAT:
            return posix_renameat(
                p1, reinterpret_cast<const char *>(p2), p3,
                reinterpret_cast<const char *>(p4));
        case POSIX_LINKAT:
            return posix_linkat(
                p1, reinterpret_cast<const char *>(p2), p3,
                reinterpret_cast<const char *>(p4), p5);
        case POSIX_SYMLINKAT:
            return posix_symlinkat(
                reinterpret_cast<const char *>(p1), p2,
                reinterpret_cast<const char *>(p3));
        case POSIX_READLINKAT:
            return posix_readlinkat(
                p1, reinterpret_cast<const char *>(p2),
                reinterpret_cast<char *>(p3), p4);
        case POSIX_FCHMODAT:
            return posix_fchmodat(
                p1, reinterpret_cast<const char *>(p2), p3, p4);
        case POSIX_FACCESSAT:
            return posix_faccessat(
                p1, reinterpret_cast<const char *>(p2), p3, p4);
        case POSIX_FSTATAT:
            return posix_fstatat(
                p1, reinterpret_cast<const char *>(p2),
                reinterpret_cast<struct stat *>(p3), p4);
        case POSIX_SETGROUPS:
            return posix_setgroups(p1, reinterpret_cast<const gid_t *>(p2));
        case POSIX_GETRLIMIT:
            return posix_getrlimit(p1, reinterpret_cast<struct rlimit *>(p2));
        case POSIX_GETPRIORITY:
            return posix_getpriority(p1, p2);
        case POSIX_SETPRIORITY:
            return posix_setpriority(p1, p2, p3);
        case POSIX_GETXATTR:
            return posix_getxattr(
                reinterpret_cast<const char *>(p1),
                reinterpret_cast<const char *>(p2),
                reinterpret_cast<void *>(p3), p4);
        case POSIX_LGETXATTR:
            return posix_lgetxattr(
                reinterpret_cast<const char *>(p1),
                reinterpret_cast<const char *>(p2),
                reinterpret_cast<void *>(p3), p4);
        case POSIX_FGETXATTR:
            return posix_fgetxattr(
                p1, reinterpret_cast<const char *>(p2),
                reinterpret_cast<void *>(p3), p4);
        case POSIX_MKNOD:
            return posix_mknod(reinterpret_cast<const char *>(p1), p2, p3);
        case POSIX_SETREUID:
            return posix_setreuid(p1, p2);
        case POSIX_SETREGID:
            return posix_setregid(p1, p2);
        case POSIX_SETRESUID:
            return posix_setresuid(p1, p2, p3);
        case POSIX_SETRESGID:
            return posix_setresgid(p1, p2, p3);
        case POSIX_GETRESUID:
            return posix_getresuid(
                reinterpret_cast<uid_t *>(p1), reinterpret_cast<uid_t *>(p2),
                reinterpret_cast<uid_t *>(p3));
        case POSIX_GETRESGID:
            return posix_getresgid(
                reinterpret_cast<gid_t *>(p1), reinterpret_cast<gid_t *>(p2),
                reinterpret_cast<gid_t *>(p3));
        case POSIX_STATFS:
            return posix_statfs(
                reinterpret_cast<const char *>(p1),
                reinterpret_cast<struct statfs *>(p2));
        case POSIX_FSTATFS:
            return posix_fstatfs(p1, reinterpret_cast<struct statfs *>(p2));
        case POSIX_SETHOSTNAME:
            return posix_sethostname(reinterpret_cast<const char *>(p1), p2);
        case POSIX_IOPERM:
            return posix_ioperm(p1, p2, p3);
        case POSIX_IOPL:
            return posix_iopl(p1);
        case POSIX_CREAT:
            return posix_open(
                reinterpret_cast<const char *>(p1),
                O_WRONLY | O_CREAT | O_TRUNC, p2);
        case POSIX_SET_ROBUST_LIST:
            return posix_set_robust_list(
                reinterpret_cast<struct robust_list_head *>(p1), p2);
        case POSIX_GET_ROBUST_LIST:
            return posix_get_robust_list(
                p1, reinterpret_cast<struct robust_list_head **>(p2),
                reinterpret_cast<size_t *>(p3));
        case POSIX_GETGROUPS:
            return posix_getgroups(p1, reinterpret_cast<gid_t *>(p2));
        case POSIX_MOUNT:
            return posix_mount(
                reinterpret_cast<const char *>(p1),
                reinterpret_cast<const char *>(p2),
                reinterpret_cast<const char *>(p3), p4,
                reinterpret_cast<const void *>(p5));
        case POSIX_SETTIMEOFDAY:
            return posix_settimeofday(
                reinterpret_cast<const struct timeval *>(p1),
                reinterpret_cast<const struct timezone *>(p2));
        case POSIX_SETRLIMIT:
            return posix_setrlimit(
                p1, reinterpret_cast<const struct rlimit *>(p2));
        case POSIX_TIME:
            return posix_time(reinterpret_cast<time_t *>(p1));
        case POSIX_GETITIMER:
            return posix_getitimer(
                p1, reinterpret_cast<struct itimerval *>(p2));
        case POSIX_SETITIMER:
            return posix_setitimer(
                p1, reinterpret_cast<const struct itimerval *>(p2),
                reinterpret_cast<struct itimerval *>(p3));
        case POSIX_SOCKETPAIR:
            return posix_socketpair(p1, p2, p3, reinterpret_cast<int *>(p4));
        case POSIX_SENDMSG:
            return posix_sendmsg(
                p1, reinterpret_cast<const struct msghdr *>(p2), p3);
        case POSIX_RECVMSG:
            return posix_recvmsg(p1, reinterpret_cast<struct msghdr *>(p2), p3);
        case POSIX_CAPGET:
            return posix_capget(
                reinterpret_cast<void *>(p1), reinterpret_cast<void *>(p2));
        case POSIX_CAPSET:
            return posix_capset(
                reinterpret_cast<void *>(p1),
                reinterpret_cast<const void *>(p2));
        case POSIX_PRCTL:
            return posix_prctl(p1, p2, p3, p4, p5);

        default:
            ERROR(
                "PosixSyscallManager: invalid syscall received: "
                << Dec << syscallNumber << Hex);
            SYSCALL_ERROR(Unimplemented);
            return -1;
    }

    return 0;
}
