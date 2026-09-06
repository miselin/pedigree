#ifndef VM_CONTRACT_H
#define VM_CONTRACT_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include <sys/types.h>

#define CHECK(condition)                                                                \
  do {                                                                                  \
    if (!(condition)) {                                                                 \
      fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition, errno); \
      failed = 1;                                                                       \
      goto out;                                                                         \
    }                                                                                   \
  } while (0)

extern size_t vm_page;
int64_t vm_now(void);
void vm_pause(int milliseconds);
int vm_wait(volatile int* flag, int milliseconds);
int vm_reap(pid_t child, int milliseconds);
int vm_fault(void* address, int writing, int expected_signal);
int vm_file(size_t pages, int* readonly);
int vm_uniform(const unsigned char* bytes, size_t length, unsigned char value);
int vm_test_remap(void);
int vm_test_remap_file(void);
int vm_test_remap_shm(void);
int vm_test_residency(void);
int vm_test_discard(void);

#endif
