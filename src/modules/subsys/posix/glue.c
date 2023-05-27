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

#include "posixSyscallNumbers.h"

// Define errno before including syscall.h.
#include "errno.h"
#define errno (*__errno())
extern int *__errno(void);
int h_errno;  // required by networking code

#include "posix-syscall.h"

#include "newlib.h"

#include <ctype.h>
#include <malloc.h>
#include <mntent.h>
#include <netinet/in.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>

#include <sys/mount.h>
#include <sys/resource.h>

#include <sys/reent.h>

#include <setjmp.h>

#include <pedigree_config.h>

#define PEDIGREE_SYSCALLS_LIBC
#include <pedigree-syscalls.h>

#define BS8(x) (x)
#define BS16(x) (((x & 0xFF00) >> 8) | ((x & 0x00FF) << 8))
#define BS32(x)                                           \
    (((x & 0xFF000000) >> 24) | ((x & 0x00FF0000) >> 8) | \
     ((x & 0x0000FF00) << 8) | ((x & 0x000000FF) << 24))
#define BS64(x) (x)

#ifdef LITTLE_ENDIAN

#define LITTLE_TO_HOST8(x) (x)
#define LITTLE_TO_HOST16(x) (x)
#define LITTLE_TO_HOST32(x) (x)
#define LITTLE_TO_HOST64(x) (x)

#define HOST_TO_LITTLE8(x) (x)
#define HOST_TO_LITTLE16(x) (x)
#define HOST_TO_LITTLE32(x) (x)
#define HOST_TO_LITTLE64(x) (x)

#define BIG_TO_HOST8(x) BS8((x))
#define BIG_TO_HOST16(x) BS16((x))
#define BIG_TO_HOST32(x) BS32((x))
#define BIG_TO_HOST64(x) BS64((x))

#define HOST_TO_BIG8(x) BS8((x))
#define HOST_TO_BIG16(x) BS16((x))
#define HOST_TO_BIG32(x) BS32((x))
#define HOST_TO_BIG64(x) BS64((x))

#else  // else Big endian

#define BIG_TO_HOST8(x) (x)
#define BIG_TO_HOST16(x) (x)
#define BIG_TO_HOST32(x) (x)
#define BIG_TO_HOST64(x) (x)

#define HOST_TO_BIG8(x) (x)
#define HOST_TO_BIG16(x) (x)
#define HOST_TO_BIG32(x) (x)
#define HOST_TO_BIG64(x) (x)

#define LITTLE_TO_HOST8(x) BS8((x))
#define LITTLE_TO_HOST16(x) BS16((x))
#define LITTLE_TO_HOST32(x) BS32((x))
#define LITTLE_TO_HOST64(x) BS64((x))

#define HOST_TO_LITTLE8(x) BS8((x))
#define HOST_TO_LITTLE16(x) BS16((x))
#define HOST_TO_LITTLE32(x) BS32((x))
#define HOST_TO_LITTLE64(x) BS64((x))

#endif

// #define MAXNAMLEN 255

const char *safepathchars =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-";
#define SAFE_PATH_LEN (sizeof safepathchars)

#define STUBBED(str)                       \
    syscall1(POSIX_STUBBED, (long) (str)); \
    errno = ENOSYS;

#define NUM_ATFORK_HANDLERS 32  // (* 3)

// For getopt(3).
int optreset = 0;

const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;
const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;

// Defines an fork handler
struct forkHandler
{
    void (*prepare)(void);
    void (*parent)(void);
    void (*child)(void);
};

// Tables of handlers
static struct forkHandler atforkHandlers[NUM_ATFORK_HANDLERS];

// Number of handlers (also an index to the next one)
static int nHandlers = 0;

int ftruncate(int a, off_t b)
{
    return syscall2(POSIX_FTRUNCATE, a, (long) b);
}

int truncate(const char *path, off_t length)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return fd;
    int r = ftruncate(fd, length);
    close(fd);

    return r;
}

char *getcwd(char *buf, unsigned long size)
{
    if (buf && !size)
    {
        errno = EINVAL;
        return 0;
    }
    else if (!buf && !size)
    {
        size = PATH_MAX;
    }

    // buf == null is unspecified but used by bash.
    int malloced = 0;
    if (!buf)
    {
        buf = (char *) malloc(size);
        if (!buf)
        {
            errno = ENOMEM;
            return 0;
        }
        malloced = 1;
    }
    long r = syscall2(POSIX_GETCWD, (long) buf, (long) size);
    char *result = (char *) r;
    if (!result)
    {
        if (malloced)
        {
            free(buf);
        }

        return 0;
    }

    return (char *) result;
}

int mkdir(const char *p, mode_t mode)
{
    return (long) syscall2(POSIX_MKDIR, (long) p, mode);
}

int close(int file)
{
    return (long) syscall1(POSIX_CLOSE, file);
}

int _execve(char *name, char **argv, char **env)
{
    return (long) syscall3(POSIX_EXECVE, (long) name, (long) argv, (long) env);
}

void _exit(int val)
{
    syscall1(POSIX_EXIT, val);
    while (1)
        ;
}

int fork(void)
{
    if (nHandlers)
    {
        for (int i = 0; i < nHandlers; i++)
        {
            if (atforkHandlers[i].prepare)
                atforkHandlers[i].prepare();
        }
    }

    int pid = (long) syscall0(POSIX_FORK);

    if (pid == 0)
    {
        if (nHandlers)
        {
            for (int i = 0; i < nHandlers; i++)
            {
                if (atforkHandlers[i].child)
                    atforkHandlers[i].child();
            }
        }
    }
    else if (pid > 0)
    {
        if (nHandlers)
        {
            for (int i = 0; i < nHandlers; i++)
            {
                if (atforkHandlers[i].parent)
                    atforkHandlers[i].parent();
            }
        }
    }

    return pid;
}

int vfork(void)
{
    return fork();
}

int fstat(int file, struct stat *st)
{
    return (long) syscall2(POSIX_FSTAT, (long) file, (long) st);
}

int _isatty(int file)
{
    return (long) syscall1(POSIX_ISATTY, file);
}

int link(const char *old, const char *_new)
{
    return (long) syscall2(POSIX_LINK, (long) old, (long) _new);
}

off_t lseek(int file, off_t ptr, int dir)
{
    return (off_t) syscall3(POSIX_LSEEK, file, ptr, dir);
}

int open(const char *name, int flags, ...)  // , mode_t mode)
{
    // Try to handle invalid arguments early, before we go to the effort of the
    // system call...
    if (!name)
    {
        errno = EINVAL;
        return -1;
    }

    // Only O_CREAT requires the 'mode' parameter.
    mode_t mode = 0;
    if (flags & O_CREAT)
    {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }

    return (long) syscall3(POSIX_OPEN, (long) name, flags, mode);
}

_READ_WRITE_RETURN_TYPE read(int file, void *ptr, size_t len)
{
    if (file < 0)
    {
        syslog(LOG_NOTICE, "[%d] read: bad file given\n", getpid());
        errno = EBADF;
        return -1;
    }
    if (len == 0)
    {
        syslog(LOG_NOTICE, "[%d] read: bad length given\n", getpid());
        errno = EINVAL;
        return 0;
    }
    return (_READ_WRITE_RETURN_TYPE) syscall3(
        POSIX_READ, file, (long) ptr, len);
}

void *sbrk(ptrdiff_t incr)
{
    uintptr_t result = syscall1(POSIX_SBRK, incr);
    return (void *) result;
}

int stat(const char *file, struct stat *st)
{
    return (long) syscall2(POSIX_STAT, (long) file, (long) st);
}

#if !PPC_COMMON
clock_t times(struct tms *buf)
{
    return syscall1(POSIX_TIMES, (long) buf);
}
#endif

int utimes(const char *filename, const struct timeval times[2])
{
    return syscall2(POSIX_UTIMES, (long) filename, (long) times);
}

int unlink(const char *name)
{
    return (long) syscall1(POSIX_UNLINK, (long) name);
}

int wait(int *status)
{
    return waitpid(-1, status, 0);
}

int waitpid(int pid, int *status, int options)
{
    return (long) syscall3(POSIX_WAITPID, pid, (long) status, options);
}

_READ_WRITE_RETURN_TYPE write(int file, const void *ptr, size_t len)
{
    if (file < 0)
    {
        syslog(LOG_NOTICE, "[%d] write: bad file given\n", getpid());
        errno = EBADF;
        return -1;
    }
    return (_READ_WRITE_RETURN_TYPE) syscall3(
        POSIX_WRITE, file, (long) ptr, len);
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
    if (!iov || !iovcnt || (fd == -1))
    {
        errno = EINVAL;
        return -1;
    }

    ssize_t ret = 0;
    int i;
    for (i = 0; i < iovcnt; i++)
    {
        if (iov[i].iov_base)
        {
            ssize_t r = read(fd, iov[i].iov_base, iov[i].iov_len);
            ret += r;
        }
    }
    return ret;
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    if (!iov || !iovcnt || (fd == -1))
    {
        errno = EINVAL;
        return -1;
    }

    ssize_t ret = 0;
    int i;
    for (i = 0; i < iovcnt; i++)
    {
        if (iov[i].iov_base)
        {
            ssize_t r = write(fd, iov[i].iov_base, iov[i].iov_len);
            ret += r;
        }
    }
    return ret;
}

int lstat(const char *file, struct stat *st)
{
    return (long) syscall2(POSIX_LSTAT, (long) file, (long) st);
}

DIR *opendir(const char *dir)
{
    DIR *p = (DIR *) malloc(sizeof(DIR));
    int r = syscall2(POSIX_OPENDIR, (long) dir, (long) p);
    if (r < 0 || p->fd < 0)
    {
        free(p);
        return 0;
    }
    return p;
}

struct dirent *readdir(DIR *dir)
{
    if (!dir)
    {
        errno = EINVAL;
        return 0;
    }

    int old_errno = errno;

    if (dir->fd < 0)
    {
        // Bad DIR object.
        errno = EINVAL;
        return 0;
    }

    if (dir->totalpos >= dir->count)
    {
        // End of directory. errno remains unchanged.
        return 0;
    }
    else if (dir->pos >= 64)
    {
        // Buffer the next batch of entries.
        if (syscall1(POSIX_READDIR, (long) dir) < 0)
        {
            // Failed to buffer more entries!
            return 0;
        }
        dir->pos = 1;
        dir->totalpos++;
        return &dir->ent[0];
    }
    else
    {
        struct dirent *result = &dir->ent[dir->pos];
        dir->pos++;
        dir->totalpos++;
        return result;
    }
}

void rewinddir(DIR *dir)
{
    if (!dir)
    {
        return;
    }

    if (dir->totalpos < 64)
    {
        // Don't need to re-buffer.
        dir->pos = dir->totalpos = 0;
    }
    else if (dir->totalpos != 0)
    {
        dir->pos = dir->totalpos = 0;
        syscall1(POSIX_READDIR, (long) dir);
    }
}

int closedir(DIR *dir)
{
    if (!dir)
    {
        errno = EINVAL;
        return -1;
    }

    syscall1(POSIX_CLOSEDIR, (long) dir);
    free(dir);
    return 0;
}

int _rename(const char *old, const char *new)
{
    return (long) syscall2(POSIX_RENAME, (long) old, (long) new);
}

int tcgetattr(int fd, struct termios *p)
{
    return (long) syscall2(POSIX_TCGETATTR, fd, (long) p);
}

int tcsetattr(int fd, int optional_actions, const struct termios *p)
{
    return (long) syscall3(POSIX_TCSETATTR, fd, optional_actions, (long) p);
}

int tcsendbreak(int fildes, int duration)
{
    STUBBED("tcsendbreak");
    return 0;
}

int tcdrain(int fd)
{
    STUBBED("tcdrain");
    return -1;
}

int tcflush(int fd, int queue_selector)
{
    intptr_t selector = queue_selector;
    return ioctl(fd, TIOCFLUSH, (void *) selector);
}

int tcflow(int fd, int action)
{
    STUBBED("tcflow");
    return 0;
}

void cfmakeraw(struct termios *t)
{
    t->c_iflag &=
        ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t->c_oflag &= ~OPOST;
    t->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag &= ~(CSIZE | PARENB);
    t->c_cflag |= CS8;
}

int cfgetospeed(const struct termios *t)
{
    STUBBED("cfgetospeed");
    return 0;
}

int cfgetispeed(const struct termios *t)
{
    STUBBED("cfgetispeed");
    return 0;
}

int cfsetospeed(const struct termios *t, int speed)
{
    STUBBED("cfsetospeed");
    return 0;
}

int cfsetispeed(const struct termios *t, int speed)
{
    STUBBED("cfsetispeed");
    return 0;
}

int tcsetpgrp(int fd, pid_t pgid_id)
{
    return (int) syscall2(POSIX_TCSETPGRP, fd, pgid_id);
}

pid_t tcgetpgrp(int fd)
{
    return (pid_t) syscall1(POSIX_TCGETPGRP, fd);
}

int mkfifo(const char *_path, mode_t __mode)
{
    STUBBED("mkfifo");
    return -1;
}

int gethostname(char *name, size_t len)
{
    int result;

    if (!name || !len)
        return -1;

    result = pedigree_config_query(
        "select * from 'network_generic' WHERE `key` = 'hostname';");
    if ((result == -1) || (pedigree_config_was_successful(result) == -1) ||
        (pedigree_config_numrows(result) == 0))
    {
        if (result != -1)
            pedigree_config_freeresult(result);
        strncpy(name, "pedigree", len);
        return 0;
    }

    pedigree_config_getstr_s(result, 0, "value", name, len);
    pedigree_config_freeresult(result);
    return 0;
}

int sethostname(char *name, size_t len)
{
    if (!name || len > 255 || !len)
    {
        errno = EINVAL;
        return -1;
    }

    // Need to add permission and name checking

    const char *query =
        "update 'network_generic' set `value`= '%s' WHERE `key` = 'hostname'";
    char *tmp = pedigree_config_escape_string(name);
    char *buffer = (char *) malloc(strlen(query) + strlen(tmp) - 2 + 1);

    sprintf(buffer, query, tmp);

    int result = pedigree_config_query(buffer);

    if (result != -1)
        pedigree_config_freeresult(result);

    free(tmp);
    free(buffer);

    return 0;
}

int ioctl(int fd, int command, ...)
{
    va_list ap;
    va_start(ap, command);

    /// \todo Properly handle the varargs here.
    void *buf = va_arg(ap, void *);

    va_end(ap);

    return (long) syscall3(POSIX_IOCTL, fd, command, (long) buf);
}

const char *const sys_siglist[] = {0,
                                   "Hangup",
                                   "Interrupt",
                                   "Quit",
                                   "Illegal instruction",
                                   "Trap",
                                   "IOT",
                                   "Abort",
                                   "EMT",
                                   "Floating point exception",
                                   "Kill",
                                   "Bus error",
                                   "Segmentation violation",
                                   "Bad argument to system call",
                                   "Pipe error",
                                   "Alarm",
                                   "Terminate"};

const char *strsignal(int sig)
{
    if (sig < 16)
        return sys_siglist[sig];
    else
        return "Unknown";
}

uid_t getuid(void)
{
    return (long) syscall0(POSIX_GETUID);
}

gid_t getgid(void)
{
    return (long) syscall0(POSIX_GETGID);
}

uid_t geteuid(void)
{
    return (uid_t) syscall0(POSIX_GETEUID);
}

gid_t getegid(void)
{
    return (gid_t) syscall0(POSIX_GETEGID);
}

int setuid(uid_t uid)
{
    return syscall1(POSIX_SETUID, uid);
}

int setgid(gid_t gid)
{
    return syscall1(POSIX_SETGID, gid);
}

int seteuid(uid_t uid)
{
    return syscall1(POSIX_SETEUID, uid);
}

int setegid(gid_t gid)
{
    return syscall1(POSIX_SETEGID, gid);
}

int setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
    STUBBED("setresuid");
    return -1;
}

int setresgid(gid_t rgid, gid_t egid, gid_t sgid)
{
    STUBBED("setresgid");
    return -1;
}

int issetugid()
{
    STUBBED("issetugid");
    return 0;
}

unsigned int alarm(unsigned int seconds)
{
    return (unsigned int) syscall1(POSIX_ALARM, seconds);
}

mode_t umask(mode_t mask)
{
    return syscall1(POSIX_UMASK, mask);
}

int chmod(const char *path, mode_t mode)
{
    return syscall2(POSIX_CHMOD, (long) path, mode);
}

int fchmod(int fildes, mode_t mode)
{
    return syscall2(POSIX_FCHMOD, fildes, mode);
}

int chown(const char *path, uid_t owner, gid_t group)
{
    return syscall3(POSIX_CHOWN, (long) path, owner, group);
}

int fchown(int fildes, uid_t owner, uid_t group)
{
    return syscall3(POSIX_FCHOWN, fildes, owner, group);
}

int utime(const char *path, const struct utimbuf *times)
{
    return syscall2(POSIX_UTIME, (long) path, (long) times);
}

int access(const char *path, int amode)
{
    return (long) syscall2(POSIX_ACCESS, (long) path, amode);
}

const char *const sys_errlist[] = {};
const int sys_nerr = 0;
long timezone;

long pathconf(const char *path, int name)
{
    STUBBED("pathconf");
    return 0;
}

long fpathconf(int filedes, int name)
{
    STUBBED("fpathconf");
    return 0;
}

int select(
    int nfds, struct fd_set *readfds, struct fd_set *writefds,
    struct fd_set *errorfds, struct timeval *timeout)
{
    return (long) syscall5(
        POSIX_SELECT, nfds, (long) readfds, (long) writefds, (long) errorfds,
        (long) timeout);
}

void setgrent(void)
{
    STUBBED("setgrent");
}

void endgrent(void)
{
    STUBBED("endgrent");
}

struct group *getgrent(void)
{
    STUBBED("getgrent");
    errno = ENOSYS;
    return 0;
}

static struct passwd g_passwd;
int g_passwd_num = 0;
char g_passwd_str[256];
void setpwent(void)
{
    g_passwd_num = 0;
}

void endpwent(void)
{
    g_passwd_num = 0;
}

struct passwd *getpwent(void)
{
    if (syscall3(
            POSIX_GETPWENT, (long) &g_passwd, g_passwd_num,
            (long) &g_passwd_str) != 0)
        return 0;
    g_passwd_num++;
    return &g_passwd;
}

struct passwd *getpwuid(uid_t uid)
{
    if (syscall3(POSIX_GETPWENT, (long) &g_passwd, uid, (long) &g_passwd_str) !=
        0)
        return 0;
    return &g_passwd;
}

struct passwd *getpwnam(const char *name)
{
    if (syscall3(
            POSIX_GETPWNAM, (long) &g_passwd, (long) name,
            (long) &g_passwd_str) != 0)
        return 0;
    return &g_passwd;
}

int chdir(const char *path)
{
    return (long) syscall1(POSIX_CHDIR, (long) path);
}

int fchdir(int fildes)
{
    return syscall1(POSIX_FCHDIR, fildes);
}

int dup(int fileno)
{
    return (long) syscall1(POSIX_DUP, fileno);
}

int dup2(int fildes, int fildes2)
{
    return (long) syscall2(POSIX_DUP2, fildes, fildes2);
}

int pipe(int filedes[2])
{
    return (long) syscall1(POSIX_PIPE, (long) filedes);
}

int fcntl(int fildes, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    return syscall3(POSIX_FCNTL, fildes, cmd, (long) arg);
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oset)
{
    return (long) syscall3(POSIX_SIGPROCMASK, how, (long) set, (long) oset);
}

int rmdir(const char *path)
{
    return syscall1(POSIX_RMDIR, (long) path);
}

int socket(int domain, int type, int protocol)
{
    return (long) syscall3(POSIX_SOCKET, domain, type, protocol);
}

int connect(int sock, const struct sockaddr *address, size_t addrlen)
{
    return (long) syscall3(POSIX_CONNECT, sock, (long) address, (long) addrlen);
}

ssize_t send(int sock, const void *buff, size_t bufflen, int flags)
{
    return (ssize_t) syscall4(
        POSIX_SEND, sock, (long) buff, (long) bufflen, flags);
}

ssize_t recv(int sock, void *buff, size_t bufflen, int flags)
{
    return (ssize_t) syscall4(
        POSIX_RECV, sock, (long) buff, (long) bufflen, flags);
}

int accept(int sock, struct sockaddr *remote_addr, size_t *addrlen)
{
    return (long) syscall3(
        POSIX_ACCEPT, sock, (long) remote_addr, (long) addrlen);
}

int bind(int sock, const struct sockaddr *local_addr, size_t addrlen)
{
    return (long) syscall3(POSIX_BIND, sock, (long) local_addr, (long) addrlen);
}

int getpeername(int sock, struct sockaddr *addr, size_t *addrlen)
{
    return syscall3(POSIX_GETPEERNAME, sock, (long) addr, (long) addrlen);
}

int getsockname(int sock, struct sockaddr *addr, size_t *addrlen)
{
    return syscall3(POSIX_GETSOCKNAME, sock, (long) addr, (long) addrlen);
}

int getsockopt(int sock, int level, int optname, void *optvalue, size_t *optlen)
{
    return syscall5(
        POSIX_GETSOCKOPT, sock, level, optname, (long) optvalue, (long) optlen);
}

int listen(int sock, int backlog)
{
    return (long) syscall2(POSIX_LISTEN, sock, backlog);
}

struct special_send_data
{
    int sock;
    const void *buff;
    size_t bufflen;
    int flags;
    const struct sockaddr *remote_addr;
    const socklen_t *addrlen;
} __attribute__((packed));

struct special_recv_data
{
    int sock;
    void *buff;
    size_t bufflen;
    int flags;
    struct sockaddr *remote_addr;
    socklen_t *addrlen;
} __attribute__((packed));

ssize_t recvfrom(
    int sock, void *buff, size_t bufflen, int flags,
    struct sockaddr *remote_addr, size_t *addrlen)
{
    struct special_recv_data *tmp =
        (struct special_recv_data *) malloc(sizeof(struct special_recv_data));
    tmp->sock = sock;
    tmp->buff = buff;
    tmp->bufflen = bufflen;
    tmp->flags = flags;
    tmp->remote_addr = remote_addr;
    tmp->addrlen = addrlen;

    int ret = syscall1(POSIX_RECVFROM, (long) tmp);

    free(tmp);

    return ret;
}

ssize_t recvmsg(int sock, struct msghdr *msg, int flags)
{
    STUBBED("recvmsg");
    return -1;
}

ssize_t sendmsg(int sock, const struct msghdr *msg, int flags)
{
    STUBBED("sendmsg");
    return -1;
}

ssize_t sendto(
    int sock, const void *buff, size_t bufflen, int flags,
    const struct sockaddr *remote_addr, socklen_t addrlen)
{
    struct special_send_data *tmp =
        (struct special_send_data *) malloc(sizeof(struct special_send_data));
    tmp->sock = sock;
    tmp->buff = buff;
    tmp->bufflen = bufflen;
    tmp->flags = flags;
    tmp->remote_addr = remote_addr;
    tmp->addrlen = &addrlen;

    int ret = syscall1(POSIX_SENDTO, (long) tmp);

    free(tmp);

    return ret;
}

int setsockopt(
    int sock, int level, int optname, const void *optvalue,
    unsigned long optlen)
{
    STUBBED("setsockopt");
    return 0;
}

int shutdown(int sock, int how)
{
    return (long) syscall2(POSIX_SHUTDOWN, sock, how);
}

int sockatmark(int sock)
{
    STUBBED("sockatmark");
    return -1;
}

int socketpair(int domain, int type, int protocol, int sock_vec[2])
{
    STUBBED("socketpair");
    return -1;
}

struct group *getgrnam(const char *name)
{
    static struct group ret = {0, 0, 0, 0};

    if (ret.gr_name)
        free(ret.gr_name);
    if (ret.gr_passwd)
        free(ret.gr_passwd);

    ret.gr_name = (char *) malloc(256);
    ret.gr_passwd = (char *) malloc(256);
    int r = syscall2(POSIX_GETGRNAM, (long) name, (long) &ret);
    if (r < 0)
    {
        return 0;
    }

    return &ret;
}

struct group *getgrgid(gid_t id)
{
    static struct group ret = {0, 0, 0, 0};

    ret.gr_name = (char *) malloc(256);
    ret.gr_passwd = (char *) malloc(256);
    int r = syscall2(POSIX_GETGRGID, id, (long) &ret);
    if (r < 0)
    {
        return 0;
    }

    return &ret;
}

int symlink(const char *path1, const char *path2)
{
    return (long) syscall2(POSIX_SYMLINK, (long) path1, (long) path2);
}

int fsync(int fd)
{
    return syscall1(POSIX_FSYNC, fd);
}

ssize_t readlink(const char *path, char *buf, size_t bufsize)
{
    return (long) syscall3(POSIX_READLINK, (long) path, (long) buf, bufsize);
}

int ftime(struct timeb *tp)
{
    STUBBED("ftime");
    return -1;
}

int sigmask(void)
{
    STUBBED("sigmask");
    return -1;
}

int sigblock(void)
{
    STUBBED("sigblock");
    return -1;
}

int sigsetmask(int mask)
{
    STUBBED("sigsetmask");
    return -1;
}

int siggetmask(void)
{
    STUBBED("siggetmask");
    return -1;
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *oact)
{
    return (long) syscall3(POSIX_SIGACTION, sig, (long) act, (long) oact);
}

_sig_func_ptr signal(int s, _sig_func_ptr func)
{
    // Obtain the old mask for the sigaction structure, fill it in with default
    // arguments and pass on to sigaction
    static struct sigaction act;
    static struct sigaction tmp;
    unsigned long mask = 0;
    sigprocmask(0, 0, &mask);
    act.sa_mask = mask;
    act.sa_handler = func;
    act.sa_flags = 0;
    memset(&tmp, 0, sizeof(struct sigaction));
    if (sigaction(s, &act, &tmp) == 0)
    {
        return tmp.sa_handler;
    }

    // errno set by sigaction
    return (_sig_func_ptr) -1;
}

int kill(pid_t pid, int sig)
{
    return (long) syscall2(POSIX_KILL, pid, sig);
}

int sigpending(long *set)
{
    STUBBED("sigpending");
    return -1;
}

int sigsuspend(const long *sigmask)
{
    STUBBED("sigsuspend");
    return -1;
}

void _init_signals(void)
{
    // syscall0(PEDIGREE_INIT_SIGRET);
}

int fdatasync(int fildes)
{
    /// \todo fdatasync isn't meant to flush metadata, while fsync is
    return syscall1(POSIX_FSYNC, fildes);
}

struct dlHandle
{
    int mode;
};

extern void *_libload_dlopen(const char *, int) __attribute__((weak));
extern void *_libload_dlsym(void *, const char *) __attribute__((weak));
extern int _libload_dlclose(void *) __attribute__((weak));

void *dlopen(const char *file, int mode)
{
    return _libload_dlopen(file, mode);
}

void *dlsym(void *handle, const char *name)
{
    return _libload_dlsym(handle, name);
}

int dlclose(void *handle)
{
    return _libload_dlclose(handle);
}

char *dlerror(void)
{
    STUBBED("dlerror");
    return 0;
}

int poll(struct pollfd fds[], unsigned int nfds, int timeout)
{
    return (long) syscall3(POSIX_POLL, (long) fds, nfds, timeout);
}

unsigned int htonl(unsigned int n)
{
    return HOST_TO_BIG32(n);
}
unsigned int ntohl(unsigned int n)
{
    return BIG_TO_HOST32(n);
}

unsigned short htons(unsigned short n)
{
    return HOST_TO_BIG16(n);
}
unsigned short ntohs(unsigned short n)
{
    return BIG_TO_HOST16(n);
}

void sync(void)
{
    STUBBED("sync");
}

int mknod(const char *path, mode_t mode, dev_t dev)
{
    STUBBED("mknod");
    return -1;
}

int getpwuid_r(
    uid_t uid, struct passwd *pwd, char *buffer, size_t bufsize,
    struct passwd **result)
{
    STUBBED("getpwuid_r");
    return -1;
}

int getgrgid_r(
    gid_t gid, struct group *grp, char *buffer, size_t bufsize,
    struct group **result)
{
    STUBBED("getgrgid_r");
    return -1;
}

int getpwnam_r(
    const char *name, struct passwd *pwd, char *buffer, size_t bufsize,
    struct passwd **result)
{
    STUBBED("getpwnam_r");
    return -1;
}

int getgrnam_r(
    const char *name, struct group *grp, char *buffer, size_t bufsize,
    struct group **result)
{
    STUBBED("getgrnam_r");
    return -1;
}

void err(int eval, const char *fmt, ...) _ATTRIBUTE((noreturn));
void err(int eval, const char *fmt, ...)
{
    printf(
        "err: %d: (todo: print format string based on arguments): %s\n", errno,
        strerror(errno));
    exit(eval);
}

long timegm(struct tm *tm)
{
    STUBBED("timegm");
    return -1;
}

int chroot(const char *path)
{
    return syscall1(POSIX_CHROOT, (long) path);
}

char *mkdtemp(char *template)
{
    if (!template)
    {
        errno = EINVAL;
        return 0;
    }

    // Check for correct template - ends in 6 'X' characters.
    size_t template_len = strlen(template);
    if (template_len < 6)
    {
        errno = EINVAL;
        return 0;
    }

    for (size_t i = (template_len - 6); i < template_len; ++i)
    {
        if (template[i] != 'X')
        {
            errno = EINVAL;
            return 0;
        }
    }

    while (1)
    {
        // Generate a filename.
        for (size_t i = (template_len - 6); i < template_len; ++i)
            template[i] = safepathchars[rand() % SAFE_PATH_LEN];

        if (mkdir(template, 0700) == 0)
            return template;
        else if (errno != EEXIST)
        {
            // eg ENOENT, ENOTDIR, EROFS, etc...
            return 0;
        }
    }

    return 0;
}

int getitimer(int which, struct itimerval *value)
{
    STUBBED("getitimer");
    return -1;
}

int setitimer(
    int which, const struct itimerval *value, struct itimerval *ovalue)
{
    STUBBED("setitimer");
    return -1;
}

struct _mmap_tmp
{
    void *addr;
    size_t len;
    int prot;
    int flags;
    int fildes;
    off_t off;
};

void *mmap(void *addr, size_t len, int prot, int flags, int fildes, off_t off)
{
    struct _mmap_tmp t;
    t.addr = addr;
    t.len = len;
    t.prot = prot;
    t.flags = flags;
    t.fildes = fildes;
    t.off = off;

    uintptr_t result = syscall1(POSIX_MMAP, (long) &t);
    return (void *) result;
}

int msync(void *addr, size_t len, int flags)
{
    return (int) syscall3(POSIX_MSYNC, (long) addr, (long) len, flags);
}

int munmap(void *addr, size_t len)
{
    return (long) syscall2(POSIX_MUNMAP, (long) addr, (long) len);
}

int getgroups(int gidsetsize, gid_t grouplist[])
{
    if (gidsetsize == 0)
    {
        return 1;
    }
    else
    {
        if (!grouplist)
        {
            errno = EINVAL;
            return -1;
        }

        grouplist[0] = getgid();
        return 1;
    }
}

size_t getpagesize(void)
{
    // Avoid masses of system calls by assuming the page size doesn't actually
    // ever change.
    static size_t sz = (size_t) ~0;
    if (sz == (size_t) ~0)
        sz = sysconf(_SC_PAGESIZE);
    return sz;
}

char *realpath(const char *file_name, char *resolved_name)
{
    if (!file_name)
    {
        errno = EINVAL;
        return 0;
    }

    if (!resolved_name)
    {
        resolved_name = (char *) malloc(PATH_MAX);
    }

    int n = syscall3(
        POSIX_REALPATH, (long) file_name, (long) resolved_name, PATH_MAX);
    if (n != 0)
        return 0;

    return resolved_name;
}

pid_t setsid(void)
{
    return syscall0(POSIX_SETSID);
}

int setpgid(pid_t pid, pid_t pgid)
{
    return syscall2(POSIX_SETPGID, (long) pid, (long) pgid);
}

pid_t getpgid(pid_t pid)
{
    if (pid)
    {
        errno = EINVAL;
        return (pid_t) -1;
    }

    return getpgrp();
}

pid_t getpgrp(void)
{
    return syscall0(POSIX_GETPGRP);
}

pid_t getppid(void)
{
    return syscall0(POSIX_GETPPID);
}

int getrlimit(int resource, struct rlimit *rlp)
{
    STUBBED("setrlimit");
    return -1;
}

int setrlimit(int resource, const struct rlimit *rlp)
{
    STUBBED("setrlimit");
    return -1;
}

/// \todo Write - should just be a simple ls-style read of the raw drive
int getmntinfo(struct statvfs **mntbufp, int flags)
{
    STUBBED("getmntinfo");
    return -1;
}

FILE *setmntent(const char *filename, const char *type)
{
    STUBBED("setmntent");
    return 0;
}

struct mntent *getmntent(FILE *fp)
{
    STUBBED("getmntent");
    return 0;
}

int endmntent(FILE *fp)
{
    STUBBED("endmntent");
    return -1;
}

int statvfs(const char *path, struct statvfs *buf)
{
    return syscall2(POSIX_STATVFS, (long) path, (long) buf);
}

int fstatvfs(int fd, struct statvfs *buf)
{
    return syscall2(POSIX_FSTATVFS, fd, (long) buf);
}

struct fstab *getfsent(void)
{
    STUBBED("getfsent");
    return 0;
}

struct fstab *getfsfile(const char *mount_point)
{
    STUBBED("getfsfile");
    return 0;
}

struct fstab *getfsspec(const char *special_file)
{
    STUBBED("getfsspec");
    return 0;
}

int setfsent(void)
{
    STUBBED("setfsent");
    return -1;
}

void endfsent(void)
{
    STUBBED("endfsent");
}

int getrusage(int who, struct rusage *r_usage)
{
    return syscall2(POSIX_GETRUSAGE, who, (long) r_usage);
}

int sigaltstack(const struct stack_t *stack, struct stack_t *oldstack)
{
    return syscall2(POSIX_SIGALTSTACK, (long) stack, (long) oldstack);
}

int sem_close(sem_t *sem)
{
    return syscall1(POSIX_SEM_CLOSE, (long) sem);
}

int sem_destroy(sem_t *sem)
{
    return syscall1(POSIX_SEM_DESTROY, (long) sem);
}

int sem_getvalue(sem_t *sem, int *val)
{
    return syscall2(POSIX_SEM_GETVALUE, (long) sem, (long) val);
}

int sem_init(sem_t *sem, int pshared, unsigned value)
{
    return syscall3(POSIX_SEM_INIT, (long) sem, pshared, value);
}

sem_t *sem_open(const char *name, int mode, ...)
{
    STUBBED("sem_open");
    return 0;
}

int sem_post(sem_t *sem)
{
    return syscall1(POSIX_SEM_POST, (long) sem);
}

int sem_timedwait(sem_t *sem, const struct timespec *tm)
{
    return syscall2(POSIX_SEM_TIMEWAIT, (long) sem, (long) tm);
}

int sem_trywait(sem_t *sem)
{
    return syscall1(POSIX_SEM_TRYWAIT, (long) sem);
}

int sem_unlink(const char *name)
{
    STUBBED("sem_unlink");
    return -1;
}

int sem_wait(sem_t *sem)
{
    return syscall1(POSIX_SEM_WAIT, (long) sem);
}

int pthread_atfork(
    void (*prepare)(void), void (*parent)(void), void (*child)(void))
{
    // Already full?
    if (nHandlers == NUM_ATFORK_HANDLERS)
    {
        errno = ENOMEM;
        return -1;
    }

    // Create and insert
    struct forkHandler handler;
    handler.prepare = prepare;
    handler.parent = parent;
    handler.child = child;
    atforkHandlers[nHandlers++] = handler;
    return 0;
}

void closelog()
{
}

void openlog(const char *log, int logopt, int facility)
{
}

int setlogmask(int mask)
{
    return 0;
}

void syslog(int prio, const char *fmt, ...)
{
    static char print_temp[1024];
    va_list argptr;
    va_start(argptr, fmt);
    vsnprintf(print_temp, sizeof print_temp, fmt, argptr);
    syscall2(POSIX_SYSLOG, (long) print_temp, prio);
    va_end(argptr);
}

int pause()
{
    STUBBED("pause");
    return -1;
}

pid_t forkpty(
    int *amaster, char *name, struct termios *termp, struct winsize *winp)
{
    STUBBED("forkpty");
    errno = ENOENT;
    return -1;
}

struct utmp *pututline(struct utmp *ut)
{
    STUBBED("pututline");
    return 0;
}

void logwtmp(const char *line, const char *name, const char *host)
{
    STUBBED("logwtmp");
}

unsigned if_nametoindex(const char *name)
{
    STUBBED("if_nametoindex");
    return 0;
}

char *if_indextoname(unsigned index, char *buf)
{
    STUBBED("if_indextoname");
    errno = ENXIO;
    return 0;
}

struct if_nameindex *if_nameindex()
{
    STUBBED("if_nameindex");
    errno = ENOBUFS;
    return 0;
}

void if_freenameindex(struct if_nameindex *nameindex)
{
    STUBBED("if_freenameindex");
}

int sigsetjmp(sigjmp_buf env, int savemask)
{
    // mask is not relevant currently.
    return setjmp(env);
}

void siglongjmp(sigjmp_buf env, int val)
{
    longjmp(env, val);
}

char *basename(char *path)
{
    static char bad[2] = {'.', 0};
    if ((path == NULL) || (path && !*path))
        return bad;

    char *p = strrchr(path, '/');
    if (!p)
        return path;
    else
        return p + 1;
}

int reboot(int howto)
{
    return pedigree_reboot();
}

int initgroups(const char *user, gid_t group)
{
    STUBBED("initgroups");
    return 0;
}

int setgroups(int ngroups, const gid_t *gidset)
{
    STUBBED("setgroups");
    return 0;
}

ssize_t getdelim(char **a, size_t *b, int c, FILE *d)
{
    return __getdelim(a, b, c, d);
}
ssize_t getline(char **a, size_t *b, FILE *c)
{
    return __getline(a, b, c);
}

int sched_yield()
{
    return syscall0(POSIX_SCHED_YIELD);
}

int getdtablesize()
{
    STUBBED("getdtablesize");

    struct rlimit tmp;
    getrlimit(RLIMIT_NOFILE, &tmp);
    return tmp.rlim_cur;
}

int mprotect(void *addr, size_t len, int prot)
{
    return syscall3(POSIX_MPROTECT, (long) addr, len, prot);
}

int nanosleep(const struct timespec *rqtp, struct timespec *rmtp)
{
    if (!rqtp)
    {
        errno = EINVAL;
        return -1;
    }

    return syscall2(POSIX_NANOSLEEP, (long) rqtp, (long) rmtp);
}

int clock_getres(clockid_t clock_id, struct timespec *res)
{
    if (!res)
    {
        errno = EINVAL;
        return -1;
    }

    // Nanosecond resolution.
    res->tv_nsec = 1;
    res->tv_sec = 0;

    return 0;
}

int setreuid(uid_t ruid, uid_t euid)
{
    STUBBED("setreuid");
    return 0;
}

int grantpt(int fildes)
{
    STUBBED("grantpt");
    return 0;
}

int unlockpt(int fildes)
{
    STUBBED("unlockpt");
    return 0;
}

char *ptsname(int fildes)
{
    static char ret[256] = {0};
    ret[0] = 0;
    int res = syscall2(POSIX_PTSNAME, fildes, (long) ret);
    if (res < 0)
        return 0;
    return ret;
}

char *ttyname(int fildes)
{
    static char ret[256] = {0};
    ret[0] = 0;
    int res = syscall2(POSIX_TTYNAME, fildes, (long) ret);
    if (res < 0)
        return 0;
    return ret;
}

char *crypt(const char *key, const char *salt)
{
    STUBBED("crypt");
    return 0;
}

int ffsl(long int i)
{
    return __builtin_ffs(i);
}

int ffsll(long long int i)
{
    return __builtin_ffsll(i);
}

void __pedigree_revoke_signal_context()
{
    // Call into the kernel.
    syscall0(PEDIGREE_UNWIND_SIGNAL);
}

/**
 * glue for newlib <-> dlmalloc
 * dlmalloc has locks if we compile it to have them.
 */

void *_malloc_r(struct _reent *ptr, size_t sz)
{
    return malloc(sz);
}

void *_calloc_r(struct _reent *ptr, size_t a, size_t b)
{
    return calloc(a, b);
}

void *_realloc_r(struct _reent *ptr, void *p, size_t sz)
{
    return realloc(p, sz);
}

void *_memalign_r(struct _reent *ptr, size_t align, size_t nbytes)
{
    return memalign(align, nbytes);
}

void _free_r(struct _reent *ptr, void *p)
{
    free(p);
}

int posix_openpt(int oflag)
{
    int master = 0;
    char name[16] = {0};
    const char *x, *y;

    oflag &= O_RDWR | O_NOCTTY;

    strcpy(name, "/dev/ptyXX");
    for (x = "pqrstuvwxyzabcde"; *x; ++x)
    {
        for (y = "0123456789abcdef"; *y; ++y)
        {
            name[8] = *x;
            name[9] = *y;

            master = open(name, oflag);
            if (master >= 0)
                return master;
            else if (errno == ENOENT)
            {
                // Console does not exist.
                return -1;
            }
            else
                continue;  // Console already used.
        }
    }

    errno = EAGAIN;
    return -1;
}

int openpty(
    int *amaster, int *aslave, char *name, const struct termios *termp,
    const struct winsize *winp)
{
    if (amaster == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    // Grab the pty master.
    *amaster = posix_openpt(O_RDWR);
    if (aslave)
    {
        // Grab the slave name (ttyname will just return the master name).
        // We don't assume BSD or UNIX 98 psuedo-terminals here.
        char *slavename = ptsname(*amaster);
        *aslave = open(slavename, O_RDWR | O_NOCTTY);
        if (name)
            strcpy(name, slavename);
    }

    if (termp)
    {
        // Set the attributes of the terminal.
        tcsetattr(*aslave, TCSANOW, termp);
    }

    if (winp)
    {
        // Set the size of the terminal to the requested size.
        ioctl(*amaster, TIOCSWINSZ, winp);
    }

    return 0;
}
