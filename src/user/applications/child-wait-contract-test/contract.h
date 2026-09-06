#ifndef CHILD_WAIT_CONTRACT_H
#define CHILD_WAIT_CONTRACT_H

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>

#define CW_RUSAGE_BYTES 144
_Static_assert(offsetof(struct rusage, __reserved) == CW_RUSAGE_BYTES,
               "musl amd64 rusage kernel prefix changed");

#define CHECK(condition)                                                                \
  do {                                                                                  \
    if (!(condition)) {                                                                 \
      fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition, errno); \
      failed = 1;                                                                       \
      goto out;                                                                         \
    }                                                                                   \
  } while (0)

struct cw_child {
  pid_t pid;
  int command, report;
};
#define CW_CHILD_INIT {.pid = -1, .command = -1, .report = -1}
extern size_t cw_page;
int64_t cw_now(void);
int cw_read(int fd, void* data, size_t size);
int cw_write(int fd, const void* data, size_t size);
int cw_send(int fd, char value);
int cw_receive(int fd, char value);
int cw_atomic_wait(atomic_uint* value, unsigned expected);
int cw_spawn(struct cw_child*, pid_t group, uid_t uid, int exit_code);
void cw_cleanup(struct cw_child*);
int cw_reap(pid_t pid, int milliseconds);
int cw_info(const siginfo_t*, pid_t, uid_t, int code, int status);
int cw_empty_info(const siginfo_t*);
int cw_usage(const struct rusage*);
int cw_bytes(const void*, size_t, unsigned char);
long cw_raw_waitid(idtype_t, id_t, void*, int, struct rusage*);
int cw_selectors(void);
int cw_events(void);
int cw_lifetime(void);
int cw_copyout(void);
int cw_errors(void);
int cw_interruption(void);
#endif
