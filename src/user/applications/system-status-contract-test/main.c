/* Copyright (c) 2026, Pedigree Developers. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pedigree/log.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#define CHECK(expression)                                                                  \
  do {                                                                                     \
    if (!(expression)) {                                                                   \
      fprintf(stderr, "SYSTEM-STATUS-CONTRACT: FAIL line=%d errno=%d\n", __LINE__, errno); \
      return 1;                                                                            \
    }                                                                                      \
  } while (0)

static int memory_status(void) {
  int fd = open("/proc/meminfo", O_RDONLY);
  CHECK(fd >= 0);
  unsigned char guarded[16];
  memset(guarded, 0xa5, sizeof(guarded));
  CHECK(pread(fd, guarded + 1, 1, 0) == 1 && guarded[1] == 'M');
  CHECK(guarded[0] == 0xa5 && guarded[2] == 0xa5);
  CHECK(pread(fd, guarded + 1, 5, 4) == 5 && !memcmp(guarded + 1, "otal:", 5));
  CHECK(guarded[0] == 0xa5 && guarded[6] == 0xa5);
  CHECK(pread(fd, guarded, sizeof(guarded), LLONG_MAX - sizeof(guarded)) == 0);

  char contents[512];
  ssize_t bytes = pread(fd, contents, sizeof(contents) - 1, 0);
  CHECK(bytes > 0);
  contents[bytes] = 0;
  unsigned long long total = 0, freeKb = 0;
  CHECK(sscanf(contents, "MemTotal: %llu kB\nMemFree: %llu kB", &total, &freeKb) == 2);
  CHECK(total > 0 && freeKb <= total);
  CHECK(close(fd) == 0);
  puts("SYSTEM-STATUS-CONTRACT: MEMORY PASS");
  return 0;
}

static int network_status(void) {
  int fd = open("/proc/net/interfaces", O_RDONLY);
  CHECK(fd >= 0);
  char contents[8192];
  ssize_t bytes = read(fd, contents, sizeof(contents) - 1);
  CHECK(bytes > 0);
  contents[bytes] = 0;
  CHECK(strstr(contents, "  Device:") || !strcmp(contents, "No network devices.\n"));
  if (strstr(contents, "  Device:")) {
    CHECK(strstr(contents, "  MAC:") && strstr(contents, "  IPv4:") &&
          strstr(contents, "  Netmask:") && strstr(contents, "  Gateway:"));
  }
  CHECK(close(fd) == 0);
  fd = open("/proc/resolv.conf", O_RDONLY);
  CHECK(fd >= 0);
  bytes = read(fd, contents, sizeof(contents) - 1);
  CHECK(bytes >= 0);
  contents[bytes] = 0;
  CHECK(!bytes || strstr(contents, "nameserver "));
  CHECK(close(fd) == 0);
  puts("SYSTEM-STATUS-CONTRACT: NETWORK PASS");
  return 0;
}

static int kernel_log(void) {
  CHECK(syscall(SYS_syslog, 0, NULL, 0) == 0);
  CHECK(syscall(SYS_syslog, 1, NULL, 0) == 0);
  long capacity = syscall(SYS_syslog, 10, NULL, 0);
  CHECK(capacity > 0 && capacity < INT_MAX);
  CHECK(syscall(SYS_syslog, 3, NULL, 0) == 0);
  errno = 0;
  CHECK(syscall(SYS_syslog, 3, NULL, -1) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(syscall(SYS_syslog, 3, (void*)(uintptr_t)1, 1) == -1 && errno == EFAULT);
  errno = 0;
  CHECK(syscall(SYS_syslog, 42, NULL, 0) == -1 && errno == EINVAL);
  void* readonly = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(readonly != MAP_FAILED);
  errno = 0;
  CHECK(syscall(SYS_syslog, 3, readonly, 1) == -1 && errno == EFAULT);
  CHECK(munmap(readonly, 4096) == 0);

  char marker[80];
  snprintf(marker, sizeof(marker), "SYSTEM-STATUS-KLOG-%ld", (long)getpid());
  CHECK(pedigree_log(LOG_NOTICE, "%s", marker) == 0);
  char* buffer = malloc((size_t)capacity + 2);
  CHECK(buffer);
  for (int pass = 0; pass < 3; ++pass) {
    memset(buffer, 0xa5, (size_t)capacity + 2);
    long count = syscall(SYS_syslog, 3, buffer + 1, capacity);
    CHECK(count > 0 && count <= capacity);
    CHECK((unsigned char)buffer[0] == 0xa5 && (unsigned char)buffer[capacity + 1] == 0xa5);
    buffer[count + 1] = 0;
    CHECK(strstr(buffer + 1, marker));
    if (pass == 1) {
      errno = 0;
      CHECK(syscall(SYS_syslog, 4, buffer + 1, capacity) == -1 && errno == ENOSYS);
      errno = 0;
      CHECK(syscall(SYS_syslog, 5, NULL, 0) == -1 && errno == ENOSYS);
    }
  }
  unsigned char tail[3] = {0xa5, 0xa5, 0xa5};
  CHECK(syscall(SYS_syslog, 3, tail + 1, 1) == 1 && tail[1] == '\n');
  CHECK(tail[0] == 0xa5 && tail[2] == 0xa5);
  free(buffer);
  puts("SYSTEM-STATUS-CONTRACT: KERNEL-LOG PASS");
  return 0;
}

int main(void) {
  if (memory_status() || network_status() || kernel_log())
    return 1;
  puts("SYSTEM-STATUS-CONTRACT: PASS");
  return 0;
}
