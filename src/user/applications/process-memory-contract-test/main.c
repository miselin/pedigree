#define _GNU_SOURCE
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "contract.h"
#include <sys/prctl.h>
#include <sys/wait.h>

size_t pm_page;

int64_t pm_now(void) {
  struct timespec now;
  return clock_gettime(CLOCK_MONOTONIC, &now) ? -1 : (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}
int pm_reap(pid_t pid, int milliseconds) {
  int64_t start = pm_now(), now, deadline = start + (int64_t)milliseconds * 1000000;
  while (start >= 0 && (now = pm_now()) >= 0 && now < deadline) {
    int status;
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid)
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (result < 0 && errno != EINTR)
      return -1;
    struct timespec pause = {0, 5000000};
    nanosleep(&pause, NULL);
  }
  kill(pid, SIGKILL);
  while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
  }
  return -1;
}
int pm_read(int fd, void* buffer, size_t length) {
  unsigned char* bytes = buffer;
  while (length) {
    struct pollfd ready = {.fd = fd, .events = POLLIN};
    int result;
    do
      result = poll(&ready, 1, 10000);
    while (result < 0 && errno == EINTR);
    if (result <= 0)
      return -1;
    ssize_t count = read(fd, bytes, length);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return -1;
    bytes += count;
    length -= count;
  }
  return 0;
}
int pm_write(int fd, const void* buffer, size_t length) {
  const unsigned char* bytes = buffer;
  while (length) {
    ssize_t count = write(fd, bytes, length);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return -1;
    bytes += count;
    length -= count;
  }
  return 0;
}
int pm_send(int fd, char command) {
  return pm_write(fd, &command, 1);
}
int pm_receive(int fd, char command) {
  char actual;
  return pm_read(fd, &actual, 1) || actual != command ? -1 : 0;
}
int pm_spawn(struct pm_peer* peer, int (*body)(int, int, void*), void* argument) {
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
    int result = body(command[0], report[1], argument);
    fflush(stdout);
    fflush(stderr);
    _exit(result ? 1 : 0);
  }
  close(command[0]);
  close(report[1]);
  if (pid < 0) {
    close(command[1]);
    close(report[0]);
    return -1;
  }
  peer->pid = pid;
  peer->command = command[1];
  peer->report = report[0];
  return 0;
}
int pm_join(struct pm_peer* peer) {
  int result = pm_reap(peer->pid, 10000);
  if (result)
    fprintf(stderr, "cooperative child pid=%d status=%d\n", peer->pid, result);
  peer->pid = -1;
  if (peer->command >= 0)
    close(peer->command);
  if (peer->report >= 0)
    close(peer->report);
  peer->command = peer->report = -1;
  return result;
}
void pm_cleanup(struct pm_peer* peer) {
  if (peer->pid > 0) {
    kill(peer->pid, SIGKILL);
    pm_reap(peer->pid, 1000);
  }
  if (peer->command >= 0)
    close(peer->command);
  if (peer->report >= 0)
    close(peer->report);
  peer->pid = peer->command = peer->report = -1;
}
int pm_isolate(int (*body)(void)) {
  pid_t pid = fork();
  if (!pid) {
    alarm(30);
    int result = body();
    fflush(stdout);
    fflush(stderr);
    _exit(result ? 1 : 0);
  }
  return pid > 0 ? pm_reap(pid, 32000) : -1;
}
ssize_t pm_copy(pid_t pid, void* local, const void* remote, size_t length, int write_remote) {
  struct iovec here = {local, length}, there = {(void*)remote, length};
  return write_remote ? process_vm_writev(pid, &here, 1, &there, 1, 0)
                      : process_vm_readv(pid, &here, 1, &there, 1, 0);
}
int pm_dumpable(int value) {
  return prctl(PR_SET_DUMPABLE, (unsigned long)value, 0UL, 0UL, 0UL);
}
static int run(const char* name, int (*body)(void)) {
  printf("PROCESS-MEMORY-CONTRACT: BEGIN %s\n", name);
  fflush(stdout);
  pid_t pid = fork();
  if (!pid) {
    alarm(40);
    int result = body();
    fflush(stdout);
    fflush(stderr);
    _exit(result ? 1 : 0);
  }
  int result = pid > 0 ? pm_reap(pid, 45000) : -1;
  printf("PROCESS-MEMORY-CONTRACT: %s %s status=%d\n", result ? "FAIL" : "PASS", name, result);
  fflush(stdout);
  return result;
}
int main(int argc, char** argv) {
  pm_page = (size_t)sysconf(_SC_PAGESIZE);
  if (pm_page < 512 || pm_page > 65536 || (pm_page & (pm_page - 1)))
    return 2;
  signal(SIGPIPE, SIG_IGN);
  if (argc > 1 && !strcmp(argv[1], "memory-exec"))
    return pm_exec(argc, argv);
  if (argc > 1 && !strcmp(argv[1], "fs-exec"))
    return pm_fs_exec(argc, argv);
  if (argc > 2)
    return 2;
  static const struct {
    const char* name;
    int (*body)(void);
  } families[] = {{"copies", pm_copies},
                  {"mappings", pm_mappings},
                  {"lifetime", pm_lifetime},
                  {"credentials", pm_credentials},
                  {"filesystem", pm_filesystem}};
  int found = 0;
  for (size_t n = 0; n < sizeof(families) / sizeof(families[0]); ++n) {
    if (argc == 2 && strcmp(argv[1], "all") && strcmp(argv[1], families[n].name))
      continue;
    found = 1;
    if (run(families[n].name, families[n].body))
      return 1;
  }
  if (!found)
    return 2;
  puts("PROCESS-MEMORY-CONTRACT: END PASS");
  return 0;
}
