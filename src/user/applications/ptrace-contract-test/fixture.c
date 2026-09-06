#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "contract.h"

size_t tc_page;

int64_t tc_now(void) {
  struct timespec value;
  return clock_gettime(CLOCK_MONOTONIC, &value)
             ? -1
             : (int64_t)value.tv_sec * INT64_C(1000000000) + value.tv_nsec;
}

static int transfer(int fd, void* data, size_t size, int writing) {
  unsigned char* bytes = data;
  int64_t now = tc_now();
  if (now < 0)
    return -1;
  const int64_t end = now + INT64_C(10000000000);
  while (size) {
    now = tc_now();
    if (now < 0)
      return -1;
    if (now >= end) {
      errno = ETIMEDOUT;
      return -1;
    }
    struct pollfd ready = {.fd = fd, .events = writing ? POLLOUT : POLLIN};
    int result = poll(&ready, 1, (int)((end - now) / 1000000 + 1));
    if (result < 0 && errno == EINTR)
      continue;
    if (result <= 0) {
      if (!result)
        errno = ETIMEDOUT;
      return -1;
    }
    ssize_t count = writing ? write(fd, bytes, size) : read(fd, bytes, size);
    if (count < 0 && (errno == EINTR || errno == EAGAIN))
      continue;
    if (count <= 0) {
      if (!count)
        errno = EPIPE;
      return -1;
    }
    bytes += count;
    size -= (size_t)count;
  }
  return 0;
}

int tc_read(int fd, void* data, size_t size) {
  return transfer(fd, data, size, 0);
}

int tc_write(int fd, const void* data, size_t size) {
  return transfer(fd, (void*)data, size, 1);
}

int tc_wait(pid_t pid, int* status, int milliseconds) {
  int64_t now = tc_now();
  if (now < 0)
    return -1;
  const int64_t end = now + (int64_t)milliseconds * 1000000;
  while (now < end) {
    pid_t result = wait4(pid, status, WNOHANG, NULL);
    if (result == pid)
      return 0;
    if (result < 0 && errno != EINTR)
      return -1;
    struct timespec delay = {0, 2000000};
    nanosleep(&delay, NULL);
    now = tc_now();
    if (now < 0)
      return -1;
  }
  errno = ETIMEDOUT;
  return -1;
}

void tc_cleanup(pid_t* pid) {
  if (*pid <= 0)
    return;
  kill(*pid, SIGKILL);
  int64_t now = tc_now();
  const int64_t end = now + INT64_C(2000000000);
  while (now >= 0 && now < end) {
    int status;
    int result = tc_wait(*pid, &status, (int)((end - now) / 1000000 + 1));
    if ((!result && (WIFEXITED(status) || WIFSIGNALED(status))) || (result && errno == ECHILD)) {
      *pid = -1;
      return;
    }
    if (result)
      break;
    now = tc_now();
  }
  fprintf(stderr, "child cleanup incomplete pid=%d errno=%d\n", *pid, errno);
}

int tc_atomic_wait(atomic_uint* value, unsigned expected) {
  int64_t now = tc_now(), end = now + INT64_C(10000000000);
  while (now >= 0 && now < end) {
    if (atomic_load(value) == expected)
      return 0;
    sched_yield();
    now = tc_now();
  }
  if (now >= 0)
    errno = ETIMEDOUT;
  return -1;
}

int tc_send(const struct tc_child* child, int operation, int signal) {
  struct tc_command command = {operation, signal};
  return tc_write(child->command, &command, sizeof(command));
}

int tc_receive(int fd, int kind, struct tc_message* message) {
  struct tc_message local;
  if (!message)
    message = &local;
  if (tc_read(fd, message, sizeof(*message)))
    return -1;
  if (message->kind == kind)
    return 0;
  fprintf(stderr, "report actual=%c expected=%c signal=%d code=%d result=%lld error=%d\n",
          message->kind, kind, message->signal, message->code, (long long)message->result,
          message->error);
  errno = EIO;
  return -1;
}

int tc_spawn(struct tc_child* child, int traced) {
  int command[2], report[2];
  if (pipe2(command, O_CLOEXEC | O_NONBLOCK))
    return -1;
  if (pipe2(report, O_CLOEXEC | O_NONBLOCK)) {
    close(command[0]);
    close(command[1]);
    return -1;
  }
  pid_t pid = fork();
  if (!pid) {
    close(command[1]);
    close(report[0]);
    tc_tracee(command[0], report[1], traced);
    _exit(121);
  }
  close(command[0]);
  close(report[1]);
  if (pid < 0) {
    close(command[1]);
    close(report[0]);
    return -1;
  }
  *child = (struct tc_child){pid, command[1], report[0]};
  struct tc_message ready = {0};
  if (tc_receive(child->report, 'R', &ready) || ready.result) {
    if (ready.result)
      fprintf(stderr, "TRACEME startup result=%lld errno=%d\n", (long long)ready.result,
              ready.error);
    tc_close(child);
    return -1;
  }
  return 0;
}

void tc_close(struct tc_child* child) {
  tc_cleanup(&child->pid);
  if (child->command >= 0)
    close(child->command);
  if (child->report >= 0)
    close(child->report);
  *child = (struct tc_child)TC_CHILD_INIT;
}

int tc_stop(struct tc_child* child, int signal) {
  if (tc_send(child, 'S', signal))
    return -1;
  return tc_wait_stop(child, signal);
}

int tc_wait_stop(struct tc_child* child, int signal) {
  int status;
  if (tc_wait(child->pid, &status, 10000))
    return -1;
  if (WIFEXITED(status) || WIFSIGNALED(status))
    child->pid = -1;
  if (WIFSTOPPED(status) && WSTOPSIG(status) == signal)
    return 0;
  fprintf(stderr, "stop signal=%d actual status=%#x\n", signal, status);
  return -1;
}

int tc_finish(struct tc_child* child) {
  if (tc_send(child, 'E', 0) || tc_receive(child->report, 'E', NULL))
    return -1;
  int status;
  if (tc_wait(child->pid, &status, 10000))
    return -1;
  if (WIFEXITED(status) || WIFSIGNALED(status))
    child->pid = -1;
  if (WIFEXITED(status) && WEXITSTATUS(status) == TC_EXIT_CODE)
    return 0;
  fprintf(stderr, "exit status=%#x\n", status);
  return -1;
}

int tc_resume(pid_t child, int request, int signal) {
  return (int)ptrace(request, child, (void*)0, (void*)(uintptr_t)(unsigned)signal);
}

int tc_signal_info(const siginfo_t* info, int signal, int code, pid_t pid, uid_t uid) {
  if (info->si_signo == signal && !info->si_errno && info->si_code == code && info->si_pid == pid &&
      info->si_uid == uid)
    return 0;
  fprintf(stderr, "siginfo actual=%d/%d/%d/%d/%u expected=%d/0/%d/%d/%u\n", info->si_signo,
          info->si_errno, info->si_code, info->si_pid, info->si_uid, signal, code, pid, uid);
  return -1;
}
