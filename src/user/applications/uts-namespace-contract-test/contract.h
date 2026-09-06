#ifndef UTS_NAMESPACE_CONTRACT_H
#define UTS_NAMESPACE_CONTRACT_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include <sys/types.h>

#define NS_APP "/applications/uts-namespace-contract-test"
#define CHECK(condition)                                                                \
  do {                                                                                  \
    if (!(condition)) {                                                                 \
      fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition, errno); \
      failed = 1;                                                                       \
      goto out;                                                                         \
    }                                                                                   \
  } while (0)

struct ns_identity {
  dev_t device;
  ino_t inode;
};
struct ns_peer {
  pid_t pid;
  int command, report;
};
#define NS_PEER_INITIALIZER {.pid = -1, .command = -1, .report = -1}

extern size_t ns_page;
int64_t ns_now(void);
int ns_reap(pid_t pid, int milliseconds);
int ns_read(int fd, void* buffer, size_t length);
int ns_write(int fd, const void* buffer, size_t length);
int ns_send(int fd, char value);
int ns_receive(int fd, char value);
int ns_spawn(struct ns_peer* peer, int (*body)(int, int, void*), void* argument);
int ns_join(struct ns_peer* peer);
void ns_cleanup(struct ns_peer* peer);
int ns_fd_identity(int fd, struct ns_identity* identity);
int ns_path_identity(const char* path, struct ns_identity* identity);
int ns_same(struct ns_identity left, struct ns_identity right);
int ns_set(const char* host, const char* domain);
int ns_expect(const char* host, const char* domain);
int ns_send_fd(int socket, int fd);
int ns_receive_fd(int socket);
int ns_names(void);
int ns_membership(void);
int ns_proc(void);
int ns_lifetime(void);
int ns_admission(void);
int ns_exec(int argc, char** argv);

#endif
