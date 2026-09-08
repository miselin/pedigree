#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define CHECK(condition)                                                                \
  do {                                                                                  \
    if (!(condition)) {                                                                 \
      fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition, errno); \
      failed = 1;                                                                       \
      goto out;                                                                         \
    }                                                                                   \
  } while (0)

static int toggle(int fd) {
  int failed = 0, alias = -1, status = fcntl(fd, F_GETFL);
  CHECK(status >= 0 && !fcntl(fd, F_SETFD, 0));
  CHECK((alias = dup(fd)) >= 0 && fcntl(alias, F_GETFD) == 0);
  // These descriptor commands must ignore the third argument entirely.
  CHECK(!ioctl(fd, FIOCLEX, (void*)(uintptr_t)1));
  CHECK(fcntl(fd, F_GETFD) == FD_CLOEXEC && fcntl(alias, F_GETFD) == 0);
  CHECK(!ioctl(fd, FIOCLEX, NULL) && fcntl(fd, F_GETFD) == FD_CLOEXEC);
  CHECK(!ioctl(alias, FIOCLEX, NULL));
  CHECK(!ioctl(fd, FIONCLEX, (void*)(uintptr_t)1));
  CHECK(fcntl(fd, F_GETFD) == 0 && fcntl(alias, F_GETFD) == FD_CLOEXEC);
  CHECK(!ioctl(fd, FIONCLEX, NULL) && fcntl(fd, F_GETFD) == 0);
  CHECK(!ioctl(alias, FIONCLEX, NULL) && fcntl(alias, F_GETFD) == 0);
  CHECK(fcntl(fd, F_GETFL) == status && fcntl(alias, F_GETFL) == status);
out:
  if (alias >= 0)
    close(alias);
  return failed;
}

static int regular(void) {
  int failed = 0, fd = -1;
  char path[] = "/tmp/descriptor-cloexec-XXXXXX";
  CHECK((fd = mkstemp(path)) >= 0);
  CHECK(!toggle(fd));
out:
  if (fd >= 0) {
    close(fd);
    unlink(path);
  }
  return failed;
}

static int pipes(void) {
  int failed = 0, pair[2] = {-1, -1};
  CHECK(!pipe(pair));
  CHECK(!toggle(pair[0]) && !toggle(pair[1]));
out:
  for (int n = 0; n < 2; ++n)
    if (pair[n] >= 0)
      close(pair[n]);
  return failed;
}

static int sockets(void) {
  int failed = 0, pair[2] = {-1, -1};
  CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
  CHECK(!toggle(pair[0]) && !toggle(pair[1]));
out:
  for (int n = 0; n < 2; ++n)
    if (pair[n] >= 0)
      close(pair[n]);
  return failed;
}

static int validation(void) {
  int failed = 0, path = -1;
  CHECK(ioctl(-1, FIOCLEX, NULL) == -1 && errno == EBADF);
  CHECK(ioctl(-1, FIONCLEX, NULL) == -1 && errno == EBADF);
  CHECK((path = open("/", O_PATH | O_DIRECTORY)) >= 0);
  CHECK(ioctl(path, FIOCLEX, NULL) == -1 && errno == EBADF);
  CHECK(ioctl(path, FIONCLEX, NULL) == -1 && errno == EBADF);
  // O_PATH remains usable through the existing fcntl fallback.
  CHECK(!fcntl(path, F_SETFD, FD_CLOEXEC) && fcntl(path, F_GETFD) == FD_CLOEXEC);
  CHECK(!fcntl(path, F_SETFD, 0) && fcntl(path, F_GETFD) == 0);
out:
  if (path >= 0)
    close(path);
  return failed;
}

static int exec_child(const char* closed_arg, const char* kept_arg) {
  int failed = 0, closed = atoi(closed_arg), kept = atoi(kept_arg);
  char content[7];
  alarm(5);
  CHECK(closed >= 64 && kept > closed);
  CHECK(fcntl(closed, F_GETFD) == -1 && errno == EBADF);
  CHECK(fcntl(kept, F_GETFD) == 0);
  CHECK(read(kept, content, sizeof(content)) == (ssize_t)sizeof(content));
  CHECK(!memcmp(content, "closure", sizeof(content)));
out:
  close(kept);
  return failed;
}

static int exec_closure(const char* executable) {
  int failed = 0, fd = -1, closed = -1, kept = -1, status = -1;
  pid_t child = -1;
  char path[] = "/tmp/descriptor-cloexec-exec-XXXXXX";
  CHECK((fd = mkstemp(path)) >= 0);
  CHECK(write(fd, "closure", 7) == 7 && lseek(fd, 0, SEEK_SET) == 0);
  // Avoid confusing a loader's reused low descriptor with one that survived exec.
  CHECK((closed = fcntl(fd, F_DUPFD, 64)) >= 64);
  CHECK((kept = fcntl(fd, F_DUPFD, closed + 1)) > closed);
  CHECK(!ioctl(closed, FIOCLEX, NULL));
  CHECK(!ioctl(kept, FIOCLEX, NULL) && !ioctl(kept, FIONCLEX, NULL));
  CHECK((child = fork()) >= 0);
  if (!child) {
    char closed_arg[24], kept_arg[24];
    snprintf(closed_arg, sizeof(closed_arg), "%d", closed);
    snprintf(kept_arg, sizeof(kept_arg), "%d", kept);
    char* arguments[] = {(char*)executable, "--exec", closed_arg, kept_arg, NULL};
    close(fd);
    alarm(5);
    // Procfs link following is a separate contract from descriptor inheritance.
    execv(executable, arguments);
    _exit(127);
  }
  for (int n = 0; n < 1000; ++n) {
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child) {
      child = -1;
      break;
    }
    CHECK(result == 0 || (result < 0 && errno == EINTR));
    struct timespec pause = {0, 10000000};
    nanosleep(&pause, NULL);
  }
  CHECK(child == -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK(fcntl(closed, F_GETFD) == FD_CLOEXEC && fcntl(kept, F_GETFD) == 0);
out:
  if (child > 0) {
    kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
    }
  }
  if (kept >= 0)
    close(kept);
  if (closed >= 0)
    close(closed);
  if (fd >= 0) {
    close(fd);
    unlink(path);
  }
  return failed;
}

int main(int argc, char** argv) {
  if (argc == 4 && !strcmp(argv[1], "--exec"))
    return exec_child(argv[2], argv[3]);
  if (argc != 1 || !argv[0] || argv[0][0] != '/' || !argv[0][1]) {
    fputs("Invoke this contract test using its absolute executable path.\n", stderr);
    return 2;
  }
  static const struct {
    const char* name;
    int (*test)(void);
  } cases[] = {
      {"regular", regular}, {"pipe", pipes}, {"socket", sockets}, {"validation", validation}};
  for (unsigned n = 0; n < sizeof(cases) / sizeof(cases[0]); ++n) {
    printf("DESCRIPTOR-CLOEXEC: BEGIN %s\n", cases[n].name);
    fflush(stdout);
    int result = cases[n].test();
    printf("DESCRIPTOR-CLOEXEC: %s %s status=%d\n", result ? "FAIL" : "PASS", cases[n].name,
           result);
    fflush(stdout);
    if (result)
      return 1;
  }
  puts("DESCRIPTOR-CLOEXEC: BEGIN exec-closure");
  fflush(stdout);
  int result = exec_closure(argv[0]);
  printf("DESCRIPTOR-CLOEXEC: %s exec-closure status=%d\n", result ? "FAIL" : "PASS", result);
  if (result)
    return 1;
  puts("DESCRIPTOR-CLOEXEC: END PASS");
  return 0;
}
