/* Copyright (c) 2026, Pedigree Developers. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define CHECK(expression)                                                             \
  do {                                                                                \
    if (!(expression)) {                                                              \
      fprintf(stderr, "RECVMMSG-CONTRACT: FAIL line=%d errno=%d\n", __LINE__, errno); \
      return 1;                                                                       \
    }                                                                                 \
  } while (0)

static char socket_paths[16][sizeof(((struct sockaddr_un*)0)->sun_path)];
static size_t socket_path_count;

static void cleanup_paths(void) {
  for (size_t i = 0; i < socket_path_count; ++i)
    unlink(socket_paths[i]);
}

static int datagram_pair(int sockets[2]) {
  struct sockaddr_un addresses[2] = {{.sun_family = AF_UNIX}, {.sun_family = AF_UNIX}};
  sockets[0] = sockets[1] = -1;
  for (int i = 0; i < 2; ++i) {
    if (socket_path_count == sizeof(socket_paths) / sizeof(socket_paths[0])) {
      errno = ENOSPC;
      goto fail;
    }
    char* path = socket_paths[socket_path_count];
    snprintf(path, sizeof(socket_paths[0]), "/tmp/recvmmsg-%ld-%zu", (long)getpid(),
             socket_path_count);
    ++socket_path_count;
    memcpy(addresses[i].sun_path, path, strlen(path) + 1);
    sockets[i] = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sockets[i] < 0 || bind(sockets[i], (struct sockaddr*)&addresses[i],
                            offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1))
      goto fail;
  }
  for (int i = 0; i < 2; ++i) {
    const struct sockaddr_un* peer = &addresses[1 - i];
    if (connect(sockets[i], (const struct sockaddr*)peer,
                offsetof(struct sockaddr_un, sun_path) + strlen(peer->sun_path) + 1))
      goto fail;
  }
  return 0;
fail: {
    const int error = errno;
    if (sockets[0] >= 0)
      close(sockets[0]);
    if (sockets[1] >= 0)
      close(sockets[1]);
    errno = error;
    return -1;
  }
}

static volatile sig_atomic_t signalled;
static void handler(int signal) {
  (void)signal;
  signalled = 1;
}

static void initialise(struct mmsghdr* messages, struct iovec* vectors, char data[][16], size_t n) {
  memset(messages, 0, n * sizeof(*messages));
  for (size_t i = 0; i < n; ++i) {
    vectors[i].iov_base = data[i];
    vectors[i].iov_len = 16;
    messages[i].msg_hdr.msg_iov = &vectors[i];
    messages[i].msg_hdr.msg_iovlen = 1;
  }
}

static int pendingError(int fd, int expected) {
  int error = 0;
  socklen_t length = sizeof(error);
  return getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == expected;
}

static int datagrams(void) {
  int sockets[2];
  CHECK(datagram_pair(sockets) == 0);
  struct mmsghdr messages[3];
  struct iovec vectors[3];
  char data[3][16];
  initialise(messages, vectors, data, 3);
  CHECK(write(sockets[0], "one", 3) == 3 && write(sockets[0], "two", 3) == 3);
  CHECK(recvmmsg(sockets[1], messages, 3, MSG_WAITFORONE, NULL) == 2);
  CHECK(messages[0].msg_len == 3 && messages[1].msg_len == 3 && !memcmp(data[0], "one", 3) &&
        !memcmp(data[1], "two", 3));
  CHECK(!(fcntl(sockets[1], F_GETFL) & O_NONBLOCK));
  CHECK(recvmmsg(sockets[1], messages, 1, MSG_DONTWAIT, NULL) == -1 && errno == EAGAIN);
  CHECK(syscall(SYS_recvmmsg, sockets[1], NULL, 0, 0, NULL) == 0);
  CHECK(syscall(SYS_recvmmsg, -1, NULL, 0, 0, NULL) == -1 && errno == EBADF);

  initialise(messages, vectors, data, 3);
  CHECK(write(sockets[0], "before", 6) == 6 && write(sockets[0], "after", 5) == 5);
  vectors[1].iov_base = (void*)1;
  CHECK(recvmmsg(sockets[1], messages, 2, MSG_DONTWAIT, NULL) == 1);
  CHECK(messages[0].msg_len == 6 && !memcmp(data[0], "before", 6));
  CHECK(pendingError(sockets[1], EFAULT));
  initialise(messages, vectors, data, 3);
  CHECK(recvmmsg(sockets[1], messages, 1, MSG_DONTWAIT, NULL) == 1);
  CHECK(messages[0].msg_len == 5 && !memcmp(data[0], "after", 5));

  CHECK(write(sockets[0], "abcdef", 6) == 6);
  initialise(messages, vectors, data, 3);
  vectors[0].iov_len = 2;
  CHECK(recvmmsg(sockets[1], messages, 1, MSG_TRUNC, NULL) == 1);
  CHECK(messages[0].msg_len == 6 && (messages[0].msg_hdr.msg_flags & MSG_TRUNC) &&
        !memcmp(data[0], "ab", 2));
  CHECK(write(sockets[0], "", 0) == 0);
  initialise(messages, vectors, data, 3);
  CHECK(recvmmsg(sockets[1], messages, 1, MSG_DONTWAIT, NULL) == 1 && messages[0].msg_len == 0);
  CHECK(fcntl(sockets[1], F_SETFL, O_NONBLOCK) == 0);
  CHECK(recvmmsg(sockets[1], messages, 1, 0, NULL) == -1 && errno == EAGAIN);
  CHECK(close(sockets[0]) == 0 && close(sockets[1]) == 0);
  return 0;
}

static int deadlines(void) {
  int sockets[2];
  CHECK(datagram_pair(sockets) == 0);
  struct mmsghdr messages[2];
  struct iovec vectors[2];
  char data[2][16];
  initialise(messages, vectors, data, 2);
  struct timespec timeout = {0, 30000000}, before, after;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
  CHECK(recvmmsg(sockets[1], messages, 2, 0, &timeout) == 0);
  CHECK(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
  int64_t elapsed = (after.tv_sec - before.tv_sec) * 1000000000LL + after.tv_nsec - before.tv_nsec;
  CHECK(elapsed >= 20000000 && elapsed < 2000000000LL);
  CHECK(write(sockets[0], "partial", 7) == 7);
  timeout = (struct timespec){0, 30000000};
  CHECK(recvmmsg(sockets[1], messages, 2, 0, &timeout) == 1);
  CHECK(messages[0].msg_len == 7 && !memcmp(data[0], "partial", 7));
  CHECK(timeout.tv_sec == 0 && timeout.tv_nsec == 0);
  timeout = (struct timespec){-1, 0};
  CHECK(recvmmsg(sockets[1], messages, 1, 0, &timeout) == -1 && errno == EINVAL);
  timeout = (struct timespec){0, 1000000000};
  CHECK(recvmmsg(sockets[1], messages, 1, 0, &timeout) == -1 && errno == EINVAL);
  CHECK(syscall(SYS_recvmmsg, sockets[1], messages, 1, 0, (void*)1) == -1 && errno == EFAULT);
  CHECK(syscall(SYS_recvmmsg, sockets[1], (void*)1, 1, MSG_DONTWAIT, NULL) == -1 &&
        errno == EFAULT);
  CHECK(close(sockets[0]) == 0 && close(sockets[1]) == 0);
  return 0;
}

static int signals(void) {
  struct sigaction action = {.sa_handler = handler};
  sigemptyset(&action.sa_mask);
  CHECK(sigaction(SIGUSR1, &action, NULL) == 0);
  int sockets[2];
  CHECK(datagram_pair(sockets) == 0);
  struct mmsghdr messages[2];
  struct iovec vectors[2];
  char data[2][16];
  for (int partial = 0; partial <= 1; ++partial) {
    initialise(messages, vectors, data, 2);
    signalled = 0;
    if (partial)
      CHECK(write(sockets[0], "saved", 5) == 5);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
      usleep(30000);
      _exit(kill(getppid(), SIGUSR1) ? 2 : 0);
    }
    struct timespec timeout = {2, 0};
    const int result = recvmmsg(sockets[1], messages, 2, 0, &timeout);
    const int error = errno;
    CHECK(signalled && (partial ? result == 1 : result == -1 && error == EINTR));
    if (partial) {
      CHECK(messages[0].msg_len == 5 && !memcmp(data[0], "saved", 5));
      CHECK(pendingError(sockets[1], EINTR));
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
  action.sa_flags = SA_RESTART;
  CHECK(sigaction(SIGUSR1, &action, NULL) == 0);
  for (int timed = 0; timed <= 1; ++timed) {
    initialise(messages, vectors, data, 2);
    signalled = 0;
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
      usleep(30000);
      if (kill(getppid(), SIGUSR1))
        _exit(2);
      usleep(30000);
      _exit(write(sockets[0], "restart", 7) == 7 ? 0 : 3);
    }
    struct timespec timeout = {2, 0};
    const int result = recvmmsg(sockets[1], messages, 1, 0, timed ? &timeout : NULL);
    const int error = errno;
    CHECK(signalled && (timed ? result == -1 && error == EINTR : result == 1));
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    if (timed) {
      CHECK(timeout.tv_sec == 2 && timeout.tv_nsec == 0);
      CHECK(recvmmsg(sockets[1], messages, 1, MSG_DONTWAIT, NULL) == 1);
    }
    CHECK(messages[0].msg_len == 7 && !memcmp(data[0], "restart", 7));
  }
  CHECK(close(sockets[0]) == 0 && close(sockets[1]) == 0);
  return 0;
}

struct delayed_receive {
  int fd;
  int result;
  char payload[16];
};

static void* receive_thread(void* argument) {
  struct delayed_receive* call = argument;
  struct iovec vector = {call->payload, sizeof(call->payload)};
  struct mmsghdr messages[2] = {{.msg_hdr = {.msg_iov = &vector, .msg_iovlen = 1}}, {0}};
  struct timespec timeout = {2, 0};
  call->result = recvmmsg(call->fd, messages, 2, MSG_WAITFORONE, &timeout);
  return NULL;
}

static int descriptor_reuse(void) {
  int original[2], replacement[2];
  CHECK(datagram_pair(original) == 0);
  CHECK(datagram_pair(replacement) == 0);
  int keep = dup(original[1]);
  CHECK(keep >= 0);
  struct delayed_receive call = {.fd = original[1], .result = -2};
  pthread_t thread;
  CHECK(pthread_create(&thread, NULL, receive_thread, &call) == 0);
  usleep(50000);
  CHECK(dup2(replacement[1], original[1]) == original[1]);
  CHECK(write(replacement[0], "new", 3) == 3);
  CHECK(write(original[0], "old", 3) == 3);
  CHECK(pthread_join(thread, NULL) == 0);
  CHECK(call.result == 1 && !memcmp(call.payload, "old", 3));
  char payload[4];
  CHECK(recv(original[1], payload, sizeof(payload), MSG_DONTWAIT) == 3 &&
        !memcmp(payload, "new", 3));
  CHECK(!(fcntl(keep, F_GETFL) & O_NONBLOCK));
  CHECK(close(keep) == 0 && close(original[0]) == 0 && close(original[1]) == 0);
  CHECK(close(replacement[0]) == 0 && close(replacement[1]) == 0);
  return 0;
}

struct error_poll {
  int fd;
  int result;
  short events;
};

static void* poll_error(void* argument) {
  struct error_poll* call = argument;
  struct pollfd descriptor = {.fd = call->fd};
  call->result = poll(&descriptor, 1, 2000);
  call->events = descriptor.revents;
  return NULL;
}

static int deferred_readiness(void) {
  int sockets[2];
  CHECK(datagram_pair(sockets) == 0);
  int epoll = epoll_create1(EPOLL_CLOEXEC);
  CHECK(epoll >= 0);
  struct epoll_event event = {.events = EPOLLET, .data.fd = sockets[1]};
  CHECK(epoll_ctl(epoll, EPOLL_CTL_ADD, sockets[1], &event) == 0);
  for (int iteration = 0; iteration < 2; ++iteration) {
    struct error_poll call = {.fd = sockets[1]};
    pthread_t waiter;
    CHECK(pthread_create(&waiter, NULL, poll_error, &call) == 0);
    usleep(30000);
    struct mmsghdr messages[2];
    struct iovec vectors[2];
    char data[2][16];
    initialise(messages, vectors, data, 2);
    vectors[1].iov_base = (void*)1;
    CHECK(write(sockets[0], "error", 5) == 5);
    CHECK(recvmmsg(sockets[1], messages, 2, MSG_DONTWAIT, NULL) == 1);
    CHECK(pthread_join(waiter, NULL) == 0);
    CHECK(call.result == 1 && (call.events & POLLERR));
    CHECK(epoll_wait(epoll, &event, 1, 0) == 1 && (event.events & EPOLLERR));
    CHECK(epoll_wait(epoll, &event, 1, 0) == 0);
    struct pollfd descriptor = {.fd = sockets[1]};
    CHECK(poll(&descriptor, 1, 0) == 1 && (descriptor.revents & POLLERR));
    CHECK(pendingError(sockets[1], EFAULT));
    CHECK(poll(&descriptor, 1, 0) == 0 && epoll_wait(epoll, &event, 1, 0) == 0);
  }
  CHECK(close(epoll) == 0 && close(sockets[0]) == 0 && close(sockets[1]) == 0);
  return 0;
}

static int rights(void) {
  int sockets[2], pipefd[2];
  CHECK(datagram_pair(sockets) == 0 && pipe(pipefd) == 0);
  union {
    struct cmsghdr alignment;
    char bytes[CMSG_SPACE(sizeof(int))];
  } control;
  memset(&control, 0, sizeof(control));
  char payload = 'x';
  struct iovec vector = {&payload, 1};
  struct msghdr message = {.msg_iov = &vector,
                           .msg_iovlen = 1,
                           .msg_control = control.bytes,
                           .msg_controllen = sizeof(control)};
  struct cmsghdr* header = CMSG_FIRSTHDR(&message);
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  header->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(header), &pipefd[0], sizeof(int));
  CHECK(sendmsg(sockets[0], &message, 0) == 1 && sendmsg(sockets[0], &message, 0) == 1);

  struct mmsghdr messages[2];
  struct iovec vectors[2];
  char data[2][16];
  union {
    struct cmsghdr alignment;
    char bytes[CMSG_SPACE(sizeof(int))];
  } received[2];
  initialise(messages, vectors, data, 2);
  for (int i = 0; i < 2; ++i) {
    messages[i].msg_hdr.msg_control = received[i].bytes;
    messages[i].msg_hdr.msg_controllen = sizeof(received[i]);
  }
  CHECK(recvmmsg(sockets[1], messages, 2, MSG_CMSG_CLOEXEC, NULL) == 2);
  for (int i = 0; i < 2; ++i) {
    header = CMSG_FIRSTHDR(&messages[i].msg_hdr);
    CHECK(header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS);
    int fd;
    memcpy(&fd, CMSG_DATA(header), sizeof(fd));
    CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    CHECK(write(pipefd[1], "p", 1) == 1 && read(fd, &payload, 1) == 1 && payload == 'p');
    CHECK(close(fd) == 0);
  }
  CHECK(close(sockets[0]) == 0 && close(sockets[1]) == 0);
  CHECK(close(pipefd[0]) == 0 && close(pipefd[1]) == 0);
  return 0;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  alarm(30);
  CHECK(atexit(cleanup_paths) == 0);
  puts("RECVMMSG-CONTRACT: datagrams");
  CHECK(datagrams() == 0);
  puts("RECVMMSG-CONTRACT: deadlines");
  CHECK(deadlines() == 0);
  puts("RECVMMSG-CONTRACT: signals");
  CHECK(signals() == 0);
  puts("RECVMMSG-CONTRACT: rights");
  CHECK(rights() == 0);
  puts("RECVMMSG-CONTRACT: descriptor_reuse");
  CHECK(descriptor_reuse() == 0);
  puts("RECVMMSG-CONTRACT: deferred_readiness");
  CHECK(deferred_readiness() == 0);
  puts("RECVMMSG-CONTRACT: PASS");
  return 0;
}
