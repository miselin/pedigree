#ifndef MEMORY_LOCK_CONTRACT_H
#define MEMORY_LOCK_CONTRACT_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include <sys/types.h>

#define CHECK(condition)                                                                \
  do {                                                                                  \
    if (!(condition)) {                                                                 \
      fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition, errno); \
      failed = 1;                                                                       \
      goto out;                                                                         \
    }                                                                                   \
  } while (0)

extern size_t ml_page;
int ml_limit(size_t bytes);
int ml_unprivileged(void);
int ml_resident(void* address, size_t pages, unsigned bits, const char* stage);
int ml_reap(pid_t child, int milliseconds);
int ml_send(int fd, char byte);
int ml_receive(int fd, char byte);
int ml_limits(void);
int ml_ranges(void);
int ml_managed(void);
int ml_raw(void);
int ml_lifetime(void);
int ml_exec(int argc, char** argv);

#endif
