/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

extern void fail(void) __attribute__((noreturn));

union seqpacket_control {
  struct cmsghdr alignment;
  unsigned char bytes[CMSG_SPACE(sizeof(int))];
};

static void expect_empty(int descriptor) {
  char byte = 0;
  errno = 0;
  if (recv(descriptor, &byte, sizeof(byte), MSG_DONTWAIT) != -1 || errno != EAGAIN) {
    fail();
  }
}

static void record_contracts(void) {
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, pair)) {
    fail();
  }
  for (int i = 0; i < 2; ++i) {
    int type = 0;
    socklen_t type_length = sizeof(type);
    if (fcntl(pair[i], F_GETFD) != FD_CLOEXEC || !(fcntl(pair[i], F_GETFL) & O_NONBLOCK) ||
        getsockopt(pair[i], SOL_SOCKET, SO_TYPE, &type, &type_length) || type != SOCK_SEQPACKET ||
        type_length != sizeof(type)) {
      fail();
    }
  }

  expect_empty(pair[1]);
  if (send(pair[0], "abcdef", 6, 0) != 6 || write(pair[0], "XY", 2) != 2) {
    fail();
  }
  char payload[32] = {0};
  struct iovec vector = {.iov_base = payload, .iov_len = 3};
  struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1};
  if (recvmsg(pair[1], &message, 0) != 3 || memcmp(payload, "abc", 3) ||
      !(message.msg_flags & MSG_TRUNC) || read(pair[1], payload, sizeof(payload)) != 2 ||
      memcmp(payload, "XY", 2)) {
    fail();
  }
  expect_empty(pair[1]);

  if (send(pair[0], "abcdef", 6, 0) != 6 || send(pair[0], "z", 1, 0) != 1) {
    fail();
  }
  message.msg_flags = 0;
  if (recvmsg(pair[1], &message, MSG_TRUNC) != 6 || memcmp(payload, "abc", 3) ||
      !(message.msg_flags & MSG_TRUNC) || recv(pair[1], payload, sizeof(payload), 0) != 1 ||
      payload[0] != 'z') {
    fail();
  }

  /* Empty sends are records; an empty receive buffer also consumes one record. */
  if (write(pair[0], "", 0) != 0 || send(pair[0], "q", 1, 0) != 1) {
    fail();
  }
  struct pollfd reader = {.fd = pair[1], .events = POLLIN};
  if (poll(&reader, 1, 0) != 1 || !(reader.revents & POLLIN) ||
      recv(pair[1], payload, sizeof(payload), 0) != 0 ||
      recv(pair[1], payload, sizeof(payload), 0) != 1 || payload[0] != 'q') {
    fail();
  }
  if (send(pair[0], "discard", 7, 0) != 7 || send(pair[0], "r", 1, 0) != 1 ||
      recv(pair[1], payload, 0, 0) != 0 || recv(pair[1], payload, sizeof(payload), 0) != 1 ||
      payload[0] != 'r') {
    fail();
  }
  expect_empty(pair[1]);

  char first[] = "one";
  char second[] = "two";
  struct iovec vectors[2] = {{first, 3}, {second, 3}};
  message.msg_iov = vectors;
  message.msg_iovlen = 2;
  if (sendmsg(pair[0], &message, MSG_DONTWAIT) != 6) {
    fail();
  }
  memset(first, 0, sizeof(first));
  memset(second, 0, sizeof(second));
  if (recvmsg(pair[1], &message, 0) != 6 || memcmp(first, "one", 3) || memcmp(second, "two", 3) ||
      (message.msg_flags & MSG_TRUNC)) {
    fail();
  }

  static char oversized[65537];
  errno = 0;
  if (send(pair[0], oversized, sizeof(oversized), 0) != -1 || errno != EMSGSIZE) {
    fail();
  }
  expect_empty(pair[1]);
  memset(oversized, 'm', sizeof(oversized));
  if (send(pair[0], oversized, sizeof(oversized) - 1, 0) != (ssize_t)sizeof(oversized) - 1) {
    fail();
  }
  memset(oversized, 0, sizeof(oversized));
  if (recv(pair[1], oversized, sizeof(oversized), 0) != (ssize_t)sizeof(oversized) - 1) {
    fail();
  }
  for (size_t i = 0; i < sizeof(oversized) - 1; ++i) {
    if (oversized[i] != 'm') {
      fail();
    }
  }
  if (close(pair[0]) || close(pair[1])) {
    fail();
  }
}

static void rights_contracts(void) {
  int pair[2];
  int channel[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) || pipe(channel)) {
    fail();
  }
  union seqpacket_control control = {0};
  char payload = 'c';
  struct iovec vector = {.iov_base = &payload, .iov_len = 1};
  struct msghdr message = {
      .msg_iov = &vector,
      .msg_iovlen = 1,
      .msg_control = control.bytes,
      .msg_controllen = sizeof(control.bytes),
  };
  struct cmsghdr* header = CMSG_FIRSTHDR(&message);
  header->cmsg_len = CMSG_LEN(sizeof(int));
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  memcpy(CMSG_DATA(header), &channel[0], sizeof(int));
  if (sendmsg(pair[0], &message, 0) != 1 || close(channel[0])) {
    fail();
  }
  payload = 0;
  memset(&control, 0, sizeof(control));
  if (recvmsg(pair[1], &message, MSG_CMSG_CLOEXEC) != 1 || payload != 'c' ||
      (message.msg_flags & MSG_CTRUNC)) {
    fail();
  }
  header = CMSG_FIRSTHDR(&message);
  if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
      header->cmsg_len != CMSG_LEN(sizeof(int))) {
    fail();
  }
  int received = -1;
  memcpy(&received, CMSG_DATA(header), sizeof(int));
  if (fcntl(received, F_GETFD) != FD_CLOEXEC || write(channel[1], "C", 1) != 1 ||
      read(received, &payload, 1) != 1 || payload != 'C' || close(received) || close(channel[1])) {
    fail();
  }

  /* SCM_RIGHTS stays attached to a zero-length record. */
  if (pipe(channel)) {
    fail();
  }
  memset(&control, 0, sizeof(control));
  vector.iov_len = 0;
  message.msg_controllen = sizeof(control.bytes);
  message.msg_flags = 0;
  header = CMSG_FIRSTHDR(&message);
  header->cmsg_len = CMSG_LEN(sizeof(int));
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  memcpy(CMSG_DATA(header), &channel[0], sizeof(int));
  if (sendmsg(pair[0], &message, 0) != 0 || close(channel[0])) {
    fail();
  }
  memset(&control, 0, sizeof(control));
  if (recvmsg(pair[1], &message, 0) != 0 || (message.msg_flags & MSG_CTRUNC)) {
    fail();
  }
  header = CMSG_FIRSTHDR(&message);
  if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
      header->cmsg_len != CMSG_LEN(sizeof(int))) {
    fail();
  }
  memcpy(&received, CMSG_DATA(header), sizeof(int));
  if (write(channel[1], "Z", 1) != 1 || read(received, &payload, 1) != 1 || payload != 'Z' ||
      close(received) || close(channel[1])) {
    fail();
  }

  /* Read shutdown releases descriptors carried by unread records. */
  if (pipe2(channel, O_NONBLOCK)) {
    fail();
  }
  memset(&control, 0, sizeof(control));
  message.msg_controllen = sizeof(control.bytes);
  message.msg_flags = 0;
  header = CMSG_FIRSTHDR(&message);
  header->cmsg_len = CMSG_LEN(sizeof(int));
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  memcpy(CMSG_DATA(header), &channel[1], sizeof(int));
  if (sendmsg(pair[0], &message, 0) != 0 || close(channel[1])) {
    fail();
  }
  errno = 0;
  if (read(channel[0], &payload, 1) != -1 || errno != EAGAIN || shutdown(pair[1], SHUT_RD) ||
      read(channel[0], &payload, 1) != 0 || close(channel[0]) || close(pair[0]) || close(pair[1])) {
    fail();
  }
}

static void shutdown_contracts(void) {
  int pair[2];
  char payload[16];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair) ||
      send(pair[0], "first", 5, 0) != 5 || send(pair[0], "", 0, 0) != 0 ||
      send(pair[0], "last", 4, 0) != 4 || shutdown(pair[0], SHUT_WR)) {
    fail();
  }
  if (recv(pair[1], payload, sizeof(payload), 0) != 5 || memcmp(payload, "first", 5) ||
      recv(pair[1], payload, sizeof(payload), 0) != 0 ||
      recv(pair[1], payload, sizeof(payload), 0) != 4 || memcmp(payload, "last", 4) ||
      recv(pair[1], payload, sizeof(payload), 0) != 0 ||
      recv(pair[1], payload, sizeof(payload), MSG_DONTWAIT) != 0) {
    fail();
  }
  errno = 0;
  if (send(pair[0], "x", 1, MSG_NOSIGNAL) != -1 || errno != EPIPE ||
      send(pair[1], "reply", 5, 0) != 5 || recv(pair[0], payload, sizeof(payload), 0) != 5 ||
      memcmp(payload, "reply", 5) || shutdown(pair[0], SHUT_RD) ||
      recv(pair[0], payload, sizeof(payload), MSG_DONTWAIT) != 0) {
    fail();
  }
  errno = 0;
  if (send(pair[1], "x", 1, MSG_NOSIGNAL) != -1 || errno != EPIPE || close(pair[0]) ||
      close(pair[1])) {
    fail();
  }

  /* Peer close preserves queued records, followed by persistent EOF. */
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair) ||
      send(pair[0], "queued", 6, 0) != 6 || close(pair[0]) ||
      recv(pair[1], payload, sizeof(payload), 0) != 6 || memcmp(payload, "queued", 6) ||
      recv(pair[1], payload, sizeof(payload), 0) != 0 || close(pair[1])) {
    fail();
  }
}

static void backpressure_contracts(void) {
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair)) {
    fail();
  }
  int count = 0;
  for (; count < 1024; ++count) {
    errno = 0;
    ssize_t result = send(pair[0], "p", 1, MSG_DONTWAIT);
    if (result == -1 && errno == EAGAIN) {
      break;
    }
    if (result != 1) {
      fail();
    }
  }
  if (!count || count == 1024) {
    fail();
  }
  struct pollfd writer = {.fd = pair[0], .events = POLLOUT};
  if (poll(&writer, 1, 0) != 0) {
    fail();
  }
  char payload = 0;
  if (recv(pair[1], &payload, 1, 0) != 1 || payload != 'p' || poll(&writer, 1, 0) != 1 ||
      !(writer.revents & POLLOUT) || send(pair[0], "", 0, MSG_DONTWAIT) != 0) {
    fail();
  }
  for (int i = 1; i < count; ++i) {
    if (recv(pair[1], &payload, 1, 0) != 1 || payload != 'p') {
      fail();
    }
  }
  if (recv(pair[1], &payload, 1, 0) != 0) {
    fail();
  }
  expect_empty(pair[1]);
  if (close(pair[0]) || close(pair[1])) {
    fail();
  }
}

struct seqpacket_writer {
  int descriptor;
  unsigned char label;
  int failed;
};

static void* write_records(void* parameter) {
  struct seqpacket_writer* writer = parameter;
  unsigned char payload[32];
  memset(payload, writer->label, sizeof(payload));
  for (unsigned char i = 0; i < 64; ++i) {
    payload[1] = i;
    if (send(writer->descriptor, payload, sizeof(payload), 0) != (ssize_t)sizeof(payload)) {
      writer->failed = 1;
      break;
    }
  }
  return NULL;
}

static void concurrent_contracts(void) {
  int pair[2];
  pthread_t threads[2];
  struct seqpacket_writer writers[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair)) {
    fail();
  }
  for (int i = 0; i < 2; ++i) {
    writers[i] =
        (struct seqpacket_writer){.descriptor = pair[0], .label = (unsigned char)('a' + i)};
    if (pthread_create(&threads[i], NULL, write_records, &writers[i])) {
      fail();
    }
  }
  unsigned char expected[2] = {0, 0};
  for (int i = 0; i < 128; ++i) {
    unsigned char payload[32];
    if (recv(pair[1], payload, sizeof(payload), 0) != (ssize_t)sizeof(payload) ||
        payload[0] < 'a' || payload[0] > 'b') {
      fail();
    }
    unsigned int source = payload[0] - 'a';
    if (payload[1] != expected[source]++) {
      fail();
    }
    for (size_t j = 2; j < sizeof(payload); ++j) {
      if (payload[j] != payload[0]) {
        fail();
      }
    }
  }
  for (int i = 0; i < 2; ++i) {
    if (pthread_join(threads[i], NULL) || writers[i].failed || expected[i] != 64) {
      fail();
    }
  }
  if (close(pair[0]) || close(pair[1])) {
    fail();
  }
}

static void pathname_contracts(void) {
  static const char path[] = "/tmp/seqpacket-contract.sock";
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  memcpy(address.sun_path, path, sizeof(path));
  socklen_t address_length = offsetof(struct sockaddr_un, sun_path) + sizeof(path);
  int listener = socket(AF_UNIX, SOCK_SEQPACKET, 0);
  int client = socket(AF_UNIX, SOCK_SEQPACKET, 0);
  if (listener < 0 || client < 0) {
    fail();
  }
  (void)unlink(path);
  if (bind(listener, (struct sockaddr*)&address, address_length) || listen(listener, 4) ||
      connect(client, (struct sockaddr*)&address, address_length)) {
    fail();
  }
  int accepted = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
  char payload[16];
  if (accepted < 0 || fcntl(accepted, F_GETFD) != FD_CLOEXEC || send(client, "path", 4, 0) != 4 ||
      recv(accepted, payload, sizeof(payload), 0) != 4 || memcmp(payload, "path", 4) ||
      send(accepted, "reply", 5, 0) != 5 || recv(client, payload, sizeof(payload), 0) != 5 ||
      memcmp(payload, "reply", 5) || close(accepted) || close(client) || close(listener) ||
      unlink(path)) {
    fail();
  }
}

void test_unix_seqpacket(void) {
  puts("Testing AF_UNIX sequenced-packet record contracts... ");
  fflush(stdout);
  record_contracts();
  puts("SEQPACKET-RECORD-PASS");
  fflush(stdout);
  rights_contracts();
  puts("SEQPACKET-RIGHTS-PASS");
  fflush(stdout);
  shutdown_contracts();
  puts("SEQPACKET-SHUTDOWN-PASS");
  fflush(stdout);
  backpressure_contracts();
  puts("SEQPACKET-BACKPRESSURE-PASS");
  fflush(stdout);
  concurrent_contracts();
  puts("SEQPACKET-CONCURRENT-PASS");
  fflush(stdout);
  pathname_contracts();
  puts("SEQPACKET-PATHNAME-PASS");
  fflush(stdout);
  puts("SEQPACKET-CONTRACTS-PASS");
  fflush(stdout);
}
