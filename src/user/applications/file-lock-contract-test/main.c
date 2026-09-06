#define _GNU_SOURCE
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/wait.h>

int64_t fl_now(void) {
  struct timespec now;
  return clock_gettime(CLOCK_MONOTONIC, &now) ? -1 : (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}
void fl_pause(int milliseconds) {
  struct timespec pause = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
  while (nanosleep(&pause, &pause) && errno == EINTR) {
  }
}
int fl_wait(volatile int* flag, int milliseconds) {
  const int64_t until = fl_now() + (int64_t)milliseconds * 1000000;
  while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
    if (fl_now() >= until)
      return -1;
    fl_pause(2);
  }
  return 0;
}
int fl_reap(pid_t child, int milliseconds) {
  const int64_t until = fl_now() + (int64_t)milliseconds * 1000000;
  while (fl_now() < until) {
    int status;
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child)
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (result < 0 && errno != EINTR)
      return -1;
    fl_pause(5);
  }
  fprintf(stderr, "FILE-LOCK-CONTRACT: child=%ld timeout\n", (long)child);
  kill(child, SIGKILL);
  while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
  }
  return -1;
}
int fl_byte(int socket, char expected) {
  struct pollfd entry = {.fd = socket, .events = POLLIN};
  int result;
  do
    result = poll(&entry, 1, 5000);
  while (result < 0 && errno == EINTR);
  char byte;
  return result > 0 && read(socket, &byte, 1) == 1 && byte == expected ? 0 : -1;
}
int fl_send_fd(int socket, int fd) {
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
int fl_receive_fd(int socket) {
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
int fl_file(char path[128], const char* directory) {
  static unsigned sequence;
  snprintf(path, 128, "%s/file-lock-%ld-%u", directory, (long)getpid(), ++sequence);
  int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd >= 0 && ftruncate(fd, 128)) {
    close(fd);
    unlink(path);
    return -1;
  }
  return fd;
}
int fl_record(int fd, int command, short type, short whence, off_t start, off_t length) {
  struct flock lock = {.l_type = type, .l_whence = whence, .l_start = start, .l_len = length};
  return fcntl(fd, command, &lock);
}
int fl_lock(int fd, int kind, short type, int blocking) {
  if (kind == FL_FLOCK) {
    int operation = type == F_UNLCK ? LOCK_UN : type == F_RDLCK ? LOCK_SH : LOCK_EX;
    return flock(fd, operation | (blocking ? 0 : LOCK_NB));
  }
  return fl_record(fd,
                   kind == FL_CLASSIC ? (blocking ? F_SETLKW : F_SETLK)
                                      : (blocking ? F_OFD_SETLKW : F_OFD_SETLK),
                   type, SEEK_SET, 0, 0);
}
int fl_query(int fd, int command, off_t start, off_t length, short type, off_t expected_start,
             off_t expected_length, pid_t owner) {
  struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = start, .l_len = length};
  if (fcntl(fd, command, &lock))
    return -1;
  if (lock.l_type == type &&
      (type == F_UNLCK || (lock.l_whence == SEEK_SET && lock.l_start == expected_start &&
                           lock.l_len == expected_length && lock.l_pid == owner)))
    return 0;
  fprintf(stderr, "FILE-LOCK-CONTRACT: GETLK type=%d whence=%d range=%lld/%lld pid=%ld\n",
          lock.l_type, lock.l_whence, (long long)lock.l_start, (long long)lock.l_len,
          (long)lock.l_pid);
  return -1;
}
static int run(const char* name, int (*test)(void)) {
  printf("FILE-LOCK-CONTRACT: BEGIN %s\n", name);
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
  const int status = fl_reap(child, 45000);
  printf("FILE-LOCK-CONTRACT: %s %s status=%d\n", status ? "FAIL" : "PASS", name, status);
  fflush(stdout);
  return status;
}
int main(int argc, char** argv) {
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
    return 1;
  if (argc > 1 && !strcmp(argv[1], "lock-exec"))
    return file_lock_exec(argc, argv);
  const struct {
    const char* name;
    int (*test)(void);
  } suites[] = {{"flock", file_lock_flock},
                {"records", file_lock_records},
                {"lifetime", file_lock_lifetime},
                {"blocking", file_lock_blocking},
                {"creation", file_lock_creation}};
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
  puts("FILE-LOCK-CONTRACT: END PASS");
  return 0;
}
