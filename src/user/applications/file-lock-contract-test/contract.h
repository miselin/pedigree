#ifndef FILE_LOCK_CONTRACT_H
#define FILE_LOCK_CONTRACT_H
#include <errno.h>
#include <fcntl.h>
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
#define LOCK_APP "/applications/file-lock-contract-test"
enum { FL_FLOCK, FL_CLASSIC, FL_OFD };

int64_t fl_now(void);
void fl_pause(int milliseconds);
int fl_wait(volatile int* flag, int milliseconds);
int fl_reap(pid_t child, int milliseconds);
int fl_byte(int socket, char expected);
int fl_send_fd(int socket, int fd);
int fl_receive_fd(int socket);
int fl_file(char path[128], const char* directory);
int fl_record(int fd, int command, short type, short whence, off_t start, off_t length);
int fl_lock(int fd, int kind, short type, int blocking);
int fl_query(int fd, int command, off_t start, off_t length, short type, off_t expected_start,
             off_t expected_length, pid_t owner);
int file_lock_flock(void);
int file_lock_records(void);
int file_lock_lifetime(void);
int file_lock_blocking(void);
int file_lock_creation(void);
int file_lock_exec(int argc, char** argv);
#endif
