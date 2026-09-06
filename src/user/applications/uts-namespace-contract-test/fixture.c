#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "contract.h"
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>

size_t ns_page;

int64_t ns_now(void) {
  struct timespec value;
  return clock_gettime(CLOCK_MONOTONIC, &value)
             ? -1
             : (int64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}

int ns_reap(pid_t pid, int milliseconds) {
  int64_t start = ns_now(), now, end = start + (int64_t)milliseconds * 1000000;
  while (start >= 0 && (now = ns_now()) >= 0 && now < end) {
    int status;
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid)
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (result < 0 && errno != EINTR)
      return -1;
    struct timespec pause = {0, 5000000};
    nanosleep(&pause, NULL);
  }
  kill(pid, SIGKILL);
  while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
  }
  return -1;
}

int ns_read(int fd, void* buffer, size_t length) {
  unsigned char* bytes = buffer;
  while (length) {
    struct pollfd ready = {.fd = fd, .events = POLLIN};
    int result;
    do
      result = poll(&ready, 1, 10000);
    while (result < 0 && errno == EINTR);
    if (result <= 0)
      return -1;
    ssize_t count = read(fd, bytes, length);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return -1;
    bytes += count;
    length -= count;
  }
  return 0;
}

int ns_write(int fd, const void* buffer, size_t length) {
  const unsigned char* bytes = buffer;
  while (length) {
    ssize_t count = write(fd, bytes, length);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return -1;
    bytes += count;
    length -= count;
  }
  return 0;
}
int ns_send(int fd, char value) {
  return ns_write(fd, &value, 1);
}
int ns_receive(int fd, char value) {
  char actual;
  return ns_read(fd, &actual, 1) || actual != value ? -1 : 0;
}

int ns_spawn(struct ns_peer* peer, int (*body)(int, int, void*), void* argument) {
  int command[2], report[2];
  if (pipe(command))
    return -1;
  if (pipe(report)) {
    close(command[0]);
    close(command[1]);
    return -1;
  }
  pid_t pid = fork();
  if (!pid) {
    close(command[1]);
    close(report[0]);
    alarm(30);
    int result = body(command[0], report[1], argument);
    fflush(stdout);
    fflush(stderr);
    _exit(result ? 1 : 0);
  }
  close(command[0]);
  close(report[1]);
  if (pid < 0) {
    close(command[1]);
    close(report[0]);
    return -1;
  }
  *peer = (struct ns_peer){pid, command[1], report[0]};
  return 0;
}

int ns_join(struct ns_peer* peer) {
  int result = ns_reap(peer->pid, 10000);
  if (result)
    fprintf(stderr, "namespace child pid=%d status=%d\n", peer->pid, result);
  peer->pid = -1;
  ns_cleanup(peer);
  return result;
}
void ns_cleanup(struct ns_peer* peer) {
  if (peer->pid > 0) {
    kill(peer->pid, SIGKILL);
    ns_reap(peer->pid, 1000);
  }
  if (peer->command >= 0)
    close(peer->command);
  if (peer->report >= 0)
    close(peer->report);
  *peer = (struct ns_peer)NS_PEER_INITIALIZER;
}

int ns_fd_identity(int fd, struct ns_identity* identity) {
  struct stat status;
  if (fstat(fd, &status) || !status.st_ino)
    return -1;
  *identity = (struct ns_identity){status.st_dev, status.st_ino};
  return 0;
}
int ns_path_identity(const char* path, struct ns_identity* identity) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return -1;
  int result = ns_fd_identity(fd, identity);
  close(fd);
  return result;
}
int ns_same(struct ns_identity left, struct ns_identity right) {
  return left.device == right.device && left.inode == right.inode;
}
int ns_set(const char* host, const char* domain) {
  return sethostname(host, strlen(host)) || setdomainname(domain, strlen(domain));
}
int ns_expect(const char* host, const char* domain) {
  struct utsname value;
  if (uname(&value))
    return -1;
  if (!strcmp(value.nodename, host) && !strcmp(value.domainname, domain))
    return 0;
  fprintf(stderr, "namespace names expected=%s/%s actual=%s/%s\n", host, domain, value.nodename,
          value.domainname);
  return -1;
}

int ns_send_fd(int socket, int fd) {
  char byte = 'N';
  struct iovec vector = {&byte, 1};
  union {
    struct cmsghdr align;
    char bytes[CMSG_SPACE(sizeof(int))];
  } control = {0};
  struct msghdr message = {.msg_iov = &vector,
                           .msg_iovlen = 1,
                           .msg_control = control.bytes,
                           .msg_controllen = sizeof(control)};
  struct cmsghdr* header = CMSG_FIRSTHDR(&message);
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  header->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(header), &fd, sizeof(fd));
  return sendmsg(socket, &message, 0) == 1 ? 0 : -1;
}
int ns_receive_fd(int socket) {
  char byte;
  struct iovec vector = {&byte, 1};
  union {
    struct cmsghdr align;
    char bytes[CMSG_SPACE(sizeof(int))];
  } control = {0};
  struct msghdr message = {.msg_iov = &vector,
                           .msg_iovlen = 1,
                           .msg_control = control.bytes,
                           .msg_controllen = sizeof(control)};
  if (recvmsg(socket, &message, 0) != 1 || byte != 'N' || (message.msg_flags & MSG_CTRUNC))
    return -1;
  struct cmsghdr* header = CMSG_FIRSTHDR(&message);
  if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
      header->cmsg_len != CMSG_LEN(sizeof(int)))
    return -1;
  int fd;
  memcpy(&fd, CMSG_DATA(header), sizeof(fd));
  return fd;
}
