#define _GNU_SOURCE

/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include <errno.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/times.h>
#include <sys/wait.h>

extern void fail(void) __attribute__((noreturn));

struct linux_rusage_abi {
  int64_t slots[18];
};

struct linux_rusage_packet {
  struct linux_rusage_abi usage;
  unsigned char canary[32];
};

_Static_assert(sizeof(struct linux_rusage_abi) == 144, "Linux amd64 rusage ABI changed");
_Static_assert(RUSAGE_SELF == 0, "musl RUSAGE_SELF selector changed");
_Static_assert(RUSAGE_CHILDREN == -1, "musl RUSAGE_CHILDREN selector changed");
_Static_assert(RUSAGE_THREAD == 1, "musl RUSAGE_THREAD selector changed");
_Static_assert(offsetof(struct rusage, __reserved) == sizeof(struct linux_rusage_abi),
               "musl rusage prefix no longer matches the Linux syscall ABI");
_Static_assert(sizeof(struct rusage) == sizeof(struct linux_rusage_abi) + 16 * sizeof(long),
               "musl rusage reserve changed");

struct descendant_usage_report {
  int64_t user_microseconds;
  int64_t system_microseconds;
  int valid;
};

static int64_t timeval_microseconds(struct timeval value) {
  return (int64_t)value.tv_sec * 1000000 + value.tv_usec;
}

static int public_rusage_tail_valid(const struct rusage* usage) {
  for (size_t i = offsetof(struct rusage, ru_maxrss); i < offsetof(struct rusage, __reserved);
       ++i) {
    if (((const unsigned char*)usage)[i])
      return 0;
  }
  for (size_t i = offsetof(struct rusage, __reserved); i < sizeof(*usage); ++i) {
    if (((const unsigned char*)usage)[i] != 0xA5)
      return 0;
  }
  return 1;
}

static int burn_observable_user_time(void) {
  struct rusage before;
  struct rusage after;
  if (getrusage(RUSAGE_SELF, &before))
    return -1;
  const int64_t before_user = timeval_microseconds(before.ru_utime);

  volatile uint64_t value = 1;
  for (size_t attempt = 0; attempt < 256; ++attempt) {
    for (size_t i = 0; i < 100000; ++i)
      value = value * 1664525 + 1013904223;
    if (getrusage(RUSAGE_SELF, &after))
      return -1;
    if (timeval_microseconds(after.ru_utime) >= before_user + 20000)
      return 0;
  }
  return -1;
}

static int write_report(int descriptor, const struct descendant_usage_report* report) {
  const unsigned char* bytes = (const unsigned char*)report;
  size_t remaining = sizeof(*report);
  while (remaining) {
    ssize_t written = write(descriptor, bytes, remaining);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0)
      return -1;
    bytes += written;
    remaining -= (size_t)written;
  }
  return 0;
}

static int read_report(int descriptor, struct descendant_usage_report* report) {
  unsigned char* bytes = (unsigned char*)report;
  size_t remaining = sizeof(*report);
  while (remaining) {
    ssize_t received = read(descriptor, bytes, remaining);
    if (received < 0 && errno == EINTR)
      continue;
    if (received <= 0)
      return -1;
    bytes += received;
    remaining -= (size_t)received;
  }
  return 0;
}

static void test_child_resource_accounting(void) {
  struct rusage children_before;
  memset(&children_before, 0xA5, sizeof(children_before));
  if (getrusage(RUSAGE_CHILDREN, &children_before) || !public_rusage_tail_valid(&children_before))
    fail();
  const int64_t user_before = timeval_microseconds(children_before.ru_utime);
  const int64_t system_before = timeval_microseconds(children_before.ru_stime);

  struct tms times_before;
  if (times(&times_before) == (clock_t)-1 ||
      times_before.tms_cutime !=
          children_before.ru_utime.tv_sec * 100 + children_before.ru_utime.tv_usec / 10000 ||
      times_before.tms_cstime !=
          children_before.ru_stime.tv_sec * 100 + children_before.ru_stime.tv_usec / 10000)
    fail();

  int report_pipe[2];
  if (pipe(report_pipe))
    fail();
  pid_t child = fork();
  if (child < 0)
    fail();
  if (!child) {
    close(report_pipe[0]);
    struct descendant_usage_report report = {0};
    pid_t descendant = fork();
    if (!descendant)
      _exit(burn_observable_user_time() ? 124 : 0);

    struct rusage waited_descendant;
    memset(&waited_descendant, 0xA5, sizeof(waited_descendant));
    int descendant_status = 0;
    if (descendant > 0 &&
        wait4(descendant, &descendant_status, 0, &waited_descendant) == descendant &&
        WIFEXITED(descendant_status) && !WEXITSTATUS(descendant_status) &&
        public_rusage_tail_valid(&waited_descendant)) {
      struct rusage descendants;
      memset(&descendants, 0xA5, sizeof(descendants));
      if (!getrusage(RUSAGE_CHILDREN, &descendants) && public_rusage_tail_valid(&descendants)) {
        report.user_microseconds = timeval_microseconds(descendants.ru_utime);
        report.system_microseconds = timeval_microseconds(descendants.ru_stime);
        report.valid =
            report.user_microseconds >= timeval_microseconds(waited_descendant.ru_utime) &&
            report.system_microseconds >= timeval_microseconds(waited_descendant.ru_stime) &&
            report.user_microseconds > 0;
      }
    }
    const int reported = write_report(report_pipe[1], &report);
    close(report_pipe[1]);
    _exit(!reported && report.valid ? 0 : 125);
  }

  close(report_pipe[1]);
  struct descendant_usage_report report = {0};
  const int report_read = read_report(report_pipe[0], &report);
  close(report_pipe[0]);
  struct rusage waited_child;
  memset(&waited_child, 0xA5, sizeof(waited_child));
  int child_status = 0;
  if (report_read || wait4(child, &child_status, 0, &waited_child) != child ||
      !WIFEXITED(child_status) || WEXITSTATUS(child_status) || !report.valid ||
      !public_rusage_tail_valid(&waited_child) ||
      timeval_microseconds(waited_child.ru_utime) < report.user_microseconds ||
      timeval_microseconds(waited_child.ru_stime) < report.system_microseconds)
    fail();

  struct rusage children_after;
  memset(&children_after, 0xA5, sizeof(children_after));
  if (getrusage(RUSAGE_CHILDREN, &children_after) || !public_rusage_tail_valid(&children_after))
    fail();
  const int64_t user_added = timeval_microseconds(children_after.ru_utime) - user_before;
  const int64_t system_added = timeval_microseconds(children_after.ru_stime) - system_before;
  const int64_t waited_user = timeval_microseconds(waited_child.ru_utime);
  const int64_t waited_system = timeval_microseconds(waited_child.ru_stime);
  if (user_added + 1 < waited_user || user_added > waited_user + 1 ||
      system_added + 1 < waited_system || system_added > waited_system + 1)
    fail();

  struct tms times_after;
  if (times(&times_after) == (clock_t)-1 ||
      times_after.tms_cutime !=
          children_after.ru_utime.tv_sec * 100 + children_after.ru_utime.tv_usec / 10000 ||
      times_after.tms_cstime !=
          children_after.ru_stime.tv_sec * 100 + children_after.ru_stime.tv_usec / 10000 ||
      times_after.tms_cutime <= times_before.tms_cutime)
    fail();

  int gate[2];
  if (pipe(gate))
    fail();
  pid_t raw_child = fork();
  if (raw_child < 0)
    fail();
  if (!raw_child) {
    close(gate[1]);
    char token = 0;
    ssize_t received;
    do {
      received = read(gate[0], &token, sizeof(token));
    } while (received < 0 && errno == EINTR);
    close(gate[0]);
    _exit(received == sizeof(token) && token == 'x' ? 0 : 126);
  }

  close(gate[0]);
  struct linux_rusage_packet packet;
  struct linux_rusage_packet untouched_packet;
  memset(&packet, 0xA5, sizeof(packet));
  untouched_packet = packet;
  int raw_status = 0x5A5A5A5A;
  errno = 0;
  if (syscall(SYS_wait4, raw_child, &raw_status, WNOHANG, packet.usage.slots) ||
      raw_status != 0x5A5A5A5A || memcmp(&packet, &untouched_packet, sizeof(packet)))
    fail();

  const char token = 'x';
  if (write(gate[1], &token, sizeof(token)) != sizeof(token) || close(gate[1]))
    fail();
  memset(&packet, 0xA5, sizeof(packet));
  if (syscall(SYS_wait4, raw_child, &raw_status, 0, packet.usage.slots) != raw_child ||
      !WIFEXITED(raw_status) || WEXITSTATUS(raw_status) || packet.usage.slots[0] < 0 ||
      packet.usage.slots[1] < 0 || packet.usage.slots[1] >= 1000000 || packet.usage.slots[2] < 0 ||
      packet.usage.slots[3] < 0 || packet.usage.slots[3] >= 1000000)
    fail();
  for (size_t i = 4; i < 18; ++i) {
    if (packet.usage.slots[i])
      fail();
  }
  for (size_t i = 0; i < sizeof(packet.canary); ++i) {
    if (packet.canary[i] != 0xA5)
      fail();
  }

  pid_t null_child = fork();
  if (null_child < 0)
    fail();
  if (!null_child)
    _exit(0);
  if (wait4(null_child, &child_status, 0, 0) != null_child || !WIFEXITED(child_status) ||
      WEXITSTATUS(child_status))
    fail();

  memset(&packet, 0xA5, sizeof(packet));
  untouched_packet = packet;
  raw_status = 0x5A5A5A5A;
  errno = 0;
  if (syscall(SYS_wait4, raw_child, &raw_status, WNOHANG, packet.usage.slots) != -1 ||
      errno != ECHILD || raw_status != 0x5A5A5A5A ||
      memcmp(&packet, &untouched_packet, sizeof(packet)))
    fail();
}

void test_resource_accounting(void) {
  puts("Testing process resource accounting... ");
  fflush(stdout);

  struct tms processTimes = {0};
  const clock_t elapsed = times(&processTimes);
  if (elapsed == (clock_t)-1 || processTimes.tms_utime < 0 || processTimes.tms_stime < 0 ||
      processTimes.tms_cutime || processTimes.tms_cstime)
    fail();

  if (times(0) < elapsed)
    fail();

  struct rusage usage = {0};
  if (getrusage(RUSAGE_SELF, &usage) || usage.ru_utime.tv_sec < 0 || usage.ru_utime.tv_usec < 0 ||
      usage.ru_utime.tv_usec >= 1000000 || usage.ru_stime.tv_sec < 0 ||
      usage.ru_stime.tv_usec < 0 || usage.ru_stime.tv_usec >= 1000000)
    fail();

  const clock_t userTicks = usage.ru_utime.tv_sec * 100 + usage.ru_utime.tv_usec / 10000;
  if (userTicks < processTimes.tms_utime || userTicks > processTimes.tms_utime + 10)
    fail();

  struct linux_rusage_packet packet;
  memset(&packet, 0xA5, sizeof(packet));
  if (syscall(SYS_getrusage, RUSAGE_SELF, packet.usage.slots) || packet.usage.slots[0] < 0 ||
      packet.usage.slots[1] < 0 || packet.usage.slots[1] >= 1000000 || packet.usage.slots[2] < 0 ||
      packet.usage.slots[3] < 0 || packet.usage.slots[3] >= 1000000)
    fail();
  for (size_t i = 0; i < sizeof(packet.canary); ++i) {
    if (packet.canary[i] != 0xA5)
      fail();
  }

  struct rusage thread_usage;
  memset(&thread_usage, 0xA5, sizeof(thread_usage));
  if (getrusage(RUSAGE_THREAD, &thread_usage) || thread_usage.ru_utime.tv_sec < 0 ||
      thread_usage.ru_utime.tv_usec < 0 || thread_usage.ru_utime.tv_usec >= 1000000 ||
      thread_usage.ru_stime.tv_sec < 0 || thread_usage.ru_stime.tv_usec < 0 ||
      thread_usage.ru_stime.tv_usec >= 1000000)
    fail();
  for (size_t i = offsetof(struct rusage, ru_maxrss); i < offsetof(struct rusage, __reserved);
       ++i) {
    if (((unsigned char*)&thread_usage)[i])
      fail();
  }
  for (size_t i = offsetof(struct rusage, __reserved); i < sizeof(thread_usage); ++i) {
    if (((unsigned char*)&thread_usage)[i] != 0xA5)
      fail();
  }

  memset(&packet, 0xA5, sizeof(packet));
  if (syscall(SYS_getrusage, RUSAGE_THREAD, packet.usage.slots) || packet.usage.slots[0] < 0 ||
      packet.usage.slots[1] < 0 || packet.usage.slots[1] >= 1000000 || packet.usage.slots[2] < 0 ||
      packet.usage.slots[3] < 0 || packet.usage.slots[3] >= 1000000)
    fail();
  for (size_t i = 4; i < 18; ++i) {
    if (packet.usage.slots[i])
      fail();
  }
  for (size_t i = 0; i < sizeof(packet.canary); ++i) {
    if (packet.canary[i] != 0xA5)
      fail();
  }

  test_child_resource_accounting();

  puts("OK\n");
  fflush(stdout);
}
