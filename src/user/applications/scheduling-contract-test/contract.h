#ifndef SCHEDULING_CONTRACT_H
#define SCHEDULING_CONTRACT_H

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include <sys/types.h>

#define SC_APP "/applications/scheduling-contract-test"
#define CHECK(condition)                                                                \
  do {                                                                                  \
    if (!(condition)) {                                                                 \
      fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition, errno); \
      failed = 1;                                                                       \
      goto out;                                                                         \
    }                                                                                   \
  } while (0)

struct sc_peer {
  pid_t pid;
  int command, report;
};
#define SC_PEER_INITIALIZER {.pid = -1, .command = -1, .report = -1}
extern cpu_set_t sc_allowed;
extern int sc_cpus[CPU_SETSIZE], sc_count;
extern size_t sc_bytes, sc_page;
extern _Thread_local unsigned sc_tls;
int sc_init(int expected);
int sc_pin(pid_t tid, int cpu);
int sc_mask(pid_t tid, int cpu);
int sc_sample(int cpu);
int sc_register_syscall(long number, long a, long b, long c, long* result);
int64_t sc_now(void);
int sc_wait(atomic_uint* value, unsigned expected);
int sc_wait_for_task_retirement(pid_t tid, const struct timespec* expected_interval);
int sc_read(int fd, void* buffer, size_t length);
int sc_write(int fd, const void* buffer, size_t length);
int sc_send(int fd, char value);
int sc_receive(int fd, char value);
int sc_reap(pid_t pid, int milliseconds);
int sc_spawn(struct sc_peer*, int (*body)(int, int, void*), void*);
int sc_join(struct sc_peer*);
void sc_cleanup(struct sc_peer*);
int sc_api(void);
int sc_placement(void);
int sc_wakeups(void);
int sc_lifecycle(void);
int sc_permissions(void);
int sc_exec(int argc, char** argv);
#endif
