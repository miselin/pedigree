#define _GNU_SOURCE
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>

size_t ml_page;

static int64_t now(void) {
  struct timespec time;
  return clock_gettime(CLOCK_MONOTONIC, &time) ? -1
                                               : (int64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

int ml_reap(pid_t child, int milliseconds) {
  int64_t deadline = now() + (int64_t)milliseconds * 1000000;
  while (now() < deadline) {
    int status;
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child)
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (result < 0 && errno != EINTR)
      return -1;
    struct timespec pause = {0, 5000000};
    nanosleep(&pause, NULL);
  }
  kill(child, SIGKILL);
  while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
  }
  return -1;
}

int ml_send(int fd, char byte) {
  ssize_t result;
  do {
    result = write(fd, &byte, 1);
  } while (result < 0 && errno == EINTR);
  return result == 1 ? 0 : -1;
}

int ml_receive(int fd, char expected) {
  int64_t deadline = now() + 5000000000;
  while (now() < deadline) {
    struct pollfd watch = {.fd = fd, .events = POLLIN};
    int result = poll(&watch, 1, 100);
    if (result < 0 && errno == EINTR)
      continue;
    if (result < 0)
      return -1;
    if (!result)
      continue;
    char byte;
    ssize_t amount = read(fd, &byte, 1);
    if (amount < 0 && errno == EINTR)
      continue;
    if (amount == 1 && byte == expected)
      return 0;
    errno = EIO;
    return -1;
  }
  errno = ETIMEDOUT;
  return -1;
}

int ml_limit(size_t bytes) {
  struct rlimit limit;
  if (getrlimit(RLIMIT_MEMLOCK, &limit))
    return -1;
  limit.rlim_cur = bytes;
  return setrlimit(RLIMIT_MEMLOCK, &limit);
}

int ml_unprivileged(void) {
  if (!geteuid() && (setgroups(0, NULL) || setgid(65534) || setuid(65534)))
    return -1;
  if (geteuid())
    return 0;
  errno = EPERM;
  return -1;
}

int ml_resident(void* address, size_t pages, unsigned bits, const char* stage) {
  unsigned char vector[8];
  memset(vector, 0xa5, sizeof(vector));
  if (pages > sizeof(vector) || mincore(address, pages * ml_page, vector)) {
    fprintf(stderr, "MEMORY-LOCK-CONTRACT: %s mincore failed errno=%d\n", stage, errno);
    return -1;
  }
  for (size_t n = 0; n < pages; ++n) {
    if ((vector[n] & 1) != ((bits >> n) & 1)) {
      fprintf(stderr, "MEMORY-LOCK-CONTRACT: %s page=%zu resident=%u expected=%u\n", stage, n,
              vector[n], (bits >> n) & 1);
      return -1;
    }
  }
  return 0;
}

static int run(const char* name, int (*function)(void)) {
  printf("MEMORY-LOCK-CONTRACT: BEGIN %s\n", name);
  fflush(stdout);
  pid_t child = fork();
  if (child < 0)
    return -1;
  if (!child) {
    alarm(40);
    int result = function();
    fflush(stdout);
    fflush(stderr);
    _exit(result ? 1 : 0);
  }
  int result = ml_reap(child, 45000);
  printf("MEMORY-LOCK-CONTRACT: %s %s status=%d\n", result ? "FAIL" : "PASS", name, result);
  fflush(stdout);
  return result;
}

int main(int argc, char** argv) {
  ml_page = (size_t)sysconf(_SC_PAGESIZE);
  if (ml_page < 512 || ml_page > 65536 || (ml_page & (ml_page - 1)))
    return 2;
  if (argc > 1 && !strcmp(argv[1], "memory-lock-exec"))
    return ml_exec(argc, argv);
  signal(SIGPIPE, SIG_IGN);
  static const struct {
    const char* name;
    int (*function)(void);
  } suites[] = {{"limits", ml_limits},
                {"ranges", ml_ranges},
                {"managed", ml_managed},
                {"raw", ml_raw},
                {"lifetime", ml_lifetime}};
  int selected = 0;
  for (size_t n = 0; n < sizeof(suites) / sizeof(suites[0]); ++n) {
    if (argc > 1 && strcmp(argv[1], suites[n].name))
      continue;
    selected = 1;
    if (run(suites[n].name, suites[n].function))
      return 1;
  }
  if (!selected)
    return 2;
  puts("MEMORY-LOCK-CONTRACT: END PASS");
  return 0;
}
