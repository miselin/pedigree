#ifndef MEMFD_CONTRACT_H
#define MEMFD_CONTRACT_H
#include <errno.h>
#include <fcntl.h>
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
#define MEMFD_APP "/applications/memfd-contract-test"

int64_t mf_now(void);
void mf_pause(int milliseconds);
int mf_wait(volatile int* flag, int milliseconds);
int mf_reap(pid_t child, int milliseconds);
int mf_byte(int socket, char expected);
int mf_send_fd(int socket, int fd);
int mf_receive_fd(int socket);
int mf_make(size_t bytes);
int mf_size(int fd, off_t expected);
int mf_contents(int fd, off_t offset, const void* expected, size_t length);
int memfd_creation(void);
int memfd_seals(void);
int memfd_mappings(void);
int memfd_lifetime(void);
int memfd_races(void);
int memfd_exec(int argc, char** argv);
#endif
