#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/syscall.h>

static _Thread_local volatile unsigned tls_value;

static int allocator_current(void) {
  int failed = 0;
  unsigned char* allocation = malloc(192 * 1024);
  CHECK(allocation != NULL);
  for (size_t n = 0; n < 192 * 1024; ++n)
    allocation[n] = (unsigned char)(n * 17 + 0x39);
  unsigned char* grown = realloc(allocation, 384 * 1024);
  CHECK(grown != NULL);
  allocation = grown;
  for (size_t n = 0; n < 192 * 1024; ++n)
    CHECK(allocation[n] == (unsigned char)(n * 17 + 0x39));
  CHECK(ml_limit(16 * 1024 * 1024) == 0 && ml_unprivileged() == 0);
  CHECK(mlockall(MCL_CURRENT | MCL_ONFAULT) == 0);
  CHECK(mlockall(MCL_CURRENT) == 0);
  CHECK(allocation[0] == 0x39 && allocation[ml_page] == (unsigned char)(ml_page * 17 + 0x39));
out:
  munlockall();
  free(allocation);
  return failed;
}

static int initial_stack(void) {
  int failed = 0;
  volatile unsigned char storage[3 * ml_page];
  volatile unsigned char* aligned =
      (volatile unsigned char*)(((uintptr_t)storage + ml_page - 1) & ~(uintptr_t)(ml_page - 1));
  void* managed = MAP_FAILED;
  aligned[0] = 0x47;
  aligned[ml_page] = 0x68;
  managed = mmap(NULL, ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(managed != MAP_FAILED && ml_limit(2 * ml_page) == 0);
  CHECK(mlock((void*)aligned, 2 * ml_page) == 0);
  errno = 0;
  CHECK(mlock(managed, ml_page) == -1 && errno == ENOMEM);
  CHECK(munlock((void*)aligned, ml_page) == 0 && mlock(managed, ml_page) == 0);
  CHECK(aligned[0] == 0x47 && aligned[ml_page] == 0x68);
out:
  munlockall();
  if (managed != MAP_FAILED)
    munmap(managed, ml_page);
  return failed;
}

static void* touch_tls(void* argument) {
  tls_value = 0x69;
  return (void*)(uintptr_t)(tls_value != 0x69 || argument != (void*)1);
}

static int future_thread(void) {
  int failed = 0;
  pthread_t thread;
  pthread_attr_t attributes;
  int attributes_live = 0;
  int thread_live = 0;
  void* result = NULL;
  CHECK(pthread_attr_init(&attributes) == 0);
  attributes_live = 1;
  CHECK(pthread_attr_setstacksize(&attributes, 32 * ml_page) == 0);
  CHECK(pthread_create(&thread, &attributes, touch_tls, (void*)1) == 0);
  thread_live = 1;
  CHECK(pthread_join(thread, &result) == 0);
  thread_live = 0;
  CHECK(result == NULL && ml_limit(ml_page) == 0);
  CHECK(mlockall(MCL_FUTURE | MCL_ONFAULT) == 0);
  int created = pthread_create(&thread, &attributes, touch_tls, (void*)1);
  if (!created)
    thread_live = 1;
  CHECK(created == EAGAIN);
  CHECK(munlockall() == 0);
  CHECK(pthread_create(&thread, &attributes, touch_tls, (void*)1) == 0);
  thread_live = 1;
  CHECK(pthread_join(thread, &result) == 0);
  thread_live = 0;
  CHECK(result == NULL);
out:
  munlockall();
  if (thread_live)
    pthread_join(thread, NULL);
  if (attributes_live)
    pthread_attr_destroy(&attributes);
  return failed;
}

static int future_heap(void) {
  int failed = 0;
  unsigned char* managed = MAP_FAILED;
  size_t managed_length = 0;
  CHECK(ml_limit(2 * ml_page) == 0);
  uintptr_t original = (uintptr_t)syscall(SYS_brk, 0);
  CHECK(original && original != UINTPTR_MAX && original < UINTPTR_MAX - 5 * ml_page);
  uintptr_t start = (original + ml_page - 1) & ~(uintptr_t)(ml_page - 1);
  CHECK((uintptr_t)syscall(SYS_brk, start) == start);
  CHECK(mlockall(MCL_FUTURE | MCL_ONFAULT) == 0);
  CHECK((uintptr_t)syscall(SYS_brk, start + 1) == start + 1);
  CHECK((uintptr_t)syscall(SYS_brk, start + ml_page - 1) == start + ml_page - 1);
  uintptr_t accepted = start + ml_page + 1;
  CHECK((uintptr_t)syscall(SYS_brk, accepted) == accepted);
  errno = 0;
  CHECK((uintptr_t)syscall(SYS_brk, start + 2 * ml_page + 1) == accepted && errno == 0);
  CHECK((uintptr_t)syscall(SYS_brk, 0) == accepted);
  volatile unsigned char* bytes = (volatile unsigned char*)start;
  CHECK(bytes[0] == 0 && bytes[ml_page] == 0);
  bytes[0] = 0x53;
  bytes[ml_page] = 0x76;
  CHECK(munlock((void*)start, ml_page) == 0);
  CHECK((uintptr_t)syscall(SYS_brk, start + 2 * ml_page + 1) == start + 2 * ml_page + 1);
  CHECK(bytes[0] == 0x53 && bytes[ml_page] == 0x76 && bytes[2 * ml_page] == 0);
  CHECK(munlockall() == 0);
  CHECK((uintptr_t)syscall(SYS_brk, start + 4 * ml_page) == start + 4 * ml_page);
  bytes[2 * ml_page] = 0x29;
  bytes[3 * ml_page] = 0x64;
  CHECK(mlock((void*)start, 2 * ml_page) == 0);
  managed = mmap(NULL, 3 * ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(managed != MAP_FAILED);
  managed_length = 3 * ml_page;
  errno = 0;
  CHECK(mlock(managed, ml_page) == -1 && errno == ENOMEM);
  CHECK(munmap(managed + 2 * ml_page, ml_page) == 0);
  managed_length = 2 * ml_page;
  CHECK(bytes[0] == 0x53 && bytes[ml_page] == 0x76 && bytes[2 * ml_page] == 0x29 &&
        bytes[3 * ml_page] == 0x64);
  errno = 0;
  CHECK(mlock(managed, ml_page) == -1 && errno == ENOMEM);

  // Retiring a raw page must release only its own lock charge.
  CHECK(munmap((void*)(start + ml_page), ml_page) == 0);
  unsigned char resident;
  errno = 0;
  CHECK(mincore((void*)(start + ml_page), ml_page, &resident) == -1 && errno == ENOMEM);
  CHECK(bytes[0] == 0x53 && bytes[2 * ml_page] == 0x29 && bytes[3 * ml_page] == 0x64);
  CHECK(mlock(managed, ml_page) == 0);
  errno = 0;
  CHECK(mlock((void*)(start + 2 * ml_page), ml_page) == -1 && errno == ENOMEM);
  CHECK(munmap((void*)start, ml_page) == 0);
  CHECK(mlock((void*)(start + 2 * ml_page), ml_page) == 0);
  errno = 0;
  CHECK(mlock(managed + ml_page, ml_page) == -1 && errno == ENOMEM);
  CHECK(bytes[2 * ml_page] == 0x29 && bytes[3 * ml_page] == 0x64);
  CHECK(munmap((void*)(start + 3 * ml_page), ml_page) == 0);
  errno = 0;
  CHECK(mincore((void*)(start + 3 * ml_page), ml_page, &resident) == -1 && errno == ENOMEM);
  CHECK(bytes[2 * ml_page] == 0x29);
  errno = 0;
  CHECK(mlock(managed + ml_page, ml_page) == -1 && errno == ENOMEM);
out:
  // The public musl brk/sbrk wrappers do not grow this heap; the isolated child owns it.
  munlockall();
  if (managed != MAP_FAILED)
    munmap(managed, managed_length);
  return failed;
}

int ml_raw(void) {
  return allocator_current() || initial_stack() || future_thread() || future_heap();
}
