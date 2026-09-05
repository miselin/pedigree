#define _GNU_SOURCE
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/socket.h>
#include <sys/wait.h>

int64_t ed_now(clockid_t clock) {
  struct timespec now;
  return clock_gettime(clock, &now) ? -1 : (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}
struct timespec ed_timespec(int64_t nanoseconds) {
  return (struct timespec){nanoseconds / 1000000000, nanoseconds % 1000000000};
}
void ed_pause(int milliseconds) {
  struct timespec pause = ed_timespec((int64_t)milliseconds * 1000000);
  while (nanosleep(&pause, &pause) && errno == EINTR) {
  }
}
int ed_wait(volatile int* flag, int milliseconds) {
  int64_t deadline = ed_now(CLOCK_MONOTONIC) + (int64_t)milliseconds * 1000000;
  while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
    if (ed_now(CLOCK_MONOTONIC) >= deadline)
      return -1;
    ed_pause(2);
  }
  return 0;
}
int ed_reap(pid_t child, int milliseconds) {
  int64_t deadline = ed_now(CLOCK_MONOTONIC) + (int64_t)milliseconds * 1000000;
  while (ed_now(CLOCK_MONOTONIC) < deadline) {
    int status;
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child)
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (result < 0 && errno != EINTR)
      return -1;
    ed_pause(5);
  }
  kill(child, SIGKILL);
  while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
  }
  return -1;
}
int ed_send_fd(int socket, int fd) {
  char byte = 'f';
  struct iovec iov = {&byte, 1};
  union {
    struct cmsghdr align;
    char bytes[CMSG_SPACE(sizeof(int))];
  } control = {0};
  struct msghdr message = {.msg_iov = &iov,
                           .msg_iovlen = 1,
                           .msg_control = control.bytes,
                           .msg_controllen = sizeof(control.bytes)};
  struct cmsghdr* item = CMSG_FIRSTHDR(&message);
  item->cmsg_level = SOL_SOCKET;
  item->cmsg_type = SCM_RIGHTS;
  item->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(item), &fd, sizeof(fd));
  return sendmsg(socket, &message, 0) == 1 ? 0 : -1;
}
int ed_receive_fd(int socket) {
  char byte;
  struct iovec iov = {&byte, 1};
  union {
    struct cmsghdr align;
    char bytes[CMSG_SPACE(sizeof(int))];
  } control = {0};
  struct msghdr message = {.msg_iov = &iov,
                           .msg_iovlen = 1,
                           .msg_control = control.bytes,
                           .msg_controllen = sizeof(control.bytes)};
  if (recvmsg(socket, &message, MSG_CMSG_CLOEXEC) != 1 || message.msg_flags & MSG_CTRUNC)
    return -1;
  struct cmsghdr* item = CMSG_FIRSTHDR(&message);
  if (!item || item->cmsg_level != SOL_SOCKET || item->cmsg_type != SCM_RIGHTS ||
      item->cmsg_len != CMSG_LEN(sizeof(int)))
    return -1;
  int fd;
  memcpy(&fd, CMSG_DATA(item), sizeof(fd));
  return fd;
}
int ed_readable(int fd, int milliseconds) {
  struct pollfd entry = {.fd = fd, .events = POLLIN};
  int result = poll(&entry, 1, milliseconds);
  return result > 0 ? !!(entry.revents & POLLIN) : result;
}
void* ed_read_thread(void* argument) {
  struct ed_reader* reader = argument;
  __atomic_store_n(&reader->started, 1, __ATOMIC_RELEASE);
  reader->result = read(reader->fd, reader->bytes, reader->size);
  reader->error = errno;
  reader->finished = ed_now(CLOCK_MONOTONIC);
  __atomic_store_n(&reader->done, 1, __ATOMIC_RELEASE);
  return NULL;
}

static int run(const char* name, int (*test)(void)) {
  printf("EVENT-DESCRIPTOR-CONTRACT: BEGIN %s\n", name);
  fflush(stdout);
  const int clock_suite = !strcmp(name, "clock");
  int64_t realtime = ed_now(CLOCK_REALTIME), monotonic = ed_now(CLOCK_MONOTONIC);
  pid_t child = fork();
  if (child < 0)
    return -1;
  if (!child) {
    alarm(30);
    int result = test();
    fflush(stdout);
    fflush(stderr);
    _exit(result ? 1 : 0);
  }
  int result = ed_reap(child, 35000);
  if (clock_suite && geteuid() == 0) {
    // Restore the guest clock even if the child fails after changing it.
    struct timespec restored = ed_timespec(realtime + ed_now(CLOCK_MONOTONIC) - monotonic);
    if (clock_settime(CLOCK_REALTIME, &restored))
      result = -1;
  }
  printf("EVENT-DESCRIPTOR-CONTRACT: %s %s status=%d\n", result ? "FAIL" : "PASS", name, result);
  fflush(stdout);
  return result;
}
int main(int argc, char** argv) {
  if (argc > 1 && !strcmp(argv[1], "signalfd-exec"))
    return event_descriptor_signalfd_exec(argc, argv);
  if (argc > 1 && !strcmp(argv[1], "timerfd-exec"))
    return event_descriptor_timerfd_exec(argc, argv);
  static const struct {
    const char* name;
    int (*test)(void);
  } suites[] = {{"signalfd", event_descriptor_test_signalfd},
                {"timerfd", event_descriptor_test_timerfd},
                {"clock", event_descriptor_test_clock}};
  int selected = 0;
  for (unsigned n = 0; n < sizeof(suites) / sizeof(suites[0]); ++n) {
    if (argc > 1 && strcmp(argv[1], suites[n].name))
      continue;
    selected = 1;
    if (run(suites[n].name, suites[n].test))
      return 1;
  }
  if (!selected)
    return 2;
  puts("EVENT-DESCRIPTOR-CONTRACT: END PASS");
  return 0;
}
