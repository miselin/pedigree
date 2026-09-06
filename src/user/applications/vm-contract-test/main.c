#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/wait.h>

size_t vm_page;

int64_t vm_now(void) {
  struct timespec now;
  return clock_gettime(CLOCK_MONOTONIC, &now) ? -1 : (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}
void vm_pause(int milliseconds) {
  struct timespec pause = {milliseconds / 1000, (milliseconds % 1000) * 1000000};
  while (nanosleep(&pause, &pause) && errno == EINTR) {
  }
}
int vm_wait(volatile int* flag, int milliseconds) {
  int64_t deadline = vm_now() + (int64_t)milliseconds * 1000000;
  while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
    if (vm_now() >= deadline)
      return -1;
    vm_pause(2);
  }
  return 0;
}
int vm_reap(pid_t child, int milliseconds) {
  int64_t deadline = vm_now() + (int64_t)milliseconds * 1000000;
  while (vm_now() < deadline) {
    int status;
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child)
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (result < 0 && errno != EINTR)
      return -1;
    vm_pause(5);
  }
  kill(child, SIGKILL);
  while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
  }
  return -1;
}
static void expected_fault(int signal) {
  (void)signal;
  _exit(0);
}
int vm_fault(void* address, int writing, int expected_signal) {
  pid_t child = fork();
  if (child < 0)
    return -1;
  if (!child) {
    alarm(3);
    struct sigaction action = {.sa_handler = expected_fault};
    sigemptyset(&action.sa_mask);
    if (sigaction(expected_signal, &action, NULL))
      _exit(2);
    if (writing)
      *(volatile unsigned char*)address = 0x7e;
    else {
      volatile unsigned char value = *(volatile unsigned char*)address;
      (void)value;
    }
    _exit(1);
  }
  return vm_reap(child, 4000);
}
int vm_file(size_t pages, int* readonly) {
  static unsigned sequence;
  char name[96];
  snprintf(name, sizeof(name), "/tmp/vm-contract-%ld-%u", (long)getpid(), sequence++);
  int fd = open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0)
    return -1;
  unsigned char* bytes = malloc(vm_page);
  int error = bytes == NULL;
  if (readonly)
    *readonly = -1;
  for (size_t n = 0; !error && n < pages; ++n) {
    memset(bytes, 0x20 + n, vm_page);
    error = write(fd, bytes, vm_page) != (ssize_t)vm_page;
  }
  free(bytes);
  if (!error && readonly) {
    *readonly = open(name, O_RDONLY);
    error = *readonly < 0;
  }
  if (unlink(name))
    error = 1;
  if (error) {
    if (readonly && *readonly >= 0) {
      close(*readonly);
      *readonly = -1;
    }
    close(fd);
    return -1;
  }
  return fd;
}
int vm_uniform(const unsigned char* bytes, size_t length, unsigned char value) {
  for (size_t n = 0; n < length; ++n)
    if (bytes[n] != value)
      return 0;
  return 1;
}

static int run(const char* name, int (*test)(void)) {
  printf("VM-EXPANSION-CONTRACT: BEGIN %s\n", name);
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
  int result = vm_reap(child, 45000);
  printf("VM-EXPANSION-CONTRACT: %s %s status=%d\n", result ? "FAIL" : "PASS", name, result);
  fflush(stdout);
  return result;
}
int main(int argc, char** argv) {
  vm_page = (size_t)sysconf(_SC_PAGESIZE);
  if (!vm_page || vm_page > 65536 || (vm_page & (vm_page - 1)))
    return 2;
  static const struct {
    const char* name;
    int (*test)(void);
  } suites[] = {
      {"remap", vm_test_remap}, {"residency", vm_test_residency}, {"discard", vm_test_discard}};
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
  puts("VM-EXPANSION-CONTRACT: END PASS");
  return 0;
}
