#ifndef PROCESS_MEMORY_CONTRACT_H
#define PROCESS_MEMORY_CONTRACT_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include <sys/types.h>
#include <sys/uio.h>

#define PM_APP "/applications/process-memory-contract-test"
#define CHECK(condition)                                                                \
  do {                                                                                  \
    if (!(condition)) {                                                                 \
      fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition, errno); \
      failed = 1;                                                                       \
      goto out;                                                                         \
    }                                                                                   \
  } while (0)

struct pm_peer {
  pid_t pid;
  int command, report;
};
#define PM_PEER_INITIALIZER {.pid = -1, .command = -1, .report = -1}

extern size_t pm_page;
int64_t pm_now(void);
int pm_reap(pid_t pid, int milliseconds);
int pm_read(int fd, void* buffer, size_t length);
int pm_write(int fd, const void* buffer, size_t length);
int pm_send(int fd, char command);
int pm_receive(int fd, char command);
int pm_spawn(struct pm_peer* peer, int (*body)(int, int, void*), void* argument);
int pm_join(struct pm_peer* peer);
void pm_cleanup(struct pm_peer* peer);
int pm_isolate(int (*body)(void));
ssize_t pm_copy(pid_t pid, void* local, const void* remote, size_t length, int write_remote);
int pm_dumpable(int value);
int pm_copies(void);
int pm_mappings(void);
int pm_lifetime(void);
int pm_credentials(void);
int pm_filesystem(void);
int pm_exec(int argc, char** argv);
int pm_fs_exec(int argc, char** argv);

#endif
