#ifndef TRANSFER_CONTRACT_H
#define TRANSFER_CONTRACT_H
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
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
enum { TF_SENDFILE, TF_COPY_RANGE, TF_CHUNK = 65536, TF_FILLER = -1 };
struct tf_file {
  int fd;
  char path[128];
};
struct tf_transfer {
  int kind, input, output;
  off_t *input_offset, *output_offset;
  size_t count;
  volatile int ready, gate, done;
  ssize_t result;
  int error;
  int64_t started, finished;
};
int64_t tf_now(void);
void tf_pause(int milliseconds);
int tf_wait(volatile int* flag, int milliseconds);
int tf_reap(pid_t child, int milliseconds);
unsigned char tf_pattern(size_t offset, int seed);
int tf_create(struct tf_file* file, const char* directory, size_t bytes, int seed);
void tf_destroy(struct tf_file* file);
int tf_verify(int fd, off_t offset, size_t length, size_t source_offset, int seed);
int tf_socket_read(int fd, size_t length, size_t source_offset, int seed);
ssize_t tf_saturate(int fd);
ssize_t tf_copy(int kind, int input, off_t* input_offset, int output, off_t* output_offset,
                size_t count);
void* tf_worker(void* argument);
void tf_diagnostic(const struct tf_transfer* transfer);
int tf_join(pthread_t thread, struct tf_transfer* transfer);
int transfer_file_copy(void);
int transfer_offsets(void);
int transfer_stream(void);
int transfer_lifetime(void);
int transfer_races(void);
#endif
