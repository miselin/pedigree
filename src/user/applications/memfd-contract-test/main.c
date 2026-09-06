#define _GNU_SOURCE
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

int64_t mf_now(void) {
  struct timespec now;
  return clock_gettime(CLOCK_MONOTONIC, &now) ? -1 : (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}
void mf_pause(int milliseconds) {
  struct timespec pause = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
  while (nanosleep(&pause, &pause) && errno == EINTR) {
  }
}
int mf_wait(volatile int* flag, int milliseconds) {
  const int64_t until = mf_now() + (int64_t)milliseconds * 1000000;
  while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
    if (mf_now() >= until)
      return -1;
    mf_pause(2);
  }
  return 0;
}
int mf_reap(pid_t child, int milliseconds) {
  const int64_t until = mf_now() + (int64_t)milliseconds * 1000000;
  while (mf_now() < until) {
    int status;
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child) {
      int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      if (code)
        fprintf(stderr, "MEMFD-CONTRACT: child=%ld status=%d\n", (long)child, code);
      return code;
    }
    if (result < 0 && errno != EINTR)
      return -1;
    mf_pause(5);
  }
  fprintf(stderr, "MEMFD-CONTRACT: child=%ld timeout\n", (long)child);
  kill(child, SIGKILL);
  while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
  }
  return -1;
}
int mf_byte(int socket, char expected) {
  struct pollfd entry = {.fd = socket, .events = POLLIN};
  int result;
  do
    result = poll(&entry, 1, 5000);
  while (result < 0 && errno == EINTR);
  char byte;
  return result > 0 && read(socket, &byte, 1) == 1 && byte == expected ? 0 : -1;
}
int mf_send_fd(int socket, int fd) {
  char byte = 'f';
  struct iovec vector = {&byte, 1};
  union {
    struct cmsghdr align;
    char bytes[CMSG_SPACE(sizeof(int))];
  } control = {0};
  struct msghdr message = {.msg_iov = &vector,
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
int mf_receive_fd(int socket) {
  char byte;
  struct iovec vector = {&byte, 1};
  union {
    struct cmsghdr align;
    char bytes[CMSG_SPACE(sizeof(int))];
  } control = {0};
  struct msghdr message = {.msg_iov = &vector,
                           .msg_iovlen = 1,
                           .msg_control = control.bytes,
                           .msg_controllen = sizeof(control.bytes)};
  if (recvmsg(socket, &message, MSG_CMSG_CLOEXEC) != 1 || (message.msg_flags & MSG_CTRUNC))
    return -1;
  struct cmsghdr* item = CMSG_FIRSTHDR(&message);
  if (!item || item->cmsg_level != SOL_SOCKET || item->cmsg_type != SCM_RIGHTS ||
      item->cmsg_len != CMSG_LEN(sizeof(int)))
    return -1;
  int fd;
  memcpy(&fd, CMSG_DATA(item), sizeof(fd));
  return fd;
}
int mf_make(size_t bytes) {
  int fd = memfd_create("contract", MFD_ALLOW_SEALING);
  if (fd >= 0 && ftruncate(fd, bytes)) {
    close(fd);
    return -1;
  }
  return fd;
}
int mf_size(int fd, off_t expected) {
  struct stat st;
  return !fstat(fd, &st) && st.st_size == expected ? 0 : -1;
}
int mf_contents(int fd, off_t offset, const void* expected, size_t length) {
  char bytes[128];
  return length <= sizeof(bytes) && pread(fd, bytes, length, offset) == (ssize_t)length &&
                 !memcmp(bytes, expected, length)
             ? 0
             : -1;
}
static int run(const char* name, int (*test)(void)) {
  printf("MEMFD-CONTRACT: BEGIN %s\n", name);
  fflush(stdout);
  pid_t child = fork();
  if (child < 0)
    return -1;
  if (!child) {
    alarm(40);
    int result = test();
    fflush(stdout);
    fflush(stderr);
    _exit(result ? 1 : 0);
  }
  const int status = mf_reap(child, 45000);
  printf("MEMFD-CONTRACT: %s %s status=%d\n", status ? "FAIL" : "PASS", name, status);
  fflush(stdout);
  return status;
}
int main(int argc, char** argv) {
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
    return 1;
  if (argc > 1 && !strcmp(argv[1], "memfd-exec"))
    return memfd_exec(argc, argv);
  const struct {
    const char* name;
    int (*test)(void);
  } suites[] = {{"creation", memfd_creation},
                {"seals", memfd_seals},
                {"mappings", memfd_mappings},
                {"lifetime", memfd_lifetime},
                {"races", memfd_races}};
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
  puts("MEMFD-CONTRACT: END PASS");
  return 0;
}
