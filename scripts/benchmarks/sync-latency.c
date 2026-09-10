// SPDX-License-Identifier: ISC
// Run as the disposable guest init; the host gates each measured sync.
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define MAX_ARGS 32
#define PAGE_BYTES 4096U
#define MAX_FIXTURE (32U * 1024U * 1024U)
#define MAX_OUTPUT (1024U * 1024U)
static int serial_fd = -1;
static int config_mode;
static pid_t active_child = -1;
extern char** environ;

static void write_all(int fd, const void* buffer, size_t size);
static void fail(const char* operation) {
  static int failing;
  if (failing++)
    _exit(1);
  int saved_errno = errno;
  if (active_child > 0) {
    (void)kill(active_child, SIGKILL);
    while (waitpid(active_child, NULL, 0) < 0 && errno == EINTR) {
    }
  }
  // Init may have redirected stderr before the config's --serial is parsed.
  int destination = serial_fd;
  if (destination < 0 && config_mode)
    destination = open("/dev/ttyS0", O_WRONLY | O_NONBLOCK);
  if (destination < 0)
    destination = STDERR_FILENO;
  char text[160];
  int n = snprintf(text, sizeof(text), "LAUNCHBENCH FAIL operation=%s errno=%d\n", operation,
                   saved_errno);
  write_all(destination, text, (size_t)n);
  exit(1);
}

static void ready_fd(int fd, short events) {
  struct pollfd p = {fd, events, 0};
  int rc;
  do {
    rc = poll(&p, 1, -1);
  } while (rc < 0 && errno == EINTR);
  if (rc < 0 || (p.revents & (POLLERR | POLLNVAL)) || !(p.revents & events))
    fail("poll");
}

static void write_all(int fd, const void* buffer, size_t size) {
  const unsigned char* p = buffer;
  while (size) {
    ssize_t n = write(fd, p, size);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      ready_fd(fd, POLLOUT);
      continue;
    }
    if (n <= 0)
      fail("write");
    p += n;
    size -= (size_t)n;
  }
}

static uint64_t now_ns(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t))
    fail("clock");
  return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

static void marker(const char* kind, const char* phase) {
  char text[128];
  int n = snprintf(text, sizeof(text), "LAUNCHBENCH %s phase=%s\n", kind, phase);
  write_all(STDOUT_FILENO, text, (size_t)n);
}

static void gate(const char* phase) {
  marker("READY", phase);
  if (serial_fd < 0)
    return;
  for (;;) {
    char c;
    ssize_t n = read(serial_fd, &c, 1);
    if (n == 1 && c == 'g')
      return;
    if (n == 1 || (n < 0 && errno == EINTR))
      continue;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
      fail("gate-read");
    ready_fd(serial_fd, POLLIN);
  }
}

static void metric(const char* phase, uint64_t start, uint64_t first, uint64_t end, uint64_t bytes,
                   uint64_t checksum) {
  char text[256];
  int n = snprintf(
      text, sizeof(text),
      "LAUNCHBENCH metric phase=%s first_us=%llu total_us=%llu bytes=%llu checksum=%llu\n", phase,
      (unsigned long long)((first - start) / 1000), (unsigned long long)((end - start) / 1000),
      (unsigned long long)bytes, (unsigned long long)checksum);
  write_all(STDOUT_FILENO, text, (size_t)n);
  marker("DONE", phase);
}

#define BYTES (1024U * 1024U)
static unsigned char data[BYTES];
static void pattern(unsigned seed) {
  for (size_t i = 0; i < BYTES; ++i)
    data[i] = (unsigned char)(seed + i * 17U + (i >> 8));
}
static void run_sync(const char* phase) {
  gate(phase);
  uint64_t start = now_ns();
  sync();
  uint64_t end = now_ns();
  metric(phase, start, end, end, BYTES, 0);
}
int main(void) {
  serial_fd = open("/dev/ttyS0", O_RDWR | O_NONBLOCK);
  if (serial_fd < 0 || dup2(serial_fd, 1) < 0 || dup2(serial_fd, 2) < 0)
    fail("serial");
  struct termios t;
  if (!tcgetattr(serial_fd, &t)) {
    t.c_iflag = t.c_oflag = t.c_lflag = 0;
    t.c_cflag = (t.c_cflag & ~(CSIZE | PARENB)) | CS8;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    (void)tcsetattr(serial_fd, TCSANOW, &t);
  }
  int fd = open("/sync-bench.bin", O_RDWR);
  if (fd < 0)
    fail("open-fixture");
  size_t total = 0;
  while (total < BYTES) {
    ssize_t n = read(fd, data + total, BYTES - total);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      fail("read-fixture");
    total += n;
  }
  const int restored = data[0] == 0x71;
  for (size_t i = 0; i < BYTES; ++i) {
    unsigned char expected = restored ? (unsigned char)(0x71 + i * 17U + (i >> 8)) : 0x11;
    if (data[i] != expected)
      fail("persisted-content");
  }
  dprintf(1, "SYNCBENCH persistence=%s bytes=%u\n", restored ? "PASS" : "initial", BYTES);
  sync();
  pattern(0x31);
  if (lseek(fd, 0, SEEK_SET))
    fail("seek");
  write_all(fd, data, BYTES);
  run_sync("sync-dirty-0");
  run_sync("sync-clean-0");
  pattern(0x71);
  if (lseek(fd, 0, SEEK_SET))
    fail("seek");
  write_all(fd, data, BYTES);
  run_sync("sync-redirty-0");
  if (close(fd))
    fail("close");
  puts("LAUNCHBENCH PASS END");
  fflush(stdout);
  for (;;)
    pause();
}
