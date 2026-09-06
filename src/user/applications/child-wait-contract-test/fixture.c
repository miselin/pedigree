#define _GNU_SOURCE
#include <poll.h>
#include <sched.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "contract.h"
#include <sys/syscall.h>

size_t cw_page;

int64_t cw_now(void) {
  struct timespec now;
  return clock_gettime(CLOCK_MONOTONIC, &now) ? -1 : (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

int cw_read(int fd, void* data, size_t size) {
  unsigned char* bytes = data;
  const int64_t end = cw_now() + INT64_C(10000000000);
  while (size) {
    int64_t left = end - cw_now();
    if (left <= 0)
      return -1;
    struct pollfd ready = {.fd = fd, .events = POLLIN};
    int result = poll(&ready, 1, (int)(left / 1000000 + 1));
    if (result < 0 && errno == EINTR)
      continue;
    if (result <= 0)
      return -1;
    ssize_t n = read(fd, bytes, size);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return -1;
    bytes += n;
    size -= (size_t)n;
  }
  return 0;
}

int cw_write(int fd, const void* data, size_t size) {
  const unsigned char* bytes = data;
  while (size) {
    ssize_t n = write(fd, bytes, size);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return -1;
    bytes += n;
    size -= (size_t)n;
  }
  return 0;
}

int cw_send(int fd, char value) {
  return cw_write(fd, &value, 1);
}

int cw_receive(int fd, char value) {
  char actual;
  return cw_read(fd, &actual, 1) || actual != value ? -1 : 0;
}

int cw_atomic_wait(atomic_uint* value, unsigned expected) {
  const int64_t end = cw_now() + INT64_C(10000000000);
  while (cw_now() < end) {
    if (atomic_load_explicit(value, memory_order_acquire) == expected)
      return 0;
    sched_yield();
  }
  return -1;
}

int cw_spawn(struct cw_child* child, pid_t group, uid_t uid, int exit_code) {
  int command[2], report[2];
  if (pipe(command))
    return -1;
  if (pipe(report)) {
    close(command[0]);
    close(command[1]);
    return -1;
  }
  pid_t pid = fork();
  if (!pid) {
    close(command[1]);
    close(report[0]);
    alarm(30);
    if ((group >= 0 && setpgid(0, group)) ||
        (uid != (uid_t)-1 && setresuid(uid, uid + 1, uid + 2)) || cw_send(report[1], 'R'))
      _exit(120);
    for (;;) {
      char request;
      if (cw_read(command[0], &request, 1))
        _exit(121);
      if (request == 'E')
        _exit(exit_code);
      if (request == 'S') {
        if (raise(SIGSTOP) || cw_send(report[1], 'C'))
          _exit(122);
      } else if (request == 'B') {
        volatile unsigned value = 1;
        for (unsigned i = 0; i < 2000000; ++i)
          value = value * 1664525u + i;
        if (cw_send(report[1], 'B'))
          _exit(123);
      } else {
        _exit(124);
      }
    }
  }
  close(command[0]);
  close(report[1]);
  if (pid < 0) {
    close(command[1]);
    close(report[0]);
    return -1;
  }
  *child = (struct cw_child){pid, command[1], report[0]};
  if (cw_receive(child->report, 'R')) {
    cw_cleanup(child);
    return -1;
  }
  return 0;
}

int cw_reap(pid_t pid, int milliseconds) {
  const int64_t end = cw_now() + (int64_t)milliseconds * 1000000;
  while (cw_now() < end) {
    int status;
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid)
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (result < 0 && errno != EINTR)
      return -1;
    struct timespec delay = {0, 2000000};
    nanosleep(&delay, NULL);
  }
  kill(pid, SIGKILL);
  while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
  }
  return -1;
}

void cw_cleanup(struct cw_child* child) {
  if (child->pid > 0) {
    kill(child->pid, SIGKILL);
    cw_reap(child->pid, 1000);
  }
  if (child->command >= 0)
    close(child->command);
  if (child->report >= 0)
    close(child->report);
  *child = (struct cw_child)CW_CHILD_INIT;
}

int cw_info(const siginfo_t* info, pid_t pid, uid_t uid, int code, int status) {
  if (info->si_signo == SIGCHLD && !info->si_errno && info->si_pid == pid && info->si_uid == uid &&
      info->si_code == code && info->si_status == status)
    return 0;
  fprintf(stderr,
          "siginfo: signo=%d errno=%d pid=%d uid=%u code=%d status=%d; expected %d/%u/%d/%d\n",
          info->si_signo, info->si_errno, info->si_pid, info->si_uid, info->si_code,
          info->si_status, pid, uid, code, status);
  return -1;
}

int cw_empty_info(const siginfo_t* info) {
  return info->si_signo || info->si_errno || info->si_pid || info->si_uid || info->si_code ||
         info->si_status;
}

int cw_usage(const struct rusage* usage) {
  return usage->ru_utime.tv_sec < 0 || usage->ru_utime.tv_usec < 0 ||
         usage->ru_utime.tv_usec >= 1000000 || usage->ru_stime.tv_sec < 0 ||
         usage->ru_stime.tv_usec < 0 || usage->ru_stime.tv_usec >= 1000000 ||
         cw_bytes((const unsigned char*)usage + CW_RUSAGE_BYTES, sizeof(*usage) - CW_RUSAGE_BYTES,
                  0xa5);
}

int cw_bytes(const void* buffer, size_t size, unsigned char value) {
  const unsigned char* bytes = buffer;
  for (size_t i = 0; i < size; ++i)
    if (bytes[i] != value)
      return -1;
  return 0;
}

long cw_raw_waitid(idtype_t type, id_t id, void* info, int options, struct rusage* usage) {
  return syscall(SYS_waitid, type, id, info, options, usage);
}
