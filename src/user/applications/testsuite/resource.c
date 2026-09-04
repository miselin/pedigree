/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/times.h>

extern void fail(void) __attribute__((noreturn));

struct linux_rusage_abi {
  int64_t slots[18];
};

struct linux_rusage_packet {
  struct linux_rusage_abi usage;
  unsigned char canary[32];
};

_Static_assert(sizeof(struct linux_rusage_abi) == 144, "Linux amd64 rusage ABI changed");
_Static_assert(offsetof(struct rusage, __reserved) == sizeof(struct linux_rusage_abi),
               "musl rusage prefix no longer matches the Linux syscall ABI");
_Static_assert(sizeof(struct rusage) == sizeof(struct linux_rusage_abi) + 16 * sizeof(long),
               "musl rusage reserve changed");

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

  struct rusage untouched;
  memset(&untouched, 0xA5, sizeof(untouched));
  usage = untouched;
  errno = 0;
  if (getrusage(RUSAGE_CHILDREN, &usage) != -1 || errno != EINVAL ||
      memcmp(&usage, &untouched, sizeof(usage)))
    fail();

  puts("OK\n");
  fflush(stdout);
}
