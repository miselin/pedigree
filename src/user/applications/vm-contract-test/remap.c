#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/syscall.h>

static int anonymous_resize(void) {
  int failed = 0;
  const size_t p = vm_page;
  unsigned char *base = MAP_FAILED, *moved = MAP_FAILED, *tail = MAP_FAILED, *reuse = MAP_FAILED;
  unsigned char resident[8];
  CHECK((base = mmap(NULL, 8 * p, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) !=
        MAP_FAILED);
  for (int n = 0; n < 8; ++n)
    memset(base + n * p, 0x40 + n, p);
  CHECK(mremap(base, 8 * p, 8 * p, 0) == base);
  CHECK(mremap(base, 8 * p, 4 * p, 0) == base);
  CHECK(mincore(base + 4 * p, p, resident) == -1 && errno == ENOMEM);
  CHECK((tail = mmap(base + 4 * p, 4 * p, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0)) == base + 4 * p);
  memset(tail, 0xee, 4 * p);
  CHECK(mremap(base, 4 * p, 6 * p, 0) == MAP_FAILED && errno == ENOMEM);
  CHECK(vm_uniform(base, p, 0x40) && vm_uniform(base + 3 * p, p, 0x43) &&
        vm_uniform(tail, 4 * p, 0xee));
  CHECK((moved = mremap(base, 4 * p, 6 * p, MREMAP_MAYMOVE)) != MAP_FAILED && moved != base);
  CHECK(mincore(moved, 6 * p, resident) == 0 && resident[4] == 0 && resident[5] == 0);
  for (int n = 0; n < 4; ++n)
    CHECK(vm_uniform(moved + n * p, p, 0x40 + n));
  CHECK(vm_uniform(moved + 4 * p, 2 * p, 0));
  CHECK(mincore(base, p, resident) == -1 && errno == ENOMEM);
  CHECK((reuse = mmap(base, 4 * p, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0)) == base);
  CHECK(vm_uniform(reuse, p, 0) && vm_uniform(tail, 4 * p, 0xee));
out:
  if (moved != MAP_FAILED)
    munmap(moved, 6 * p);
  if (base != MAP_FAILED)
    munmap(base, 8 * p);
  return failed;
}

static int growth_and_rejection(void) {
  int failed = 0;
  const size_t p = vm_page;
  unsigned char* base =
      mmap(NULL, 4 * p, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  unsigned char resident[4];
  volatile int stack_value = 17;
  void* stack_page = (void*)((uintptr_t)&stack_value & ~(uintptr_t)(p - 1));
  CHECK(base != MAP_FAILED);
  memset(base, 0x51, 2 * p);
  CHECK(munmap(base + 2 * p, 2 * p) == 0);
  CHECK(mremap(base, 2 * p, 4 * p, 0) == base);
  CHECK(mincore(base, 4 * p, resident) == 0 && resident[0] == 1 && resident[1] == 1 &&
        resident[2] == 0 && resident[3] == 0);
  CHECK(vm_uniform(base, 2 * p, 0x51) && vm_uniform(base + 2 * p, 2 * p, 0));
  CHECK(mremap(base + 1, p, p, 0) == MAP_FAILED && errno == EINVAL);
  CHECK(mremap(base, 4 * p, 0, 0) == MAP_FAILED && errno == EINVAL);
  CHECK(mremap(base, 4 * p, 4 * p, 8) == MAP_FAILED && errno == EINVAL);
  CHECK(mremap(base, 0, p, 0) == MAP_FAILED && errno == EINVAL);
  CHECK(mremap(base, 0, p, MREMAP_MAYMOVE) == MAP_FAILED && errno == EOPNOTSUPP);
  CHECK(mremap(base, 4 * p, 4 * p, MREMAP_MAYMOVE | MREMAP_DONTUNMAP) == MAP_FAILED &&
        errno == EOPNOTSUPP);
  CHECK(mremap(base, 4 * p, 4 * p, MREMAP_FIXED, base + 8 * p) == MAP_FAILED && errno == EINVAL);
  CHECK(mremap(base, 4 * p, 2 * p, MREMAP_FIXED | MREMAP_MAYMOVE, base + p) == MAP_FAILED &&
        errno == EINVAL);
  CHECK(mremap(base, 4 * p, 2 * p, MREMAP_FIXED | MREMAP_MAYMOVE, base + 1) == MAP_FAILED &&
        errno == EINVAL);
  CHECK(mremap(base, 4 * p, 65537 * p, MREMAP_MAYMOVE) == MAP_FAILED && errno == ENOMEM);
  CHECK((void*)syscall(SYS_mremap, base, 4 * p, SIZE_MAX, 0, NULL) == MAP_FAILED &&
        errno == EINVAL);
  CHECK(mremap(stack_page, p, p, 0) == MAP_FAILED && errno == EOPNOTSUPP && stack_value == 17);
  CHECK(vm_uniform(base, 2 * p, 0x51) && vm_uniform(base + 2 * p, 2 * p, 0));
out:
  if (base != MAP_FAILED)
    munmap(base, 4 * p);
  return failed;
}

static int fixed_victims(void) {
  int failed = 0;
  const size_t p = vm_page;
  unsigned char* source =
      mmap(NULL, 3 * p, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  unsigned char* target =
      mmap(NULL, 5 * p, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  unsigned char resident;
  CHECK(source != MAP_FAILED && target != MAP_FAILED);
  for (int n = 0; n < 3; ++n)
    memset(source + n * p, 0x61 + n, p);
  for (int n = 0; n < 5; ++n)
    memset(target + n * p, 0x71 + n, p);
  CHECK(mprotect(target + 4 * p, p, PROT_READ) == 0);
  CHECK(mremap(source, 3 * p, 2 * p, MREMAP_FIXED | MREMAP_MAYMOVE, target + p) == target + p);
  CHECK(vm_uniform(target, p, 0x71) && vm_uniform(target + p, p, 0x61) &&
        vm_uniform(target + 2 * p, p, 0x62) && vm_uniform(target + 3 * p, p, 0x74) &&
        vm_uniform(target + 4 * p, p, 0x75));
  CHECK(vm_fault(target + 4 * p, 1, SIGSEGV) == 0);
  CHECK(mincore(source, p, &resident) == -1 && errno == ENOMEM);
  CHECK(mremap(source, p, p, 0) == MAP_FAILED && errno == EFAULT);
  CHECK(mmap(source, 3 * p, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == source);
  CHECK(vm_uniform(source, 3 * p, 0));
out:
  if (source != MAP_FAILED)
    munmap(source, 3 * p);
  if (target != MAP_FAILED)
    munmap(target, 5 * p);
  return failed;
}

static int fork_cow(void) {
  int failed = 0, gate[2] = {-1, -1};
  const size_t p = vm_page;
  pid_t child = -1;
  unsigned char* areas[3] = {MAP_FAILED, MAP_FAILED, MAP_FAILED};
  unsigned char* current;
  for (int n = 0; n < 3; ++n) {
    areas[n] = mmap(NULL, 2 * p, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(areas[n] != MAP_FAILED);
  }
  current = areas[0];
  memset(current, 0x31, 2 * p);
  CHECK(pipe(gate) == 0 && (child = fork()) >= 0);
  if (!child) {
    alarm(5);
    char byte;
    if (read(gate[0], &byte, 1) != 1)
      _exit(10);
    for (int n = 0; n < 12; ++n) {
      memset(areas[0], 0xc0 + n, 2 * p);
      vm_pause(2);
      if (!vm_uniform(areas[0], 2 * p, 0xc0 + n))
        _exit(11);
    }
    _exit(0);
  }
  CHECK(write(gate[1], "g", 1) == 1);
  for (int n = 0; n < 12; ++n) {
    memset(current, 0x40 + n, 2 * p);
    unsigned char* next = areas[1 + (n & 1)];
    CHECK(mremap(current, 2 * p, 2 * p, MREMAP_FIXED | MREMAP_MAYMOVE, next) == next);
    current = next;
    vm_pause(2);
    CHECK(vm_uniform(current, 2 * p, 0x40 + n));
  }
  int status = vm_reap(child, 3000);
  child = -1;
  CHECK(status == 0 && vm_uniform(current, 2 * p, 0x4b));
out:
  if (child > 0) {
    kill(child, SIGKILL);
    vm_reap(child, 1000);
  }
  for (int n = 0; n < 2; ++n)
    if (gate[n] >= 0)
      close(gate[n]);
  for (int n = 0; n < 3; ++n)
    if (areas[n] != MAP_FAILED)
      munmap(areas[n], 2 * p);
  return failed;
}

static __thread unsigned char tls_bytes[8192];
static void* tls_probe(void* argument) {
  unsigned char value = (uintptr_t)argument;
  memset(tls_bytes, value, sizeof(tls_bytes));
  vm_pause(2);
  return vm_uniform(tls_bytes, sizeof(tls_bytes), value) ? NULL : (void*)1;
}
static void* thread_churn(void* ignored) {
  (void)ignored;
  for (uintptr_t n = 1; n <= 12; ++n) {
    pthread_t thread;
    void* result;
    if (pthread_create(&thread, NULL, tls_probe, (void*)n) || pthread_join(thread, &result) ||
        result)
      return (void*)1;
  }
  return NULL;
}
static int allocation_and_tls(void) {
  int failed = 0, active = 0;
  const size_t p = vm_page;
  pthread_t worker;
  void* result;
  unsigned char* heap = malloc(192 * 1024);
  unsigned char *current = MAP_FAILED, *next = MAP_FAILED;
  CHECK(heap != NULL);
  memset(heap, 0x83, 192 * 1024);
  unsigned char* replacement = realloc(heap, 384 * 1024);
  CHECK(replacement != NULL);
  heap = replacement;
  CHECK(vm_uniform(heap, 192 * 1024, 0x83));
  memset(heap + 192 * 1024, 0x84, 192 * 1024);
  replacement = realloc(heap, 160 * 1024);
  CHECK(replacement != NULL);
  heap = replacement;
  CHECK(vm_uniform(heap, 160 * 1024, 0x83));
  CHECK((current = mmap(NULL, 2 * p, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) !=
        MAP_FAILED);
  memset(current, 0x93, 2 * p);
  CHECK(pthread_create(&worker, NULL, thread_churn, NULL) == 0);
  active = 1;
  for (int n = 0; n < 24; ++n) {
    // A vacated address can become a thread stack; reserve a fresh victim.
    CHECK((next = mmap(NULL, 2 * p, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED);
    CHECK(mremap(current, 2 * p, 2 * p, MREMAP_FIXED | MREMAP_MAYMOVE, next) == next);
    current = next;
    next = MAP_FAILED;
    vm_pause(2);
    CHECK(vm_uniform(current, 2 * p, 0x93));
  }
  CHECK(pthread_join(worker, &result) == 0);
  active = 0;
  CHECK(result == NULL && vm_uniform(heap, 160 * 1024, 0x83));
out:
  if (active)
    pthread_join(worker, NULL);
  if (current != MAP_FAILED)
    munmap(current, 2 * p);
  if (next != MAP_FAILED)
    munmap(next, 2 * p);
  free(heap);
  return failed;
}

int vm_test_remap(void) {
  return anonymous_resize() || growth_and_rejection() || fixed_victims() || vm_test_remap_file() ||
         vm_test_remap_shm() || fork_cow() || allocation_and_tls();
}
