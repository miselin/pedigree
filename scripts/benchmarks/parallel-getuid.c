/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <sys/syscall.h>

enum { Workers = 4, Iterations = 1000000 };
static pthread_barrier_t barrier;
static long expected_uid;
static int failures;

static void* run(void* arg) {
  (void)arg;
  pthread_barrier_wait(&barrier);
  for (unsigned i = 0; i < Iterations; ++i) {
    long value = SYS_getuid;
    __asm__ volatile("syscall" : "+a"(value) : : "rcx", "r11", "memory", "cc");
    if (value != expected_uid)
      __atomic_store_n(&failures, 1, __ATOMIC_RELAXED);
  }
  return NULL;
}

static uint64_t now(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts))
    exit(1);
  return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int main(void) {
  pthread_t threads[Workers];
  alarm(30);
  setvbuf(stdout, NULL, _IONBF, 0);
  expected_uid = syscall(SYS_getuid);
  if (pthread_barrier_init(&barrier, NULL, Workers + 1))
    return 1;
  for (unsigned i = 0; i < Workers; ++i)
    if (pthread_create(&threads[i], NULL, run, NULL))
      return 1;
  uint64_t started = now();
  pthread_barrier_wait(&barrier);
  for (unsigned i = 0; i < Workers; ++i)
    if (pthread_join(threads[i], NULL))
      return 1;
  uint64_t elapsed = now() - started;
  if (failures)
    return 1;
  printf("IOBENCH metric phase=parallel_getuid elapsed_us=%llu operations=%u pages=0 checksum=0\n",
         (unsigned long long)elapsed, Workers * Iterations);
  puts("IOBENCH PASS END");
  return 0;
}
