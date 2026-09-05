#ifndef EVENT_DESCRIPTOR_CONTRACT_H
#define EVENT_DESCRIPTOR_CONTRACT_H

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

#define EVENT_DESCRIPTOR_APP "/applications/event-descriptor-contract-test"

int64_t ed_now(clockid_t clock);
struct timespec ed_timespec(int64_t nanoseconds);
void ed_pause(int milliseconds);
int ed_wait(volatile int* flag, int milliseconds);
int ed_reap(pid_t child, int milliseconds);
int ed_send_fd(int socket, int fd);
int ed_receive_fd(int socket);
int ed_readable(int fd, int milliseconds);

struct ed_reader {
  int fd;
  size_t size;
  volatile int started, done;
  ssize_t result;
  int error;
  int64_t finished;
  unsigned char bytes[128];
};
void* ed_read_thread(void* argument);

int event_descriptor_test_signalfd(void);
int event_descriptor_test_timerfd(void);
int event_descriptor_test_clock(void);
int event_descriptor_signalfd_exec(int argc, char** argv);
int event_descriptor_timerfd_exec(int argc, char** argv);

#endif
