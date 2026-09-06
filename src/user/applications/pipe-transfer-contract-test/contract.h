#ifndef PIPE_TRANSFER_CONTRACT_H
#define PIPE_TRANSFER_CONTRACT_H
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include <sys/types.h>
#include <sys/uio.h>

#define CHECK(condition)                                                                \
  do {                                                                                  \
    if (!(condition)) {                                                                 \
      fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition, errno); \
      failed = 1;                                                                       \
      goto out;                                                                         \
    }                                                                                   \
  } while (0)
enum { PT_SPLICE, PT_TEE, PT_VMSPLICE, PT_CAPACITY = 4096, PT_FILLER = -1 };
struct pt_call {
  int kind, input, output;
  off_t *input_offset, *output_offset;
  size_t count;
  unsigned flags;
  struct iovec* vectors;
  size_t vector_count;
  volatile int ready, gate, entered, done;
  ssize_t result;
  int error;
};
int64_t pt_now(void);
void pt_pause(int milliseconds);
int pt_wait(volatile int* flag, int milliseconds);
int pt_reap(pid_t child, int milliseconds);
int pt_ready(int fd, short events, int milliseconds);
int pt_wait_readiness(int fd, short events, int expected);
unsigned char pt_pattern(size_t offset, int seed);
int pt_fill(int fd, size_t count, int seed);
int pt_read(int fd, size_t count, size_t source_offset, int seed);
int pt_file(size_t count, int seed);
int pt_verify(int fd, off_t offset, size_t count, size_t source_offset, int seed);
ssize_t pt_socket_full(int fd);
int pt_send_fd(int socket, int fd);
int pt_receive_fd(int socket);
void* pt_worker(void* argument);
int pt_join(pthread_t worker, struct pt_call* call);
void pt_diagnostic(const struct pt_call* call);
int pipe_transfer_splice(void);
int pipe_transfer_tee(void);
int pipe_transfer_vmsplice(void);
int pipe_transfer_lifetime(void);
int pipe_transfer_readiness(void);
int pipe_transfer_concurrency(void);
#endif
