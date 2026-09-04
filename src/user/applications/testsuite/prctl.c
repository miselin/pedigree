/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <sys/prctl.h>

extern void fail(void) __attribute__((noreturn));

void test_prctl(void) {
  puts("Testing prctl(2) thread names... ");
  fflush(stdout);

  char original[16] = {0};
  char returned[16];
  if (prctl(PR_GET_NAME, original, 0UL, 0UL, 0UL))
    fail();

  if (prctl(PR_SET_NAME, "0123456789abcdef-long", 0UL, 0UL, 0UL))
    fail();
  memset(returned, 0xA5, sizeof(returned));
  if (prctl(PR_GET_NAME, returned, 0UL, 0UL, 0UL) || memcmp(returned, "0123456789abcde", 15) ||
      returned[15])
    fail();

  errno = 0;
  if (prctl(-1, 0UL, 0UL, 0UL, 0UL) != -1 || errno != EINVAL)
    fail();
  errno = 0;
  if (prctl(PR_GET_NAME, (char*)-1, 0UL, 0UL, 0UL) != -1 || errno != EFAULT)
    fail();
  errno = 0;
  if (prctl(PR_SET_NAME, (char*)-1, 0UL, 0UL, 0UL) != -1 || errno != EFAULT)
    fail();

  if (prctl(PR_SET_NAME, original, 0UL, 0UL, 0UL))
    fail();
  puts("OK\n");
  fflush(stdout);
}
