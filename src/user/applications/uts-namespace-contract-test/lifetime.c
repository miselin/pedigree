#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/socket.h>

struct rights_sender {
  int socket;
};
static int send_namespace(int command, int report, void* argument) {
  (void)command;
  struct rights_sender* state = argument;
  if (unshare(CLONE_NEWUTS) || ns_set("orphan-namespace", "orphan-domain"))
    return 1;
  int fd = open("/proc/thread-self/ns/uts", O_RDONLY | O_CLOEXEC);
  struct ns_identity identity;
  if (fd < 0 || ns_fd_identity(fd, &identity) || ns_write(report, &identity, sizeof(identity)) ||
      ns_send_fd(state->socket, fd)) {
    if (fd >= 0)
      close(fd);
    return 1;
  }
  close(fd);
  close(state->socket);
  return 0;
}

static int join_inherited(int command, int report, void* argument) {
  int fd = *(int*)argument;
  if (ns_send(report, 'R') || ns_receive(command, 'J') || setns(fd, CLONE_NEWUTS) ||
      ns_expect("orphan-namespace", "orphan-domain"))
    return 1;
  close(fd);
  return ns_expect("orphan-namespace", "orphan-domain");
}

static int retained_descriptors(void) {
  int failed = 0, socket[2] = {-1, -1}, fd = -1, alias = -1, regular = -1;
  struct ns_peer source = NS_PEER_INITIALIZER, child = NS_PEER_INITIALIZER;
  struct ns_identity expected, actual, original, current;
  CHECK(ns_set("rights-parent", "rights-parent") == 0);
  CHECK(ns_path_identity("/proc/thread-self/ns/uts", &original) == 0);
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, socket) == 0);
  struct rights_sender sender = {socket[1]};
  CHECK(ns_spawn(&source, send_namespace, &sender) == 0);
  close(socket[1]);
  socket[1] = -1;
  CHECK(ns_read(source.report, &expected, sizeof(expected)) == 0);
  CHECK(ns_join(&source) == 0);
  /* The source task and its descriptor are gone before rights are received. */
  fd = ns_receive_fd(socket[0]);
  CHECK(fd >= 0 && ns_fd_identity(fd, &actual) == 0 && ns_same(actual, expected));
  alias = dup(fd);
  CHECK(alias >= 0);
  close(fd);
  fd = -1;
  CHECK(ns_fd_identity(alias, &actual) == 0 && ns_same(actual, expected));
  CHECK(ns_spawn(&child, join_inherited, &alias) == 0 && ns_receive(child.report, 'R') == 0);
  int reused = alias;
  close(alias);
  alias = -1;
  regular = memfd_create("namespace-fd-reuse", MFD_CLOEXEC);
  CHECK(regular >= 0);
  if (regular != reused) {
    CHECK(dup2(regular, reused) == reused);
    close(regular);
    regular = reused;
  }
  errno = 0;
  CHECK(setns(regular, 0) == -1 && errno == EINVAL);
  CHECK(ns_path_identity("/proc/thread-self/ns/uts", &current) == 0 && ns_same(current, original));
  CHECK(ns_send(child.command, 'J') == 0 && ns_join(&child) == 0);
  CHECK(ns_expect("rights-parent", "rights-parent") == 0);
out:
  ns_cleanup(&child);
  ns_cleanup(&source);
  if (regular >= 0)
    close(regular);
  if (alias >= 0)
    close(alias);
  if (fd >= 0)
    close(fd);
  for (int i = 0; i < 2; ++i)
    if (socket[i] >= 0)
      close(socket[i]);
  return failed;
}

int ns_exec(int argc, char** argv) {
  int failed = 0;
  alarm(15);
  CHECK(argc == 6);
  int retained = atoi(argv[4]), closed = atoi(argv[5]);
  struct ns_identity own, saved, leader;
  CHECK(ns_expect(argv[2], argv[3]) == 0);
  CHECK(ns_fd_identity(retained, &saved) == 0);
  CHECK(ns_path_identity("/proc/thread-self/ns/uts", &own) == 0 && ns_same(saved, own));
  CHECK(ns_path_identity("/proc/self/ns/uts", &leader) == 0 && ns_same(leader, own));
  errno = 0;
  CHECK(fcntl(closed, F_GETFD) == -1 && errno == EBADF);
  CHECK(!(fcntl(retained, F_GETFD) & FD_CLOEXEC));
  CHECK(unshare(CLONE_NEWUTS) == 0 && ns_set("exec-temporary", "exec-temporary") == 0);
  CHECK(setns(retained, 0) == 0 && ns_expect(argv[2], argv[3]) == 0);
  close(retained);
  CHECK(ns_expect(argv[2], argv[3]) == 0);
out:
  return failed;
}

static int enter_exec(const char* host) {
  if (ns_set(host, "exec-domain"))
    return 1;
  int original = open("/proc/thread-self/ns/uts", O_RDONLY | O_CLOEXEC);
  if (original < 0)
    return 1;
  int retained = fcntl(original, F_DUPFD, 100);
  int closed = fcntl(original, F_DUPFD_CLOEXEC, 200);
  close(original);
  if (retained < 0 || closed < 0)
    return 1;
  char retained_text[16], closed_text[16];
  snprintf(retained_text, sizeof(retained_text), "%d", retained);
  snprintf(closed_text, sizeof(closed_text), "%d", closed);
  execl("/applications/uts-namespace-no-such-executable", "absent", (char*)NULL);
  if (errno != ENOENT || ns_expect(host, "exec-domain"))
    return 1;
  execl(NS_APP, NS_APP, "uts-exec", host, "exec-domain", retained_text, closed_text, (char*)NULL);
  return 1;
}
static void* exec_worker(void* argument) {
  (void)argument;
  if (unshare(CLONE_NEWUTS))
    _exit(2);
  _exit(enter_exec("exec-worker") ? 3 : 0);
}
static int exec_child(int command, int report, void* argument) {
  (void)command;
  (void)report;
  if (unshare(CLONE_NEWUTS))
    return 1;
  if (!*(int*)argument)
    return enter_exec("exec-leader");
  pthread_t worker;
  if (ns_set("old-exec-leader", "old-exec-leader") ||
      pthread_create(&worker, NULL, exec_worker, NULL))
    return 1;
  for (;;)
    pause();
}
static int exec_membership(void) {
  int failed = 0;
  struct ns_peer child = NS_PEER_INITIALIZER;
  for (int threaded = 0; threaded < 2; ++threaded) {
    CHECK(ns_spawn(&child, exec_child, &threaded) == 0);
    CHECK(ns_join(&child) == 0);
  }
out:
  ns_cleanup(&child);
  return failed;
}

int ns_lifetime(void) {
  return retained_descriptors() || exec_membership();
}
